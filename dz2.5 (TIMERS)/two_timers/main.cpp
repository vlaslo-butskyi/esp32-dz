#include <Arduino.h>
#include <esp_timer.h>
#include <driver/timer.h>

#define FAST_DEMO 1

struct Config {
  // Периферія
  static constexpr uint8_t RELAY_PIN    = 8;
  static constexpr uint8_t LED_FAN_PIN  = 4;
  static constexpr uint8_t LED_IDLE_PIN = 5;  // червоний: пауза

  static constexpr uint8_t RELAY_ON_LEVEL       = HIGH;

  // Час роботи і період
#if FAST_DEMO
  static constexpr uint32_t PERIOD_S    = 20;
  static constexpr uint32_t RUN_S       = 5;
  static constexpr uint32_t LOG_EVERY_S = 1;
#else
  static constexpr uint32_t PERIOD_S    = 60UL * 60UL;
  static constexpr uint32_t RUN_S       = 15UL * 60UL;
  static constexpr uint32_t LOG_EVERY_S = 60;
#endif
  static constexpr uint32_t IDLE_S = PERIOD_S - RUN_S;

  static constexpr uint64_t PERIOD_US = static_cast<uint64_t>(PERIOD_S) * 1000000ULL;
  static constexpr uint64_t RUN_US    = static_cast<uint64_t>(RUN_S) * 1000000ULL;

  // Таймери
  static constexpr uint8_t  PERIOD_TIMER = 0;  // TIMERG0, таймер 0 — ритм
  static constexpr uint8_t  RUN_TIMER    = 1;  // TIMERG1, таймер 0 — тривалість
  static constexpr uint32_t APB_HZ       = 80000000;
  static constexpr uint16_t DIVIDER      = 80;  // тік 1 мкс

  static constexpr uint32_t SERIAL_BAUD = 115200;
};

static_assert(Config::RUN_S > 0, "час роботи має бути додатним");
static_assert(Config::RUN_S < Config::PERIOD_S, "вентилятор не може працювати довше за період");
static_assert(Config::PERIOD_S == Config::RUN_S * 4,
              "налагоджувальний масштаб має зберігати пропорцію 15 хв із 60");
static_assert(Config::PERIOD_TIMER != Config::RUN_TIMER, "таймери мають бути різні");

static constexpr uint8_t GROUP_OF[4] = {0, 1, 0, 1};
static constexpr uint8_t INDEX_OF[4] = {0, 0, 1, 1};

static constexpr timer_group_t RUN_TG  = static_cast<timer_group_t>(GROUP_OF[Config::RUN_TIMER]);
static constexpr timer_idx_t   RUN_TIX = static_cast<timer_idx_t>(INDEX_OF[Config::RUN_TIMER]);
static constexpr timer_group_t PER_TG  = static_cast<timer_group_t>(GROUP_OF[Config::PERIOD_TIMER]);
static constexpr timer_idx_t   PER_TIX = static_cast<timer_idx_t>(INDEX_OF[Config::PERIOD_TIMER]);

constexpr uint8_t inverted(uint8_t level) {
  return level == HIGH ? LOW : HIGH;
}

// --- стан автомата ----------------------------------------------------------

static hw_timer_t* periodTimer = nullptr;
static hw_timer_t* runTimer    = nullptr;

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool     fanRunning = false;
static volatile uint32_t cycles     = 0;
static volatile uint32_t starts     = 0;  // спрацювань таймера періоду
static volatile uint32_t stops      = 0;  // спрацювань таймера тривалості
static volatile int64_t  startedUs  = 0;
static volatile int64_t  stoppedUs  = 0;
static volatile uint32_t startLatencyUs = 0;
static volatile uint32_t stopLatencyUs  = 0;
static volatile int64_t  lastRunUs = 0;
static volatile uint64_t runAlarmUs = 0;

static void IRAM_ATTR applyOutputs(bool running) {
  digitalWrite(Config::RELAY_PIN, running ? Config::RELAY_ON_LEVEL
                                          : inverted(Config::RELAY_ON_LEVEL));
  digitalWrite(Config::LED_FAN_PIN,  running ? HIGH : LOW);
  digitalWrite(Config::LED_IDLE_PIN, running ? LOW : HIGH);
}

void IRAM_ATTR onPeriod() {
  const uint32_t latency = static_cast<uint32_t>(timer_group_get_counter_value_in_isr(PER_TG, PER_TIX));
  const uint64_t runNow  = timer_group_get_counter_value_in_isr(RUN_TG, RUN_TIX);
  const uint64_t alarm   = runNow + Config::RUN_US;

  portENTER_CRITICAL_ISR(&stateMux);
  fanRunning = true;
  ++cycles;
  ++starts;
  startedUs      = esp_timer_get_time();
  startLatencyUs = latency;
  runAlarmUs     = alarm;
  portEXIT_CRITICAL_ISR(&stateMux);

  applyOutputs(true);

  timer_group_set_alarm_value_in_isr(RUN_TG, RUN_TIX, alarm);
  timer_group_enable_alarm_in_isr(RUN_TG, RUN_TIX);
}

void IRAM_ATTR onRunEnd() {
  const uint64_t runNow = timer_group_get_counter_value_in_isr(RUN_TG, RUN_TIX);

  portENTER_CRITICAL_ISR(&stateMux);
  fanRunning = false;
  ++stops;
  stoppedUs = esp_timer_get_time();
  lastRunUs = stoppedUs - startedUs;
  stopLatencyUs = runNow > runAlarmUs ? static_cast<uint32_t>(runNow - runAlarmUs) : 0;
  portEXIT_CRITICAL_ISR(&stateMux);

  applyOutputs(false);

  timer_group_set_alarm_value_in_isr(RUN_TG, RUN_TIX, runNow + 2 * Config::PERIOD_US);
}

// --- знімок стану для логу --------------------------------------------------

struct Snapshot {
  bool     running;
  uint32_t cycles;
  uint32_t starts;
  uint32_t stops;
  int64_t  startedUs;
  int64_t  stoppedUs;
  int64_t  lastRunUs;
  uint32_t startLatencyUs;
  uint32_t stopLatencyUs;
};

static Snapshot snapshot() {
  Snapshot s;
  portENTER_CRITICAL(&stateMux);
  s.running        = fanRunning;
  s.cycles         = cycles;
  s.starts         = starts;
  s.stops          = stops;
  s.startedUs      = startedUs;
  s.stoppedUs      = stoppedUs;
  s.lastRunUs      = lastRunUs;
  s.startLatencyUs = startLatencyUs;
  s.stopLatencyUs  = stopLatencyUs;
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
  Serial.printf("[%9.3f] %s лишилось %4u s  цикл %-3u  пусків %-3u  стопів %-3u%s\r\n",
                esp_timer_get_time() / 1e6,
                phaseLabel(s.running),
                secondsLeft(s),
                s.cycles, s.starts, s.stops,
                mark);
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pinMode(Config::RELAY_PIN, OUTPUT);
  pinMode(Config::LED_FAN_PIN, OUTPUT);
  pinMode(Config::LED_IDLE_PIN, OUTPUT);
  applyOutputs(false);

  Serial.println();
  Serial.println("=== ДЗ 2.5, варіант 3: два апаратні таймери ===");
  Serial.printf("період %u с, робота %u с, пауза %u с\r\n",
                Config::PERIOD_S, Config::RUN_S, Config::IDLE_S);
  Serial.printf("таймер періоду: індекс %u -> TIMERG%u.%u, аларм %llu мкс, авто-перезавантаження\r\n",
                Config::PERIOD_TIMER, static_cast<unsigned>(PER_TG),
                static_cast<unsigned>(PER_TIX), Config::PERIOD_US);
  Serial.printf("таймер тривалості: індекс %u -> TIMERG%u.%u, компаратор +%llu мкс, одноразовий\r\n",
                Config::RUN_TIMER, static_cast<unsigned>(RUN_TG),
                static_cast<unsigned>(RUN_TIX), Config::RUN_US);

  periodTimer = timerBegin(Config::PERIOD_TIMER, Config::DIVIDER, true);
  runTimer    = timerBegin(Config::RUN_TIMER, Config::DIVIDER, true);
  if (periodTimer == nullptr || runTimer == nullptr) {
    Serial.println("ПРОБЛЕМА: один із таймерів недоступний, цикл не запуститься");
    Serial.printf("  періоду: %s, тривалості: %s\r\n",
                  periodTimer ? "ок" : "nullptr", runTimer ? "ок" : "nullptr");
    return;
  }

  timerAttachInterrupt(runTimer, &onRunEnd, false);
  timerAlarmWrite(runTimer, 2 * Config::PERIOD_US, false);
  timerAlarmEnable(runTimer);

  timerAttachInterrupt(periodTimer, &onPeriod, false);
  timerAlarmWrite(periodTimer, Config::PERIOD_US, true);
  timerWrite(periodTimer, Config::RUN_US);
  timerAlarmEnable(periodTimer);

  Serial.println("обидва таймери запущено, loop() у циклі не бере участі");
  Serial.println();
}

void loop() {
  static uint32_t lastStarts = 0;
  static uint32_t lastStops  = 0;
  static int64_t  lastLogUs  = 0;

  const Snapshot s = snapshot();

  if (s.starts != lastStarts || s.stops != lastStops) {
    const bool wasStart = s.starts != lastStarts;
    lastStarts = s.starts;
    lastStops  = s.stops;

    char mark[128] = "";
    if (wasStart) {
      snprintf(mark, sizeof(mark), "  <- пуск, затримка ISR %u мкс", s.startLatencyUs);
    } else if (s.lastRunUs > 0) {
      snprintf(mark, sizeof(mark),
               "  <- пропрацював %lld.%06llu с (треба %u), затримка ISR %u мкс",
               s.lastRunUs / 1000000, static_cast<uint64_t>(s.lastRunUs % 1000000),
               Config::RUN_S, s.stopLatencyUs);
    }

    printStatus(s, mark);
    lastLogUs = esp_timer_get_time();
    return;
  }

  if (esp_timer_get_time() - lastLogUs >= static_cast<int64_t>(Config::LOG_EVERY_S) * 1000000) {
    lastLogUs = esp_timer_get_time();
    printStatus(s, "");
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
