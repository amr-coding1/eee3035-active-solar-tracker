// Closed-loop "digital twin" of the tracker on the PC (19 Nov 2026): a lamp
// sweeps across the arc, four photodiodes see it (cosine response, 1 % gain
// mismatch, noise), the sun estimate drives the tracking decisions, the servo
// sequencer moves the panel, and the control code's reports go through the
// data link's telemetry and minute aggregator into channel-1 JSON - the same
// path as tracker/control.cpp -> transmission -> ThingSpeak. Then the
// lamp goes off and the panel must stow.
#include <math.h>

#include <string>
#include <vector>

#include "ast_aggregator.h"
#include "ast_json.h"
#include "ast_servo.h"
#include "ast_status.h"
#include "ast_sun.h"
#include "ast_telemetry.h"
#include "ast_tracker.h"
#include "test_framework.h"

using namespace ast;

namespace {

constexpr double kPiD = 3.14159265358979323846;

struct Twin {
  SunArcConfig arc;
  ServoConfig servo_cfg;
  TrackerConfig trk_cfg;
  ServoSequencer servo{ServoConfig(), -45.0f};
  Tracker tracker{TrackerConfig()};
  StallGuard stall{StallConfig()};
  Telemetry tel;
  Aggregator agg;
  std::vector<Record> records;
  double gains[4] = {1.008, 0.993, 1.004, 0.995};   // 1 % mismatch after `sun cal flat`
  unsigned seed = 2026;
  uint32_t moves_requested = 0;
  double trk_mwh = 0, fix_mwh = 0;
  int64_t t0_unix = 1795046400;   // 2026-11-19 00:00:00 UTC

  Twin() {
    trk_cfg.dark_to_stow_ms = 120000;   // lamp tests: 2 min of darkness -> stow
    tracker.set_config(trk_cfg);
    servo.set_config(servo_cfg);
  }

  double noise() {
    seed = seed * 1103515245u + 12345u;
    return static_cast<double>((seed >> 8) & 0xFFFF) / 32767.5 - 1.0;
  }

  void photodiodes(bool lamp_on, double lamp_deg, float* mv) {
    for (int i = 0; i < 4; ++i) {
      const double c = cos((lamp_deg - static_cast<double>(arc.sensor_deg[i])) * kPiD / 180.0);
      const double beam = lamp_on ? 1400.0 * (c > 0 ? c : 0.0) : 0.0;
      mv[i] = static_cast<float>(gains[i] * (beam + (lamp_on ? 60.0 : 4.0)) + 4.0 * noise());
    }
  }

  // Runs the twin from t_start to t_end (ms); lamp(t) gives on/off and angle.
  template <typename Lamp>
  void run(uint32_t t_start, uint32_t t_end, Lamp lamp) {
    for (uint32_t t = t_start; t < t_end; t += 10) {
      const ServoSequencer::Output o = servo.tick(t);
      const float panel = servo.position_deg();
      bool on = false;
      double lamp_deg = 0;
      lamp(t, &on, &lamp_deg);
      if (t % 200 == 0) {   // control step, 5 Hz
        float mv[4];
        photodiodes(on, lamp_deg, mv);
        const SunEstimate e = estimate_sun(mv, arc);
        TrackerInput in;
        in.now_ms = t;
        in.sun_valid = e.valid;
        in.sun_dark = e.dark;
        in.sun_deg = e.sun_deg;
        in.panel_deg = panel;
        in.servo_busy = servo.busy();
        const TrackerDecision d = tracker.step(in);
        if (d.move) {
          servo.command(d.target_deg, t, nullptr);
          ++moves_requested;
        }
        tel.set_angles(t, panel, d.sun_deg, d.sun_valid);
        tel.set_state(t, d.state);
        tel.set_motor_running(t, servo.busy());
        tel.set_fault(t, 0);
      }
      if (t % 1000 == 0) {   // data-link sampler, 1 Hz
        const TelemetrySnapshot snap = tel.take(t);
        const double pt = on ? 2000.0 * fmax(0.0, cos((lamp_deg - panel) * kPiD / 180.0)) + 40.0 : 0.0;
        const double pf = on ? 2000.0 * fmax(0.0, cos(lamp_deg * kPiD / 180.0)) + 40.0 : 0.0;
        trk_mwh += pt / 3600.0;
        fix_mwh += pf / 3600.0;
        Sample1Hz s;
        s.p_trk_valid = s.p_fix_valid = s.p_use_valid = s.vsup_valid = true;
        s.p_trk_mw = static_cast<float>(pt);
        s.p_fix_mw = static_cast<float>(pf);
        s.p_use_mw = 130.0f + (o.power ? 600.0f : 0.0f);
        s.vsup_v = 5.02f;
        s.ang_panel_valid = snap.panel_valid;
        s.ang_panel_deg = snap.panel_deg;
        s.ang_sun_valid = snap.sun_valid;
        s.ang_sun_deg = snap.sun_deg;
        s.state = snap.state;
        s.motor_running = snap.motor_ran;
        s.ctrl_fault = snap.fault;
        Record r;
        if (agg.add(t0_unix + t / 1000, s, &r)) records.push_back(r);
      }
    }
  }
};

}  // namespace

TEST(closed_loop_twin_tracks_a_moving_lamp_then_stows_in_the_dark) {
  Twin w;
  // 0-2 h: the lamp sweeps -60 -> +60 deg at 1 deg/min (four times the sun's
  // pace, to make the tracker work), then 10 min off, then on again at -30.
  const uint32_t sweep_ms = 120u * 60000u;
  auto lamp = [&](uint32_t t, bool* on, double* deg) {
    if (t < sweep_ms) {
      *on = true;
      *deg = -60.0 + 120.0 * static_cast<double>(t) / sweep_ms;
    } else if (t < sweep_ms + 600000u) {
      *on = false;
      *deg = 0;
    } else {
      *on = true;
      *deg = -30.0;
    }
  };
  w.run(0, sweep_ms + 600000u + 120000u, lamp);

  // ---- pointing while the lamp is inside the +/-45 deg range ----------------------
  double sum = 0, worst = 0;
  int n = 0;
  int mov = 0, trk = 0, idl = 0, stw = 0, flt = 0;
  for (const Record& r : w.records) {
    const int64_t m = static_cast<int64_t>(r.minute_unix) - w.t0_unix;   // seconds since start
    if (r.state == State::MOV) ++mov;
    if (r.state == State::TRK) ++trk;
    if (r.state == State::IDL) ++idl;
    if (r.state == State::STW) ++stw;
    if (r.state == State::FLT) ++flt;
    const double lamp_end = -60.0 + 120.0 * static_cast<double>(m + 60) / (sweep_ms / 1000.0);
    if (m + 60 <= static_cast<int64_t>(sweep_ms / 1000) && fabs(lamp_end) < 43.0 && r.has(kHasAngPanel)) {
      const double err = fabs(lamp_end - r.ang_panel_ddeg / 10.0);
      sum += err;
      worst = err > worst ? err : worst;
      ++n;
    }
  }
  CHECK(n > 60);
  CHECK(sum / n < 4.0);     // deadband 5 deg: the panel lags the lamp by 0-5 deg
  CHECK(worst < 8.0);
  // ---- moves, energy, states --------------------------------------------------------
  // 105 deg of travel inside the range at 5 deg per move -> about 21 moves.
  CHECK(w.moves_requested >= 15u && w.moves_requested <= 35u);
  CHECK(w.servo.powered_ms() < 60000u);   // the servo is powered well under a minute in 2 h
  CHECK(mov >= 15);                       // the minutes with a move show MOV
  CHECK(trk > 60);
  CHECK(idl >= 1);                        // dark, waiting to stow
  CHECK(stw >= 5);                        // then stowed
  CHECK_EQ(flt, 0);
  // Stowed at -45 at the end of the dark spell, then back to the lamp at -30.
  bool saw_stow_angle = false;
  for (const Record& r : w.records) {
    if (r.state == State::STW && r.has(kHasAngPanel) && fabs(r.ang_panel_ddeg / 10.0 + 45.0) < 0.6) saw_stow_angle = true;
  }
  CHECK(saw_stow_angle);
  CHECK_NEAR(w.servo.position_deg(), -30.0, 3.0);
  // ---- energy: the tracked panel collects more than the fixed one -----------------------
  // Geometry over this sweep: about +18 % (cos-weighted mean 0.98 vs 0.83).
  CHECK(w.trk_mwh > 1.12 * w.fix_mwh);
  CHECK(w.trk_mwh < 1.30 * w.fix_mwh);
  // ---- what goes to ThingSpeak: valid channel-1 entries with the right status --------
  char json[kJsonEntryMax];
  int movs_in_json = 0;
  for (const Record& r : w.records) {
    const size_t len = format_entry(r, json, sizeof json);
    CHECK(len > 0);
    const std::string e(json);
    CHECK(e.find("\"field4\":") != std::string::npos);   // panel angle always present
    if (e.find("\"status\":\"MOV;") != std::string::npos) ++movs_in_json;
  }
  CHECK_EQ(movs_in_json, mov);
}

TEST(closed_loop_twin_stall_guard_stops_a_jammed_servo) {
  // The panel is jammed: during the move P_use sits at the stall level. The
  // guard must trip within its window, and the servo be switched off.
  ServoConfig sc;
  sc.slew_deg_per_s = 5.0f;   // a long move (40 deg = 8 s) so the stall can show
  ServoSequencer servo(sc, -20.0f);
  StallGuard guard{StallConfig()};
  servo.tick(0);
  servo.command(20.0f, 0, nullptr);
  bool tripped = false;
  uint32_t trip_ms = 0;
  for (uint32_t t = 0; t < 10000 && !tripped; t += 10) {
    const ServoSequencer::Output o = servo.tick(t);
    const float p_use = o.power ? 7000.0f : 130.0f;   // 1.4 A at 5 V while powered
    if (t % 1000 == 0 && guard.update(t, servo.busy(), true, p_use)) {
      servo.stop(t);
      tripped = true;
      trip_ms = t;
    }
  }
  CHECK(tripped);
  CHECK(trip_ms >= 2500u && trip_ms <= 4000u);
  CHECK(!servo.powered());
  CHECK(!servo.position_known());
}
