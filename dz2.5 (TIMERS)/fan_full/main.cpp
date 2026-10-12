#include <Arduino.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_intr_alloc.h>
#include <driver/timer.h>

// ДЗ 2.5, варіант 5: та сама витяжка, але з усім опційним з умови.
//
// RELAY_PIN   -> R 10 кОм -> база BC547B -> колектор на вхід IN модуля реле
// Контакт реле (COM-NO) подає 5 В на двигун-вентилятор, паралельно двигуну 1N4007
// BUTTON_PIN  <- кнопка ручного пуску, другий контакт на GND
//
// Основа — два апаратні таймери з варіанта two_timers. Додано:
//
//   Watchdog                   Task WDT з власним таймаутом; команда 'h' вішає
//                              loop() навмисно, щоб побачити перезавантаження
//   логування станів           кожна подія з міткою часу, статистика, причина ребуту
//   захист від повторного      кнопка не діє, поки вентилятор працює; після стопу
//   запуску                    ручний пуск заблокований MIN_OFF_S; після ребуту
//                              фаза продовжується, а не починається заново
//   LED-індикатори             зелений — робота, другий — пауза, третій — увага
//
// Стан фази живе в RTC-пам'яті, яка переживає програмний ребут. Це не косметика:
// без неї кожне спрацювання watchdog лікувало б зависання ціною зіпсованого
// розкладу — відлік години почався б з нуля, а вентилятор із повних 15 хвилин.
//
// Силова частина тут живиться від окремого модуля, тільки землі спільні. У ДЗ 2.2
// моторчик сидів на тій самій шині 5 В, що й плата, і пусковий струм заганяв
// мікроконтролер у brownout; рознесене живлення — прямий висновок звідти.

#define FAST_DEMO 1

// Переривання таймера можна зареєструвати з прапорцем ESP_INTR_FLAG_IRAM. Тоді
// воно обслуговується навіть коли кеш flash вимкнений (запис у NVS, OTA), але
// кожна функція в обробнику зобов'язана лежати в IRAM. Усі виклики тут це
// витримують, і прошивка все одно перевіряє результат у runtime: якщо з прапорцем
// переривання не зареєструвалось, вона відкочується на стандартне підключення.
#define USE_IRAM_INTERRUPT 1

struct Config {
  // Периферія
  static constexpr uint8_t RELAY_PIN    = 8;
  static constexpr uint8_t LED_FAN_PIN  = 4;  // зелений
  static constexpr uint8_t LED_IDLE_PIN = 5;  // пауза
  static constexpr uint8_t LED_WARN_PIN = 6;  // увага
  static constexpr uint8_t BUTTON_PIN   = 16;

  static constexpr uint8_t RELAY_ON_LEVEL       = HIGH;
  static constexpr uint8_t BUTTON_PRESSED_LEVEL = LOW;

  // Час роботи і період
#if FAST_DEMO
  static constexpr uint32_t PERIOD_S    = 20;
  static constexpr uint32_t RUN_S       = 5;
  static constexpr uint32_t MIN_OFF_S   = 8;  // «10 хвилин» захисту від повторного пуску
  static constexpr uint32_t LOG_EVERY_S = 1;
#else
  static constexpr uint32_t PERIOD_S    = 60UL * 60UL;
  static constexpr uint32_t RUN_S       = 15UL * 60UL;
  static constexpr uint32_t MIN_OFF_S   = 10UL * 60UL;
  static constexpr uint32_t LOG_EVERY_S = 60;
#endif
  static constexpr uint32_t IDLE_S = PERIOD_S - RUN_S;

  static constexpr uint64_t PERIOD_US = static_cast<uint64_t>(PERIOD_S) * 1000000ULL;
  static constexpr uint64_t RUN_US    = static_cast<uint64_t>(RUN_S) * 1000000ULL;

  // Таймери
  static constexpr uint8_t  PERIOD_TIMER = 0;
  static constexpr uint8_t  RUN_TIMER    = 1;
  static constexpr uint16_t DIVIDER      = 80;  // тік 1 мкс

  // Watchdog
  static constexpr uint32_t WDT_TIMEOUT_S = 8;

  // Кнопка: той самий фільтр по чотирьох вимірах, що в ДЗ 2.4 polling_debounce —
  // він там дав нуль хибних спрацювань на двох серіях.
  static constexpr uint32_t POLL_INTERVAL_MS = 5;
  static constexpr uint8_t  STABLE_SAMPLES   = 4;

  // Індикація
  static constexpr uint32_t WARN_BLINK_MS = 150;
  static constexpr uint32_t WARN_HOLD_MS  = 3000;  // скільки світити після ненормального ребуту

  static constexpr uint32_t SERIAL_BAUD = 115200;
};

static_assert(Config::RUN_S > 0 && Config::RUN_S < Config::PERIOD_S, "перевір час роботи і період");
static_assert(Config::PERIOD_S == Config::RUN_S * 4,
              "налагоджувальний масштаб має зберігати пропорцію 15 хв із 60");
static_assert(Config::MIN_OFF_S <= Config::IDLE_S,
              "блокування ручного пуску не може бути довшим за паузу");
static_assert(Config::WDT_TIMEOUT_S >= 2, "надто короткий таймаут watchdog перезавантажить плату на старті");
// Для пінів від 32 і вище потрібні інші регістри (OUT1_W1TS / OUT1_W1TC).
static_assert(Config::RELAY_PIN < 32 && Config::LED_FAN_PIN < 32 && Config::LED_IDLE_PIN < 32,
              "запис у GPIO з переривання тут розрахований на піни 0..31");

static constexpr uint8_t GROUP_OF[4] = {0, 1, 0, 1};
static constexpr uint8_t INDEX_OF[4] = {0, 0, 1, 1};
static constexpr timer_group_t RUN_TG  = static_cast<timer_group_t>(GROUP_OF[Config::RUN_TIMER]);
static constexpr timer_idx_t   RUN_TIX = static_cast<timer_idx_t>(INDEX_OF[Config::RUN_TIMER]);
static constexpr timer_group_t PER_TG  = static_cast<timer_group_t>(GROUP_OF[Config::PERIOD_TIMER]);
static constexpr timer_idx_t   PER_TIX = static_cast<timer_idx_t>(INDEX_OF[Config::PERIOD_TIMER]);

// --- запис у GPIO з переривання ---------------------------------------------
//
// В обробниках нижче digitalWrite() не використовується, і це не стилістика.
// ARDUINO_ISR_ATTR, яким він позначений у core 2.0.17, розкривається в IRAM_ATTR
// тільки при CONFIG_ARDUINO_ISR_IRAM, а в готовому sdkconfig arduino-esp32 цей
// параметр не встановлений — тож __digitalWrite лежить у flash. Поки переривання
// реєструється без ESP_INTR_FLAG_IRAM, це нікому не шкодить (воно просто
// замасковане, доки кеш flash вимкнений), але з прапорцем така гілка впала б.
//
// Тому рівень виставляється одним записом у регістр. Карта регістрів та сама, що
// в ДЗ 2.1 (blink_mmio), звідси потрібні рівно два.

namespace Reg {
  constexpr uint32_t GPIO_BASE = 0x60004000;
  constexpr uint32_t OUT_W1TS  = GPIO_BASE + 0x008;  // одиниця в біт -> пін у HIGH
  constexpr uint32_t OUT_W1TC  = GPIO_BASE + 0x00C;  // одиниця в біт -> пін у LOW

  inline volatile uint32_t* at(uint32_t address) {
    return reinterpret_cast<volatile uint32_t*>(address);
  }
}  // namespace Reg

// Без inline: inline-функція могла б не вбудуватись і опинитися у flash, а тут
// потрібна гарантія. Напрямок виходу вмикає звичайний pinMode() у setup(), з
// переривання змінюється тільки рівень.
static void IRAM_ATTR writePin(uint8_t pin, bool high) {
  *Reg::at(high ? Reg::OUT_W1TS : Reg::OUT_W1TC) = 1u << pin;
}

constexpr bool RELAY_ON_IS_HIGH = Config::RELAY_ON_LEVEL == HIGH;

// --- стан, що переживає програмний ребут ------------------------------------
//
// RTC_NOINIT_ATTR означає «не ініціалізувати взагалі, ні при якому старті». Тому
// вміст цих змінних після подачі живлення — сміття, і єдиний спосіб відрізнити
// збережений стан від сміття — контрольне слово.

static constexpr uint32_t RTC_MAGIC = 0x2E35FA11;  // «dz2.5 fan»

RTC_NOINIT_ATTR static uint32_t rtcMagic;
RTC_NOINIT_ATTR static uint32_t rtcCycles;      // скільки разів вентилятор запускався
RTC_NOINIT_ATTR static uint32_t rtcRunSeconds;  // скільки всього пропрацював
RTC_NOINIT_ATTR static uint32_t rtcBoots;       // скільки разів плата перезавантажувалась
RTC_NOINIT_ATTR static uint8_t  rtcPhaseRunning;
RTC_NOINIT_ATTR static uint32_t rtcPhaseLeftS;  // скільки лишалось у фазі на момент запису

// --- стан автомата ----------------------------------------------------------

static hw_timer_t* periodTimer = nullptr;
static hw_timer_t* runTimer    = nullptr;

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool     fanRunning = false;
static volatile uint32_t cycles     = 0;
static volatile uint32_t starts     = 0;
static volatile uint32_t stops      = 0;
static volatile int64_t  startedUs  = 0;
static volatile int64_t  lastRunUs  = 0;
static volatile uint64_t runAlarmUs = 0;
static volatile uint32_t startLatencyUs = 0;
static volatile uint32_t stopLatencyUs  = 0;
static volatile uint32_t maxLatencyUs   = 0;
static volatile bool     lastStartManual = false;

// Поза перериванням: статистика, яку веде loop()
static uint32_t rejectedStarts = 0;  // ручних пусків, відхилених захистом
static int64_t  lastStopUs     = 0;  // коли вентилятор зупинився останній раз
static int64_t  totalRunUs     = 0;

static void IRAM_ATTR applyOutputs(bool running) {
  writePin(Config::RELAY_PIN, running == RELAY_ON_IS_HIGH);
  writePin(Config::LED_FAN_PIN, running);
  writePin(Config::LED_IDLE_PIN, !running);
}

// --- переривання ------------------------------------------------------------
//
// В обох обробниках немає жодного виклику, який лежав би у flash: рівень на GPIO
// виставляється записом у регістр, esp_timer_get_time() і спінлоки FreeRTOS уже в
// IRAM, а родина *_in_isr() з driver/timer.h складена в секції .iram1. Саме через
// це тут можна дозволити собі ESP_INTR_FLAG_IRAM. Перевірити це на зібраній
// прошивці можна за адресами символів — як саме, описано в README ДЗ.

void IRAM_ATTR onPeriod() {
  const uint32_t latency = static_cast<uint32_t>(timer_group_get_counter_value_in_isr(PER_TG, PER_TIX));
  const uint64_t runNow  = timer_group_get_counter_value_in_isr(RUN_TG, RUN_TIX);

  // Захист від повторного запуску на найнижчому рівні: якщо вентилятор уже
  // крутиться (його пустили кнопкою кілька секунд тому), періодичний таймер не
  // перезаводить тривалість і не накручує лічильник циклів. Інакше ручний пуск
  // міг би продовжитись на повні 15 хвилин від моменту автоматичного.
  const uint64_t alarm = runNow + Config::RUN_US;

  bool already;
  portENTER_CRITICAL_ISR(&stateMux);
  already = fanRunning;
  if (!already) {
    fanRunning = true;
    ++cycles;
    ++starts;
    startedUs       = esp_timer_get_time();
    startLatencyUs  = latency;
    lastStartManual = false;
    runAlarmUs      = alarm;
    if (latency > maxLatencyUs) maxLatencyUs = latency;
  }
  portEXIT_CRITICAL_ISR(&stateMux);

  if (already) return;

  applyOutputs(true);
  timer_group_set_alarm_value_in_isr(RUN_TG, RUN_TIX, alarm);
  timer_group_enable_alarm_in_isr(RUN_TG, RUN_TIX);
}

void IRAM_ATTR onRunEnd() {
  const uint64_t runNow = timer_group_get_counter_value_in_isr(RUN_TG, RUN_TIX);

  portENTER_CRITICAL_ISR(&stateMux);
  fanRunning = false;
  ++stops;
  lastRunUs     = esp_timer_get_time() - startedUs;
  stopLatencyUs = runNow > runAlarmUs ? static_cast<uint32_t>(runNow - runAlarmUs) : 0;
  if (stopLatencyUs > maxLatencyUs) maxLatencyUs = stopLatencyUs;
  portEXIT_CRITICAL_ISR(&stateMux);

  applyOutputs(false);

  // Компаратор відводиться туди, куди лічильник не дійде до наступного пуску.
  timer_group_set_alarm_value_in_isr(RUN_TG, RUN_TIX, runNow + 2 * Config::PERIOD_US);
}

// --- самоперевірка переривання ----------------------------------------------

static volatile bool selfTestFired = false;

void IRAM_ATTR onSelfTest() {
  selfTestFired = true;
}

// Повертає true, якщо з такими прапорцями переривання таймера справді спрацьовує.
// timerAttachInterruptFlag() у core 2.0.17 не повертає код помилки, тому єдиний
// спосіб дізнатися, чи прапорець прийнявся, — коротко завести таймер і подивитись.
static bool interruptWorks(hw_timer_t* timer, int flags) {
  selfTestFired = false;

  timerAlarmDisable(timer);
  timerAttachInterruptFlag(timer, &onSelfTest, false, flags);
  timerWrite(timer, 0);
  timerAlarmWrite(timer, 20000, false);  // 20 мс
  timerAlarmEnable(timer);

  const uint32_t deadline = millis() + 200;
  while (!selfTestFired && static_cast<int32_t>(millis() - deadline) < 0) {
    // навмисно зайнятий цикл: планувальник тут ні до чого, чекаємо саме переривання
  }

  timerAlarmDisable(timer);
  timerDetachInterrupt(timer);
  timerWrite(timer, 0);
  return selfTestFired;
}

// --- кнопка ----------------------------------------------------------------
//
// Фільтр по чотирьох однакових вимірах із кроком 5 мс, як у ДЗ 2.4. Переривання
// тут не потрібне: ручний пуск не часочутливий, а loop() і так вільний.

enum class ButtonState : uint8_t { Released, PressRising, Pressed, ReleaseRising };

static ButtonState buttonState = ButtonState::Released;
static uint8_t     stableCount = 0;
static uint32_t    lastPollMs  = 0;

static bool buttonPressedEdge() {
  if (millis() - lastPollMs < Config::POLL_INTERVAL_MS) return false;
  lastPollMs = millis();

  const bool down = digitalRead(Config::BUTTON_PIN) == Config::BUTTON_PRESSED_LEVEL;

  switch (buttonState) {
    case ButtonState::Released:
      if (down) { buttonState = ButtonState::PressRising; stableCount = 1; }
      break;
    case ButtonState::PressRising:
      if (!down) { buttonState = ButtonState::Released; break; }
      if (++stableCount >= Config::STABLE_SAMPLES) {
        buttonState = ButtonState::Pressed;
        return true;
      }
      break;
    case ButtonState::Pressed:
      if (!down) { buttonState = ButtonState::ReleaseRising; stableCount = 1; }
      break;
    case ButtonState::ReleaseRising:
      if (down) { buttonState = ButtonState::Pressed; break; }
      if (++stableCount >= Config::STABLE_SAMPLES) buttonState = ButtonState::Released;
      break;
  }
  return false;
}

// --- індикація «увага» ------------------------------------------------------

static int64_t warnUntilUs = 0;

static void warn(uint32_t ms) {
  warnUntilUs = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
}

static void updateWarnLed() {
  const int64_t now = esp_timer_get_time();
  if (now >= warnUntilUs) {
    digitalWrite(Config::LED_WARN_PIN, LOW);
    return;
  }
  const bool on = ((now / 1000) / Config::WARN_BLINK_MS) % 2 == 0;
  digitalWrite(Config::LED_WARN_PIN, on ? HIGH : LOW);
}

// --- знімок стану -----------------------------------------------------------

struct Snapshot {
  bool     running;
  uint32_t cycles;
  uint32_t starts;
  uint32_t stops;
  int64_t  startedUs;
  int64_t  lastRunUs;
  uint32_t startLatencyUs;
  uint32_t stopLatencyUs;
  uint32_t maxLatencyUs;
  bool     lastStartManual;
};

static Snapshot snapshot() {
  Snapshot s;
  portENTER_CRITICAL(&stateMux);
  s.running         = fanRunning;
  s.cycles          = cycles;
  s.starts          = starts;
  s.stops           = stops;
  s.startedUs       = startedUs;
  s.lastRunUs       = lastRunUs;
  s.startLatencyUs  = startLatencyUs;
  s.stopLatencyUs   = stopLatencyUs;
  s.maxLatencyUs    = maxLatencyUs;
  s.lastStartManual = lastStartManual;
  portEXIT_CRITICAL(&stateMux);
  return s;
}

static const char* phaseLabel(bool running) {
  return running ? "РОБОТА" : "ПАУЗА ";
}

static uint32_t secondsLeft(const Snapshot& s) {
  if (periodTimer == nullptr) return 0;
  if (s.running) {
    const int64_t left = static_cast<int64_t>(Config::RUN_US) - (esp_timer_get_time() - s.startedUs);
    return left <= 0 ? 0 : static_cast<uint32_t>(left / 1000000);
  }
  const uint64_t elapsed = timerRead(periodTimer);
  return elapsed >= Config::PERIOD_US
             ? 0
             : static_cast<uint32_t>((Config::PERIOD_US - elapsed) / 1000000ULL);
}

static void printStatus(const Snapshot& s, const char* mark) {
  Serial.printf("[%9.3f] %s лишилось %5u s  цикл %-4u%s\r\n",
                esp_timer_get_time() / 1e6,
                phaseLabel(s.running),
                secondsLeft(s),
                s.cycles,
                mark);
}

// --- ручний пуск ------------------------------------------------------------

static void manualStart() {
  const Snapshot s = snapshot();

  // Перший рівень захисту: вентилятор уже працює.
  if (s.running) {
    ++rejectedStarts;
    warn(600);
    Serial.printf("[%9.3f] ручний пуск відхилено: вентилятор уже працює, лишилось %u s\r\n",
                  esp_timer_get_time() / 1e6, secondsLeft(s));
    return;
  }

  // Другий: після зупинки має пройти MIN_OFF_S. Це захист не від подвійного
  // натискання, а від самого режиму «увімкнув-вимкнув-увімкнув»: ні реле, ні
  // двигун частих пусків не люблять.
  if (lastStopUs != 0) {
    const int64_t offUs = esp_timer_get_time() - lastStopUs;
    const int64_t needUs = static_cast<int64_t>(Config::MIN_OFF_S) * 1000000;
    if (offUs < needUs) {
      ++rejectedStarts;
      warn(600);
      Serial.printf("[%9.3f] ручний пуск відхилено: від зупинки минуло %lld s, потрібно %u\r\n",
                    esp_timer_get_time() / 1e6, offUs / 1000000, Config::MIN_OFF_S);
      return;
    }
  }

  // Пуск приймається. Лічильник періодичного таймера скидається, щоб наступний
  // автоматичний пуск стояв за цілий період від цього моменту, а не приїхав
  // одразу слідом.
  portENTER_CRITICAL(&stateMux);
  fanRunning      = true;
  ++cycles;
  ++starts;
  startedUs       = esp_timer_get_time();
  lastStartManual = true;
  portEXIT_CRITICAL(&stateMux);

  applyOutputs(true);
  timerWrite(periodTimer, 0);
  timerWrite(runTimer, 0);
  runAlarmUs = Config::RUN_US;
  timerAlarmWrite(runTimer, Config::RUN_US, false);
  timerAlarmEnable(runTimer);

  Serial.printf("[%9.3f] ручний пуск прийнято, вентилятор на %u s\r\n",
                esp_timer_get_time() / 1e6, Config::RUN_S);
}

// --- відновлення стану після ребуту ----------------------------------------

static const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "подача живлення";
    case ESP_RST_EXT:      return "зовнішній сигнал RESET";
    case ESP_RST_SW:       return "програмний esp_restart()";
    case ESP_RST_PANIC:    return "panic або виняток";
    case ESP_RST_INT_WDT:  return "Interrupt WDT";
    case ESP_RST_TASK_WDT: return "Task WDT";
    case ESP_RST_WDT:      return "інший watchdog";
    case ESP_RST_BROWNOUT: return "brownout, просідання живлення";
    case ESP_RST_DEEPSLEEP: return "вихід із deep sleep";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "невідома";
  }
}

// Заводить таймери так, щоб фаза продовжилась із того місця, де її застав ребут.
static void armTimers(bool running, uint32_t leftS) {
  if (running) {
    // У робочій фазі лишилось leftS -> вона почалася (RUN_S - leftS) тому.
    const uint32_t elapsed = Config::RUN_S - leftS;
    timerWrite(periodTimer, static_cast<uint64_t>(elapsed) * 1000000ULL);
    timerWrite(runTimer, 0);
    runAlarmUs = static_cast<uint64_t>(leftS) * 1000000ULL;
    timerAlarmWrite(runTimer, runAlarmUs, false);
    timerAlarmEnable(runTimer);

    portENTER_CRITICAL(&stateMux);
    fanRunning = true;
    startedUs  = esp_timer_get_time() - static_cast<int64_t>(elapsed) * 1000000;
    portEXIT_CRITICAL(&stateMux);
    applyOutputs(true);
  } else {
    // У паузі лишилось leftS -> періодичний таймер має спрацювати через leftS.
    timerWrite(periodTimer, Config::PERIOD_US - static_cast<uint64_t>(leftS) * 1000000ULL);
    timerWrite(runTimer, 0);
    runAlarmUs = 2 * Config::PERIOD_US;
    timerAlarmWrite(runTimer, runAlarmUs, false);
    timerAlarmEnable(runTimer);
    applyOutputs(false);
  }

  timerAlarmWrite(periodTimer, Config::PERIOD_US, true);
  timerAlarmEnable(periodTimer);
}

static void saveState(const Snapshot& s) {
  rtcMagic        = RTC_MAGIC;
  rtcCycles       = s.cycles;
  rtcPhaseRunning = s.running ? 1 : 0;
  rtcPhaseLeftS   = secondsLeft(s);
  rtcRunSeconds   = static_cast<uint32_t>(totalRunUs / 1000000);
}

static void clearState() {
  rtcMagic = 0;
  Serial.println("збережений стан стерто, наступний старт почнеться з чистої паузи");
}

// --- команди в монітор ------------------------------------------------------

static void printHelp() {
  Serial.println("команди: s - стан, m - ручний пуск, h - повісити loop (демо watchdog),");
  Serial.println("         z - стерти збережений стан, ? - ця довідка");
}

static void printStats(const Snapshot& s) {
  Serial.println();
  Serial.println("--- статистика ---");
  Serial.printf("циклів: %u, пусків: %u, стопів: %u, відхилених ручних пусків: %u\r\n",
                s.cycles, s.starts, s.stops, rejectedStarts);
  Serial.printf("усього пропрацював: %lld s\r\n", totalRunUs / 1000000);
  Serial.printf("перезавантажень плати: %u\r\n", rtcBoots);
  Serial.printf("найгірша затримка входу в ISR: %u мкс\r\n", s.maxLatencyUs);
  Serial.printf("останній пуск: %s\r\n", s.lastStartManual ? "ручний" : "за таймером");
  Serial.println();
}

static void hangOnPurpose() {
  Serial.printf("вішаю loop() навмисно. Task WDT має перезавантажити плату через %u s\r\n",
                Config::WDT_TIMEOUT_S);
  Serial.flush();
  // Ні esp_task_wdt_reset(), ні виходу з функції. Саме та ситуація, від якої
  // watchdog і потрібен: програма жива, але нічого корисного не робить.
  // Цикл навмисно з volatile-змінною: порожній while(true) компілятор має право
  // прибрати. Переривання при цьому продовжують приходити, тож вентилятор
  // перемикатиметься до самого ребуту — апаратний таймер навіть не помітить, що
  // основна програма мертва.
  volatile uint32_t spin = 0;
  while (true) ++spin;
}

static void handleSerial(const Snapshot& s) {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 's': printStatus(s, ""); printStats(s); break;
      case 'm': manualStart(); break;
      case 'h': hangOnPurpose(); break;
      case 'z': clearState(); break;
      case '?': printHelp(); break;
      default: break;
    }
  }
}

// --- setup / loop -----------------------------------------------------------

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pinMode(Config::RELAY_PIN, OUTPUT);
  pinMode(Config::LED_FAN_PIN, OUTPUT);
  pinMode(Config::LED_IDLE_PIN, OUTPUT);
  pinMode(Config::LED_WARN_PIN, OUTPUT);
  pinMode(Config::BUTTON_PIN, INPUT_PULLUP);
  applyOutputs(false);
  digitalWrite(Config::LED_WARN_PIN, LOW);

  const esp_reset_reason_t reason = esp_reset_reason();

  Serial.println();
  Serial.println("=== ДЗ 2.5, варіант 5: витяжка з watchdog, кнопкою і збереженням стану ===");
  Serial.printf("причина старту: %s\r\n", resetReasonText(reason));
  Serial.printf("період %u с, робота %u с, пауза %u с, блокування ручного пуску %u с\r\n",
                Config::PERIOD_S, Config::RUN_S, Config::IDLE_S, Config::MIN_OFF_S);

  // Збережений стан
  bool     restored = false;
  bool     phase    = false;
  uint32_t leftS    = Config::IDLE_S;

  if (rtcMagic == RTC_MAGIC) {
    restored = true;
    ++rtcBoots;
    phase = rtcPhaseRunning != 0;
    leftS = rtcPhaseLeftS;
    // Межі перевіряються: у RTC-пам'яті може лежати що завгодно, якщо контрольне
    // слово випадково збіглося або запис застав ребут на півдорозі.
    const uint32_t limit = phase ? Config::RUN_S : Config::PERIOD_S;
    if (leftS == 0 || leftS > limit) {
      Serial.printf("збережений залишок %u s не влазить у фазу, беру повну\r\n", leftS);
      leftS = limit;
    }
    portENTER_CRITICAL(&stateMux);
    cycles = rtcCycles;
    portEXIT_CRITICAL(&stateMux);
    totalRunUs = static_cast<int64_t>(rtcRunSeconds) * 1000000;

    Serial.printf("стан відновлено: %s, лишилось %u s, циклів до цього %u\r\n",
                  phase ? "РОБОТА" : "ПАУЗА", leftS, rtcCycles);
  } else {
    rtcBoots      = 1;
    rtcCycles     = 0;
    rtcRunSeconds = 0;
    Serial.println("збереженого стану немає, починаю з паузи");
  }

  if (reason != ESP_RST_POWERON && reason != ESP_RST_EXT) {
    warn(Config::WARN_HOLD_MS);
    Serial.println("старт ненормальний — червоний світлодіод блимає");
  }

  // Таймери
  periodTimer = timerBegin(Config::PERIOD_TIMER, Config::DIVIDER, true);
  runTimer    = timerBegin(Config::RUN_TIMER, Config::DIVIDER, true);
  if (periodTimer == nullptr || runTimer == nullptr) {
    Serial.println("ПРОБЛЕМА: таймер недоступний, цикл не запуститься");
    Serial.printf("  періоду: %s, тривалості: %s\r\n",
                  periodTimer ? "ок" : "nullptr", runTimer ? "ок" : "nullptr");
    return;
  }

  int flags = USE_IRAM_INTERRUPT ? ESP_INTR_FLAG_IRAM : 0;
  if (flags != 0 && !interruptWorks(periodTimer, flags)) {
    Serial.println("ESP_INTR_FLAG_IRAM не прийнявся, відкочуюсь на стандартне підключення");
    flags = 0;
  }
  if (!interruptWorks(periodTimer, flags)) {
    Serial.println("ПРОБЛЕМА: переривання таймера не спрацьовує взагалі");
    return;
  }
  Serial.printf("переривання таймера перевірено%s\r\n",
                flags == ESP_INTR_FLAG_IRAM ? " (в IRAM, працює й при вимкненому кеші flash)" : "");

  timerAttachInterruptFlag(periodTimer, &onPeriod, false, flags);
  timerAttachInterruptFlag(runTimer, &onRunEnd, false, flags);
  armTimers(phase, leftS);

  // Watchdog. Task WDT у arduino-esp32 уже ввімкнений (таймаут 5 с, panic), але
  // підписані на нього лише idle-задачі ядра 0. loopTask живе на ядрі 1 і за
  // замовчуванням не стежиться взагалі — тобто зависання loop() нічого не ламає.
  // Тому тут таймаут переставляється і loopTask підписується явно.
  esp_task_wdt_init(Config::WDT_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);
  Serial.printf("Task WDT: таймаут %u с, режим перезавантаження, loopTask підписано\r\n",
                Config::WDT_TIMEOUT_S);

  if (!restored) Serial.println("цикл запущено");
  printHelp();
  Serial.println();
}

void loop() {
  static uint32_t lastStarts = 0;
  static uint32_t lastStops  = 0;
  static int64_t  lastLogUs  = 0;
  static int64_t  lastSaveUs = 0;

  // Watchdog годується тут — у єдиному місці, де програма справді живе. Якщо
  // нижче щось зависне, цей рядок більше не виконається.
  esp_task_wdt_reset();

  const Snapshot s = snapshot();

  handleSerial(s);
  if (buttonPressedEdge()) manualStart();
  updateWarnLed();

  // Події від таймерів
  if (s.starts != lastStarts) {
    lastStarts = s.starts;
    char mark[128];
    snprintf(mark, sizeof(mark), "  <- пуск %s, затримка ISR %u мкс",
             s.lastStartManual ? "ручний" : "за таймером", s.startLatencyUs);
    printStatus(s, mark);
  }
  if (s.stops != lastStops) {
    lastStops  = s.stops;
    lastStopUs = esp_timer_get_time();
    totalRunUs += s.lastRunUs;
    char mark[128];
    snprintf(mark, sizeof(mark), "  <- стоп, пропрацював %lld.%06llu с (треба %u), затримка ISR %u мкс",
             s.lastRunUs / 1000000, static_cast<uint64_t>(s.lastRunUs % 1000000),
             Config::RUN_S, s.stopLatencyUs);
    printStatus(s, mark);
  }

  const int64_t now = esp_timer_get_time();

  // Стан у RTC-пам'ять раз на секунду: ребут втратить не більше однієї секунди фази.
  if (now - lastSaveUs >= 1000000) {
    lastSaveUs = now;
    saveState(s);
  }

  if (now - lastLogUs >= static_cast<int64_t>(Config::LOG_EVERY_S) * 1000000) {
    lastLogUs = now;
    printStatus(s, "");
  }

  vTaskDelay(pdMS_TO_TICKS(5));
}
