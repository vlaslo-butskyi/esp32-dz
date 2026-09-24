#pragma once

#include <Arduino.h>

class LED {
  private:
    const uint8_t pin_;
    bool state_ = LOW;
    uint32_t lastBlinkTime_ = 0;
  
  public:
    explicit LED(const uint8_t pin);

    void init();

    void setState(bool on);
    void turnOn();
    void turnOff();
    void toggle();

    void resetBlink(uint32_t time);
    void blink(uint32_t time, uint32_t intervalMs);
};
