// DS-R001 servo: pulse mapping and the move sequence (Actuators; owner CH,
// starter code by AM). Portable logic only: the PWM and the power switch are
// driven in tracker/servo.cpp from the Output this class returns.
//
// The servo turns to the angle given by its pulse width (500-2500 us over
// 0-180 deg, 50 Hz) with its own driver and position loop. It has plastic
// spur gears and a slip clutch, and it draws standby current whenever it is
// powered, so the firmware switches it OFF between moves (high-side MOSFET):
//
//   off --command--> power-up (pulses at the OLD angle, so it does not jump)
//       --> ramp (pulse moves to the new angle at slew_deg_per_s: gentle on the
//           power bank) --> settle (pulse held at the target) --> off
//
// Panel angle convention (contract): 0 = level, east negative, west positive.
// panel_deg = servo_deg - servo_deg_at_level when west_is_higher_pulse.
#pragma once
#include <stdint.h>

namespace ast {

struct ServoConfig {
  float pulse_min_us = 500.0f;        // servo at 0 deg (DS-R001 listing)
  float pulse_max_us = 2500.0f;       // servo at travel_deg
  float travel_deg = 180.0f;
  float servo_deg_at_level = 90.0f;   // servo angle when the panel is level
  bool west_is_higher_pulse = true;   // flip if the servo is mounted the other way round
  float trim_deg = 0.0f;              // added to every command (found with the inclinometer)
  float limit_deg = 50.0f;            // |panel| is never commanded beyond this (F3 end-stop margin)
  float slew_deg_per_s = 30.0f;       // ramp speed of the pulse
  uint32_t power_on_ms = 150;         // pulses at the old angle before the ramp starts
  uint32_t settle_ms = 400;           // pulses held at the target after the ramp
  bool gate_power = true;             // false: no power switch fitted, servo always powered
  bool pulse_when_idle = false;       // only without a switch: keep pulsing between moves (holds, costs current)
};

// Pulse width (us) for a panel angle; the angle is clamped to +/-limit_deg
// after the trim is added, and to the servo's own travel.
uint16_t servo_pulse_us(float panel_deg, const ServoConfig& cfg);
// Clamps a requested panel angle to +/-limit_deg; *clamped is set when it was outside.
float servo_clamp(float panel_deg, const ServoConfig& cfg, bool* clamped);
// LEDC duty for a pulse: pulse_us / period_us * 2^bits, rounded.
uint32_t servo_duty(uint16_t pulse_us, uint32_t period_us, uint8_t bits);

class ServoSequencer {
 public:
  enum class Phase : uint8_t { kOff, kPowerUp, kRamp, kSettle, kIdle };
  struct Output {
    bool power = false;      // servo supply switch
    bool pulses = false;     // PWM running (false: signal pin held LOW)
    uint16_t pulse_us = 0;   // valid when pulses
  };

  // start_deg: where the panel is believed to be at boot (unknown in truth:
  // the first move may jump from wherever it really is).
  ServoSequencer(const ServoConfig& cfg, float start_deg);

  void set_config(const ServoConfig& cfg);
  const ServoConfig& config() const { return cfg_; }

  // Requests a move to panel_deg (clamped to +/-limit_deg, *clamped set if it
  // was). A new command during a move retargets it. Returns false when the
  // servo is already at that angle and idle (nothing to do).
  bool command(float panel_deg, uint32_t now_ms, bool* clamped);
  // Emergency stop (stall, fault): power off at once, the move is abandoned
  // and the position is marked uncertain.
  void stop(uint32_t now_ms);
  // Advances the sequence; call every few milliseconds (the ramp is computed
  // from the elapsed time, so a late call only makes the steps coarser).
  Output tick(uint32_t now_ms);

  bool busy() const { return phase_ == Phase::kPowerUp || phase_ == Phase::kRamp || phase_ == Phase::kSettle; }
  bool powered() const { return phase_ != Phase::kOff; }
  Phase phase() const { return phase_; }
  float position_deg() const { return pos_; }      // follows the ramp (where the pulse points now)
  float target_deg() const { return target_; }
  bool position_known() const { return known_; }   // false after stop() until the next completed move
  uint32_t moves() const { return moves_; }        // completed moves since boot
  uint32_t powered_ms() const { return powered_ms_; }  // total time powered since boot

 private:
  Output idle_output() const;
  ServoConfig cfg_;
  Phase phase_ = Phase::kOff;
  float pos_ = 0, target_ = 0;
  bool known_ = true;
  uint32_t phase_start_ms_ = 0, last_tick_ms_ = 0;
  bool ticked_ = false;
  uint32_t moves_ = 0, powered_ms_ = 0;
};

const char* servo_phase_name(ServoSequencer::Phase p);

}  // namespace ast
