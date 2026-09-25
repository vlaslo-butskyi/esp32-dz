#include <Arduino.h>

// Уся конфігурація прошивки в одному місці. static constexpr => значення
// існують тільки на етапі компіляції, в RAM під них не виділяється нічого.
struct BlinkConfig {
  static constexpr uint8_t  LED_PIN           = 16;
  static constexpr bool     LED_ACTIVE_HIGH   = true;
  static constexpr uint32_t BLINK_INTERVAL_MS = 500;
  static constexpr uint32_t SERIAL_BAUD       = 115200;

  // constexpr-функція: період рахується компілятором, а не платою.
  static constexpr uint32_t periodMs() { return BLINK_INTERVAL_MS * 2; }
};

static_assert(BlinkConfig::BLINK_INTERVAL_MS > 0, "blink interval must be positive");

// Стан світлодіода — окремий тип, а не bool і не int: enum class не
// конвертується неявно в число, тому переплутати його з піном неможливо.
enum class LedState : uint8_t {
  Off = 0,
  On  = 1,
};

class Led {
  private:
    const uint8_t pin_;
    const bool activeHigh_;
    LedState state_ = LedState::Off;

    // Рівень на піні для заданого стану: для active-low схеми все інвертується.
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

// Єдиний об'єкт зі станом на весь файл. static => внутрішнє зв'язування,
// constexpr-конструктор => об'єкт збирається на етапі компіляції і лягає
// в .data готовим, без виклику конструктора на старті.
static Led led(BlinkConfig::LED_PIN, BlinkConfig::LED_ACTIVE_HIGH);

void setup() {
  Serial.begin(BlinkConfig::SERIAL_BAUD);
  led.init();

  Serial.println();
  Serial.println("=== DZ 2.1: blink (Embedded C++) ===");
  Serial.printf(
    "LED on GPIO %u, active %s\r\n",
    BlinkConfig::LED_PIN, BlinkConfig::LED_ACTIVE_HIGH ? "HIGH" : "LOW"
  );
  Serial.printf(
    "interval %u ms, period %u ms\r\n",
    BlinkConfig::BLINK_INTERVAL_MS, BlinkConfig::periodMs()
  );
  Serial.printf(
    "sizeof(Led) = %u B, sizeof(LedState) = %u B\r\n",
    sizeof(Led), sizeof(LedState)
  );
  Serial.println();
}

void loop() {
  // Стан блимання живе тут: static-локальна змінна переживає вихід з loop(),
  // але поза loop() її не видно. Ініціалізація константою — значить без
  // прихованої guard-змінної і без коду ініціалізації при першому заході.
  static uint32_t lastToggleMs = 0;

  const uint32_t now = millis();

  // Різниця беззнакових коректна і після переповнення millis() через 49.7 доби.
  if (now - lastToggleMs < BlinkConfig::BLINK_INTERVAL_MS) return;

  lastToggleMs = now;
  led.toggle();

  Serial.printf(
    "[%8u ms] LED %s\r\n",
    now, led.state() == LedState::On ? "ON" : "OFF"
  );
}
