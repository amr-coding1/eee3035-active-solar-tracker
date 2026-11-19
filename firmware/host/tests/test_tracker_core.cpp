// Tracker firmware core (19 Nov 2026): INA219 support, sun estimate from the
// photodiode arc, DS-R001 servo sequence, tracking decisions, stall guard.
#include <math.h>

#include "ast_cal.h"
#include "ast_servo.h"
#include "ast_sun.h"
#include "ast_tracker.h"
#include "test_framework.h"

using namespace ast;

// ---- INA219 and chip identification ---------------------------------------------

TEST(ina219_bus_and_current_conversion) {
  InaChannelCal cal;   // 0.1 ohm, no correction
  // Bus register: 9.000 V / 4 mV = 2250 -> << 3, with the CNVR flag (bit 1) set.
  const uint16_t bus = static_cast<uint16_t>((2250u << 3) | 0x2u);
  // Shunt: 45.00 mV = 4500 x 10 uV -> 450 mA through 0.1 ohm.
  const InaReading r = ina219_convert(bus, 4500, cal);
  CHECK_NEAR(r.raw_volts, 9.0, 1e-6);
  CHECK_NEAR(r.raw_milliamps, 450.0, 1e-3);
  CHECK_NEAR(r.power_mw, 4050.0, 0.01);
  CHECK(!ina219_overflow(bus));
  CHECK(ina219_overflow(static_cast<uint16_t>(bus | 0x1u)));
  // Negative shunt voltage (reverse current) stays negative; a 0.01 ohm shunt scales by 10.
  cal.scale.shunt_ohm = 0.01f;
  CHECK_NEAR(ina219_convert(bus, -100, cal).raw_milliamps, -100.0, 1e-3);
  // Two-point corrections are applied after the conversion, like the INA226.
  cal.scale.shunt_ohm = 0.1f;
  cal.volts.gain = 1.01f;
  cal.milliamps.offset = -2.0f;
  const InaReading c = ina219_convert(bus, 4500, cal);
  CHECK_NEAR(c.volts, 9.09, 1e-4);
  CHECK_NEAR(c.milliamps, 448.0, 1e-3);
  // No shunt value: no current rather than a division by zero.
  cal.scale.shunt_ohm = 0.0f;
  CHECK_NEAR(ina219_convert(bus, 4500, cal).raw_milliamps, 0.0, 1e-9);
}

TEST(ina_identify_tells_the_chips_apart) {
  CHECK(ina_identify(true, 0x5449, true, 0x2260, false, 0) == InaChip::kIna226);
  CHECK(ina_identify(true, 0x5449, false, 0, false, 0) == InaChip::kIna226);   // die id unreadable: still an INA226
  CHECK(ina_identify(true, 0x5449, true, 0x3220, true, 0x7127) == InaChip::kNone);   // INA3221: refused
  CHECK(ina_identify(true, 0x5449, true, 0x2270, false, 0) == InaChip::kNone);        // INA260: refused
  CHECK(ina_identify(true, 0x5449, true, 0x2261, false, 0) == InaChip::kIna226);      // INA226, another die revision
  CHECK(ina_identify(false, 0, false, 0, true, 0x399F) == InaChip::kIna219);
  CHECK(ina_identify(true, 0x0000, true, 0x0000, true, 0x399F) == InaChip::kIna219);  // INA219 answers 0xFE with junk
  CHECK(ina_identify(true, 0x1234, true, 0, true, 0x1234) == InaChip::kNone);      // something else at that address
  CHECK(ina_identify(false, 0, false, 0, false, 0) == InaChip::kNone);             // nothing there
  CHECK_STR(ina_chip_name(InaChip::kIna219), "INA219");
  CHECK_STR(ina_chip_name(InaChip::kIna226), "INA226");
  CHECK_STR(ina_chip_name(InaChip::kNone), "none");
}

// ---- sun estimate ---------------------------------------------------------------

// Synthetic arc: each sensor sees A*max(0, cos(theta - phi)) + B.
static void synth(const SunArcConfig& cfg, double theta, double amp, double ambient, float* out) {
  for (int i = 0; i < cfg.n; ++i) {
    const double c = cos((theta - static_cast<double>(cfg.sensor_deg[i])) * 3.14159265358979 / 180.0);
    out[i] = static_cast<float>(amp * (c > 0 ? c : 0.0) + ambient);
  }
}

TEST(sun_cosine_fit_recovers_the_angle_across_the_field_of_view) {
  SunArcConfig cfg;   // 4 sensors at -45, -15, 15, 45
  float mv[4];
  double worst = 0;
  for (int t = -65; t <= 65; t += 1) {
    synth(cfg, t, 1500.0, 80.0, mv);
    const SunEstimate e = estimate_sun(mv, cfg);
    CHECK(e.valid);
    CHECK(!e.dark && !e.saturated);
    const double err = fabs(static_cast<double>(e.sun_deg) - t);
    worst = err > worst ? err : worst;
  }
  CHECK(worst < 0.05);   // exact model: only rounding
}

TEST(sun_cosine_fit_is_robust_to_noise_and_small_mismatch) {
  // After `sun cal flat` the channels match to about 1 %; add 5 mV of noise.
  SunArcConfig cfg;
  float mv[4];
  unsigned seed = 12345;
  auto rnd = [&]() {   // uniform -1..1, deterministic
    seed = seed * 1103515245u + 12345u;
    return static_cast<double>((seed >> 8) & 0xFFFF) / 32767.5 - 1.0;
  };
  double worst = 0, sum = 0;
  int count = 0;
  for (int rep = 0; rep < 50; ++rep) {
    double g[4];
    for (double& v : g) v = 1.0 + 0.01 * rnd();
    for (int t = -45; t <= 45; t += 3) {
      synth(cfg, t, 1200.0, 60.0, mv);
      for (int i = 0; i < 4; ++i) mv[i] = static_cast<float>(mv[i] * g[i] + 5.0 * rnd());
      const SunEstimate e = estimate_sun(mv, cfg);
      CHECK(e.valid);
      const double err = fabs(static_cast<double>(e.sun_deg) - t);
      worst = err > worst ? err : worst;
      sum += err;
      ++count;
    }
  }
  CHECK(sum / count < 1.0);   // typical error well inside the 5 deg deadband
  CHECK(worst < 5.0);         // even the worst case at the ends of the arc
}

TEST(sun_dark_saturated_and_out_of_view) {
  SunArcConfig cfg;
  float mv[4] = {5, 8, 6, 7};
  SunEstimate e = estimate_sun(mv, cfg);
  CHECK(e.dark && !e.valid);
  CHECK_STR(e.why, "dark");

  // One channel clipped by the ADC (3.1 V): it is left out and the other
  // three still give the angle.
  synth(cfg, 25.0, 3100.0, 50.0, mv);
  for (float& v : mv) v = v > 3100.0f ? 3100.0f : v;
  CHECK(mv[2] >= 3000.0f && mv[3] < 3000.0f);
  e = estimate_sun(mv, cfg);
  CHECK(e.saturated);
  CHECK(e.valid);
  CHECK_NEAR(e.sun_deg, 25.0, 0.05);

  // Two channels clipped: the peak is hidden; refuse and say what to change.
  synth(cfg, 30.0, 3400.0, 50.0, mv);
  for (float& v : mv) v = v > 3100.0f ? 3100.0f : v;
  e = estimate_sun(mv, cfg);
  CHECK(!e.valid && e.saturated);
  CHECK_STR(e.why, "saturated: lower the photodiode load resistor");

  // Light far beyond the arc's view (80 deg east): not trusted.
  synth(cfg, -85.0, 1500.0, 30.0, mv);
  e = estimate_sun(mv, cfg);
  CHECK(!e.valid);
  CHECK(!e.dark);

  // A reading that is not a number is refused.
  synth(cfg, 0.0, 1500.0, 30.0, mv);
  mv[2] = NAN;
  e = estimate_sun(mv, cfg);
  CHECK(!e.valid);
}

TEST(sun_gain_and_offset_corrections_are_applied) {
  SunArcConfig cfg;
  float mv[4];
  synth(cfg, 20.0, 1000.0, 50.0, mv);
  // Channel 1 reads 20 % high with a 30 mV offset; correct it in the config.
  mv[1] = mv[1] * 1.2f + 30.0f;
  SunEstimate raw = estimate_sun(mv, cfg);
  cfg.gain[1] = 1.0f / 1.2f;
  cfg.dark_offset_mv[1] = 30.0f;
  SunEstimate fixed = estimate_sun(mv, cfg);
  CHECK(fixed.valid);
  CHECK_NEAR(fixed.sun_deg, 20.0, 0.05);
  CHECK(fabs(static_cast<double>(raw.sun_deg) - 20.0) > fabs(static_cast<double>(fixed.sun_deg) - 20.0));
}

TEST(sun_centroid_method_and_angle_diff) {
  SunArcConfig cfg;
  cfg.method = SunMethod::kCentroid;
  float mv[4];
  float prev = -1000.0f;
  for (int t = -40; t <= 40; t += 5) {   // monotonic, and the sign is right
    synth(cfg, t, 1500.0, 40.0, mv);
    const SunEstimate e = estimate_sun(mv, cfg);
    CHECK(e.valid);
    CHECK(e.sun_deg > prev);
    prev = e.sun_deg;
  }
  synth(cfg, 0.0, 1500.0, 40.0, mv);
  CHECK_NEAR(estimate_sun(mv, cfg).sun_deg, 0.0, 1e-3);
  CHECK_NEAR(angle_diff_deg(170.0f, -170.0f), -20.0, 1e-4);
  CHECK_NEAR(angle_diff_deg(-170.0f, 170.0f), 20.0, 1e-4);
  CHECK_NEAR(angle_diff_deg(10.0f, 370.0f), 0.0, 1e-4);
}

TEST(sun_diffuse_light_gives_no_direction) {
  // Overcast or room light: every channel about the same. Noise must not be
  // read as a direction (the panel would hunt back and forth).
  SunArcConfig cfg;
  float mv[4];
  unsigned seed = 99;
  auto rnd = [&]() {
    seed = seed * 1103515245u + 12345u;
    return static_cast<double>((seed >> 8) & 0xFFFF) / 32767.5 - 1.0;
  };
  int valid = 0;
  for (int k = 0; k < 500; ++k) {
    for (float& v : mv) v = static_cast<float>(500.0 + 3.0 * rnd());
    const SunEstimate e = estimate_sun(mv, cfg);
    if (e.valid) ++valid;
    CHECK(e.valid || e.diffuse);
  }
  CHECK_EQ(valid, 0);
  // A weak beam on a bright background: contrast 0.08 is diffuse, 0.2 is a direction.
  synth(cfg, 20.0, 50.0, 600.0, mv);
  CHECK(estimate_sun(mv, cfg).diffuse);
  synth(cfg, 20.0, 150.0, 600.0, mv);
  SunEstimate e = estimate_sun(mv, cfg);
  CHECK(e.valid);
  CHECK_NEAR(e.sun_deg, 20.0, 0.1);
  CHECK_NEAR(e.contrast, 0.2, 0.01);
}

TEST(sun_dead_photodiode_is_flagged_not_trusted) {
  SunArcConfig cfg;
  float mv[4];
  // Outer east channel dead (reads 0): the light at -40 deg would otherwise
  // be reported near 0 deg.
  for (int t = -40; t <= 20; t += 10) {
    synth(cfg, t, 1500.0, 80.0, mv);
    mv[0] = 0.0f;
    const SunEstimate e = estimate_sun(mv, cfg);
    CHECK(!e.valid);
    CHECK(e.poor_fit);
  }
  // Inner channel dead: never a valid direction.
  for (int t = -40; t <= 40; t += 20) {
    synth(cfg, t, 1500.0, 80.0, mv);
    mv[1] = 0.0f;
    CHECK(!estimate_sun(mv, cfg).valid);
  }
}

TEST(sun_response_power_matches_narrower_photodiodes) {
  // A shroud or lens makes the response narrower than a cosine. With the
  // right k the fit is exact again; with the wrong one it says so.
  SunArcConfig cfg;
  float mv[4];
  auto narrow = [&](double t) {
    for (int i = 0; i < 4; ++i) {
      const double c = cos((t - static_cast<double>(cfg.sensor_deg[i])) * 3.14159265358979 / 180.0);
      mv[i] = static_cast<float>(1500.0 * (c > 0 ? c * c : 0.0) + 60.0);
    }
  };
  int flagged = 0;
  for (int t = -60; t <= 60; t += 5) {
    narrow(t);
    if (!estimate_sun(mv, cfg).valid) ++flagged;   // cos^1 model on cos^2 data
  }
  CHECK(flagged > 5);
  cfg.response_power = 2.0f;
  for (int t = -60; t <= 60; t += 5) {
    narrow(t);
    const SunEstimate e = estimate_sun(mv, cfg);
    CHECK(e.valid);
    CHECK_NEAR(e.sun_deg, t, 0.05);
  }
}

// ---- servo ----------------------------------------------------------------------

TEST(servo_pulse_mapping_limits_and_duty) {
  ServoConfig cfg;   // 500-2500 us over 180 deg, level = 90, west = higher pulse
  CHECK_EQ(servo_pulse_us(0.0f, cfg), 1500);
  CHECK_EQ(servo_pulse_us(45.0f, cfg), 2000);
  CHECK_EQ(servo_pulse_us(-45.0f, cfg), 1000);
  CHECK_EQ(servo_pulse_us(80.0f, cfg), 2056);    // clamped to +50 deg
  CHECK_EQ(servo_pulse_us(-80.0f, cfg), 944);
  bool clamped = false;
  CHECK_NEAR(servo_clamp(70.0f, cfg, &clamped), 50.0, 1e-6);
  CHECK(clamped);
  CHECK_NEAR(servo_clamp(-12.5f, cfg, &clamped), -12.5, 1e-6);
  CHECK(!clamped);
  CHECK_NEAR(servo_clamp(NAN, cfg, &clamped), 0.0, 1e-6);
  CHECK(clamped);
  cfg.west_is_higher_pulse = false;   // mounted the other way round
  CHECK_EQ(servo_pulse_us(45.0f, cfg), 1000);
  cfg.west_is_higher_pulse = true;
  cfg.trim_deg = 2.0f;                // inclinometer showed the panel 2 deg short
  CHECK_EQ(servo_pulse_us(0.0f, cfg), 1522);
  // The trim can never push the command past the limit (50 deg = 2056 us)...
  cfg.trim_deg = 5.0f;
  CHECK_EQ(servo_pulse_us(50.0f, cfg), 2056);
  CHECK_EQ(servo_pulse_us(-50.0f, cfg), 1000);   // -50 + 5 = -45
  // ... and a wild trim is limited to +/-10 deg.
  cfg.trim_deg = 40.0f;
  CHECK_EQ(servo_pulse_us(0.0f, cfg), 1611);     // 0 + 10 deg
  cfg.trim_deg = 0.0f;
  // 50 Hz, 14-bit LEDC: 500 us -> 410, 1500 us -> 1229, 2500 us -> 2048.
  CHECK_EQ(servo_duty(500, 20000, 14), 410u);
  CHECK_EQ(servo_duty(1500, 20000, 14), 1229u);
  CHECK_EQ(servo_duty(2500, 20000, 14), 2048u);
  CHECK_EQ(servo_duty(1500, 0, 14), 0u);
}

TEST(servo_sequence_powers_up_ramps_settles_and_switches_off) {
  ServoConfig cfg;   // gate on, 30 deg/s, power-up 150 ms, settle 400 ms
  ServoSequencer s(cfg, -45.0f);
  uint32_t t = 1000;
  ServoSequencer::Output o = s.tick(t);
  CHECK(!o.power && !o.pulses);
  CHECK(!s.busy());
  bool clamped = true;
  CHECK(s.command(-30.0f, t, &clamped));
  CHECK(!clamped);
  CHECK(s.busy());
  // Power-up: supply on, pulses at the OLD angle (no jump).
  o = s.tick(t + 10);
  CHECK(o.power && o.pulses);
  CHECK_EQ(o.pulse_us, servo_pulse_us(-45.0f, cfg));
  // Ramp: 15 deg at 30 deg/s = 500 ms; check it moves steadily.
  t += 150;
  o = s.tick(t);
  CHECK(s.phase() == ServoSequencer::Phase::kRamp);
  uint16_t last = o.pulse_us;
  for (int k = 0; k < 25; ++k) {
    t += 20;
    o = s.tick(t);
    CHECK(o.power && o.pulses);
    CHECK(o.pulse_us >= last);
    last = o.pulse_us;
  }
  CHECK(s.phase() == ServoSequencer::Phase::kSettle);
  CHECK_EQ(o.pulse_us, servo_pulse_us(-30.0f, cfg));
  CHECK_NEAR(s.position_deg(), -30.0, 1e-4);
  // Settle 400 ms; then the pulses stop while the supply stays on for 40 ms
  // (the last pulse must finish before the signal pin is released); then off.
  t += 399;
  o = s.tick(t);
  CHECK(o.power && o.pulses);
  t += 2;
  o = s.tick(t);
  CHECK(o.power && !o.pulses);
  CHECK(s.phase() == ServoSequencer::Phase::kStopping);
  CHECK(s.busy());
  t += 39;
  o = s.tick(t);
  CHECK(o.power && !o.pulses);
  t += 2;
  o = s.tick(t);
  CHECK(!o.power && !o.pulses);
  CHECK(!s.busy());
  CHECK_EQ(s.moves(), 1u);
  CHECK(s.position_known());
  // Same angle again: nothing to do.
  CHECK(!s.command(-30.0f, t, nullptr));
  // Powered time is booked (power-up + ramp + settle + stop, about 1.1 s).
  CHECK(s.powered_ms() > 1000u && s.powered_ms() < 1250u);
}

TEST(servo_unknown_start_goes_straight_to_target_and_late_ticks_do_not_jump) {
  ServoConfig cfg;
  // After a reset mid-move the angle is unknown: power up straight at the
  // target (one jump), not at a stale angle followed by a ramp (two).
  ServoSequencer s(cfg, -45.0f, false);
  CHECK(!s.position_known());
  s.tick(0);
  CHECK(s.command(10.0f, 0, nullptr));
  ServoSequencer::Output o = s.tick(10);
  CHECK(o.power && o.pulses);
  CHECK_EQ(o.pulse_us, servo_pulse_us(10.0f, cfg));
  // A blocked loop (2 s between calls) must not make the ramp jump 60 deg.
  ServoSequencer r(cfg, 0.0f);
  r.tick(0);
  r.command(45.0f, 0, nullptr);
  r.tick(200);    // power-up done
  r.tick(2200);   // 2 s late
  CHECK(r.position_deg() <= 1.6f);   // at most 50 ms of travel at 30 deg/s
}

TEST(servo_retarget_stop_and_no_switch_modes) {
  ServoConfig cfg;
  ServoSequencer s(cfg, 0.0f);
  uint32_t t = 0;
  s.tick(t);
  s.command(20.0f, t, nullptr);
  for (int k = 0; k < 20; ++k) { t += 20; s.tick(t); }   // 150 ms power-up + 250 ms of ramp
  CHECK(s.position_deg() > 1.0f && s.position_deg() < 20.0f);
  // New target during the ramp: turns round without powering down.
  CHECK(s.command(-10.0f, t, nullptr));
  for (int k = 0; k < 200 && s.busy(); ++k) { t += 20; s.tick(t); }
  CHECK(!s.busy());
  CHECK_NEAR(s.position_deg(), -10.0, 1e-4);
  CHECK_EQ(s.moves(), 1u);
  // Emergency stop: off at once, position no longer trusted.
  s.command(30.0f, t, nullptr);
  t += 200;
  s.tick(t);
  s.stop(t);
  CHECK(!s.busy() && !s.powered());
  CHECK(!s.position_known());
  CHECK(s.command(s.position_deg(), t, nullptr));   // unknown position: a command always moves
  // Commands beyond the limit are clamped and reported.
  bool clamped = false;
  s.command(75.0f, t, &clamped);
  CHECK(clamped);
  CHECK_NEAR(s.target_deg(), 50.0, 1e-6);

  // No power switch fitted: always powered; pulses between moves only when asked.
  cfg.gate_power = false;
  ServoSequencer a(cfg, 0.0f);
  ServoSequencer::Output o = a.tick(0);
  CHECK(o.power && !o.pulses);
  a.command(5.0f, 0, nullptr);
  CHECK(a.phase() == ServoSequencer::Phase::kRamp);   // no power-up phase needed
  uint32_t u = 0;
  for (int k = 0; k < 100 && a.busy(); ++k) { u += 20; o = a.tick(u); }
  CHECK(o.power && !o.pulses);
  cfg.pulse_when_idle = true;
  a.set_config(cfg);
  o = a.tick(u + 20);
  CHECK(o.power && o.pulses);
  CHECK_EQ(o.pulse_us, servo_pulse_us(5.0f, cfg));
  CHECK_STR(servo_phase_name(ServoSequencer::Phase::kRamp), "moving");
}

// ---- tracking decisions ---------------------------------------------------------

static TrackerInput tin(uint32_t now, bool valid, bool dark, float sun, float panel, bool busy = false, uint8_t fault = 0) {
  TrackerInput in;
  in.now_ms = now;
  in.sun_valid = valid;
  in.sun_dark = dark;
  in.sun_deg = sun;
  in.panel_deg = panel;
  in.servo_busy = busy;
  in.fault = fault;
  return in;
}

TEST(tracker_deadband_limits_and_move_gap) {
  TrackerConfig cfg;
  cfg.filter_alpha = 1.0f;   // no smoothing in this test
  Tracker tr(cfg);
  TrackerDecision d = tr.step(tin(0, true, false, 3.0f, 0.0f));
  CHECK(d.state == State::TRK);
  CHECK(!d.move);              // 3 deg off: inside the 5 deg deadband
  d = tr.step(tin(1000, true, false, 12.0f, 0.0f));
  CHECK(d.move);
  CHECK_NEAR(d.target_deg, 12.0, 1e-6);
  // Busy: no new move while the servo is still going.
  d = tr.step(tin(1500, true, false, 30.0f, 0.0f, true));
  CHECK(!d.move);
  // Within the 10 s gap: no move even though 18 deg off.
  d = tr.step(tin(5000, true, false, 30.0f, 12.0f));
  CHECK(!d.move);
  d = tr.step(tin(11000, true, false, 30.0f, 12.0f));
  CHECK(d.move);
  // Sun beyond the 45 deg range: the panel goes to the limit and stays.
  d = tr.step(tin(30000, true, false, 62.0f, 30.0f));
  CHECK(d.move);
  CHECK_NEAR(d.target_deg, 45.0, 1e-6);
  d = tr.step(tin(60000, true, false, 66.0f, 45.0f));
  CHECK(!d.move);
  CHECK(d.state == State::TRK);
}

TEST(tracker_dark_stows_after_delay_and_cloud_holds) {
  TrackerConfig cfg;
  cfg.dark_to_stow_ms = 60000;
  Tracker tr(cfg);
  TrackerDecision d = tr.step(tin(0, false, true, 0, 20.0f));
  CHECK(d.state == State::IDL);
  CHECK(!d.move);
  d = tr.step(tin(59000, false, true, 0, 20.0f));
  CHECK(!d.move);
  d = tr.step(tin(60000, false, true, 0, 20.0f));
  CHECK(d.move);
  CHECK_NEAR(d.target_deg, -45.0, 1e-6);
  CHECK(d.state == State::IDL);   // still on its way
  d = tr.step(tin(61000, false, true, 0, -45.0f));
  CHECK(d.state == State::STW);
  CHECK(!d.move);
  // Light comes back: tracking resumes (filter restarted, no smoothing lag),
  // with the first move once the 10 s gap since the stow move has passed.
  d = tr.step(tin(62000, true, false, -20.0f, -45.0f));
  CHECK(d.state == State::TRK);
  CHECK(!d.move);
  d = tr.step(tin(70000, true, false, -20.0f, -45.0f));
  CHECK(d.move);
  CHECK_NEAR(d.target_deg, -20.0, 1e-6);
  // Light but no usable direction for a while (20 s since the last
  // estimate): hold, and the smoothed value is dropped.
  d = tr.step(tin(90000, false, false, 0, -20.0f));
  CHECK(d.state == State::HLD);
  CHECK(!d.move);
  CHECK(!d.sun_valid);
  // One noisy reading in between (200 ms gap): hold, but keep the smoothed
  // value, so a flickering estimate cannot keep restarting the filter.
  d = tr.step(tin(90200, true, false, -19.0f, -20.0f));
  CHECK(d.state == State::TRK);
  d = tr.step(tin(90400, false, false, 0, -20.0f));
  CHECK(d.state == State::HLD);
  CHECK(d.sun_valid);
  CHECK_NEAR(d.sun_deg, -19.0, 1e-4);
  CHECK(!d.move);
  // A short shadow does not stow: darkness has to last.
  d = tr.step(tin(91000, false, true, 0, -20.0f));
  d = tr.step(tin(100000, true, false, -19.0f, -20.0f));
  CHECK(d.state == State::TRK);
  d = tr.step(tin(101000, false, true, 0, -20.0f));
  d = tr.step(tin(150000, false, true, 0, -20.0f));   // 49 s of the new dark spell
  CHECK(!d.move);
}

TEST(tracker_filter_fault_and_manual_mode) {
  TrackerConfig cfg;   // alpha 0.3
  Tracker tr(cfg);
  TrackerDecision d = tr.step(tin(0, true, false, 10.0f, 10.0f));
  CHECK_NEAR(d.sun_deg, 10.0, 1e-6);
  d = tr.step(tin(200, true, false, 20.0f, 10.0f));
  CHECK_NEAR(d.sun_deg, 13.0, 1e-4);   // 10 + 0.3 x 10: a single jump is smoothed
  CHECK(!d.move);
  // A fault stops everything.
  d = tr.step(tin(20000, true, false, 40.0f, 10.0f, false, 8));
  CHECK(d.state == State::FLT);
  CHECK(!d.move);
  // Manual: one move to the requested angle, then hold; auto resumes tracking.
  tr.set_manual(-30.0f);
  CHECK(tr.manual());
  d = tr.step(tin(21000, true, false, 40.0f, 10.0f, true));
  CHECK(!d.move);   // waits for the servo
  d = tr.step(tin(21200, true, false, 40.0f, 10.0f));
  CHECK(d.move);
  CHECK_NEAR(d.target_deg, -30.0, 1e-6);
  CHECK(d.state == State::HLD);
  d = tr.step(tin(21400, true, false, 40.0f, -30.0f));
  CHECK(!d.move);
  tr.set_auto();
  d = tr.step(tin(40000, true, false, 40.0f, -30.0f));
  CHECK(d.move);
  CHECK_NEAR(d.target_deg, 40.0, 1e-6);
  // Hold (after `servo off`): manual, and no move at all.
  tr.hold();
  d = tr.step(tin(60000, true, false, -40.0f, 40.0f));
  CHECK(tr.manual());
  CHECK(!d.move);
  CHECK(d.state == State::HLD);
}

TEST(stall_guard_uses_the_energy_of_each_move) {
  StallConfig cfg;   // 1.5 J + 0.06 J/deg, 1.5 s tail
  // A jammed 5 deg move: 0.75 s at 7 W. The INA226 reports the 1 s average
  // once a second, so the stall shows in the reading after the move.
  {
    StallGuard g(cfg);
    CHECK(!g.update(0, false, 0.0f, true, 130.0f));      // level before the move
    bool tripped = false;
    float p = 130.0f;
    for (uint32_t t = 200; t <= 3000 && !tripped; t += 200) {
      if (t == 1000) p = 130.0f + 0.75f * 6870.0f;       // average over 0.25-1.0 s
      if (t == 2000) p = 130.0f;
      tripped = g.update(t, t >= 250 && t < 1000, 5.0f, true, p);
    }
    CHECK(tripped);
    CHECK(g.excess_j() > g.allowance_j());
    CHECK_NEAR(g.allowance_j(), 1.8, 1e-4);
  }
  // A free 5 deg move (about 1 W for 0.75 s): no trip.
  {
    StallGuard g(cfg);
    g.update(0, false, 0.0f, true, 130.0f);
    bool tripped = false;
    float p = 130.0f;
    for (uint32_t t = 200; t <= 4000; t += 200) {
      if (t == 1000) p = 130.0f + 0.75f * 900.0f;
      if (t == 2000) p = 130.0f;
      tripped = tripped || g.update(t, t >= 250 && t < 1000, 5.0f, true, p);
    }
    CHECK(!tripped);
    CHECK(g.excess_j() < 1.0f);
  }
  // A free 90 deg stow move: 3.6 s at about 1.2 W, allowance 6.9 J: no trip.
  {
    StallGuard g(cfg);
    g.update(0, false, 0.0f, true, 130.0f);
    bool tripped = false;
    for (uint32_t t = 200; t <= 7000; t += 200) {
      const bool busy = t >= 200 && t < 3800;
      const float p = (t >= 1000 && t <= 4000) ? 1330.0f : 130.0f;
      tripped = tripped || g.update(t, busy, 90.0f, true, p);
    }
    CHECK(!tripped);
  }
  // No reading at all: never a stall.
  {
    StallGuard g(cfg);
    bool tripped = false;
    for (uint32_t t = 0; t <= 4000; t += 200) tripped = tripped || g.update(t, t < 2000, 5.0f, false, 9000.0f);
    CHECK(!tripped);
  }
}
