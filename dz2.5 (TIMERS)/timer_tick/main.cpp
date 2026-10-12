#include <Arduino.h>
#include <esp_timer.h>

// Налагоджувальний масштаб часу: 1 — секунди замість хвилин, щоб повний цикл було
// видно в моніторі за 20 с; 0 — справжні 15 хв роботи на годину.
#define FAST_DEMO 1

struct Config {
  // Периферія
  static constexpr uint8_t RELAY_PIN    = 8;  // керування обмоткою через ключ на BC547B
  static constexpr uint8_t LED_FAN_PIN  = 4;  // зелений: вентилятор працює
  static constexpr uint8_t LED_IDLE_PIN = 5;  // червоний: пауза

  static constexpr uint8_t RELAY_ON_LEVEL       = HIGH;

  // Час роботи і період — єдине місце, де вони задаються.
#if FAST_DEMO
  static constexpr uint32_t PERIOD_S    = 20;  // «година»
  static constexpr uint32_t RUN_S       = 5;   // «15 хвилин»
  static constexpr uint32_t LOG_EVERY_S = 1;
#else
  static constexpr uint32_t PERIOD_S    = 60UL * 60UL;  // година
  static constexpr uint32_t RUN_S       = 15UL * 60UL;  // 15 хвилин
  static constexpr uint32_t LOG_EVERY_S = 60;
#endif
  static constexpr uint32_t IDLE_S = PERIOD_S - RUN_S;

  // Таймер
  static constexpr uint8_t  TIMER_NUM = 0;        // індекс 0 -> TIMERG0, таймер 0
  static constexpr uint32_t APB_HZ    = 80000000;
  static constexpr uint16_t DIVIDER   = 80;       // 80 МГц / 80 = 1 МГц, тік = 1 мкс
  static constexpr uint64_t TICK_US   = 1000000;  // аларм раз на секунду

  static constexpr uint32_t SERIAL_BAUD = 115200;
};

static_assert(Config::RUN_S > 0, "час роботи має бути додатним");
static_assert(Config::RUN_S < Config::PERIOD_S, "вентилятор не може працювати довше за період");
static_assert(Config::PERIOD_S == Config::RUN_S * 4,
              "налагоджувальний масштаб має зберігати пропорцію 15 хв із 60");
static_assert(Config::DIVIDER >= 2, "дільник таймера ESP32 починається з 2");

constexpr uint8_t inverted(uint8_t level) {
  return level == HIGH ? LOW : HIGH;
}

static hw_timer_t* tickTimer = nullptr;

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t phaseSeconds = 0;      // секунд у поточній фазі
static volatile bool     fanRunning   = false;  // реле зараз замкнуте?
static volatile uint32_t cycles       = 0;      // скільки разів вентилятор запускався
static volatile uint32_t switches     = 0;      // скільки разів ISR перекидав реле
static volatile uint32_t ticks        = 0;      // усього тіків таймера
static volatile int64_t  lastSwitchUs = 0;      // час останнього перемикання

static void IRAM_ATTR applyOutputs(bool running) {
  digitalWrite(Config::RELAY_PIN, running ? Config::RELAY_ON_LEVEL
                                          : inverted(Config::RELAY_ON_LEVEL));
  digitalWrite(Config::LED_FAN_PIN,  running ? HIGH : LOW);
  digitalWrite(Config::LED_IDLE_PIN, running ? LOW : HIGH);
}

void IRAM_ATTR onTick() {
  bool changed = false;
  bool running;

  portENTER_CRITICAL_ISR(&stateMux);
  ++ticks;
  ++phaseSeconds;
  if (phaseSeconds >= (fanRunning ? Config::RUN_S : Config::IDLE_S)) {
    phaseSeconds = 0;
    fanRunning   = !fanRunning;
    if (fanRunning) ++cycles;
    ++switches;
    lastSwitchUs = esp_timer_get_time();
    changed = true;
  }
  running = fanRunning;
  portEXIT_CRITICAL_ISR(&stateMux);

  if (changed) applyOutputs(running);
}

struct Snapshot {
  uint32_t phaseSeconds;
  bool     running;
  uint32_t cycles;
  uint32_t switches;
  uint32_t ticks;
  int64_t  lastSwitchUs;
};

static Snapshot snapshot() {
  Snapshot s;
  portENTER_CRITICAL(&stateMux);
  s.phaseSeconds = phaseSeconds;
  s.running      = fanRunning;
  s.cycles       = cycles;
  s.switches     = switches;
  s.ticks        = ticks;
  s.lastSwitchUs = lastSwitchUs;
  portEXIT_CRITICAL(&stateMux);
  return s;
}

static const char* phaseLabel(bool running) {
  return running ? "РОБОТА" : "ПАУЗА ";
}

static void printStatus(const Snapshot& s, const char* mark) {
  const uint32_t limit = s.running ? Config::RUN_S : Config::IDLE_S;

  Serial.printf("[%9.3f] %s %4u/%-4u s  цикл %-3u  тіків %-6u%s\r\n",
                esp_timer_get_time() / 1e6,
                phaseLabel(s.running),
                s.phaseSeconds, limit,
                s.cycles, s.ticks,
                mark);
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pinMode(Config::RELAY_PIN, OUTPUT);
  pinMode(Config::LED_FAN_PIN, OUTPUT);
  pinMode(Config::LED_IDLE_PIN, OUTPUT);
  applyOutputs(false);

  Serial.println();
  Serial.println("=== ДЗ 2.5, варіант 1: тік 1 с на апаратному таймері ===");
  Serial.printf("період %u с, робота %u с, пауза %u с\r\n",
                Config::PERIOD_S, Config::RUN_S, Config::IDLE_S);
  Serial.printf("таймер %u, дільник %u -> %u Гц, тік %u нс\r\n",
                Config::TIMER_NUM, Config::DIVIDER,
                Config::APB_HZ / Config::DIVIDER,
                1000000000UL / (Config::APB_HZ / Config::DIVIDER));
  Serial.printf("аларм кожні %llu мкс, переривань за повний цикл: %u\r\n",
                Config::TICK_US, Config::PERIOD_S);

  tickTimer = timerBegin(Config::TIMER_NUM, Config::DIVIDER, true);
  if (tickTimer == nullptr) {
    Serial.printf("ПРОБЛЕМА: таймер %u недоступний, цикл не запуститься\r\n", Config::TIMER_NUM);
    return;
  }

  timerAttachInterrupt(tickTimer, &onTick, false);
  timerAlarmWrite(tickTimer, Config::TICK_US, true);
  timerWrite(tickTimer, 0);
  timerAlarmEnable(tickTimer);

  Serial.println("таймер запущено");
  Serial.println();
}

void loop() {
  static uint32_t lastSwitches = 0;
  static int64_t  lastLogUs    = 0;
  static int64_t  prevSwitchUs = 0;

  const Snapshot s = snapshot();

  if (s.switches != lastSwitches) {
    lastSwitches = s.switches;

    char mark[96] = "";
    if (prevSwitchUs != 0) {
      const int64_t  spanUs   = s.lastSwitchUs - prevSwitchUs;
      const uint32_t expected = s.running ? Config::IDLE_S : Config::RUN_S;
      snprintf(mark, sizeof(mark), "  <- фаза тривала %lld.%06llu с (треба %u)",
               spanUs / 1000000, static_cast<uint64_t>(spanUs % 1000000), expected);
    }
    prevSwitchUs = s.lastSwitchUs;

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
