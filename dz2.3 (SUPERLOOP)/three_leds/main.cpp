#include <Arduino.h>

struct Config {
  static constexpr uint8_t  LED_RED_PIN   = 4;
  static constexpr uint8_t  LED_GREEN_PIN = 5;
  static constexpr uint8_t  LED_YELLOW_PIN  = 6;

  static constexpr uint16_t LED_RED_INTERVAL_MS    = 200;
  static constexpr uint16_t LED_GREEN_INTERVAL_MS  = 500;
  static constexpr uint16_t LED_YELLOW_INTERVAL_MS = 1000;

  static constexpr uint32_t SERIAL_BAUD = 115200;
};

class LED {
public:
  uint8_t pin_;
  uint16_t interval_;
  uint32_t lastToggleTime_;

  LED(uint8_t p, uint16_t i) : pin_(p), interval_(i), lastToggleTime_(0) {}

  void init() {
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, LOW);
  }

  void update() {
    uint32_t currentTime = millis();
    if (currentTime - lastToggleTime_ >= interval_) {
      lastToggleTime_ = currentTime;
      digitalWrite(pin_, !digitalRead(pin_));
    }
  }
};

static LED redLED(Config::LED_RED_PIN, Config::LED_RED_INTERVAL_MS);
static LED greenLED(Config::LED_GREEN_PIN, Config::LED_GREEN_INTERVAL_MS);
static LED yellowLED(Config::LED_YELLOW_PIN, Config::LED_YELLOW_INTERVAL_MS);

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  redLED.init();
  greenLED.init();
  yellowLED.init();
}

void loop() {
  redLED.update();
  greenLED.update();
  yellowLED.update();
}
