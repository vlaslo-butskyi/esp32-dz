#include <Arduino.h>

#define BLUE_BTN_PIN 7
#define RED_BTN_PIN 15

#define BLUE_LED_PIN 16
#define RED_LED_PIN 3

#define DEBOUNCE_DELAY 50 // milliseconds


class LED {
  private:
    uint8_t pin;
    int state;
    const char* name;

  public:
    LED(uint8_t ledPin, const char* ledName) : pin(ledPin), state(LOW), name(ledName) {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, state); // Initialize LED to off (active low)
    }

    void toggle() {
      state = !state;
      Serial.println(state ? "LED ON" + String(name) : "LED OFF" + String(name));
      Serial.println("LED state: " + String(state));
      Serial.println("LED pin: " + String(pin));
      digitalWrite(pin, state);
    }
};

class Button {
  private:
    uint8_t pin;
    int lastState;
    int currentState;
    unsigned long lastDebounceTime;
    LED* led;

  public:
    Button(uint8_t buttonPin, LED* buttonLed) : pin(buttonPin), led(buttonLed), lastState(HIGH), currentState(HIGH), lastDebounceTime(0) {
      pinMode(pin, INPUT_PULLUP);
    }

    void update() {
      uint8_t now = millis();
      int reading = digitalRead(pin);
      if (reading != lastState) {
        lastDebounceTime = now;
      }
      if ((now - lastDebounceTime) > DEBOUNCE_DELAY) {
        if (reading != currentState) {
          currentState = reading;
          if (currentState == LOW) {
            led->toggle();
          } else {
            // Do nothing on release
          }
        }
      }
      lastState = reading;
    }
};

LED ledRed(RED_LED_PIN, "Red");
LED ledBlue(BLUE_LED_PIN, "Blue");

Button btnRed(RED_BTN_PIN, &ledRed);
Button btnBlue(BLUE_BTN_PIN, &ledBlue);

#define PWM_CHANNEL 0
#define PWM_FREQUENCY 5000
#define PWM_RESOLUTION 8

void setup() {
  Serial.begin(115200);
  Serial.println("START");
}


void loop() {
  btnRed.update();
  btnBlue.update();
}
