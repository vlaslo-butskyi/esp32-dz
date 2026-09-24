#include "Alarm.h"

#include <Arduino.h>
#include <cstring>

#include "config.h"

#include "LED.h"
#include "Button.h"

#include "MelodyPlayer.h"
#include "melodies.h"

namespace {

enum class AlarmState {
  DISARMED,
  ARMING,
  ARMED,
  ALARM,
};

enum class EntryPhase {
  WAITING,
  VOTING,
  DING,
  RESULT,
};

LED greenLED(Config::LED_GREEN_PIN);
LED yellowLED(Config::LED_YELLOW_PIN);
LED redLED(Config::LED_RED_PIN);

// Pointers refer to the original objects, not copies.
LED* const leds[] = {
    &greenLED,
    &yellowLED,
    &redLED,
}; 

Button leftButton(
    Config::BUTTON_LEFT_PIN,
    Config::DEBOUNCE_MS,
    Config::CLICK_MAX_MS
);

Button rightButton(
    Config::BUTTON_RIGHT_PIN,
    Config::DEBOUNCE_MS,
    Config::CLICK_MAX_MS
);

MelodyPlayer player(Config::BUZZER_PIN);

AlarmState currentState = AlarmState::DISARMED;
EntryPhase entryPhase = EntryPhase::WAITING;

constexpr size_t PASSWORD_LENGTH = sizeof(Config::PASSWORD) - 1;

static_assert(
    PASSWORD_LENGTH > 0,
    "Password must not be empty"
);

char enteredCode[PASSWORD_LENGTH + 1] = {};
size_t enteredLength = 0;

bool codeAccepted = false;

// Prevent repeated activation from one continuous hold.
bool armGestureLatched = false;


void resetCodeEntry() {
  enteredLength = 0;
  enteredCode[0] = '\0';
}

void pushCodeSymbol(char symbol) {
  if (enteredLength < PASSWORD_LENGTH) {
    enteredCode[enteredLength++] = symbol;
  } else {
    // Keep the most recent PASSWORD_LENGTH symbols.
    std::memmove(
        enteredCode,
        enteredCode + 1,
        PASSWORD_LENGTH - 1
    );

    enteredCode[PASSWORD_LENGTH - 1] = symbol;
  }

  enteredCode[enteredLength] = '\0';
}

bool isCodeCorrect() {
  return enteredLength == PASSWORD_LENGTH &&
         std::strcmp(enteredCode, Config::PASSWORD) == 0;
}

bool canEnterCode() {
  return
      (
          currentState == AlarmState::ARMING &&
          entryPhase == EntryPhase::VOTING
      ) ||
      currentState == AlarmState::ARMED ||
      currentState == AlarmState::ALARM;
}

void processCodeInput() {
  if (!canEnterCode()) {
    return;
  }

  if (leftButton.wasClicked()) {
    pushCodeSymbol('L');
  }

  if (rightButton.wasClicked()) {
    pushCodeSymbol('R');
  }
}


void changeState(AlarmState nextState, uint32_t now) {
  currentState = nextState;

  yellowLED.resetBlink(now);
  redLED.resetBlink(now);

  // Define the initial LED levels for each new state.
  yellowLED.turnOff();
  redLED.turnOff();

  if (nextState == AlarmState::ALARM) {
    yellowLED.turnOn();
    redLED.turnOn();
  }
}

void updateIndicators(uint32_t now) {
  greenLED.turnOn();

  switch (currentState) {
    case AlarmState::DISARMED:
      yellowLED.turnOff();
      redLED.turnOff();
      break;

    case AlarmState::ARMING:
      yellowLED.setState(player.isSounding());
      redLED.turnOff();
      break;

    case AlarmState::ARMED:
      yellowLED.blink(now, Config::ARMED_BLINK_MS);
      redLED.turnOff();
      break;

    case AlarmState::ALARM:
      yellowLED.turnOn();
      redLED.blink(now, Config::ALARM_BLINK_MS);
      break;
  }
}


void startArming(uint32_t now) {
  resetCodeEntry();
  codeAccepted = false;

  entryPhase = EntryPhase::VOTING;
  changeState(AlarmState::ARMING, now);

  player.play(
      Melodies::RADA_BEEPS,
      Config::ARMING_TEMPO
  );

  Serial.println("Transitioning to ARMING");
}

void disarm(uint32_t now) {
  entryPhase = EntryPhase::WAITING;
  codeAccepted = false;

  resetCodeEntry();
  changeState(AlarmState::DISARMED, now);

  // play() stops any previous melody, including the alarm.
  player.play(
      Melodies::CUE_OK,
      Config::RESULT_TEMPO
  );

  Serial.println("Correct code: alarm disarmed");
}

void triggerAlarm(uint32_t now) {
  entryPhase = EntryPhase::WAITING;

  resetCodeEntry();
  changeState(AlarmState::ALARM, now);

  player.play(
      Melodies::MODEM_ALARM,
      Config::ALARM_TEMPO
  );

  Serial.println("Alarm triggered");
}


void updateDisarmed(uint32_t now) {
  const bool bothHeld =
      leftButton.isPressed() &&
      rightButton.isPressed() &&
      leftButton.heldFor(now) >= Config::ARM_HOLD_MS &&
      rightButton.heldFor(now) >= Config::ARM_HOLD_MS;

  if (bothHeld && !armGestureLatched) {
    armGestureLatched = true;
    startArming(now);
  }
}

void updateArming(uint32_t now) {
  if (player.isPlaying()) {
    return;
  }

  switch (entryPhase) {
    case EntryPhase::WAITING:
      break;

    case EntryPhase::VOTING:
      codeAccepted = isCodeCorrect();

      Serial.print("Entered code: ");
      Serial.println(enteredCode);

      resetCodeEntry();

      entryPhase = EntryPhase::DING;

      player.play(
          Melodies::RADA_DING,
          Config::ARMING_TEMPO
      );
      break;

    case EntryPhase::DING:
      entryPhase = EntryPhase::RESULT;

      if (codeAccepted) {
        player.play(
            Melodies::CUE_OK,
            Config::RESULT_TEMPO
        );
      } else {
        player.play(
            Melodies::CUE_WRONG,
            Config::RESULT_TEMPO
        );
      }
      break;

    case EntryPhase::RESULT:
      entryPhase = EntryPhase::WAITING;
      resetCodeEntry();

      changeState(
          codeAccepted
              ? AlarmState::ARMED
              : AlarmState::DISARMED,
          now
      );

      Serial.println(
          codeAccepted
              ? "Transitioning to ARMED"
              : "Code rejected: returning to DISARMED"
      );
      break;
  }
}

void updateArmed(uint32_t now) {
  // Give a completed valid code priority over the sensor.
  if (isCodeCorrect()) {
    disarm(now);
    return;
  }

  const int ldrValue = analogRead(Config::LDR_PIN);

  if (ldrValue < Config::LDR_THRESHOLD) {
    triggerAlarm(now);
  }
}

void updateAlarm(uint32_t now) {
  if (isCodeCorrect()) {
    disarm(now);
    return;
  }

  if (!player.isPlaying()) {
    player.play(
        Melodies::MODEM_ALARM,
        Config::ALARM_TEMPO
    );
  }
}


void runStartupAnimation() {
  for (LED* led : leds) {
    led->turnOn();
    delay(Config::STARTUP_LED_STEP_MS);
  }

  for (LED* led : leds) {
    led->turnOff();
    delay(Config::STARTUP_LED_STEP_MS);
  }

  for (LED* led : leds) {
    led->turnOn();
  }

  delay(Config::STARTUP_LED_STEP_MS);

  for (LED* led : leds) {
    led->turnOff();
  }

  greenLED.turnOn();
}

void playStartupSound() {
  player.play(
      Melodies::NOKIA_STARTUP,
      Config::STARTUP_TEMPO
  );

  // Intentional blocking during setup only.
  while (player.isPlaying()) {
    player.update();
    delay(1);
  }
}

}  // namespace


namespace Alarm {

    void init() {
        currentState = AlarmState::DISARMED;
        entryPhase = EntryPhase::WAITING;

        codeAccepted = false;
        armGestureLatched = false;
        resetCodeEntry();

        for (LED* led : leds) {
            led->init();
        }

        pinMode(Config::LDR_PIN, INPUT);
        player.init();

        runStartupAnimation();
        playStartupSound();

        leftButton.init();
        rightButton.init();

        const uint32_t now = millis();

        changeState(AlarmState::DISARMED, now);
        updateIndicators(now);

        Serial.println("Smart Alarm System initialized");
    }

    void update() {
        player.update();

        const uint32_t now = millis();

        leftButton.update(now);
        rightButton.update(now);

        if (!leftButton.isPressed() || !rightButton.isPressed()) {
            armGestureLatched = false;
        }

        processCodeInput();

        switch (currentState) {
            case AlarmState::DISARMED:
                updateDisarmed(now);
                break;

            case AlarmState::ARMING:
                updateArming(now);
                break;

            case AlarmState::ARMED:
                updateArmed(now);
                break;

            case AlarmState::ALARM:
                updateAlarm(now);
                break;
        }

        // Runs every iteration, including during ARMING sounds.
        updateIndicators(now);
    }

}  // namespace Alarm
