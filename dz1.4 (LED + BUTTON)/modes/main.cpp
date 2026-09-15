#include <Arduino.h>

#define BTN_MODE_FAST_PIN 7
#define BTN_MODE_SLOW_PIN 15

#define BLUE_LED_PIN 16
#define RED_LED_PIN 3

#define DEBOUNCE_DELAY 50 // milliseconds


class LED {
  private:
    uint8_t pin;

  public:
    explicit LED(uint8_t ledPin) : pin(ledPin) {}

    void begin() {
      pinMode(pin, OUTPUT);
      set(false); // Ensure the LED is off initially
    }

    void set(bool state) {
      digitalWrite(pin, state ? HIGH : LOW);
    }
};

class Button {
  private:
    uint8_t pin;
    uint8_t inputMode;

  public:
    Button(uint8_t buttonPin, uint8_t mode = INPUT_PULLUP) : pin(buttonPin), inputMode(mode) {}

    void begin() {
      pinMode(pin, inputMode);
    }
    
    bool isPressed() {
      return digitalRead(pin) == LOW; // Assuming active-low button
    }
};

class Mode {
  protected:
    LED& led1;
    LED& led2;
    bool phase = false;

  public:
    Mode(LED& led1, LED& led2) : led1(led1), led2(led2) {}
    virtual ~Mode() = default;

    virtual const char* name() const = 0;
    virtual unsigned long interval() const = 0;
    virtual void step() = 0;

    void reset() {
      phase = false;
    }
};

class FastMode : public Mode {
  public:
    using Mode::Mode; // Inherit constructor

    const char* name() const override {
      return "Fast Mode";
    }
    unsigned long interval() const override {
      return 200; // milliseconds
    }

    void step() override {
      phase = !phase;
      led1.set(phase);
      led2.set(phase);
    }
};

class SlowMode: public Mode {
  public:
    using Mode::Mode; // Inherit constructor

    const char* name() const override {
      return "Slow Mode";
    }

    unsigned long interval() const override {
      return 1000; // milliseconds
    }

    void step() override {
      phase = !phase;
      led1.set(phase);
      led2.set(!phase);
    }
};
    
LED ledBlue(BLUE_LED_PIN);
LED ledRed(RED_LED_PIN);

Button btnFast(BTN_MODE_FAST_PIN);
Button btnSlow(BTN_MODE_SLOW_PIN);

FastMode fastMode(ledBlue, ledRed);
SlowMode slowMode(ledBlue, ledRed);

Mode* currentMode = &fastMode;

void switchMode(Mode* newMode) {
  if (newMode == currentMode) return; // No change
  currentMode = newMode;
  currentMode->reset();
  Serial.print("Switched to: ");
  Serial.println(currentMode->name());
}

void pullButtons() {
  if (btnFast.isPressed()) {
    switchMode(&fastMode);
  } else if (btnSlow.isPressed()) {
    switchMode(&slowMode);
  }
}

void waitWithButtons(unsigned long duration) {
  Mode* lastMode = currentMode;
  unsigned long startTime = millis();
  while (millis() - startTime < duration) {
    pullButtons();
    if (currentMode != lastMode) return; // Exit if mode changed
    delay(DEBOUNCE_DELAY); // Small delay to avoid busy waiting
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting...");

  ledBlue.begin();
  ledRed.begin();
  btnFast.begin();
  btnSlow.begin();

  Serial.print("Initial mode: ");
  Serial.println(currentMode->name());
}


void loop() {
  currentMode->step();
  waitWithButtons(currentMode->interval());
}
