#include <Arduino.h>

struct Config {
  static constexpr uint8_t  BUTTON_PIN   = 16;
  static constexpr uint8_t  LED_PIN      = 4;

  static constexpr uint32_t DEBOUNCE_MS  = 50;  
  static constexpr uint8_t  BUTTON_MODE = INPUT_PULLUP;

  static constexpr uint32_t SERIAL_BAUD  = 115200;
};

volatile bool buttonFallingFlag = false;
volatile uint32_t buttonFallingTime = 0;
volatile uint32_t buttonInterruptCount = 0;

void IRAM_ATTR onButtonFalling() {
  buttonFallingTime = micros();
  buttonFallingFlag = true;
  buttonInterruptCount++;
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);
  pinMode(Config::BUTTON_PIN, Config::BUTTON_MODE);
  pinMode(Config::LED_PIN, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(Config::BUTTON_PIN), onButtonFalling, FALLING);
}

void loop() {
  static uint32_t lastAcceptedTime = 0;
  static uint32_t acceptedCount = 0;

  if (buttonFallingFlag) {
    uint32_t now = buttonFallingTime;
    buttonFallingFlag = false;
    if (now - lastAcceptedTime >= Config::DEBOUNCE_MS * 1000UL) {
      lastAcceptedTime = now;
      acceptedCount++;
      digitalWrite(Config::LED_PIN, !digitalRead(Config::LED_PIN));
      Serial.printf("Accepted button events: %u, total interrupts: %u\r\n", acceptedCount, buttonInterruptCount);
    } else {
      Serial.printf("Rejected button event at: %u us\r\n", now);
    }
  }
}
