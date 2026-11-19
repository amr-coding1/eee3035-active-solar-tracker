// Pins and settings of the tracker firmware: change them here, nowhere else.
// EEE3035 Group 8, Active Solar Tracker. Board: Arduino Nano ESP32.
//
// Use the D/A pin names (D9, A0, ...): they are right with either setting of
// Tools > Pin Numbering in the Arduino IDE. No Nano ESP32 pin takes 5 V.
#pragma once
#include <Arduino.h>

namespace cfg {

// ---- pins --------------------------------------------------------------------------
// Photodiodes (BPX65 + load resistor, powered from 3V3), east to west. A0-A3 are
// on ADC1, which keeps working while Wi-Fi is on (A6/A7 are ADC2: avoid).
constexpr uint8_t kSunPins[4] = {A0, A1, A2, A3};
// A4 = SDA, A5 = SCL: I2C bus of the power sensors and the DS3231 (data link).
constexpr int kServoSignalPin = D9;   // DS-R001 orange wire, through a 330 ohm resistor
constexpr int kServoPowerPin = D8;    // HIGH = servo powered (drives the MOSFET switch); -1 = no switch fitted
constexpr int kPanelPotPin = -1;      // D7 (ADC1) if the 10 k pot is coupled to the panel shaft; -1 = not fitted

// ---- power sensors (INA226 or INA219, found automatically) ----------------------
constexpr uint8_t kInaTracked = 0x40;   // tracked panel output
constexpr uint8_t kInaFixed = 0x41;     // fixed panel output
constexpr uint8_t kInaUse = 0x44;       // 5 V feed to the tracker: P_use and V_supply
// If the MCU and the servo are fed as two separate branches, each through its
// own sensor, put the second one here: P_use is then the sum. 0 = not used.
constexpr uint8_t kInaUse2 = 0x00;
// A four-channel INA219 board usually sits at 0x40-0x43: change these to match
// what the boot-time I2C scan prints (e.g. 0x40, 0x41, 0x42 and use2 0x43).

// ---- photodiode arc (Solar Sensing) ----------------------------------------------
constexpr float kSunSensorDeg[4] = {-45.0f, -15.0f, 15.0f, 45.0f};  // direction each photodiode faces
constexpr bool kSunArcOnPanel = false;   // true if the arc is mounted on the panel (it then sees sun minus panel)
constexpr int kSunSamples = 16;          // ADC readings averaged per channel per control cycle
constexpr float kSunDarkMv = 40.0f;      // all channels together below this = dark
constexpr float kSunSaturatedMv = 3000.0f;   // the ADC clips at about 3.1 V
constexpr float kSunMaxAbsDeg = 70.0f;   // light further out than this is not trusted
constexpr float kSunResponsePower = 1.0f;  // cos^k response: 1 bare photodiode; `sun` shows which k fits better
constexpr float kSunMinContrast = 0.15f;   // beam / (beam + ambient) below this = diffuse light: hold
constexpr float kSunMaxMisfit = 0.30f;     // misfit / beam above this = readings do not fit (sensor fault?)
constexpr uint32_t kSunFaultMs = 30000;    // poor fit for this long while lit = fault 5

// ---- panel angle pot (optional, catches a slipping servo clutch) -----------------
constexpr float kPotMvAtMinus45 = 800.0f;    // read both with `pot` at -45 and +45 deg (inclinometer)
constexpr float kPotMvAtPlus45 = 2400.0f;
constexpr float kPotSlipDeg = 6.0f;          // measured vs commanded beyond this after a move = fault 8

// ---- servo (DS-R001, Actuators) ----------------------------------------------------
constexpr float kServoPulseMinUs = 500.0f;   // servo 0 deg
constexpr float kServoPulseMaxUs = 2500.0f;  // servo 180 deg
constexpr float kServoDegAtLevel = 90.0f;    // servo angle with the panel level
constexpr bool kServoWestIsHigherPulse = true;   // flip if the panel turns the wrong way
constexpr float kServoTrimDeg = 0.0f;        // set from the inclinometer check (command-to-angle calibration)
constexpr float kServoLimitDeg = 50.0f;      // never commanded beyond +/-50 deg (F3 end-stop margin)
constexpr float kServoSlewDegPerS = 30.0f;   // gentle ramp: smaller current spikes on the power bank
constexpr uint32_t kServoPowerOnMs = 150;    // pulses at the old angle after switching on
constexpr uint32_t kServoSettleMs = 400;     // pulses held at the target before switching off
constexpr uint32_t kServoStopMs = 40;        // pulses stopped, supply still on, before switching off

// ---- tracking (Control) --------------------------------------------------------------
// false: after every boot the servo stays OFF until you type `servo auto` (or
// `servo <deg>`), so nothing moves before the direction, level and limits
// have been checked on the bench. Set true for unattended runs.
constexpr bool kStartTracking = false;
constexpr float kDeadbandDeg = 5.0f;         // F2a: re-point when more than 5 deg off
constexpr float kTrackLimitDeg = 45.0f;      // F2b: tracking range +/-45 deg
constexpr float kStowDeg = -45.0f;           // F3b: idle position (east, ready for sunrise)
constexpr uint32_t kDarkToStowMs = 60000;    // dark for 1 min -> stow (lamp tests; outdoors use 10 min)
constexpr uint32_t kMinMoveGapMs = 10000;    // never re-point more often than every 10 s
constexpr float kSunFilterAlpha = 0.3f;      // smoothing of the sun angle (1 = none)

// ---- protection ------------------------------------------------------------------------
// Stall (fault 8): energy drawn above the pre-move level, over the move and
// 1.5 s after it, beyond base + per-degree allowance. A jammed 5 deg move at
// the DS-R001's ~7 W stall costs about 5 J; a free one a few tenths.
constexpr float kStallBaseJ = 1.5f;
constexpr float kStallJPerDeg = 0.06f;
constexpr uint32_t kFaultShowMs = 2000;      // a short fault (7) is reported this long so the minute records it

// ---- timing -----------------------------------------------------------------------------
constexpr uint32_t kControlPeriodMs = 200;   // control loop at 5 Hz
constexpr uint32_t kLivePeriodMs = 1000;     // `live on`: one line a second

}  // namespace cfg
