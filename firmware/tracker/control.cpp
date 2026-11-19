// control.cpp - see control.h. Owner: CO (starter code by AM).
#include "control.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "sensing.h"
#include "servo.h"
#include "src/datalink/core/ast_tracker.h"
#include "transmission.h"

namespace control {

namespace {

const char* const kHelp =
    "Control and servo:\n"
    "  servo <deg>              manual: move to that panel angle and hold (0 level, east -)\n"
    "  servo test               manual steps -45..+45 deg, 8 s each (inclinometer check)\n"
    "  servo auto               start tracking (the servo is held off after boot)\n"
    "  servo off                switch the servo off now and hold\n"
    "  live on|off              one line a second: sun, panel, powers, state\n"
    "  ctrl | ctrl reset        control status | clear a latched servo fault\n";

constexpr float kTestAngles[] = {-45.0f, -30.0f, -15.0f, 0.0f, 15.0f, 30.0f, 45.0f, 0.0f};
constexpr int kTestCount = sizeof kTestAngles / sizeof kTestAngles[0];
constexpr uint32_t kTestHoldMs = 8000;
constexpr uint32_t kSaturatedFaultMs = 10000;   // saturated for this long = sun-sensor arc fault (5)

ast::Tracker* g_tracker = nullptr;
ast::StallGuard* g_stall = nullptr;

uint8_t g_latched_fault = 0;       // 8: servo stall or clutch slip, until `ctrl reset`
const char* g_latched_why = "";
uint8_t g_short_fault = 0;         // 7: reported for kFaultShowMs
uint32_t g_short_since = 0;
uint32_t g_saturated_since = 0;
bool g_saturated = false;
uint32_t g_misfit_since = 0;
bool g_misfit = false;
float g_move_deg = 0.0f;           // size of the move in progress (stall allowance)

bool g_live = false;
uint32_t g_last_live = 0;
bool g_test = false;
int g_test_idx = 0;
bool g_test_step_now = false;      // take the next test step as soon as the servo is idle
uint32_t g_test_step_ms = 0;

// Last values, for `ctrl` and `live`.
sensing::Sun g_sun;
ast::TrackerDecision g_dec;
float g_panel = 0.0f;
bool g_panel_measured = false;
uint8_t g_fault_now = 0;

ast::TrackerConfig make_tracker_config() {
  ast::TrackerConfig c;
  c.deadband_deg = cfg::kDeadbandDeg;
  c.track_limit_deg = cfg::kTrackLimitDeg;
  c.stow_deg = cfg::kStowDeg;
  c.dark_to_stow_ms = cfg::kDarkToStowMs;
  c.min_move_gap_ms = cfg::kMinMoveGapMs;
  c.filter_alpha = cfg::kSunFilterAlpha;
  return c;
}

transmission::State to_report(ast::State s) {
  switch (s) {
    case ast::State::TRK:
    case ast::State::MOV: return transmission::State::kTracking;
    case ast::State::HLD: return transmission::State::kHolding;
    case ast::State::IDL: return transmission::State::kIdle;
    case ast::State::STW: return transmission::State::kStowed;
    case ast::State::FLT: return transmission::State::kFault;
  }
  return transmission::State::kIdle;
}

void latch_fault(uint8_t code, const char* why) {
  if (g_latched_fault != 0) return;
  g_latched_fault = code;
  g_latched_why = why;
  transmission::log("FAULT %u: %s - tracking stopped; fix it, then type `ctrl reset`", static_cast<unsigned>(code), why);
}

void manual_move(float deg) {
  if (g_latched_fault != 0) {
    Serial.printf("servo: fault %u is latched (%s); fix it, then `ctrl reset` - no move\n", static_cast<unsigned>(g_latched_fault), g_latched_why);
    return;
  }
  g_tracker->set_manual(deg);
  transmission::log("servo: manual, to %.1f deg", static_cast<double>(deg));
}

void print_status() {
  const transmission::Power pw = transmission::power();
  const transmission::Status st = transmission::status();
  Serial.printf("mode      %s%s, state %s, fault %u%s%s\n", g_tracker->manual() ? "MANUAL" : "auto (tracking)", g_test ? " (servo test running)" : "",
                ast::state_name(g_dec.state), static_cast<unsigned>(g_fault_now), g_latched_fault ? " LATCHED: " : "", g_latched_why);
  if (g_sun.valid) {
    Serial.printf("sun       %.1f deg (filtered %.1f)%s\n", static_cast<double>(g_sun.sun_deg), static_cast<double>(g_dec.sun_deg),
                  g_sun.saturated ? ", one channel clipped" : "");
  } else {
    Serial.printf("sun       no estimate: %s (total %.0f mV)\n", g_sun.why, static_cast<double>(g_sun.total_mv));
  }
  Serial.printf("panel     %.1f deg (%s), target %.1f, servo %s, %lu moves, powered %lu s since boot%s\n", static_cast<double>(g_panel),
                g_panel_measured ? "measured by the pot" : "commanded", static_cast<double>(servo::target()), servo::phase_name(),
                static_cast<unsigned long>(servo::moves()), static_cast<unsigned long>(servo::powered_ms() / 1000u),
                servo::position_known() ? "" : ", POSITION UNCERTAIN");
  Serial.printf("power     trk %s fix %s use %s mW, supply %.3f V\n", pw.tracked_ok ? String(pw.tracked_mw, 1).c_str() : "--",
                pw.fixed_ok ? String(pw.fixed_mw, 1).c_str() : "--", pw.use_ok ? String(pw.use_mw, 1).c_str() : "--",
                static_cast<double>(pw.supply_v));
  Serial.printf("data link time %s, waiting %lu, last upload %s, wifi %s, fault %u%s\n", st.time_valid ? "valid" : "NOT VALID (not logging)",
                static_cast<unsigned long>(st.waiting), st.last_upload_s_ago < 0 ? "never" : (String(st.last_upload_s_ago) + " s ago").c_str(),
                st.wifi_on ? "on" : "off", static_cast<unsigned>(st.fault), st.simulator ? ", SIMULATOR ON" : "");
  Serial.printf("settings  deadband %.1f, range +/-%.0f, stow %.0f, dark->stow %lu s, gap %lu s\n", static_cast<double>(cfg::kDeadbandDeg),
                static_cast<double>(cfg::kTrackLimitDeg), static_cast<double>(cfg::kStowDeg), static_cast<unsigned long>(cfg::kDarkToStowMs / 1000u),
                static_cast<unsigned long>(cfg::kMinMoveGapMs / 1000u));
  Serial.printf("stall     last move window %.2f J above the pre-move level (allowed %.2f J)\n", static_cast<double>(g_stall->excess_j()),
                static_cast<double>(g_stall->allowance_j()));
}

void print_live() {
  const transmission::Power pw = transmission::power();
  const transmission::Status st = transmission::status();
  Serial.printf("live sun=%s panel=%.1f target=%.1f servo=%s state=%s fault=%u mv=%.0f/%.0f/%.0f/%.0f P_trk=%.1f P_fix=%.1f P_use=%.1f V=%.3f wait=%lu wifi=%s\n",
                g_dec.sun_valid ? String(g_dec.sun_deg, 1).c_str() : (g_sun.dark ? "dark" : "none"), static_cast<double>(g_panel),
                static_cast<double>(servo::target()), servo::phase_name(), ast::state_name(g_dec.state), static_cast<unsigned>(g_fault_now),
                static_cast<double>(g_sun.mv[0]), static_cast<double>(g_sun.mv[1]), static_cast<double>(g_sun.mv[2]),
                static_cast<double>(g_sun.mv[3]), static_cast<double>(pw.tracked_ok ? pw.tracked_mw : NAN),
                static_cast<double>(pw.fixed_ok ? pw.fixed_mw : NAN), static_cast<double>(pw.use_ok ? pw.use_mw : NAN),
                static_cast<double>(pw.supply_v), static_cast<unsigned long>(st.waiting), st.wifi_on ? "on" : "off");
}

bool handle(const char* line) {
  char buf[48];
  strncpy(buf, line, sizeof buf - 1);
  buf[sizeof buf - 1] = '\0';
  char* save = nullptr;
  const char* w0 = strtok_r(buf, " \t", &save);
  const char* w1 = w0 != nullptr ? strtok_r(nullptr, " \t", &save) : nullptr;
  if (w0 == nullptr) return false;

  if (strcmp(w0, "live") == 0) {
    if (w1 != nullptr && strcmp(w1, "off") == 0) g_live = false;
    else g_live = true;
    Serial.printf("live %s\n", g_live ? "on (one line a second; `live off` to stop)" : "off");
    return true;
  }
  if (strcmp(w0, "ctrl") == 0) {
    if (w1 != nullptr && strcmp(w1, "reset") == 0) {
      g_latched_fault = 0;
      g_latched_why = "";
      g_stall->reset();
      Serial.printf("ctrl: latched fault cleared\n");
    } else {
      print_status();
    }
    return true;
  }
  if (strcmp(w0, "servo") != 0) return false;
  if (w1 == nullptr) {
    print_status();
  } else if (strcmp(w1, "auto") == 0) {
    g_test = false;
    g_tracker->set_auto();
    Serial.printf("servo: back to tracking\n");
  } else if (strcmp(w1, "off") == 0) {
    g_test = false;
    servo::stop();
    g_tracker->hold();   // do not move again until told
    Serial.printf("servo: switched off, manual hold (`servo auto` to track again)\n");
  } else if (strcmp(w1, "test") == 0) {
    if (g_latched_fault != 0) {
      Serial.printf("servo test: fault %u is latched (%s); `ctrl reset` first\n", static_cast<unsigned>(g_latched_fault), g_latched_why);
      return true;
    }
    g_test = true;
    g_test_idx = 0;
    g_test_step_now = true;
    Serial.printf("servo test: %d steps of %lu s; read the inclinometer at each and note commanded vs measured\n", kTestCount,
                  static_cast<unsigned long>(kTestHoldMs / 1000u));
  } else {
    char* end = nullptr;
    const float deg = strtof(w1, &end);
    if (end == w1 || !isfinite(deg)) {
      Serial.printf("error: servo <deg> | servo test | servo auto | servo off\n");
      return true;
    }
    g_test = false;
    manual_move(deg);
  }
  return true;
}

}  // namespace

void begin() {
  g_tracker = new ast::Tracker(make_tracker_config());
  ast::StallConfig sc;
  sc.base_j = cfg::kStallBaseJ;
  sc.j_per_deg = cfg::kStallJPerDeg;
  g_stall = new ast::StallGuard(sc);
  transmission::add_commands(handle, kHelp);
  if (!cfg::kStartTracking) {
    g_tracker->hold();   // nothing moves until someone types `servo auto` or `servo <deg>`
    transmission::log("control: servo held OFF after boot - `servo <deg>` to move by hand, `servo auto` to start tracking");
  }
}

void step() {
  const uint32_t now = millis();

  // 1. Sensing
  g_sun = sensing::read_sun();
  float measured = 0.0f;
  g_panel_measured = sensing::panel_angle(&measured);
  g_panel = g_panel_measured ? measured : servo::angle();
  const float sun_abs = g_sun.sun_deg + (cfg::kSunArcOnPanel ? g_panel : 0.0f);

  // 2. Faults
  const transmission::Power pw = transmission::power();
  if (g_stall->update(now, servo::moving(), g_move_deg, pw.use_ok, pw.use_mw)) {
    servo::stop();
    latch_fault(8, "servo stalled or overloaded (too much energy drawn for the move)");
  }
  if (g_panel_measured && !servo::moving() && servo::position_known() && fabsf(measured - servo::angle()) > cfg::kPotSlipDeg) {
    latch_fault(8, "panel angle differs from the command (clutch slip or jam)");
  }
  // Fault 5: the arc gives nonsense for a while although it is lit -
  // saturated channels (10 s) or readings that fit no light direction (30 s).
  const bool saturated_now = g_sun.saturated && !g_sun.valid;
  if (saturated_now && !g_saturated) g_saturated_since = now;
  g_saturated = saturated_now;
  const bool misfit_now = g_sun.poor_fit;
  if (misfit_now && !g_misfit) g_misfit_since = now;
  g_misfit = misfit_now;
  const bool fault5 = (g_saturated && now - g_saturated_since >= kSaturatedFaultMs) || (g_misfit && now - g_misfit_since >= cfg::kSunFaultMs);
  if (g_short_fault != 0 && now - g_short_since >= cfg::kFaultShowMs) g_short_fault = 0;
  g_fault_now = g_latched_fault != 0 ? g_latched_fault : (g_short_fault != 0 ? g_short_fault : (fault5 ? 5 : 0));

  // 3. Servo test sequence (manual steps, kTestHoldMs at each)
  if (g_test && !servo::moving() && (g_test_step_now || now - g_test_step_ms >= kTestHoldMs)) {
    g_test_step_now = false;
    if (g_test_idx >= kTestCount) {
      g_test = false;
      Serial.printf("servo test done: manual hold at 0 deg (`servo auto` to track again)\n");
    } else {
      manual_move(kTestAngles[g_test_idx]);
      ++g_test_idx;
      g_test_step_ms = now;
    }
  }

  // 4. Decide
  ast::TrackerInput in;
  in.now_ms = now;
  in.sun_valid = g_sun.valid;
  in.sun_dark = g_sun.dark;
  in.sun_deg = sun_abs;
  in.panel_deg = g_panel;
  in.servo_busy = servo::moving();
  in.fault = g_fault_now;
  g_dec = g_tracker->step(in);
  if (g_dec.move) {
    bool clamped = false;
    const float from = servo::angle();
    if (servo::move_to(g_dec.target_deg, &clamped)) g_move_deg = fabsf(servo::target() - from);
    if (clamped) {
      g_short_fault = 7;
      g_short_since = now;
      transmission::log("servo: %.1f deg is outside +/-%.0f deg, clamped (fault 7)", static_cast<double>(g_dec.target_deg),
                        static_cast<double>(cfg::kServoLimitDeg));
    }
  }

  // 5. Tell the data link
  transmission::Report r;
  r.panel_deg = g_panel;
  r.sun_deg = g_dec.sun_deg;
  r.sun_valid = g_dec.sun_valid;
  r.state = to_report(g_dec.state);
  r.servo_moving = servo::moving();
  r.fault = g_fault_now;
  transmission::report(r);

  if (g_live && now - g_last_live >= cfg::kLivePeriodMs) {
    g_last_live = now;
    print_live();
  }
}

}  // namespace control
