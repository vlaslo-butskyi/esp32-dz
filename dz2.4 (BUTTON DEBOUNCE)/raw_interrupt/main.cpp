#include <Arduino.h>

struct Config {
  static constexpr uint8_t  BUTTON_PIN  = 16;
  static constexpr uint8_t  LED_PIN     = 4;

  static constexpr uint8_t  BUTTON_MODE = INPUT_PULLUP;
  static constexpr uint32_t SERIAL_BAUD = 115200;
};

volatile uint32_t buttonInterruptCount = 0;

void IRAM_ATTR onButtonFalling() {
  buttonInterruptCount++;
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);
  pinMode(Config::BUTTON_PIN, Config::BUTTON_MODE);
  pinMode(Config::LED_PIN, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(Config::BUTTON_PIN), onButtonFalling, FALLING);
}

void loop() {
  static uint32_t lastInterruptCount = 0;
  if (buttonInterruptCount > lastInterruptCount) {
    lastInterruptCount = buttonInterruptCount;
    digitalWrite(Config::LED_PIN, !digitalRead(Config::LED_PIN));
    Serial.printf("Button interrupt count: %u\r\n", lastInterruptCount);
  }
}
