// Minute aggregator (7.1) and status string / fault priority (section 4).
#include "ast_aggregator.h"
#include "ast_status.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ast;
using ast_test::kT0;
using ast_test::sample;

static std::string status_of(const Record& r) {
  char b[kStatusMax];
  build_status(r, b, sizeof b);
  return b;
}

TEST(aggregator_one_record_per_minute_with_mean_power) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    // Power ramps 0..59 mW: mean 29.5 mW
    CHECK(!a.add(kT0 + s, sample(static_cast<float>(s), 100.0f, 120.0f), &r));
  }
  CHECK(a.add(kT0 + 60, sample(), &r));    // first sample of the next minute closes the first
  CHECK_EQ(r.minute_unix, static_cast<uint32_t>(kT0));
  CHECK_EQ(r.p_trk_dmw, 295u);
  CHECK_EQ(r.p_fix_dmw, 1000u);
  CHECK_EQ(r.p_use_dmw, 1200u);
  CHECK_EQ(r.vsup_mv, 5000);
  CHECK(r.has(kHasPtrk) && r.has(kHasPfix) && r.has(kHasPuse) && r.has(kHasVsup) && r.has(kHasAngPanel));
  CHECK(!r.has(kHasAngSun));
  CHECK(!r.has(kHasTemp));
  CHECK(!r.has(kSimulated));
}

TEST(aggregator_minute_is_utc_aligned_not_start_aligned) {
  Aggregator a;
  Record r;
  // Start at second 37: the first record covers only 23 samples of that minute.
  int emitted = 0;
  for (int64_t t = kT0 + 37; t < kT0 + 37 + 180; ++t) {
    if (a.add(t, sample(), &r)) {
      CHECK_EQ(r.minute_unix % 60u, 0u);
      CHECK_EQ(r.minute_unix, static_cast<uint32_t>(kT0 + 60 * emitted));
      ++emitted;
    }
  }
  CHECK_EQ(emitted, 3);
}

TEST(aggregator_angles_and_state_are_end_of_minute_values) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    Sample1Hz x = sample();
    x.ang_panel_deg = -30.0f + static_cast<float>(s) * 0.1f;   // ends at -24.1
    x.ang_sun_valid = true;
    x.ang_sun_deg = -20.0f - static_cast<float>(s) * 0.01f;    // ends at -20.59
    x.state = s < 59 ? State::HLD : State::TRK;
    a.add(kT0 + s, x, &r);
  }
  CHECK(a.flush(kT0 + 60, &r));
  CHECK_EQ(r.ang_panel_ddeg, -241);
  CHECK_EQ(r.ang_sun_ddeg, -206);
  CHECK(r.state == State::TRK);
}

TEST(aggregator_mov_if_motor_ran_at_any_time_in_the_minute) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    Sample1Hz x = sample();
    x.motor_running = (s == 17);           // a single second of movement
    a.add(kT0 + s, x, &r);
  }
  CHECK(a.flush(kT0 + 60, &r));
  CHECK(r.state == State::MOV);
  // Next minute without movement goes back to the reported state.
  for (int s = 60; s < 120; ++s) a.add(kT0 + s, sample(), &r);
  CHECK(a.flush(kT0 + 120, &r));
  CHECK(r.state == State::TRK);
}

TEST(aggregator_fault_state_is_not_hidden_by_mov) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    Sample1Hz x = sample();
    x.motor_running = s < 10;
    if (s >= 10) { x.state = State::FLT; x.ctrl_fault = 8; }   // stalled
    a.add(kT0 + s, x, &r);
  }
  CHECK(a.flush(kT0 + 60, &r));
  CHECK(r.state == State::FLT);
  CHECK_EQ(r.fault, 8);
}

TEST(aggregator_negative_mean_power_clamped_to_zero) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) a.add(kT0 + s, sample(-0.4f, 0.04f, 120.0f), &r);
  CHECK(a.flush(kT0 + 60, &r));
  CHECK(r.has(kHasPtrk));                  // still a valid sample ...
  CHECK_EQ(r.p_trk_dmw, 0u);               // ... written as 0.0
  CHECK_EQ(r.p_fix_dmw, 0u);               // 0.04 mW rounds to 0.0
  CHECK_EQ(r.fault, 0);                    // e unchanged
  CHECK_EQ(a.negative_clamps(), 1u);
}

TEST(aggregator_negative_readings_inside_positive_mean_are_kept_in_the_mean) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) a.add(kT0 + s, sample(s % 2 ? 3.0f : -1.0f), &r);
  CHECK(a.flush(kT0 + 60, &r));
  CHECK_EQ(r.p_trk_dmw, 10u);              // mean exactly 1.0 mW
  CHECK_EQ(a.negative_clamps(), 0u);
}

TEST(aggregator_rounding_half_away_from_zero) {
  CHECK_EQ(round_scaled(0.05, 10, -100, 100), 1);
  CHECK_EQ(round_scaled(0.04, 10, -100, 100), 0);
  CHECK_EQ(round_scaled(-0.05, 10, -100, 100), -1);
  CHECK_EQ(round_scaled(12.34, 10, -1000, 1000), 123);
  CHECK_EQ(round_scaled(12.36, 10, -1000, 1000), 124);
  CHECK_EQ(round_scaled(1e12, 10, -1000, 1000), 1000);     // saturates
  CHECK_EQ(round_scaled(-1e12, 10, -1000, 1000), -1000);
}

TEST(aggregator_unknown_values_are_flagged_not_zero) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    Sample1Hz x = sample();
    x.p_fix_valid = false;                 // fixed-panel sensor missing all minute
    x.vsup_valid = false;
    x.ang_panel_valid = false;
    a.add(kT0 + s, x, &r);
  }
  CHECK(a.flush(kT0 + 60, &r));
  CHECK(r.has(kHasPtrk));
  CHECK(!r.has(kHasPfix));
  CHECK(!r.has(kHasVsup));
  CHECK(!r.has(kHasAngPanel));
  CHECK_EQ(sensor_fault_from_flags(r.flags), 2);
}

TEST(aggregator_mean_uses_only_valid_readings) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    Sample1Hz x = sample(100.0f);
    if (s < 30) x.p_trk_valid = false;     // sensor dropped out for half the minute
    if (s == 40) x.p_fix_mw = NAN;         // a NaN reading is ignored
    a.add(kT0 + s, x, &r);
  }
  CHECK(a.flush(kT0 + 60, &r));
  CHECK_EQ(r.p_trk_dmw, 1000u);
  CHECK_EQ(r.p_fix_dmw, 4000u);
}

TEST(aggregator_sun_angle_unknown_when_last_sample_has_no_sun) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    Sample1Hz x = sample();
    x.ang_sun_valid = s < 59;              // cloud arrives in the last second
    x.ang_sun_deg = 5.0f;
    a.add(kT0 + s, x, &r);
  }
  CHECK(a.flush(kT0 + 60, &r));
  CHECK(!r.has(kHasAngSun));
}

TEST(aggregator_temperature_mean_and_negative_values) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    Sample1Hz x = sample();
    x.temp_valid = true;
    x.temp_c = -3.25f;
    a.add(kT0 + s, x, &r);
  }
  CHECK(a.flush(kT0 + 60, &r));
  CHECK(r.has(kHasTemp));
  CHECK_EQ(r.temp_dc, -33);                // -3.25 -> -3.3 (half away from zero)
}

TEST(aggregator_gap_in_sampling_emits_only_minutes_with_data) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) a.add(kT0 + s, sample(), &r);
  // Sampler was stalled for 5 minutes.
  CHECK(a.add(kT0 + 360, sample(), &r));
  CHECK_EQ(r.minute_unix, static_cast<uint32_t>(kT0));
  CHECK(!a.flush(kT0 + 361, &r));          // minute 6 is still running
  CHECK(a.flush(kT0 + 420, &r));
  CHECK_EQ(r.minute_unix, static_cast<uint32_t>(kT0 + 360));
}

TEST(aggregator_clock_step_backwards_drops_partial_minute) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 30; ++s) a.add(kT0 + 600 + s, sample(), &r);
  CHECK(!a.add(kT0 + 300, sample(), &r));  // clock corrected 5 min back: nothing emitted
  CHECK_EQ(a.current_minute(), kT0 + 300);
  CHECK_EQ(a.samples_in_minute(), 1);
}

TEST(aggregator_flush_before_minute_end_does_nothing) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 59; ++s) a.add(kT0 + s, sample(), &r);
  CHECK(!a.flush(kT0 + 59, &r));
  CHECK(a.active());
  CHECK(a.flush(kT0 + 60, &r));
  CHECK(!a.active());
  CHECK(!a.flush(kT0 + 120, &r));          // nothing left
}

TEST(aggregator_simulated_flag_if_any_sample_simulated) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    Sample1Hz x = sample();
    x.simulated = (s == 3);
    a.add(kT0 + s, x, &r);
  }
  CHECK(a.flush(kT0 + 60, &r));
  CHECK(r.has(kSimulated));
}

TEST(aggregator_control_fault_seen_mid_minute_is_reported) {
  Aggregator a;
  Record r;
  for (int s = 0; s < 60; ++s) {
    Sample1Hz x = sample();
    x.ctrl_fault = (s >= 20 && s < 25) ? 7 : 0;   // limit switch hit, cleared again
    a.add(kT0 + s, x, &r);
  }
  CHECK(a.flush(kT0 + 60, &r));
  CHECK_EQ(r.fault, 7);
}

TEST(status_examples_from_the_contract) {
  Record r;
  r.state = State::TRK;
  r.run = 12;
  CHECK_STR(status_of(r), "TRK;r=12;p=A;e=0");
  r.state = State::MOV;
  r.flags |= kSimulated;
  CHECK_STR(status_of(r), "MOV;r=12;p=A;e=0;s=1");
}

TEST(status_all_states_panels_and_faults) {
  const char* names[] = {"TRK", "MOV", "HLD", "IDL", "STW", "FLT"};
  for (uint8_t s = 0; s < kStateCount; ++s) {
    for (uint8_t e = 0; e <= 9; ++e) {
      Record r;
      r.state = static_cast<State>(s);
      r.fault = e;
      r.run = 999;
      r.flags = kPanelB;
      CHECK_STR(status_of(r), std::string(names[s]) + ";r=999;p=B;e=" + std::to_string(e));
    }
  }
}

TEST(status_longest_string_fits_and_small_buffer_is_safe) {
  Record r;
  r.state = State::MOV;
  r.run = 999;
  r.fault = 9;
  r.flags = kPanelB | kSimulated;
  char b[kStatusMax];
  CHECK_EQ(build_status(r, b, sizeof b), static_cast<size_t>(21));
  CHECK_STR(b, "MOV;r=999;p=B;e=9;s=1");
  char tiny[8];
  CHECK_EQ(build_status(r, tiny, sizeof tiny), static_cast<size_t>(0));
  CHECK_STR(tiny, "");
}

TEST(fault_priority_control_code_wins) {
  CHECK_EQ(select_fault(8, 1, true, true, true), 8);
  CHECK_EQ(select_fault(5, 0, true, false, false), 5);
  CHECK_EQ(select_fault(7, 0, false, true, true), 7);
}

TEST(fault_priority_datalink_codes_4_then_6_then_9) {
  CHECK_EQ(select_fault(0, 0, true, true, true), 4);
  CHECK_EQ(select_fault(0, 0, false, true, true), 6);
  CHECK_EQ(select_fault(0, 0, false, false, true), 9);
  CHECK_EQ(select_fault(0, 0, false, false, false), 0);
}

TEST(fault_priority_missing_power_sensor) {
  CHECK_EQ(select_fault(0, 2, true, true, true), 2);
  CHECK_EQ(select_fault(0, 3, false, false, false), 3);
  CHECK_EQ(sensor_fault_from_flags(kHasPfix | kHasPuse), 1);
  CHECK_EQ(sensor_fault_from_flags(kHasPtrk | kHasPfix), 3);
  CHECK_EQ(sensor_fault_from_flags(kHasPtrk | kHasPfix | kHasPuse), 0);
  CHECK_EQ(select_fault(200, 0, false, false, true), 9);   // nonsense control code ignored
}
