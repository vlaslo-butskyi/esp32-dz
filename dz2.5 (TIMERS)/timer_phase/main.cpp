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

  static constexpr uint64_t RUN_US  = static_cast<uint64_t>(RUN_S) * 1000000ULL;
  static constexpr uint64_t IDLE_US = static_cast<uint64_t>(IDLE_S) * 1000000ULL;

  // Таймер
  static constexpr uint8_t  TIMER_NUM = 0;
  static constexpr uint32_t APB_HZ    = 80000000;
  static constexpr uint16_t DIVIDER   = 80;  // тік 1 мкс

  static constexpr uint32_t SERIAL_BAUD = 115200;
};

static_assert(Config::RUN_S > 0, "час роботи має бути додатним");
static_assert(Config::RUN_S < Config::PERIOD_S, "вентилятор не може працювати довше за період");
static_assert(Config::PERIOD_S == Config::RUN_S * 4,
              "налагоджувальний масштаб має зберігати пропорцію 15 хв із 60");
static_assert(Config::IDLE_US < (1ULL << 54), "фаза задовга для 54-бітного лічильника");

static constexpr uint8_t GROUP_OF[4] = {0, 1, 0, 1};
static constexpr uint8_t INDEX_OF[4] = {0, 0, 1, 1};

static constexpr timer_group_t TG  = static_cast<timer_group_t>(GROUP_OF[Config::TIMER_NUM]);
static constexpr timer_idx_t   TIX = static_cast<timer_idx_t>(INDEX_OF[Config::TIMER_NUM]);

constexpr uint8_t inverted(uint8_t level) {
  return level == HIGH ? LOW : HIGH;
}

// --- стан автомата ----------------------------------------------------------

static hw_timer_t* phaseTimer = nullptr;

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool     fanRunning   = false;
static volatile uint32_t cycles       = 0;
static volatile uint32_t switches     = 0;
static volatile int64_t  lastSwitchUs = 0;
static volatile uint32_t lastLatencyUs = 0;
static volatile uint32_t maxLatencyUs  = 0;

static void IRAM_ATTR applyOutputs(bool running) {
  digitalWrite(Config::RELAY_PIN, running ? Config::RELAY_ON_LEVEL
                                          : inverted(Config::RELAY_ON_LEVEL));
  digitalWrite(Config::LED_FAN_PIN,  running ? HIGH : LOW);
  digitalWrite(Config::LED_IDLE_PIN, running ? LOW : HIGH);
}

void IRAM_ATTR onPhaseEnd() {
  const uint32_t latency = static_cast<uint32_t>(timer_group_get_counter_value_in_isr(TG, TIX));

  bool running;
  portENTER_CRITICAL_ISR(&stateMux);
  fanRunning = !fanRunning;
  if (fanRunning) ++cycles;
  ++switches;
  lastSwitchUs  = esp_timer_get_time();
  lastLatencyUs = latency;
  if (latency > maxLatencyUs) maxLatencyUs = latency;
  running = fanRunning;
  portEXIT_CRITICAL_ISR(&stateMux);

  applyOutputs(running);

  timer_group_set_alarm_value_in_isr(TG, TIX, running ? Config::RUN_US : Config::IDLE_US);
  timer_group_enable_alarm_in_isr(TG, TIX);
}

// --- знімок стану для логу --------------------------------------------------

struct Snapshot {
  bool     running;
  uint32_t cycles;
  uint32_t switches;
  int64_t  lastSwitchUs;
  uint32_t lastLatencyUs;
  uint32_t maxLatencyUs;
};

static Snapshot snapshot() {
  Snapshot s;
  portENTER_CRITICAL(&stateMux);
  s.running       = fanRunning;
  s.cycles        = cycles;
  s.switches      = switches;
  s.lastSwitchUs  = lastSwitchUs;
  s.lastLatencyUs = lastLatencyUs;
  s.maxLatencyUs  = maxLatencyUs;
  portEXIT_CRITICAL(&stateMux);
  return s;
}

static const char* phaseLabel(bool running) {
  return running ? "РОБОТА" : "ПАУЗА ";
}

static uint32_t secondsLeft(bool running) {
  if (phaseTimer == nullptr) return 0;
  const uint64_t elapsed = timerRead(phaseTimer);
  const uint64_t total   = running ? Config::RUN_US : Config::IDLE_US;
  return elapsed >= total ? 0 : static_cast<uint32_t>((total - elapsed) / 1000000ULL);
}

static void printStatus(const Snapshot& s, const char* mark) {
  Serial.printf("[%9.3f] %s лишилось %4u s  цикл %-3u  переривань %-3u%s\r\n",
                esp_timer_get_time() / 1e6,
                phaseLabel(s.running),
                secondsLeft(s.running),
                s.cycles, s.switches,
                mark);
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pinMode(Config::RELAY_PIN, OUTPUT);
  pinMode(Config::LED_FAN_PIN, OUTPUT);
  pinMode(Config::LED_IDLE_PIN, OUTPUT);
  applyOutputs(false);

  Serial.println();
  Serial.println("=== ДЗ 2.5, варіант 2: аларм дорівнює довжині фази ===");
  Serial.printf("період %u с, робота %u с, пауза %u с\r\n",
                Config::PERIOD_S, Config::RUN_S, Config::IDLE_S);
  Serial.printf("таймер %u (TIMERG%u, таймер %u), дільник %u -> тік %u нс\r\n",
                Config::TIMER_NUM, static_cast<unsigned>(TG), static_cast<unsigned>(TIX),
                Config::DIVIDER, 1000000000UL / (Config::APB_HZ / Config::DIVIDER));
  Serial.printf("аларм робочої фази %llu мкс, аларм паузи %llu мкс\r\n",
                Config::RUN_US, Config::IDLE_US);
  Serial.printf("переривань за повний цикл: 2 (у варіанті timer_tick їх було %u)\r\n",
                Config::PERIOD_S);

  phaseTimer = timerBegin(Config::TIMER_NUM, Config::DIVIDER, true);
  if (phaseTimer == nullptr) {
    Serial.printf("ПРОБЛЕМА: таймер %u недоступний, цикл не запуститься\r\n", Config::TIMER_NUM);
    return;
  }

  timerAttachInterrupt(phaseTimer, &onPhaseEnd, false);
  timerAlarmWrite(phaseTimer, Config::IDLE_US, true);
  timerWrite(phaseTimer, 0);
  timerAlarmEnable(phaseTimer);

  Serial.println("таймер запущено, далі він перемикає фази сам і сам переписує аларм");
  Serial.println();
}

void loop() {
  static uint32_t lastSwitches = 0;
  static int64_t  lastLogUs    = 0;
  static int64_t  prevSwitchUs = 0;

  const Snapshot s = snapshot();

  if (s.switches != lastSwitches) {
    lastSwitches = s.switches;

    char mark[128] = "";
    if (prevSwitchUs != 0) {
      const int64_t  spanUs   = s.lastSwitchUs - prevSwitchUs;
      const uint32_t expected = s.running ? Config::IDLE_S : Config::RUN_S;
      snprintf(mark, sizeof(mark),
               "  <- фаза %lld.%06llu с (треба %u), затримка ISR %u мкс, найгірша %u",
               spanUs / 1000000, static_cast<uint64_t>(spanUs % 1000000), expected,
               s.lastLatencyUs, s.maxLatencyUs);
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
