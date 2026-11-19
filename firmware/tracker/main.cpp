// main.cpp - the tracker firmware's setup() and loop(). EEE3035 Group 8,
// Active Solar Tracker. Board: Arduino Nano ESP32.
//
// Each module is its own .h/.cpp pair, all compiled together into one program:
//   transmission  data link: power sensors, clock, flash, ThingSpeak, serial commands (AM)
//   sensing       photodiode arc -> light direction (CO)
//   servo         DS-R001 servo with its power switch (CH)
//   control       tracking decisions, faults, manual/test commands (CO)
//   config.h      every pin and setting
// "Running at the same time": the data link works in its own FreeRTOS tasks;
// loop() below runs the control cycle at 5 Hz and keeps the servo sequence
// and the serial port going in between. Nothing here blocks for long.
#include <Arduino.h>

#include "config.h"
#include "control.h"
#include "sensing.h"
#include "servo.h"
#include "transmission.h"

void setup() {
  transmission::begin();   // first: serial port, clock, power sensors, flash, uploads
  sensing::begin();
  servo::begin();
  control::begin();
  transmission::log("tracker ready: type `help`, or `live on` to watch every value");
}

void loop() {
  static uint32_t last_step = 0;
  const uint32_t now = millis();
  if (now - last_step >= cfg::kControlPeriodMs) {
    last_step = now;
    control::step();       // sense, decide, command the servo, report to the data link
  }
  servo::update();         // power-up / ramp / settle / power-off sequence
  transmission::poll();    // serial commands and log lines
  delay(5);
}
