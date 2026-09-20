#pragma once

#include <Arduino.h>

namespace Config {
    // Pin configuration for the smart alarm system
    constexpr uint8_t LED_GREEN_PIN = 4;
    constexpr uint8_t LED_YELLOW_PIN = 5;
    constexpr uint8_t LED_RED_PIN = 6;

    // Button configuration
    constexpr uint8_t BUTTON_LEFT_PIN = 7;
    constexpr uint8_t BUTTON_RIGHT_PIN = 3;

    // Sensor and actuator configuration
    constexpr uint8_t LDR_PIN = 1;
    constexpr uint8_t BUZZER_PIN = 2;

    // Timing configuration for the smart alarm system
    constexpr uint8_t DEBOUNCE_MS = 50;
    constexpr uint16_t CLICK_MAX_MS = 500;
    constexpr uint16_t ARM_HOLD_MS = 2000;
    constexpr uint16_t ARMED_BLINK_MS = 2000;
    constexpr uint16_t ALARM_BLINK_MS = 200;
    constexpr uint16_t STARTUP_LED_STEP_MS = 200;
    constexpr uint16_t LDR_THRESHOLD = 200;

    // Password configuration
    constexpr char PASSWORD[] = "LRRL";

    // Tempo configuration for the smart alarm system
    constexpr uint16_t STARTUP_TEMPO = 118;
    constexpr uint16_t ARMING_TEMPO = 56;
    constexpr uint16_t RESULT_TEMPO = 150;
    constexpr uint16_t ALARM_TEMPO = 375;
} // namespace Config
