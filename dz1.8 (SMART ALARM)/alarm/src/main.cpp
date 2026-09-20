#include <Arduino.h>

#include "Alarm.h"

void setup() {
  Serial.begin(115200);

  Serial.println("--------------------------------");
  Serial.println("Initializing Smart Alarm System...");

  Alarm::init();

  Serial.println("--------------------------------");
}

void loop() {
  Alarm::update();
}