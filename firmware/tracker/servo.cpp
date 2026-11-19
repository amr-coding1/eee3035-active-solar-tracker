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
enum class Pwm : uint8_t { kOff, kOn, kStopping };
Pwm g_pwm = Pwm::kOff;
uint32_t g_pwm_stop_ms = 0;
constexpr uint32_t kPwmDrainMs = 25;   // > one 20 ms frame: the last pulse ends before the pin is released
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
  c.stop_ms = cfg::kServoStopMs;
  c.gate_power = cfg::kServoPowerPin >= 0;
  c.pulse_when_idle = false;
  return c;
}

void set_power(bool on) {
  if (cfg::kServoPowerPin < 0 || g_power_state == (on ? 1 : 0)) return;
  digitalWrite(cfg::kServoPowerPin, on ? HIGH : LOW);
  g_power_state = on ? 1 : 0;
}

void pin_low() {
  if (!g_signal_low) {
    pinMode(cfg::kServoSignalPin, OUTPUT);
    digitalWrite(cfg::kServoSignalPin, LOW);
    g_signal_low = true;
  }
}

// Stops the pulses without cutting one short: duty 0 takes effect at the end
// of the current 20 ms frame, so the pin is only released after kPwmDrainMs.
// Returns true once the signal is a plain LOW (only then may the supply go
// off, so the pin cannot feed the servo through its input protection).
bool pwm_stop(uint32_t now) {
  if (g_pwm == Pwm::kOn) {
    ledcWrite(cfg::kServoSignalPin, 0);
    g_pwm = Pwm::kStopping;
    g_pwm_stop_ms = now;
  }
  if (g_pwm == Pwm::kStopping) {
    if (now - g_pwm_stop_ms < kPwmDrainMs) return false;
    ledcDetach(cfg::kServoSignalPin);
    g_pwm = Pwm::kOff;
    g_signal_low = false;
  }
  pin_low();
  return true;
}

void pwm_set(uint16_t pulse_us) {
  const uint32_t duty = ast::servo_duty(pulse_us, kPeriodUs, kPwmBits);
  if (g_pwm == Pwm::kOff) {
    if (!ledcAttach(cfg::kServoSignalPin, kPwmHz, kPwmBits)) {
      transmission::log("servo: PWM could not start on the signal pin");
      return;
    }
    g_signal_low = false;
    g_duty = duty + 1;   // force the first write
  }
  g_pwm = Pwm::kOn;
  if (duty != g_duty) {
    ledcWrite(cfg::kServoSignalPin, duty);
    g_duty = duty;
  }
}

// NVS keys: "pos" = angle the last completed move ended at; "ok" = that angle
// can be trusted (cleared when a move starts and on an emergency stop, set
// when a move completes). After a reset in the middle of a move, a stop or a
// brown-out, "ok" is false and the next boot treats the angle as unknown.
void save_state(bool ok) {
  Preferences p;
  if (!p.begin(kPrefsNs, false)) return;
  if (ok) p.putFloat("pos", g_seq->position_deg());
  p.putBool("ok", ok);
  p.end();
}

}  // namespace

void begin() {
  if (cfg::kServoPowerPin >= 0) {
    pinMode(cfg::kServoPowerPin, OUTPUT);
    digitalWrite(cfg::kServoPowerPin, LOW);   // off (the gate driver also has a pull-down)
    g_power_state = 0;
  }
  pin_low();
  // Start from the last angle a move ended at, so that the first power-up
  // pulses where the panel really is (no jump), unless the last move did not
  // end cleanly or the panel has been moved by hand.
  float start = cfg::kStowDeg;
  bool known = false;
  Preferences p;
  if (p.begin(kPrefsNs, true)) {
    start = p.getFloat("pos", cfg::kStowDeg);
    known = p.getBool("ok", false);
    p.end();
  }
  if (!isfinite(start)) {
    start = cfg::kStowDeg;
    known = false;
  }
  g_seq = new ast::ServoSequencer(make_config(), start, known);
  transmission::log("servo: DS-R001, power switch %s, %s %.1f deg", cfg::kServoPowerPin >= 0 ? "fitted" : "NOT fitted (servo always powered)",
                    known ? "last at" : "position UNKNOWN (first move goes straight to its target), last saved", static_cast<double>(start));
}

bool move_to(float panel_deg, bool* clamped) {
  if (g_seq == nullptr) return false;
  const bool was_busy = g_seq->busy();
  const bool started = g_seq->command(panel_deg, millis(), clamped);
  if (started && !was_busy) save_state(false);   // in case power fails mid-move
  return started;
}

void update() {
  if (g_seq == nullptr) return;
  const uint32_t now = millis();
  const ast::ServoSequencer::Output o = g_seq->tick(now);
  if (o.pulses) {
    set_power(o.power);   // supply first, then the signal
    pwm_set(o.pulse_us);
  } else if (pwm_stop(now)) {
    set_power(o.power);   // the signal is LOW: now the supply may go off
  }
  if (g_seq->moves() != g_saved_moves) {
    g_saved_moves = g_seq->moves();
    save_state(true);
  }
}

void stop() {
  if (g_seq == nullptr) return;
  g_seq->stop(millis());
  const uint32_t t0 = millis();
  while (!pwm_stop(millis()) && millis() - t0 < 100) delay(5);   // let the last pulse finish
  set_power(false);
  save_state(false);
}

bool moving() { return g_seq != nullptr && g_seq->busy(); }
float angle() { return g_seq != nullptr ? g_seq->position_deg() : 0.0f; }
float target() { return g_seq != nullptr ? g_seq->target_deg() : 0.0f; }
bool position_known() { return g_seq != nullptr && g_seq->position_known(); }
uint32_t moves() { return g_seq != nullptr ? g_seq->moves() : 0; }
uint32_t powered_ms() { return g_seq != nullptr ? g_seq->powered_ms() : 0; }
const char* phase_name() { return g_seq != nullptr ? ast::servo_phase_name(g_seq->phase()) : "?"; }

}  // namespace servo
