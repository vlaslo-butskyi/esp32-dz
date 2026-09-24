#include "LED.h"

LED::LED(const uint8_t pin) : pin_(pin) {}

void LED::init() {
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, state_);
}

void LED::setState(bool on) {
    if (state_ == on) return;
    state_ = on;
    digitalWrite(pin_, state_ ? HIGH : LOW);
}

void LED::turnOn() {
    setState(true);
}

void LED::turnOff() {
    setState(false);
}

void LED::toggle() {
    setState(!state_);
}

void LED::resetBlink(uint32_t time) {
    lastBlinkTime_ = time;
}

void LED::blink(uint32_t time, uint32_t intervalMs) {
    if (intervalMs == 0) return;

    if (time - lastBlinkTime_ >= intervalMs) {
        lastBlinkTime_ = time;
        toggle();
    }
}