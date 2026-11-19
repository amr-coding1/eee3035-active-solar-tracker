// servo.cpp - see servo.h. Owner: CH (starter code by AM).
#include "servo.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

#include "config.h"
#include "src/datalink/core/ast_servo.h"
#include "transmission.h"

namespace servo {

namespace {

constexpr uint32_t kPwmHz = 50;          // 20 ms frame
constexpr uint32_t kPeriodUs = 20000;
constexpr uint8_t kPwmBits = 14;         // the most the ESP32-S3 LEDC allows at 50 Hz
const char* const kPrefsNs = "ast_servo";

ast::ServoSequencer* g_seq = nullptr;
bool g_pwm_on = false;
bool g_signal_low = false;   // pin set to a plain LOW output
int g_power_state = -1;      // -1 unknown, 0 off, 1 on
uint32_t g_duty = 0;
uint32_t g_saved_moves = 0;

ast::ServoConfig make_config() {
  ast::ServoConfig c;
  c.pulse_min_us = cfg::kServoPulseMinUs;
  c.pulse_max_us = cfg::kServoPulseMaxUs;
  c.servo_deg_at_level = cfg::kServoDegAtLevel;
  c.west_is_higher_pulse = cfg::kServoWestIsHigherPulse;
  c.trim_deg = cfg::kServoTrimDeg;
  c.limit_deg = cfg::kServoLimitDeg;
  c.slew_deg_per_s = cfg::kServoSlewDegPerS;
  c.power_on_ms = cfg::kServoPowerOnMs;
  c.settle_ms = cfg::kServoSettleMs;
  c.gate_power = cfg::kServoPowerPin >= 0;
  c.pulse_when_idle = false;
  return c;
}

void set_power(bool on) {
  if (cfg::kServoPowerPin < 0 || g_power_state == (on ? 1 : 0)) return;
  digitalWrite(cfg::kServoPowerPin, on ? HIGH : LOW);
  g_power_state = on ? 1 : 0;
}

void pwm_off() {
  if (g_pwm_on) {
    ledcWrite(cfg::kServoSignalPin, 0);
    ledcDetach(cfg::kServoSignalPin);
    g_pwm_on = false;
    g_signal_low = false;
  }
  // Signal LOW before the supply goes off, so the pin cannot feed the servo
  // through its input protection.
  if (!g_signal_low) {
    pinMode(cfg::kServoSignalPin, OUTPUT);
    digitalWrite(cfg::kServoSignalPin, LOW);
    g_signal_low = true;
  }
}

void pwm_set(uint16_t pulse_us) {
  const uint32_t duty = ast::servo_duty(pulse_us, kPeriodUs, kPwmBits);
  if (!g_pwm_on) {
    if (!ledcAttach(cfg::kServoSignalPin, kPwmHz, kPwmBits)) {
      transmission::log("servo: PWM could not start on the signal pin");
      return;
    }
    g_pwm_on = true;
    g_signal_low = false;
    g_duty = duty + 1;   // force the first write
  }
  if (duty != g_duty) {
    ledcWrite(cfg::kServoSignalPin, duty);
    g_duty = duty;
  }
}

void save_position() {
  Preferences p;
  if (!p.begin(kPrefsNs, false)) return;
  p.putFloat("pos", g_seq->position_deg());
  p.end();
}

}  // namespace

void begin() {
  if (cfg::kServoPowerPin >= 0) {
    pinMode(cfg::kServoPowerPin, OUTPUT);
    digitalWrite(cfg::kServoPowerPin, LOW);   // off (the gate driver also has a pull-down)
    g_power_state = 0;
  }
  pwm_off();
  // Start from the last angle a move ended at, so that the first power-up
  // pulses where the panel really is (no jump), if it has not been moved by hand.
  float start = cfg::kStowDeg;
  Preferences p;
  if (p.begin(kPrefsNs, true)) {
    start = p.getFloat("pos", cfg::kStowDeg);
    p.end();
  }
  if (!isfinite(start)) start = cfg::kStowDeg;
  g_seq = new ast::ServoSequencer(make_config(), start);
  transmission::log("servo: DS-R001, power switch %s, believed at %.1f deg",
                    cfg::kServoPowerPin >= 0 ? "fitted" : "NOT fitted (servo always powered)", static_cast<double>(start));
}

bool move_to(float panel_deg, bool* clamped) {
  if (g_seq == nullptr) return false;
  return g_seq->command(panel_deg, millis(), clamped);
}

void update() {
  if (g_seq == nullptr) return;
  const ast::ServoSequencer::Output o = g_seq->tick(millis());
  if (o.pulses) {
    set_power(o.power);   // supply first, then the signal
    pwm_set(o.pulse_us);
  } else {
    pwm_off();            // signal LOW first, then the supply
    set_power(o.power);
  }
  if (g_seq->moves() != g_saved_moves) {
    g_saved_moves = g_seq->moves();
    save_position();
  }
}

void stop() {
  if (g_seq == nullptr) return;
  g_seq->stop(millis());
  pwm_off();
  set_power(false);
}

bool moving() { return g_seq != nullptr && g_seq->busy(); }
float angle() { return g_seq != nullptr ? g_seq->position_deg() : 0.0f; }
float target() { return g_seq != nullptr ? g_seq->target_deg() : 0.0f; }
bool position_known() { return g_seq != nullptr && g_seq->position_known(); }
uint32_t moves() { return g_seq != nullptr ? g_seq->moves() : 0; }
uint32_t powered_ms() { return g_seq != nullptr ? g_seq->powered_ms() : 0; }
const char* phase_name() { return g_seq != nullptr ? ast::servo_phase_name(g_seq->phase()) : "?"; }

}  // namespace servo
