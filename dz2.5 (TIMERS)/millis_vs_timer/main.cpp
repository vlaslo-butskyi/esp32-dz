#include <Arduino.h>
#include <esp_timer.h>
#include <driver/timer.h>


#define FAST_DEMO 1

struct Config {
  // Периферія
  static constexpr uint8_t RELAY_PIN   = 8;  // реле веде апаратний таймер
  static constexpr uint8_t HW_PIN      = 4;  // канал D0: апаратний таймер
  static constexpr uint8_t FIXED_PIN   = 5;  // канал D1: millis() з накопиченням
  static constexpr uint8_t NAIVE_PIN   = 6;  // канал D2: millis() з перезаписом

  static constexpr uint8_t RELAY_ON_LEVEL       = HIGH;

  // Час роботи і період
#if FAST_DEMO
  static constexpr uint32_t PERIOD_S = 20;
  static constexpr uint32_t RUN_S    = 5;
#else
  static constexpr uint32_t PERIOD_S = 60UL * 60UL;
  static constexpr uint32_t RUN_S    = 15UL * 60UL;
#endif
  static constexpr uint32_t IDLE_S = PERIOD_S - RUN_S;

  static constexpr uint64_t RUN_US  = static_cast<uint64_t>(RUN_S) * 1000000ULL;
  static constexpr uint64_t IDLE_US = static_cast<uint64_t>(IDLE_S) * 1000000ULL;

  // Навантаження loop()
  static constexpr uint32_t WORK_MIN_MS = 50;
  static constexpr uint32_t WORK_MAX_MS = 350;

  // Таймер
  static constexpr uint8_t  TIMER_NUM = 0;
  static constexpr uint16_t DIVIDER   = 80;  // тік 1 мкс

  // Скільки перемикань зберігати для таблиці
  static constexpr uint16_t MAX_SWITCHES = 64;

  static constexpr uint32_t SERIAL_BAUD = 115200;
};

static_assert(Config::RUN_S > 0 && Config::RUN_S < Config::PERIOD_S, "перевір час роботи і період");
static_assert(Config::PERIOD_S == Config::RUN_S * 4,
              "налагоджувальний масштаб має зберігати пропорцію 15 хв із 60");
static_assert(Config::WORK_MAX_MS * 1000 < Config::RUN_US,
              "одна ітерація loop() має вкладатися в робочу фазу, інакше порівнювати нічого");

static constexpr uint8_t GROUP_OF[4] = {0, 1, 0, 1};
static constexpr uint8_t INDEX_OF[4] = {0, 0, 1, 1};
static constexpr timer_group_t TG  = static_cast<timer_group_t>(GROUP_OF[Config::TIMER_NUM]);
static constexpr timer_idx_t   TIX = static_cast<timer_idx_t>(INDEX_OF[Config::TIMER_NUM]);

constexpr uint8_t inverted(uint8_t level) {
  return level == HIGH ? LOW : HIGH;
}

enum Source : uint8_t { SRC_HW = 0, SRC_FIXED = 1, SRC_NAIVE = 2, SRC_COUNT = 3 };

static volatile int64_t  switchAtUs[SRC_COUNT][Config::MAX_SWITCHES];
static volatile uint16_t switchCount[SRC_COUNT] = {0, 0, 0};

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool    hwRunning = false;
static volatile int64_t startUs   = 0;  // момент, коли завели таймер

static hw_timer_t* phaseTimer = nullptr;

static void IRAM_ATTR record(Source src, int64_t atUs) {
  if (switchCount[src] < Config::MAX_SWITCHES) {
    switchAtUs[src][switchCount[src]] = atUs;
  }
  ++switchCount[src];
}

void IRAM_ATTR onPhaseEnd() {
  bool running;

  portENTER_CRITICAL_ISR(&stateMux);
  hwRunning = !hwRunning;
  running   = hwRunning;
  record(SRC_HW, esp_timer_get_time());
  portEXIT_CRITICAL_ISR(&stateMux);

  digitalWrite(Config::HW_PIN, running ? HIGH : LOW);
  digitalWrite(Config::RELAY_PIN, running ? Config::RELAY_ON_LEVEL
                                          : inverted(Config::RELAY_ON_LEVEL));

  timer_group_set_alarm_value_in_isr(TG, TIX, running ? Config::RUN_US : Config::IDLE_US);
  timer_group_enable_alarm_in_isr(TG, TIX);
}

// --- програмні таймери ------------------------------------------------------

struct FixedTimer {
  bool     running = false;
  uint32_t dueMs   = 0;

  void start(uint32_t nowMs) { dueMs = nowMs + Config::IDLE_S * 1000UL; }

  bool update(uint32_t nowMs) {
    if (static_cast<int32_t>(nowMs - dueMs) < 0) return false;
    running = !running;
    dueMs += (running ? Config::RUN_S : Config::IDLE_S) * 1000UL;
    return true;
  }
};

struct NaiveTimer {
  bool     running = false;
  uint32_t lastMs  = 0;

  void start(uint32_t nowMs) { lastMs = nowMs; }

  bool update(uint32_t nowMs) {
    const uint32_t interval = (running ? Config::RUN_S : Config::IDLE_S) * 1000UL;
    if (nowMs - lastMs < interval) return false;
    running = !running;
    lastMs  = nowMs;
    return true;
  }
};

static FixedTimer fixedTimer;
static NaiveTimer naiveTimer;

// --- імітація завантаженого loop() ------------------------------------------

static uint32_t rngState = 0x2545F491;

static uint32_t nextWorkMs() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  const uint32_t span = Config::WORK_MAX_MS - Config::WORK_MIN_MS + 1;
  return Config::WORK_MIN_MS + rngState % span;
}

static uint32_t busyWork(uint32_t ms) {
  const int64_t until = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
  volatile uint32_t sink = 0;
  while (esp_timer_get_time() < until) ++sink;
  return sink;
}

// --- таблиця порівняння -----------------------------------------------------

static int64_t idealUs(uint16_t n) {
  const uint64_t idles = (n + 1) / 2;
  const uint64_t runs  = n / 2;
  return static_cast<int64_t>(idles * Config::IDLE_US + runs * Config::RUN_US);
}

static void printHeader() {
  Serial.println();
  Serial.println("  # | фаза   | апаратний | millis += крок | millis = now | накопичено");
  Serial.println(" ---+--------+-----------+----------------+--------------+-----------");
}

static void printRow(uint16_t n) {
  const int64_t ideal = idealUs(n);
  const int64_t hw    = switchAtUs[SRC_HW][n - 1] - startUs - ideal;
  const int64_t fx    = switchAtUs[SRC_FIXED][n - 1] - startUs - ideal;
  const int64_t nv    = switchAtUs[SRC_NAIVE][n - 1] - startUs - ideal;

  const int64_t gap = nv - fx;

  Serial.printf("%4u | %s | %+7.1f мс | %+10.1f мс | %+8.1f мс | %+7.1f мс\r\n",
                n,
                (n % 2 == 1) ? "РОБОТА" : "ПАУЗА ",
                hw / 1000.0, fx / 1000.0, nv / 1000.0, gap / 1000.0);
}

static void printSummary() {
  int64_t worst[SRC_COUNT] = {0, 0, 0};

  for (uint16_t n = 1; n <= Config::MAX_SWITCHES; ++n) {
    const int64_t ideal = idealUs(n);
    for (uint8_t src = 0; src < SRC_COUNT; ++src) {
      int64_t err = switchAtUs[src][n - 1] - startUs - ideal;
      if (err < 0) err = -err;
      if (err > worst[src]) worst[src] = err;
    }
  }

  const int64_t spanUs = idealUs(Config::MAX_SWITCHES);

  Serial.println();
  Serial.printf("=== підсумок за %u перемикань (%lld с модельного часу) ===\r\n",
                Config::MAX_SWITCHES, spanUs / 1000000);
  Serial.printf("апаратний таймер:   найгірша похибка %+.3f мс\r\n", worst[SRC_HW] / 1000.0);
  Serial.printf("millis() += крок:   найгірша похибка %+.3f мс\r\n", worst[SRC_FIXED] / 1000.0);
  Serial.printf("millis() = now:     найгірша похибка %+.3f мс\r\n", worst[SRC_NAIVE] / 1000.0);
  Serial.println();
  Serial.println("далі прошивка просто продовжує цикл без друку таблиці");
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pinMode(Config::RELAY_PIN, OUTPUT);
  pinMode(Config::HW_PIN, OUTPUT);
  pinMode(Config::FIXED_PIN, OUTPUT);
  pinMode(Config::NAIVE_PIN, OUTPUT);

  digitalWrite(Config::RELAY_PIN, inverted(Config::RELAY_ON_LEVEL));
  digitalWrite(Config::HW_PIN, LOW);
  digitalWrite(Config::FIXED_PIN, LOW);
  digitalWrite(Config::NAIVE_PIN, LOW);

  Serial.println();
  Serial.println("=== ДЗ 2.5, варіант 4: апаратний таймер проти millis() ===");
  Serial.printf("період %u с, робота %u с, пауза %u с\r\n",
                Config::PERIOD_S, Config::RUN_S, Config::IDLE_S);
  Serial.printf("loop() зайнятий %u...%u мс на ітерацію\r\n",
                Config::WORK_MIN_MS, Config::WORK_MAX_MS);
  Serial.printf("GPIO %u — апаратний таймер (і реле), GPIO %u — millis() += крок, GPIO %u — millis() = now\r\n",
                Config::HW_PIN, Config::FIXED_PIN, Config::NAIVE_PIN);

  phaseTimer = timerBegin(Config::TIMER_NUM, Config::DIVIDER, true);
  if (phaseTimer == nullptr) {
    Serial.printf("ПРОБЛЕМА: таймер %u недоступний\r\n", Config::TIMER_NUM);
    return;
  }

  timerAttachInterrupt(phaseTimer, &onPhaseEnd, false);
  timerAlarmWrite(phaseTimer, Config::IDLE_US, true);

  const uint32_t nowMs = millis();
  fixedTimer.start(nowMs);
  naiveTimer.start(nowMs);
  timerWrite(phaseTimer, 0);
  startUs = esp_timer_get_time();
  timerAlarmEnable(phaseTimer);

  printHeader();
}

void loop() {
  static uint16_t printedUpTo = 0;

  const uint32_t nowMs = millis();

  if (fixedTimer.update(nowMs)) {
    digitalWrite(Config::FIXED_PIN, fixedTimer.running ? HIGH : LOW);
    record(SRC_FIXED, esp_timer_get_time());
  }
  if (naiveTimer.update(nowMs)) {
    digitalWrite(Config::NAIVE_PIN, naiveTimer.running ? HIGH : LOW);
    record(SRC_NAIVE, esp_timer_get_time());
  }

  portENTER_CRITICAL(&stateMux);
  const uint16_t hwCount = switchCount[SRC_HW];
  portEXIT_CRITICAL(&stateMux);

  while (printedUpTo < Config::MAX_SWITCHES &&
         hwCount > printedUpTo &&
         switchCount[SRC_FIXED] > printedUpTo &&
         switchCount[SRC_NAIVE] > printedUpTo) {
    ++printedUpTo;
    printRow(printedUpTo);
    if (printedUpTo == Config::MAX_SWITCHES) printSummary();
    else if (printedUpTo % 10 == 0) printHeader();
  }

  busyWork(nextWorkMs());
}
