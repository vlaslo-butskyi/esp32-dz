#pragma once

#include <Arduino.h>

class Button {
  private:
    const uint8_t pin_;

    unsigned long debounceMs_ = 0;
    unsigned long clickMaxMs_ = 0;

    unsigned long lastReadingChangeTime_ = 0;
    unsigned long pressedAt_ = 0;

    bool pressed_ = false;
    bool lastReading_ = false;
    bool clicked_ = false;
    
  public:
    Button(
        const uint8_t pin, 
        unsigned long debounceMs,
        unsigned long clickMaxMs
    );

    void init();
    void update(unsigned long time);

    bool isPressed() const;
    bool wasClicked() const;

    unsigned long heldFor(unsigned long time) const;
};
