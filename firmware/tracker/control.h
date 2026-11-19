// control.h - the tracking loop: sensing -> decision -> servo -> data link.
// Owner: CO (Solar Sensing and Tracking Control); starter code by AM.
// The decisions themselves (deadband, limits, stow, hold, faults) are in
// core/ast_tracker.h and unit-tested on the PC; this file is the glue.
//
// After boot the servo is held OFF until `servo <deg>` or `servo auto`
// (config.h kStartTracking = false), so nothing moves before the bench checks.
//
// Serial commands:
//   servo <deg>     manual mode: move to that panel angle and hold (bench, inclinometer)
//   servo test      manual steps -45, -30, ... +45, 0 with 8 s at each (command-to-angle check)
//   servo auto      back to tracking
//   servo off       switch the servo off now and hold (manual)
//   live on|off     one line a second with every value (sun, panel, powers, state)
//   ctrl            control status        ctrl reset      clear a latched fault (8)
#pragma once

namespace control {

void begin();   // after transmission::begin(), sensing::begin() and servo::begin()
void step();    // every cfg::kControlPeriodMs (5 Hz) from loop()

}  // namespace control
