#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

constexpr uint8_t LDR_PIN = 1;
constexpr uint8_t RELAY_PIN = 2;

constexpr uint8_t BUTTON_PIN = 3;

constexpr uint8_t RGB_LED_PIN = 48;

constexpr uint8_t RELAY_ON_LEVEL = HIGH;

constexpr uint16_t THRESHOLD_DARK = 1200;
constexpr uint16_t THRESHOLD_LIGHT = 1800;

constexpr uint8_t SAMPLE_INTERVAL_MS = 200;
constexpr uint8_t LOG_EVERY_N_SAMPLES = 10;

constexpr uint8_t BTN_DEBOUNCE_MS = 50;

bool relayOn = false;
uint32_t lastSampleMs = 0;
uint16_t sampleNumber = 0;

bool buttonReading = false;
bool buttonStable = false;
uint32_t lastButtonPressMs = 0;

enum RelayState {
  OFF,
  ON,
  PERMANENT_ON
};

RelayState relayState = RelayState::ON;

Adafruit_NeoPixel rgbLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

void applyRelay(bool on) {
  relayOn = on;
  digitalWrite(RELAY_PIN, on ? RELAY_ON_LEVEL : !RELAY_ON_LEVEL);
}

void printSample(uint16_t raw, uint16_t millivolts, const char* reason) {
  Serial.printf(
    "%5u | %5u | %5u | %-5s | %s\r\n", 
    sampleNumber, raw, millivolts, relayOn ? "ON" : "OFF", reason
  );
}

void turnOnPermanently() {
  if (relayState != RelayState::PERMANENT_ON) {
    relayState = RelayState::PERMANENT_ON;
    applyRelay(true);
    rgbLed.setPixelColor(0, rgbLed.Color(0, 255, 0)); // Green
    rgbLed.show();
  }
}

void turnOffPermanently() {
  if (relayState != RelayState::OFF) {
    relayState = RelayState::OFF;
    applyRelay(false);
    rgbLed.setPixelColor(0, rgbLed.Color(255, 0, 0)); // Red
    rgbLed.show();
  }
}

void turnOn() {
  if (relayState != RelayState::ON) {
    relayState = RelayState::ON;
    rgbLed.setPixelColor(0, rgbLed.Color(255, 255, 0)); // Yellow
    rgbLed.show();
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  rgbLed.begin();
  rgbLed.setBrightness(50); // Set brightness to 50 (out of 255)
  
  applyRelay(false);

  rgbLed.setPixelColor(0, rgbLed.Color(255, 255, 0)); // Yellow
  rgbLed.show();

  analogReadResolution(12);
  analogSetPinAttenuation(LDR_PIN, ADC_11db);

  Serial.println();
  Serial.println("=== DZ 1.7: twilight switch ===");
  Serial.printf(
    "LDR on GPIO %d (ADC1), relay on GPIO %d, sample every %d ms\r\n",
    LDR_PIN, RELAY_PIN, SAMPLE_INTERVAL_MS
  );
  Serial.printf(
    "dark < %d, light > %d, between - keep the current state\r\n",
    THRESHOLD_DARK, THRESHOLD_LIGHT
  );
  Serial.printf(
    "relay is on when GPIO %d is %s\r\n",
    RELAY_PIN, RELAY_ON_LEVEL == HIGH ? "HIGH" : "LOW"
  );
  Serial.println();
  Serial.println("    # |   RAW | U, mV | relay | note");
  Serial.println("------+-------+-------+-------+----------------------");
}

void loop() {
  const uint32_t now = millis();
  const bool btnReading = digitalRead(BUTTON_PIN);

  if (btnReading != buttonReading) {
    buttonReading = btnReading;
    lastButtonPressMs = now;
  }

  if (now - lastButtonPressMs > BTN_DEBOUNCE_MS && btnReading != buttonStable) {
    buttonStable = btnReading;
    if (buttonStable == LOW) {
      if (relayState == RelayState::ON) {
        turnOnPermanently();
        Serial.println("Switched to PERMANENT_ON");
      } else if (relayState == RelayState::PERMANENT_ON) {
        turnOffPermanently();
        Serial.println("Switched to OFF");
      } else if (relayState == RelayState::OFF) {
        turnOn();
        Serial.println("Switched to ON");
      }
    }
  }

  if (relayState == RelayState::ON) {
    if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
    lastSampleMs = now;

    const uint16_t raw = analogRead(LDR_PIN);
    const uint16_t millivolts = analogReadMilliVolts(LDR_PIN);
    sampleNumber++;

    const bool wasOn = relayOn;

    if (raw < THRESHOLD_DARK) {
      applyRelay(true);
    } else if (raw > THRESHOLD_LIGHT) {
      applyRelay(false);
    }

    if (relayOn != wasOn) {
      printSample(
        raw, millivolts, 
        relayOn ? "-> DARK, load on" : "-> LIGHT, load off"
      );
    } else if (sampleNumber % LOG_EVERY_N_SAMPLES == 0) {
      printSample(
        raw, millivolts, 
        raw < THRESHOLD_DARK ? "dark" : 
          raw > THRESHOLD_LIGHT ? "light" : "between thresholds"
      );
    }
  }
}
