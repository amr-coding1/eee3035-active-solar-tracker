// Firmware simulator (contract 7.5): sun model, clouds, tracker, own use, lamp.
#include "ast_sim.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ast;

namespace {
const int64_t kJune21 = 1782043320 - 12 * 3600 - 120;   // 2026-06-21 00:00:00 UTC
const int64_t kNov20 = 1795132800;                      // 2026-11-20 00:00:00 UTC

struct DayStats {
  double e_trk = 0, e_fix = 0, e_use = 0;   // mWh
  double use_sum = 0, use_quiet_min = 1e9, use_quiet_max = 0;
  int samples = 0, motor_seconds = 0, moves = 0, trk_seconds = 0, sun_valid_seconds = 0;
  double max_abs_err_trk = 0, min_angle = 1e9, max_angle = -1e9;
  bool all_simulated = true;
};

DayStats run_day(Scenario sc, int64_t start, int64_t seconds = 86400, uint32_t seed = 1) {
  SimConfig c;
  c.scenario = sc;
  c.seed = seed;
  Simulator sim(c);
  DayStats d;
  bool was_moving = false;
  for (int64_t t = start; t < start + seconds; ++t) {
    const Sample1Hz s = sim.step(t);
    d.all_simulated = d.all_simulated && s.simulated;
    d.e_trk += s.p_trk_mw / 3600.0;
    d.e_fix += s.p_fix_mw / 3600.0;
    d.e_use += s.p_use_mw / 3600.0;
    d.use_sum += s.p_use_mw;
    ++d.samples;
    if (s.motor_running) {
      ++d.motor_seconds;
      if (!was_moving) ++d.moves;
    } else {
      if (s.p_use_mw < d.use_quiet_min) d.use_quiet_min = s.p_use_mw;
      if (s.p_use_mw > d.use_quiet_max) d.use_quiet_max = s.p_use_mw;
    }
    was_moving = s.motor_running;
    if (s.ang_sun_valid) ++d.sun_valid_seconds;
    if (s.state == State::TRK) {
      ++d.trk_seconds;
      const double err = fabs(static_cast<double>(s.ang_sun_deg - s.ang_panel_deg));
      if (err > d.max_abs_err_trk) d.max_abs_err_trk = err;
    }
    if (s.ang_panel_deg < d.min_angle) d.min_angle = s.ang_panel_deg;
    if (s.ang_panel_deg > d.max_angle) d.max_angle = s.ang_panel_deg;
  }
  return d;
}
}  // namespace

TEST(sim_sun_position_guildford_midsummer_noon) {
  // 21 June, local solar noon is about 12:04 UTC: elevation = 90 - 51.24 + 23.44.
  const SunPosition p = sun_position(1782043320, 51.24, -0.57);
  CHECK_NEAR(p.elevation_deg, 62.2, 0.3);
  CHECK_NEAR(p.azimuth_deg, 180.0, 2.0);
}

TEST(sim_sun_position_guildford_november) {
  const SunPosition noon = sun_position(1795176000, 51.24, -0.57);     // 2026-11-20 12:00 UTC
  CHECK_NEAR(noon.elevation_deg, 19.0, 0.6);
  CHECK_NEAR(noon.azimuth_deg, 183.0, 4.0);
  const SunPosition midnight = sun_position(kNov20, 51.24, -0.57);
  CHECK(midnight.elevation_deg < -50.0);
  const SunPosition morning = sun_position(kNov20 + 9 * 3600, 51.24, -0.57);
  const SunPosition evening = sun_position(kNov20 + 15 * 3600, 51.24, -0.57);
  CHECK(morning.elevation_deg > 0 && morning.azimuth_deg > 100 && morning.azimuth_deg < 170);
  CHECK(evening.elevation_deg > 0 && evening.azimuth_deg > 200 && evening.azimuth_deg < 260);
}

TEST(sim_sun_rise_and_set_times_are_plausible) {
  // Guildford 20 Nov 2026: sunrise about 07:25, sunset about 16:05 UTC.
  int64_t rise = 0, set = 0;
  for (int64_t t = kNov20; t < kNov20 + 86400; t += 60) {
    const double el = sun_position(t, 51.24, -0.57).elevation_deg;
    if (rise == 0 && el > -0.83) rise = t;
    if (rise != 0 && set == 0 && el < -0.83) set = t;
  }
  CHECK_NEAR(static_cast<double>(rise - kNov20) / 3600.0, 7.42, 0.2);
  CHECK_NEAR(static_cast<double>(set - kNov20) / 3600.0, 16.1, 0.2);
}

TEST(sim_axis_angle_east_negative_west_positive) {
  const SunPosition morning = sun_position(kJune21 + 8 * 3600, 51.24, -0.57);
  const SunPosition noon = sun_position(1782043320, 51.24, -0.57);
  const SunPosition evening = sun_position(kJune21 + 16 * 3600, 51.24, -0.57);
  CHECK(sun_axis_angle_deg(morning, 35.0) < -30.0);
  CHECK_NEAR(sun_axis_angle_deg(noon, 35.0), 0.0, 2.0);
  CHECK(sun_axis_angle_deg(evening, 35.0) > 30.0);
}

TEST(sim_every_sample_is_marked_simulated_in_every_scenario) {
  for (Scenario sc : {Scenario::kClear, Scenario::kCloudy, Scenario::kOvercast, Scenario::kNight, Scenario::kLamp}) {
    const DayStats d = run_day(sc, kNov20 + 6 * 3600, 6 * 3600);
    CHECK(d.all_simulated);
  }
}

TEST(sim_clear_day_tracker_beats_fixed_panel_net_of_own_use_in_summer) {
  const DayStats d = run_day(Scenario::kClear, kJune21);
  CHECK(d.e_trk > d.e_fix * 1.15);          // tracking gain is substantial on a clear June day
  CHECK(d.e_trk < d.e_fix * 1.8);           // and physically plausible
  CHECK(d.e_trk - d.e_fix > d.e_use);       // net gain positive
  CHECK(d.e_fix > 5000.0 && d.e_fix < 20000.0);   // 2 W panel: several Wh on a clear summer day
}

TEST(sim_own_use_power_is_realistic) {
  const DayStats d = run_day(Scenario::kClear, kJune21);
  const double mean = d.use_sum / d.samples;
  CHECK(mean > 100.0 && mean < 150.0);      // ESP32-S3 + sensors + driver, motor bursts included
  CHECK(d.use_quiet_min >= 95.0);           // motor off: stays near 100-150 mW
  CHECK(d.use_quiet_max <= 155.0);
  CHECK(d.motor_seconds > 0);
  CHECK(d.motor_seconds < 600);             // the motor runs for seconds, not minutes
}

TEST(sim_tracker_moves_about_every_20_minutes_in_daylight) {
  // 21 June 09:00-15:00 UTC: 6 h of clear sun inside the mechanical travel.
  const DayStats d = run_day(Scenario::kClear, kJune21 + 9 * 3600, 6 * 3600);
  CHECK(d.moves >= 15 && d.moves <= 22);    // 18 would be exactly every 20 min
  CHECK(d.trk_seconds > 5 * 3600 + 3000);
}

TEST(sim_pointing_error_stays_within_deadband_while_tracking) {
  const DayStats d = run_day(Scenario::kClear, kJune21 + 9 * 3600, 6 * 3600);
  CHECK(d.max_abs_err_trk <= 2.5 + 0.4);    // dead-band + sensor noise + one second of sun travel
  CHECK(d.max_abs_err_trk > 1.5);           // it really does wait for the dead-band
}

TEST(sim_panel_angle_respects_mechanical_limits) {
  const DayStats d = run_day(Scenario::kClear, kJune21);
  CHECK(d.min_angle >= -60.0);
  CHECK(d.max_angle <= 60.0);
  CHECK(d.min_angle < -50.0);               // uses most of the travel in midsummer
  CHECK(d.max_angle > 50.0);
}

TEST(sim_night_scenario_is_dark_and_idle) {
  SimConfig c;
  c.scenario = Scenario::kNight;
  Simulator sim(c);
  for (int64_t t = kJune21 + 10 * 3600; t < kJune21 + 11 * 3600; ++t) {
    const Sample1Hz s = sim.step(t);
    CHECK_EQ(s.p_trk_mw, 0.0f);
    CHECK_EQ(s.p_fix_mw, 0.0f);
    CHECK(!s.ang_sun_valid);
    CHECK(s.state == State::IDL);
    CHECK(s.p_use_mw > 95.0f && s.p_use_mw < 155.0f);
    CHECK(s.simulated);
  }
}

TEST(sim_real_night_parks_level_and_goes_idle) {
  SimConfig c;
  Simulator sim(c);
  Sample1Hz s;
  for (int64_t t = kJune21 + 18 * 3600; t < kJune21 + 23 * 3600; ++t) s = sim.step(t);
  CHECK(s.state == State::IDL);
  CHECK_NEAR(s.ang_panel_deg, 0.0, 0.01);
  CHECK(!s.ang_sun_valid);
  CHECK_NEAR(s.p_trk_mw, 0.0, 0.001);
}

TEST(sim_overcast_holds_position_and_gains_nothing) {
  const DayStats d = run_day(Scenario::kOvercast, kJune21 + 8 * 3600, 6 * 3600);
  CHECK_EQ(d.sun_valid_seconds, 0);         // the sensor arc cannot find the sun
  CHECK_EQ(d.trk_seconds, 0);
  CHECK(d.e_trk > 0.0);
  CHECK_NEAR(d.e_trk / d.e_fix, 1.0, 0.1);  // diffuse light: both panels the same
  SimConfig c;
  c.scenario = Scenario::kOvercast;
  Simulator sim(c);
  Sample1Hz s;
  for (int64_t t = kJune21 + 12 * 3600; t < kJune21 + 12 * 3600 + 100; ++t) s = sim.step(t);
  CHECK(s.state == State::HLD);
}

TEST(sim_cloudy_day_has_sun_and_cloud_and_is_reproducible) {
  SimConfig c;
  c.scenario = Scenario::kCloudy;
  c.seed = 7;
  Simulator a(c), b(c);
  double lo = 1.0, hi = 0.0;
  for (int64_t t = kJune21 + 6 * 3600; t < kJune21 + 18 * 3600; t += 30) {
    const double f = a.beam_fraction(t);
    lo = f < lo ? f : lo;
    hi = f > hi ? f : hi;
    CHECK(f >= 0.0 && f <= 1.0);
  }
  CHECK(lo < 0.15);
  CHECK(hi > 0.9);
  bool differs_from_other_seed = false;
  c.seed = 8;
  Simulator other(c);
  for (int64_t t = kJune21 + 10 * 3600; t < kJune21 + 11 * 3600; ++t) {
    const Sample1Hz x = a.step(t), y = b.step(t), z = other.step(t);
    CHECK_EQ(x.p_trk_mw, y.p_trk_mw);       // same seed, same numbers (on every platform)
    CHECK_EQ(x.ang_panel_deg, y.ang_panel_deg);
    if (x.p_trk_mw != z.p_trk_mw) differs_from_other_seed = true;
  }
  CHECK(differs_from_other_seed);
  const DayStats cloudy = run_day(Scenario::kCloudy, kJune21, 86400, 7);
  const DayStats clear = run_day(Scenario::kClear, kJune21);
  CHECK(cloudy.e_fix < clear.e_fix * 0.9);
  CHECK(cloudy.e_fix > clear.e_fix * 0.2);
}

TEST(sim_lamp_moves_east_to_west_in_two_hours_then_goes_off) {
  SimConfig c;
  c.scenario = Scenario::kLamp;
  Simulator sim(c);
  const int64_t start = kNov20 + 9 * 3600;  // a multiple of 3 h: start of a sweep
  Sample1Hz first{}, mid{}, last{}, off{};
  double e_trk = 0, e_fix = 0;
  int moves = 0;
  bool was = false;
  for (int64_t t = start; t < start + 3 * 3600; ++t) {
    const Sample1Hz s = sim.step(t);
    if (t == start + 5) first = s;
    if (t == start + 3600) mid = s;
    if (t == start + 7190) last = s;
    if (t == start + 9000) off = s;
    e_trk += s.p_trk_mw;
    e_fix += s.p_fix_mw;
    if (s.motor_running && !was) ++moves;
    was = s.motor_running;
    CHECK(s.simulated);
  }
  CHECK(first.ang_sun_valid && mid.ang_sun_valid && last.ang_sun_valid);
  CHECK_NEAR(first.ang_sun_deg, -60.0, 1.0);
  CHECK_NEAR(mid.ang_sun_deg, 0.0, 1.0);
  CHECK_NEAR(last.ang_sun_deg, 60.0, 1.0);
  CHECK_NEAR(last.ang_panel_deg, 60.0, 4.0);    // the tracker followed it
  CHECK(!off.ang_sun_valid);
  CHECK(off.state == State::IDL);
  CHECK(off.p_trk_mw < 10.0f);
  CHECK(e_trk > e_fix * 1.1);
  CHECK(mid.p_trk_mw > 200.0f && mid.p_trk_mw < 600.0f);   // indoor lamp: fraction of a watt
  CHECK(moves >= 22 && moves <= 30);        // 120 deg in 4.6 deg steps, plus parking and the return
}

TEST(sim_scenario_names_round_trip) {
  for (Scenario sc : {Scenario::kClear, Scenario::kCloudy, Scenario::kOvercast, Scenario::kNight, Scenario::kLamp}) {
    Scenario back;
    CHECK(scenario_from_name(scenario_name(sc), &back));
    CHECK(back == sc);
  }
  Scenario x;
  CHECK(!scenario_from_name("sunny", &x));
  CHECK(!scenario_from_name(nullptr, &x));
}

TEST(sim_hash_noise_is_uniform_enough_and_deterministic) {
  double sum = 0;
  for (int i = 0; i < 10000; ++i) {
    const double v = Simulator::hash01(1, i, 2);
    CHECK(v >= 0.0 && v < 1.0);
    sum += v;
  }
  CHECK_NEAR(sum / 10000.0, 0.5, 0.02);
  CHECK_EQ(Simulator::hash01(5, 123456789, 3), Simulator::hash01(5, 123456789, 3));
  CHECK(Simulator::hash01(5, 123456789, 3) != Simulator::hash01(6, 123456789, 3));
}
