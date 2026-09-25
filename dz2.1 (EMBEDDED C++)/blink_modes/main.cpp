#include <Arduino.h>

// Уся конфігурація прошивки в одному місці. static constexpr => значення
// існують тільки на етапі компіляції, в RAM під них не виділяється нічого.
struct Config {
  // Периферія
  static constexpr uint8_t LED_PIN         = 16;
  static constexpr uint8_t BUTTON_PIN      = 7;
  static constexpr bool    LED_ACTIVE_HIGH = true;

  // Час
  static constexpr uint32_t BLINK_INTERVAL_MS = 500;
  static constexpr uint32_t DEBOUNCE_MS       = 50;

  // Діагностика
  static constexpr uint32_t SERIAL_BAUD              = 115200;
  static constexpr uint16_t STATS_EVERY_N_ITERATIONS = 1000;
};

static_assert(Config::BLINK_INTERVAL_MS > 0, "blink interval must be positive");
static_assert(Config::STATS_EVERY_N_ITERATIONS > 0, "stats window must be positive");

enum class LedState : uint8_t {
  Off = 0,
  On  = 1,
};

// Режими по колу: Blink -> AlwaysOn -> AlwaysOff -> Blink.
// Count — не режим, а кількість режимів, щоб цикл не ламався при додаванні нового.
enum class LedMode : uint8_t {
  Blink = 0,
  AlwaysOn,
  AlwaysOff,
  Count,
};

constexpr LedMode nextMode(LedMode mode) {
  return static_cast<LedMode>(
    (static_cast<uint8_t>(mode) + 1) % static_cast<uint8_t>(LedMode::Count)
  );
}

constexpr const char* modeName(LedMode mode) {
  return mode == LedMode::Blink     ? "BLINK"
       : mode == LedMode::AlwaysOn  ? "ALWAYS_ON"
       : mode == LedMode::AlwaysOff ? "ALWAYS_OFF"
       : "UNKNOWN";
}

class Led {
  private:
    const uint8_t pin_;
    const bool activeHigh_;
    LedState state_ = LedState::Off;

    constexpr uint8_t levelFor(LedState state) const {
      return (state == LedState::On) == activeHigh_ ? HIGH : LOW;
    }

  public:
    constexpr explicit Led(uint8_t pin, bool activeHigh = true)
      : pin_(pin), activeHigh_(activeHigh) {}

    void init() {
      pinMode(pin_, OUTPUT);
      state_ = LedState::Off;
      digitalWrite(pin_, levelFor(state_));
    }

    void set(LedState state) {
      if (state == state_) return; // зайвий запис у пін нічого не змінює
      state_ = state;
      digitalWrite(pin_, levelFor(state_));
    }

    void toggle() {
      set(state_ == LedState::On ? LedState::Off : LedState::On);
    }

    LedState state() const { return state_; }
};

// Статистика однієї ітерації superloop. Нічого не друкує сама —
// тільки накопичує і віддає цифри, друк лишається в loop().
class LoopProfiler {
  private:
    uint32_t sumUs_ = 0;
    uint32_t minUs_ = UINT32_MAX;
    uint32_t maxUs_ = 0;
    uint16_t count_ = 0;

  public:
    void add(uint32_t durationUs) {
      sumUs_ += durationUs;
      if (durationUs < minUs_) minUs_ = durationUs;
      if (durationUs > maxUs_) maxUs_ = durationUs;
      ++count_;
    }

    bool full() const { return count_ >= Config::STATS_EVERY_N_ITERATIONS; }

    uint32_t averageUs() const { return count_ == 0 ? 0 : sumUs_ / count_; }
    uint32_t minUs() const { return count_ == 0 ? 0 : minUs_; }
    uint32_t maxUs() const { return maxUs_; }

    void reset() {
      sumUs_ = 0;
      minUs_ = UINT32_MAX;
      maxUs_ = 0;
      count_ = 0;
    }
};

static Led led(Config::LED_PIN, Config::LED_ACTIVE_HIGH);

// Єдина глобальна змінна прошивки — прапорець події від кнопки.
// volatile обов'язковий: пише ISR, читає loop. Без нього компілятор має повне
// право тримати прапорець у регістрі і цикл ніколи не побачить натискання.
static volatile bool buttonEvent = false;

// ISR лежить в IRAM і робить рівно одну дію. Ніякого Serial, ніяких затримок,
// ніякої логіки режимів — усе це в loop().
void IRAM_ATTR onButtonEdge() {
  buttonEvent = true;
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  led.init();

  pinMode(Config::BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(Config::BUTTON_PIN), onButtonEdge, FALLING);

  Serial.println();
  Serial.println("=== DZ 2.1: blink with modes + loop profiler ===");
  Serial.printf(
    "LED on GPIO %u (active %s), button on GPIO %u (INPUT_PULLUP, FALLING)\r\n",
    Config::LED_PIN, Config::LED_ACTIVE_HIGH ? "HIGH" : "LOW", Config::BUTTON_PIN
  );
  Serial.printf(
    "blink interval %u ms, debounce %u ms, stats every %u iterations\r\n",
    Config::BLINK_INTERVAL_MS, Config::DEBOUNCE_MS, Config::STATS_EVERY_N_ITERATIONS
  );
  Serial.printf(
    "sizeof(Led) = %u B, sizeof(LoopProfiler) = %u B\r\n",
    sizeof(Led), sizeof(LoopProfiler)
  );
  Serial.println();
  Serial.println("  window | body avg | body min | body max | period avg | period max | mode");
  Serial.println("---------+----------+----------+----------+------------+------------+------------");
}

void loop() {
  const uint32_t startUs = micros();
  const uint32_t now = millis();

  // Весь стан superloop — у static-локальних змінних: живуть між ітераціями,
  // але зовні файлу їх не існує.
  static LedMode mode = LedMode::Blink;
  static uint32_t lastToggleMs = 0;
  static uint32_t lastEdgeMs = 0;
  static bool waitingForRelease = false;

  static LoopProfiler body;
  static LoopProfiler period;
  static uint32_t previousStartUs = 0;
  static uint32_t windowNumber = 0;

  // 1. Подія від ISR: знімаємо прапорець і далі працюємо зі звичайними змінними.
  if (buttonEvent) {
    buttonEvent = false;
    lastEdgeMs = now; // будь-який фронт означає, що контакт ще може дзвеніти

    if (!waitingForRelease) {
      waitingForRelease = true;
      mode = nextMode(mode);
      lastToggleMs = now; // нове блимання рахуємо з моменту перемикання
      Serial.printf("mode -> %s\r\n", modeName(mode));
    }
  }

  // Антидребезг цілком у loop: кнопка знову «заряджена» тільки коли пін тримає
  // HIGH довше за DEBOUNCE_MS після останнього побаченого фронту. Це прибирає і
  // дзвін при натисканні, і дзвін при відпусканні, який теж дає фронти FALLING.
  if (waitingForRelease &&
      digitalRead(Config::BUTTON_PIN) == HIGH &&
      now - lastEdgeMs >= Config::DEBOUNCE_MS) {
    waitingForRelease = false;
  }

  // 2. Режим -> стан світлодіода.
  switch (mode) {
    case LedMode::Blink:
      if (now - lastToggleMs >= Config::BLINK_INTERVAL_MS) {
        lastToggleMs = now;
        led.toggle();
      }
      break;

    case LedMode::AlwaysOn:
      led.set(LedState::On);
      break;

    case LedMode::AlwaysOff:
      led.set(LedState::Off);
      break;

    default:
      break;
  }

  // 3. Профіль ітерації: body — робота вище, period — повний такт loop()
  // разом із накладними витратами Arduino між викликами.
  body.add(micros() - startUs);

  if (previousStartUs != 0) {
    period.add(startUs - previousStartUs);
  }
  previousStartUs = startUs;

  if (body.full()) {
    windowNumber++;
    Serial.printf(
      "%8u | %5u us | %5u us | %5u us | %7u us | %7u us | %s\r\n",
      windowNumber,
      body.averageUs(), body.minUs(), body.maxUs(),
      period.averageUs(), period.maxUs(),
      modeName(mode)
    );
    body.reset();
    period.reset();
  }
}
