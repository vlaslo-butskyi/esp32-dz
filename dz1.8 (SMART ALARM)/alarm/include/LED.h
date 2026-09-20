#pragma once

#include <Arduino.h>

class LED {
  private:
    const uint8_t pin_;
    bool state_ = LOW;
    unsigned long lastBlinkTime_ = 0;
  
  public:
    explicit LED(const uint8_t pin);

    void init();

    void setState(bool on);
    void turnOn();
    void turnOff();
    void toggle();
    // void lightBySound(bool sound);
    // void updateLastBlinkTime(unsigned long time);

    // bool getState() const;
    // unsigned long getLastBlinkTime() const;

    void resetBlink(unsigned long time);
    void blink(unsigned long time, unsigned long intervalMs);

    // bool isOn() const;
    // bool isOff() const;
};
