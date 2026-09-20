#include "Button.h"

Button::Button(
    const uint8_t pin,
    unsigned long debounceMs,
    unsigned long clickMaxMs
): 
    pin_(pin),
    debounceMs_(debounceMs),
    clickMaxMs_(clickMaxMs) {
  }

void Button::init() {
  pinMode(pin_, INPUT_PULLUP);

  lastReading_ = digitalRead(pin_) == LOW;
  pressed_ = false;
  clicked_ = false;

  lastReadingChangeTime_ = millis();
  pressedAt_ = 0;
}

void Button::update(unsigned long time) {
  clicked_ = false;

  const bool reading = digitalRead(pin_) == LOW;

  if (reading != lastReading_) {
    lastReading_ = reading;
    lastReadingChangeTime_ = time;
  }

  if (time - lastReadingChangeTime_ < debounceMs_) return;

  if (reading == pressed_) return;

  pressed_ = reading;

  if (pressed_) {
    pressedAt_ = time;
    return;
  }

  clicked_ = time - pressedAt_ < clickMaxMs_;
}

bool Button::isPressed() const {
  return pressed_;
}

bool Button::wasClicked() const {
  return clicked_;
}

unsigned long Button::heldFor(unsigned long time) const {
  if (!pressed_) {
    return 0;
  }

  return time - pressedAt_;
}
