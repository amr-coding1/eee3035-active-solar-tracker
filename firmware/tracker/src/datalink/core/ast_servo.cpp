#include "ast_servo.h"

#include <math.h>

namespace ast {

float servo_clamp(float panel_deg, const ServoConfig& cfg, bool* clamped) {
  bool c = false;
  float v = panel_deg;
  const float lim = cfg.limit_deg > 0.0f ? cfg.limit_deg : 0.0f;
  if (!isfinite(v)) { v = 0.0f; c = true; }
  if (v > lim) { v = lim; c = true; }
  if (v < -lim) { v = -lim; c = true; }
  if (clamped != nullptr) *clamped = c;
  return v;
}

uint16_t servo_pulse_us(float panel_deg, const ServoConfig& cfg) {
  float trim = isfinite(cfg.trim_deg) ? cfg.trim_deg : 0.0f;
  if (trim > 10.0f) trim = 10.0f;
  if (trim < -10.0f) trim = -10.0f;
  const float panel = servo_clamp(servo_clamp(panel_deg, cfg, nullptr) + trim, cfg, nullptr);
  float servo_deg = cfg.west_is_higher_pulse ? cfg.servo_deg_at_level + panel : cfg.servo_deg_at_level - panel;
  if (servo_deg < 0.0f) servo_deg = 0.0f;
  if (servo_deg > cfg.travel_deg) servo_deg = cfg.travel_deg;
  const float span = cfg.pulse_max_us - cfg.pulse_min_us;
  const float travel = cfg.travel_deg > 0.0f ? cfg.travel_deg : 180.0f;
  const float us = cfg.pulse_min_us + servo_deg / travel * span;
  return static_cast<uint16_t>(lroundf(us < 0.0f ? 0.0f : us));
}

uint32_t servo_duty(uint16_t pulse_us, uint32_t period_us, uint8_t bits) {
  if (period_us == 0 || bits == 0 || bits > 20) return 0;
  const double full = static_cast<double>(1UL << bits);
  const double d = static_cast<double>(pulse_us) * full / static_cast<double>(period_us);
  return static_cast<uint32_t>(d + 0.5);
}

const char* servo_phase_name(ServoSequencer::Phase p) {
  switch (p) {
    case ServoSequencer::Phase::kOff: return "off";
    case ServoSequencer::Phase::kPowerUp: return "power-up";
    case ServoSequencer::Phase::kRamp: return "moving";
    case ServoSequencer::Phase::kSettle: return "settling";
    case ServoSequencer::Phase::kStopping: return "stopping";
    case ServoSequencer::Phase::kIdle: return "idle (powered)";
  }
  return "?";
}

ServoSequencer::ServoSequencer(const ServoConfig& cfg, float start_deg, bool start_known) : cfg_(cfg) {
  pos_ = target_ = servo_clamp(start_deg, cfg_, nullptr);
  known_ = start_known;
  phase_ = cfg_.gate_power ? Phase::kOff : Phase::kIdle;
}

void ServoSequencer::set_config(const ServoConfig& cfg) {
  cfg_ = cfg;
  if (!busy()) phase_ = cfg_.gate_power ? Phase::kOff : Phase::kIdle;
}

bool ServoSequencer::command(float panel_deg, uint32_t now_ms, bool* clamped) {
  const float t = servo_clamp(panel_deg, cfg_, clamped);
  if (!busy() && known_ && fabsf(t - pos_) < 0.05f) return false;
  target_ = t;
  if (!known_ && !busy()) pos_ = t;   // unknown start: power up straight at the target
  switch (phase_) {
    case Phase::kOff:
      phase_ = Phase::kPowerUp;
      phase_start_ms_ = now_ms;
      break;
    case Phase::kIdle:
    case Phase::kRamp:
    case Phase::kSettle:
    case Phase::kStopping:
      phase_ = Phase::kRamp;
      phase_start_ms_ = now_ms;
      break;
    case Phase::kPowerUp:
      break;   // keeps powering up; the ramp then goes to the new target
  }
  return true;
}

void ServoSequencer::stop(uint32_t now_ms) {
  (void)tick(now_ms);   // books the powered time up to now
  phase_ = cfg_.gate_power ? Phase::kOff : Phase::kIdle;
  known_ = false;
  target_ = pos_;
}

ServoSequencer::Output ServoSequencer::idle_output() const {
  Output o;
  if (phase_ == Phase::kIdle) {
    o.power = true;
    o.pulses = cfg_.pulse_when_idle;
    o.pulse_us = servo_pulse_us(pos_, cfg_);
  }
  return o;
}

ServoSequencer::Output ServoSequencer::tick(uint32_t now_ms) {
  const uint32_t dt = ticked_ ? now_ms - last_tick_ms_ : 0u;
  last_tick_ms_ = now_ms;
  ticked_ = true;
  if (phase_ != Phase::kOff) powered_ms_ += dt;

  Output o;
  switch (phase_) {
    case Phase::kOff:
    case Phase::kIdle:
      return idle_output();
    case Phase::kPowerUp:
      o.power = o.pulses = true;
      o.pulse_us = servo_pulse_us(pos_, cfg_);   // hold the old angle while the supply comes up
      if (now_ms - phase_start_ms_ >= cfg_.power_on_ms) {
        phase_ = Phase::kRamp;
        phase_start_ms_ = now_ms;
      }
      return o;
    case Phase::kRamp: {
      const uint32_t dt_ramp = dt > 50u ? 50u : dt;   // a late call slows the move, never makes it jump
      const float step = cfg_.slew_deg_per_s > 0.0f ? cfg_.slew_deg_per_s * static_cast<float>(dt_ramp) / 1000.0f : 1e9f;
      const float d = target_ - pos_;
      if (fabsf(d) <= step + 0.001f) {   // the small margin absorbs float rounding of the steps
        pos_ = target_;
        phase_ = Phase::kSettle;
        phase_start_ms_ = now_ms;
      } else {
        pos_ += d > 0.0f ? step : -step;
      }
      o.power = o.pulses = true;
      o.pulse_us = servo_pulse_us(pos_, cfg_);
      return o;
    }
    case Phase::kSettle:
      if (now_ms - phase_start_ms_ >= cfg_.settle_ms) {
        ++moves_;
        known_ = true;
        phase_ = Phase::kStopping;
        phase_start_ms_ = now_ms;
        o.power = true;   // pulses stop first; the supply follows in kStopping
        return o;
      }
      o.power = o.pulses = true;
      o.pulse_us = servo_pulse_us(target_, cfg_);
      return o;
    case Phase::kStopping:
      if (now_ms - phase_start_ms_ >= cfg_.stop_ms) {
        phase_ = cfg_.gate_power ? Phase::kOff : Phase::kIdle;
        return idle_output();
      }
      o.power = true;
      return o;
  }
  return o;
}

}  // namespace ast
