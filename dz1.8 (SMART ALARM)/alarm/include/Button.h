#pragma once

#include <Arduino.h>

class Button {
  private:
    const uint8_t pin_;

    uint32_t debounceMs_ = 0;
    uint32_t clickMaxMs_ = 0;

    uint32_t lastReadingChangeTime_ = 0;
    uint32_t pressedAt_ = 0;

    bool pressed_ = false;
    bool lastReading_ = false;
    bool clicked_ = false;
    
  public:
    Button(
        const uint8_t pin, 
        uint32_t debounceMs,
        uint32_t clickMaxMs
    );

    void init();
    void update(uint32_t time);

    bool isPressed() const;
    bool wasClicked() const;

    uint32_t heldFor(uint32_t time) const;
};
