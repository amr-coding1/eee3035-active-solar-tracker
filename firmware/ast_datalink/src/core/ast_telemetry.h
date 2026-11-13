// Telemetry API: how the control-system code hands its state to the data link
// (contract 7.7, documented with an example in firmware/INTERFACE.md).
//
// The control loop calls the telemetry_set_*() functions about once per
// second (any task, NOT from an interrupt handler). The data link's sampler
// reads a snapshot once per second. Nothing here blocks for longer than a
// few microseconds.
#pragma once
#include <stdint.h>

#include <mutex>

#include "ast_record.h"

namespace ast {

// ---- functions for the control system ----------------------------------------
// Panel angle about the tracking axis in degrees: 0 = level, east negative,
// west positive. sun_valid = false when the sensor arc cannot see the sun
// (dark, cloud, sensor fault): the sun angle is then not uploaded.
void telemetry_set_angles(float panel_deg, float sun_deg, bool sun_valid);
// Tracker state: State::TRK, HLD, IDL, STW or FLT (MOV is added by the data
// link for any minute in which the motor ran; passing MOV is allowed).
void telemetry_set_state(State state);
// Call with true when the motor starts and false when it stops. A burst
// shorter than one second is still seen (the flag is latched until sampled).
void telemetry_set_motor_running(bool running);
// Control-system fault code: 0 none, 5 sun-sensor arc, 7 limit switch,
// 8 motor stall/timeout (section 4). Keep calling with the current code.
void telemetry_set_fault(uint8_t code);
// Optional panel temperature (requirement F6). Without it field6 is omitted.
void telemetry_set_temperature(float deg_c);
void telemetry_clear_temperature();
// Optional: test run id (0..999) and panel letter ('A' or 'B') if the control
// system has a selector; normally they are set with the serial CLI.
void telemetry_set_run(uint16_t run_id);
void telemetry_set_panel(char panel_letter);

// ---- data-link side ------------------------------------------------------------
struct TelemetrySnapshot {
  bool fresh = false;        // the control system reported within the last 5 s
  bool panel_valid = false, sun_valid = false, temp_valid = false;
  float panel_deg = 0, sun_deg = 0, temp_c = 0;
  State state = State::IDL;
  uint8_t fault = 0;
  bool motor_ran = false;    // running now, or ran since the previous snapshot
  bool has_run = false, has_panel = false;
  uint16_t run = 0;
  char panel = 'A';
};

constexpr uint32_t kTelemetryStaleMs = 5000;

// The class behind the free functions (separate so it can be unit-tested with
// an explicit clock). All methods are thread-safe.
class Telemetry {
 public:
  void set_angles(uint32_t now_ms, float panel_deg, float sun_deg, bool sun_valid);
  void set_state(uint32_t now_ms, State state);
  void set_motor_running(uint32_t now_ms, bool running);
  void set_fault(uint32_t now_ms, uint8_t code);
  void set_temperature(float deg_c);
  void clear_temperature();
  void set_run(uint16_t run_id);
  void set_panel(char panel_letter);
  // Returns the current values and clears the motor latch and the run/panel
  // requests. When the control system has been silent for more than 5 s the
  // snapshot is not `fresh`: angles are unknown and the state is IDL.
  TelemetrySnapshot take(uint32_t now_ms);

 private:
  void touch(uint32_t now_ms) { last_ms_ = now_ms; ever_ = true; }
  std::mutex mu_;
  bool ever_ = false;
  uint32_t last_ms_ = 0;
  bool angles_set_ = false, sun_valid_ = false, temp_valid_ = false;
  float panel_deg_ = 0, sun_deg_ = 0, temp_c_ = 0;
  State state_ = State::IDL;
  uint8_t fault_ = 0;
  bool motor_running_ = false, motor_latch_ = false;
  bool has_run_ = false, has_panel_ = false;
  uint16_t run_ = 0;
  char panel_ = 'A';
};

Telemetry& telemetry_instance();   // the object used by the free functions
uint32_t telemetry_now_ms();       // monotonic milliseconds (std::chrono::steady_clock)

}  // namespace ast
