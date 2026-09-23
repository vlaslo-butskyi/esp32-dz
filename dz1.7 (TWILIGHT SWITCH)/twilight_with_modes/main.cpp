#include <Arduino.h>

#define LDR_PIN 1
#define RELAY_PIN 2

#define BUTTON_PIN 3
#define GREEN_LED_PIN 4
#define RED_LED_PIN 5
#define YELLOW_LED_PIN 6

#define RELAY_ON_LEVEL HIGH

#define THRESHOLD_DARK 1200
#define THRESHOLD_LIGHT 1800

#define SAMPLE_INTERVAL_MS 200
#define LOG_EVERY_N_SAMPLES 10

#define BTN_DEBOUNCE_MS 50

bool relayOn = false;
uint64_t lastSampleMs = 0;
uint16_t sampleNumber = 0;

bool buttonReading = false;
bool buttonStable = false;
uint64_t lastButtonPressMs = 0;

enum RelayState {
  OFF,
  ON,
  PERMANENT_ON
};

RelayState relayState = RelayState::ON;

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
    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, LOW);
  }
}

void turnOffPermanently() {
  if (relayState != RelayState::OFF) {
    relayState = RelayState::OFF;
    applyRelay(false);
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(YELLOW_LED_PIN, LOW);
  }
}

void turnOn() {
  if (relayState != RelayState::ON) {
    relayState = RelayState::ON;
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, HIGH);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  applyRelay(false);

  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, HIGH);

  analogReadResolution(12);
  analogSetPinAttenuation(LDR_PIN, ADC_11db);

  delay(300);

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
  const uint64_t now = millis();
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
