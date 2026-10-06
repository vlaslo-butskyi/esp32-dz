#include <Arduino.h>

struct Config {
  static constexpr uint8_t  BUTTON_PIN         = 16;
  static constexpr uint8_t  LED_PIN            = 4;
  static constexpr uint8_t  PRESSED_LEVEL      = LOW;
  static constexpr uint8_t  POLL_INTERVAL_MS   = 5;
  static constexpr uint8_t  STABLE_SAMPLES     = 4;
  static constexpr uint8_t  BUTTON_MODE = INPUT_PULLUP;

  static constexpr uint32_t SERIAL_BAUD        = 115200;
};

static_assert(Config::POLL_INTERVAL_MS >= 5 && Config::POLL_INTERVAL_MS <= 10,
              "умова вимагає опитування кожні 5-10 мс");

enum class ButtonState : uint8_t {
  Released,     // стабільно відпущена
  PressRising,  // побачили LOW, рахуємо підтвердження
  Pressed,      // стабільно натиснута
  ReleaseRising // побачили HIGH, рахуємо підтвердження
};

ButtonState state = ButtonState::Released;
uint8_t stableCount = 0;
uint32_t lastPollMs = 0;
uint32_t pressCount = 0;

void setup() {
  Serial.begin(Config::SERIAL_BAUD);
  pinMode(Config::BUTTON_PIN, Config::BUTTON_MODE);
  pinMode(Config::LED_PIN, OUTPUT);
  lastPollMs = millis();
}

void loop() {
  if (millis() - lastPollMs < Config::POLL_INTERVAL_MS) return;
  lastPollMs = millis();

  bool currentLevel = digitalRead(Config::BUTTON_PIN);
  switch (state) {
    case ButtonState::Released:
      if (currentLevel == Config::PRESSED_LEVEL) {
        state = ButtonState::PressRising;
        stableCount = 1;
      }
      break;
    case ButtonState::PressRising:
      if (currentLevel == Config::PRESSED_LEVEL) {
        stableCount++;
        if (stableCount >= Config::STABLE_SAMPLES) {
          state = ButtonState::Pressed;
          pressCount++;
          digitalWrite(Config::LED_PIN, !digitalRead(Config::LED_PIN));
          Serial.printf("Button pressed: %u\r\n", pressCount);
        }
      } else {
        state = ButtonState::Released;
      }
      break;
    case ButtonState::Pressed:
      if (currentLevel != Config::PRESSED_LEVEL) {
        state = ButtonState::ReleaseRising;
        stableCount = 1;
      }
      break;
    case ButtonState::ReleaseRising:
      if (currentLevel != Config::PRESSED_LEVEL) {
        stableCount++;
        if (stableCount >= Config::STABLE_SAMPLES) {
          state = ButtonState::Released;
          Serial.println("Button released");
        }
      } else {
        state = ButtonState::Pressed;
      }
      break;
  }
}
