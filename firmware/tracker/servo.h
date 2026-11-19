// servo.h - the DS-R001 servo that turns the tracked panel. Owner: CH
// (Actuators); starter code by AM, to be checked and tuned by CH.
//
// Wiring (see tracker/README.md):
//   orange (signal) <- D9 through 330 ohm      brown (GND) -> common GND
//   red (V+)        <- 5 V branch of the power bank THROUGH the high-side
//                      MOSFET switch driven by D8 (HIGH = on), 470-1000 uF
//                      across the servo supply
// The servo is only powered for a move (power-up, ramp, settle, then off):
// powered and idle it would draw its standby current all day, which is more
// than the whole tracking gain in winter. Whether it holds the panel when
// unpowered (plastic gears, slip clutch) must be tested (bench item M9).
//
// Panel angle convention: 0 = level, east negative, west positive. The pulse
// mapping and the move sequence are in core/ast_servo.h (unit-tested).
#pragma once
#include <stdint.h>

namespace servo {

// Call in setup(): switch off, signal LOW, last known angle from flash.
void begin();
// Starts a move (returns at once; update() does the work). The angle is
// clamped to +/-kServoLimitDeg; *clamped (optional) says it was outside.
// Returns false if the servo is already there and idle.
bool move_to(float panel_deg, bool* clamped = nullptr);
// Call on every pass of loop(): runs power-up, ramp, settle and power-off.
void update();
// Emergency stop (stall, fault): power off now; the angle becomes uncertain.
void stop();

bool moving();            // powered for a move right now
float angle();            // where the panel is believed to point (follows the ramp)
float target();           // where the current/last move goes
bool position_known();    // false after stop() until a move completes
uint32_t moves();         // completed moves since boot
uint32_t powered_ms();    // total time powered since boot (energy check)
const char* phase_name(); // "off", "power-up", "moving", "settling", "idle (powered)"

}  // namespace servo
