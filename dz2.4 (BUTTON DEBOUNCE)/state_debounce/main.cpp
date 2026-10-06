#include <Arduino.h>

struct Config {
  static constexpr uint8_t  BUTTON_PIN    = 16;
  static constexpr uint8_t  LED_PIN       = 4;
  static constexpr uint8_t  PRESSED_LEVEL = LOW;
  static constexpr uint32_t SETTLE_MS     = 10;
  static constexpr uint8_t  BUTTON_MODE = INPUT_PULLUP;

  static constexpr uint32_t SERIAL_BAUD   = 115200;
};

volatile bool buttonEventFlag = false;
bool pressed_ = false;

void IRAM_ATTR onButtonEdge() {
  buttonEventFlag = true;
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);
  pinMode(Config::BUTTON_PIN, Config::BUTTON_MODE);
  pinMode(Config::LED_PIN, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(Config::BUTTON_PIN), onButtonEdge, CHANGE);
}

void loop() {
  if (buttonEventFlag) {
    buttonEventFlag = false;
    delay(Config::SETTLE_MS);
    bool currentState = digitalRead(Config::BUTTON_PIN);
    if (currentState == Config::PRESSED_LEVEL && !pressed_) {
      pressed_ = true;
      digitalWrite(Config::LED_PIN, !digitalRead(Config::LED_PIN));
      Serial.println("Button pressed");
    } else if (currentState != Config::PRESSED_LEVEL && pressed_) {
      pressed_ = false;
      Serial.println("Button released");
    }
  }
}
