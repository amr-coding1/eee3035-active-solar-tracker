#include "ast_telemetry.h"

#include <chrono>

#include "ast_config.h"

namespace ast {

void Telemetry::set_angles(uint32_t now_ms, float panel_deg, float sun_deg, bool sun_valid) {
  std::lock_guard<std::mutex> g(mu_);
  panel_deg_ = panel_deg;
  sun_deg_ = sun_deg;
  sun_valid_ = sun_valid;
  angles_set_ = true;
  touch(now_ms);
}

void Telemetry::set_state(uint32_t now_ms, State state) {
  std::lock_guard<std::mutex> g(mu_);
  state_ = static_cast<uint8_t>(state) < kStateCount ? state : State::FLT;
  touch(now_ms);
}

void Telemetry::set_motor_running(uint32_t now_ms, bool running) {
  std::lock_guard<std::mutex> g(mu_);
  motor_running_ = running;
  if (running) motor_latch_ = true;
  touch(now_ms);
}

void Telemetry::set_fault(uint32_t now_ms, uint8_t code) {
  std::lock_guard<std::mutex> g(mu_);
  fault_ = code <= 9 ? code : 0;
  touch(now_ms);
}

void Telemetry::set_temperature(float deg_c) {
  std::lock_guard<std::mutex> g(mu_);
  temp_c_ = deg_c;
  temp_valid_ = true;
}

void Telemetry::clear_temperature() {
  std::lock_guard<std::mutex> g(mu_);
  temp_valid_ = false;
}

void Telemetry::set_run(uint16_t run_id) {
  std::lock_guard<std::mutex> g(mu_);
  run_ = run_id > kRunMax ? kRunMax : run_id;
  has_run_ = true;
}

void Telemetry::set_panel(char panel_letter) {
  std::lock_guard<std::mutex> g(mu_);
  if (panel_letter == 'a') panel_letter = 'A';
  if (panel_letter == 'b') panel_letter = 'B';
  if (panel_letter != 'A' && panel_letter != 'B') return;  // ignore anything else
  panel_ = panel_letter;
  has_panel_ = true;
}

TelemetrySnapshot Telemetry::take(uint32_t now_ms) {
  std::lock_guard<std::mutex> g(mu_);
  TelemetrySnapshot s;
  // Unsigned subtraction is correct across the 49-day wrap of the counter.
  s.fresh = ever_ && static_cast<uint32_t>(now_ms - last_ms_) <= kTelemetryStaleMs;
  if (s.fresh) {
    s.panel_valid = angles_set_;
    s.sun_valid = angles_set_ && sun_valid_;
    s.panel_deg = panel_deg_;
    s.sun_deg = sun_deg_;
    s.state = state_;
    s.fault = fault_;
    s.motor_ran = motor_running_ || motor_latch_;
  } else {
    // Control system silent: never report old angles as if they were current.
    s.state = State::IDL;
    s.motor_ran = motor_latch_;
  }
  // If the motor is still running it also counts for the next interval.
  motor_latch_ = motor_running_;
  s.temp_valid = temp_valid_;
  s.temp_c = temp_c_;
  s.has_run = has_run_;
  s.has_panel = has_panel_;
  s.run = run_;
  s.panel = panel_;
  has_run_ = false;
  has_panel_ = false;
  return s;
}

Telemetry& telemetry_instance() {
  static Telemetry t;
  return t;
}

uint32_t telemetry_now_ms() {
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void telemetry_set_angles(float panel_deg, float sun_deg, bool sun_valid) {
  telemetry_instance().set_angles(telemetry_now_ms(), panel_deg, sun_deg, sun_valid);
}
void telemetry_set_state(State state) { telemetry_instance().set_state(telemetry_now_ms(), state); }
void telemetry_set_motor_running(bool running) {
  telemetry_instance().set_motor_running(telemetry_now_ms(), running);
}
void telemetry_set_fault(uint8_t code) { telemetry_instance().set_fault(telemetry_now_ms(), code); }
void telemetry_set_temperature(float deg_c) { telemetry_instance().set_temperature(deg_c); }
void telemetry_clear_temperature() { telemetry_instance().clear_temperature(); }
void telemetry_set_run(uint16_t run_id) { telemetry_instance().set_run(run_id); }
void telemetry_set_panel(char panel_letter) { telemetry_instance().set_panel(panel_letter); }

}  // namespace ast
