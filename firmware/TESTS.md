# Firmware tests and results

## Update, 19 Nov 2026: tracker firmware (sensing, servo, control, transmission)

Run on 19 Nov 2026 (UTC) on the build machine: Ubuntu 24.04, g++ 13.3.0,
CMake 3.28.3, arduino-cli 1.6.0, cores `esp32:esp32` 3.3.12 and 4.0.0.

| Check | Result |
|---|---|
| Unit tests `host/build/ast_tests` (ASan + UBSan, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`) | **211 tests, 0 failed** (195 data-link tests as before + 16 new) |
| New: INA219 conversion and chip identification (INA226 / INA219 / INA3221 refused / nothing) | pass |
| New: sun estimate from four photodiodes: exact model -65..+65 deg; 1 % gain mismatch + 5 mV noise over 50 random arcs: mean error < 1 deg, worst < 5 deg; one clipped channel tolerated; two clipped refused with the reason; dark; out of view; NaN | pass |
| New: DS-R001 pulse map (500-2500 us, level = 1500 us, +/-50 deg clamp, trim, direction flip), LEDC duty (14-bit, 50 Hz); move sequence power-up at the old angle, 30 deg/s ramp, settle, power off; retarget; emergency stop; no-switch mode | pass |
| New: tracking decisions: 5 deg deadband, +/-45 deg range, 10 s move gap, stow after darkness lasts, hold when the light has no direction, faults stop moves, manual and hold modes; stall guard | pass |
| New: closed-loop digital twin: lamp sweeps -60..+60 deg at 1 deg/min, 1 % sensor mismatch, through estimate -> decisions -> servo -> telemetry -> minute aggregator -> channel-1 JSON. Pointing error mean < 4 deg, worst < 8 deg; 15-35 moves; servo powered < 60 s in 2 h; MOV, IDL, STW minutes; stowed at -45 deg in the dark; tracked energy 12-30 % above fixed for this sweep; a jammed servo trips the stall guard in 2.5-4 s and is switched off | pass |
| Concurrency test (TSan), `ast_host_device --selftest`, `smoke_mock.sh` | pass, as before |
| ESP32 compile, core 3.3.12, both sketches (`build.sh esp32`) | **0 errors, 0 warnings in our files**; tracker 1,310,197 bytes (41 %), data link 1,282,421 bytes (40 %) |
| ESP32 compile, core 4.0.0 (the version the Boards Manager installs today), tracker | **0 errors, 0 warnings** (whole log); 1,382,350 bytes (43 %) |
| `sync_core.sh --check` (core and adapter copies in both sketches) | identical |
| End-to-end suite `tests/e2e/run_e2e.py` with the new core (host firmware, mock ThingSpeak, real dashboard in Chrome, Python nightly stand-in because Octave is not installed) | **129 of 129 PASS**; the committed results files (13 Nov, real Octave) were kept |
| Dashboard unit 32/32, browser 9/9; Python 41 + 69 pytest | pass |

Not covered by any automatic test (needs the real board): photodiode ADC
readings and their real angular response, servo PWM and power switch, the
INA219 and INA226 I2C drivers, Wi-Fi + TLS, DS3231, LittleFS, NVS, watchdog,
measured current. The bench runbook (`docs/BENCH_RUNBOOK_26NOV.md`, W1-W8 and
S1-S7) covers them on 25-26 Nov.

---

## Data link, 13 Nov 2026

Run on 13 Nov 2026 (UTC) on the build machine: Ubuntu 24.04, g++ 13.3.0,
CMake 3.28.3, arduino-cli 1.6.0, core `esp32:esp32` 3.3.12.
Reproduce with `firmware/build.sh` (everything) or `firmware/build.sh host`.

## Summary

| Check | Result |
|---|---|
| Unit tests `host/build/ast_tests`, built with `-fsanitize=address,undefined -fno-sanitize-recover=undefined`, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` | **195 tests, 157832 checks, 0 tests failed (0 checks failed)**; no sanitizer report |
| Concurrency test `host/build/ast_tsan_test`, built with `-fsanitize=thread` (sampler, uploader and CLI threads on one `DataLink`, slow and failing transport) | **PASS**, no data race reported (run 10 times in a row) |
| `ast_host_device --selftest` (45 + 15 virtual minutes offline, restart, records and seq persist) | **ok** |
| `host/smoke_mock.sh`: `ast_host_device` against `tools/mock_thingspeak.py` (normal uploads, lost response + read-back, HTTP 500, crash/restart) | **SMOKE PASS** (8 checks) |
| ESP32 compile `arduino-cli compile --fqbn esp32:esp32:nano_nora:PartitionScheme=spiffs --warnings all` | **0 errors, 0 warnings** (whole log, core included) |
| Same sources on ESP32 and PC | `sync_core.sh --check`: copy identical |
| clang++ 18 `-fsyntax-only` with the same warning flags on core and host sources | no warnings |

Not covered by any automatic test (needs the real board; see README "Known
limitations"): Wi-Fi + TLS to api.thingspeak.com, DS3231 and INA226 I2C
drivers, LittleFS adapter, NVS, watchdog, measured current.

## ESP32 build result

| Item | Value |
|---|---|
| Core | `esp32:esp32` 3.3.12 (arduino-cli 1.6.0) |
| FQBN | `esp32:esp32:nano_nora:PartitionScheme=spiffs` (partition table `app3M_spiffs9M_fact512k_16MB`: 2 x 3 MB app, 9.4 MB data for LittleFS) |
| Program storage (flash) | 1,280,153 bytes = 40 % of 3,145,728 |
| Global variables (static RAM) | 81,940 bytes = 25 % of 327,680; 245,740 bytes left for heap and stacks |
| Largest run-time allocations | upload batch buffer 960 records x 36 B = 34.6 kB (once, at start); task stacks 8 kB (sampler) + 16 kB (uploader) |
| With `secrets.h` defining `TS_ROOT_CA_PEM` (pinned-root code path) | also compiles, 1,280,225 bytes |

`arduino:esp32` 2.0.18 was not tried (optional; large download).

## Thread-sanitizer run (output)

```
tsan_concurrency: 600 minutes logged, 65 requests, 0 lost, longest sampler_tick 0.708 ms
PASS
```

## Unit tests (195)

### Minute aggregator (7.1), status string and fault priority (4) - `host/tests/test_aggregator_status.cpp` (23)

- `aggregator_one_record_per_minute_with_mean_power`
- `aggregator_minute_is_utc_aligned_not_start_aligned`
- `aggregator_angles_and_state_are_end_of_minute_values`
- `aggregator_mov_if_motor_ran_at_any_time_in_the_minute`
- `aggregator_fault_state_is_not_hidden_by_mov`
- `aggregator_negative_mean_power_clamped_to_zero`
- `aggregator_negative_readings_inside_positive_mean_are_kept_in_the_mean`
- `aggregator_rounding_half_away_from_zero`
- `aggregator_unknown_values_are_flagged_not_zero`
- `aggregator_mean_uses_only_valid_readings`
- `aggregator_sun_angle_unknown_when_last_sample_has_no_sun`
- `aggregator_temperature_mean_and_negative_values`
- `aggregator_gap_in_sampling_emits_only_minutes_with_data`
- `aggregator_clock_step_backwards_drops_partial_minute`
- `aggregator_flush_before_minute_end_does_nothing`
- `aggregator_simulated_flag_if_any_sample_simulated`
- `aggregator_control_fault_seen_mid_minute_is_reported`
- `status_examples_from_the_contract`
- `status_all_states_panels_and_faults`
- `status_longest_string_fits_and_small_buffer_is_safe`
- `fault_priority_control_code_wins`
- `fault_priority_datalink_codes_4_then_6_then_9`
- `fault_priority_missing_power_sensor`

### Serial CLI (7.6), calibration maths, telemetry API (7.7), radio accounting, PC HTTP client - `host/tests/test_cli_misc.cpp` (33)

- `cli_simple_commands`
- `cli_is_case_insensitive_and_tolerates_white_space`
- `cli_unknown_command_and_extra_arguments_are_errors`
- `cli_run_id_range`
- `cli_panel_letter`
- `cli_sim_on_off_and_scenario`
- `cli_wifi_with_quotes_and_validation`
- `cli_keys`
- `cli_upload_every`
- `cli_time_set`
- `cli_buffer_subcommands`
- `cli_cal_commands`
- `cli_help_mentions_every_contract_command`
- `cli_tokenizer`
- `line_reader_handles_cr_lf_crlf_and_backspace`
- `line_reader_discards_over_long_lines_and_control_characters`
- `cal_two_point_solves_gain_and_offset`
- `cal_two_point_refuses_bad_input`
- `ina226_scaling_matches_datasheet`
- `ina226_convert_applies_calibration_and_gives_milliwatts`
- `telemetry_snapshot_reflects_setters`
- `telemetry_stale_control_system_gives_unknown_angles_and_idle`
- `telemetry_motor_flag_is_latched_until_sampled`
- `telemetry_sun_invalid_and_temperature_clear`
- `telemetry_run_and_panel_requests_are_delivered_once`
- `telemetry_out_of_range_inputs_are_made_safe`
- `telemetry_free_functions_use_the_shared_instance`
- `radio_meter_counts_sessions_and_today`
- `radio_meter_today_resets_at_utc_midnight_and_splits_a_session`
- `radio_policy_on_before_upload_off_in_between`
- `radio_is_on_for_seconds_per_upload_over_a_day`
- `host_http_url_parsing`
- `host_http_response_parsing`

### CRC-32 and UTC calendar / `created_at` (contract 3.1) - `host/tests/test_crc_time.cpp` (14)

- `crc32_matches_standard_check_value`
- `crc32_detects_single_bit_flip`
- `little_endian_helpers_round_trip`
- `created_at_exact_format`
- `created_at_seconds_are_always_zero`
- `created_at_year_boundary`
- `created_at_leap_day_2028`
- `created_at_2100_is_not_a_leap_year`
- `created_at_month_boundaries_through_a_year`
- `created_at_ignores_uk_clock_changes`
- `civil_round_trip_every_day_for_80_years`
- `iso_format_and_parse`
- `iso_parse_rejects_bad_input`
- `minute_floor_handles_all_seconds`

### DataLink: whole device behaviour, time validity (7.1-7.5) - `host/tests/test_datalink.cpp` (25)

- `datalink_logs_one_record_per_minute_and_uploads_every_15_min`
- `datalink_run_and_panel_appear_in_status`
- `datalink_rtc_lost_means_no_logging_and_fault_4_until_ntp`
- `datalink_rtc_with_implausible_date_is_treated_as_lost`
- `time_validity_rtc_is_set_only_when_more_than_2_s_off`
- `time_validity_lost_rtc_is_always_written_from_ntp`
- `time_validity_manual_set`
- `datalink_rtc_missing_logs_after_ntp_but_keeps_fault_4`
- `datalink_buffer_overflow_raises_fault_6_on_the_next_sample`
- `datalink_fault_9_appears_in_samples_after_60_min_offline_and_clears`
- `datalink_control_fault_wins_but_overflow_is_reported_afterwards`
- `datalink_missing_power_sensor_gives_fault_code_and_omits_field`
- `datalink_simulator_marks_every_sample_and_ignores_real_inputs`
- `datalink_simulator_is_off_by_default_and_real_samples_have_no_marker`
- `datalink_minute_with_mixed_sources_is_marked_simulated`
- `datalink_clock_stepping_back_never_logs_a_minute_twice`
- `datalink_reboot_inside_the_same_minute_does_not_duplicate_it`
- `datalink_state_survives_restart_and_pending_records_are_uploaded_later`
- `datalink_seven_day_outage_loses_nothing_and_backfills_in_order`
- `datalink_ninth_day_of_outage_overwrites_oldest_and_says_so`
- `datalink_corrupt_record_in_flash_is_skipped_by_the_uploader`
- `datalink_storage_failure_is_reported_as_fault_6_on_next_stored_sample`
- `datalink_flush_stores_the_last_complete_minute_on_shutdown`
- `datalink_clear_buffer_keeps_seq`
- `datalink_status_snapshot_is_consistent`

### Exact bulk-update JSON (7.3, 3.1) - `host/tests/test_json.cpp` (14)

- `json_entry_exact_text_all_fields`
- `json_unknown_values_are_omitted_never_zero`
- `json_number_formats`
- `json_zero_power_is_written_as_zero_when_known`
- `json_simulated_marker_and_panel_in_status`
- `json_batch_exact_text_two_entries`
- `json_batch_has_no_spaces_outside_strings_and_no_newlines`
- `json_batch_streamed_in_any_chunk_size_equals_whole_text`
- `json_batch_is_capped_at_960_entries`
- `json_batch_empty_is_still_valid_json`
- `json_key_is_escaped`
- `json_entry_longest_case_fits_buffer`
- `json_batch_skips_duplicate_and_backward_timestamps`
- `json_batch_after_minute_can_exclude_the_first_record`

### Record codec and ring buffer with power-loss-safe metadata (7.2) - `host/tests/test_record_ring.cpp` (31)

- `record_round_trip_all_fields`
- `record_layout_is_fixed_little_endian`
- `record_any_corrupted_byte_is_rejected`
- `record_erased_flash_is_not_a_record`
- `record_out_of_range_values_rejected_even_with_good_crc`
- `state_names`
- `ring_fresh_starts_empty_with_seq_1`
- `ring_push_assigns_consecutive_seq_and_peek_returns_oldest_first`
- `ring_ack_moves_cursor_and_is_clamped`
- `ring_ack_through_seq`
- `ring_wraps_many_times_without_loss_when_acked`
- `ring_overflow_overwrites_oldest_unsent_and_flags_fault`
- `ring_overflow_of_already_sent_record_is_not_a_loss`
- `ring_overflow_flag_cleared_only_when_reported`
- `ring_continuous_overflow_keeps_flag_set`
- `ring_state_survives_restart`
- `ring_seq_persists_across_many_restarts_and_wraps`
- `ring_metadata_alternates_between_two_copies`
- `ring_one_corrupt_metadata_copy_falls_back_to_the_other`
- `ring_torn_metadata_write_at_every_length_recovers_previous_state`
- `ring_torn_record_write_is_invisible`
- `ring_power_loss_at_every_write_of_a_long_run_never_breaks_invariants`
- `ring_both_metadata_copies_lost_rebuilds_from_records`
- `ring_scan_recovery_stops_at_a_gap`
- `ring_metadata_with_good_crc_but_broken_invariants_is_rejected`
- `ring_metadata_for_other_capacity_is_ignored`
- `ring_corrupt_pending_record_is_skipped_not_blocking`
- `ring_rejects_minute_not_after_last_logged`
- `ring_clear_keeps_seq_and_allows_earlier_time`
- `ring_full_size_buffer_holds_8_days_and_peeks_960`
- `ring_storage_failure_changes_nothing`

### Firmware simulator (7.5) - `host/tests/test_sim.cpp` (17)

- `sim_sun_position_guildford_midsummer_noon`
- `sim_sun_position_guildford_november`
- `sim_sun_rise_and_set_times_are_plausible`
- `sim_axis_angle_east_negative_west_positive`
- `sim_every_sample_is_marked_simulated_in_every_scenario`
- `sim_clear_day_tracker_beats_fixed_panel_net_of_own_use_in_summer`
- `sim_own_use_power_is_realistic`
- `sim_tracker_moves_about_every_20_minutes_in_daylight`
- `sim_pointing_error_stays_within_deadband_while_tracking`
- `sim_panel_angle_respects_mechanical_limits`
- `sim_night_scenario_is_dark_and_idle`
- `sim_real_night_parks_level_and_goes_idle`
- `sim_overcast_holds_position_and_gains_nothing`
- `sim_cloudy_day_has_sun_and_cloud_and_is_reproducible`
- `sim_lamp_moves_east_to_west_in_two_hours_then_goes_off`
- `sim_scenario_names_round_trip`
- `sim_hash_noise_is_uniform_enough_and_deterministic`

### Upload state machine (7.3) - `host/tests/test_uploader.cpp` (38)

- `uploader_slot_times_are_second_20_of_every_upload_min`
- `uploader_backoff_sequence_1_2_4_8_15_minutes`
- `uploader_success_body_detection`
- `uploader_last_field8_parsing`
- `uploader_posts_every_15_min_at_second_20_with_exact_url`
- `uploader_sends_nothing_when_nothing_is_pending`
- `uploader_interval_5_and_60_minutes`
- `uploader_upload_min_limits`
- `uploader_backlog_split_into_960_with_16_s_spacing`
- `uploader_exactly_960_pending_is_one_request`
- `uploader_spacing_is_measured_from_the_end_of_the_previous_request`
- `uploader_record_arriving_during_upload_waits_for_next_slot`
- `uploader_connect_failure_retries_with_backoff_1_2_4_8_15_15`
- `uploader_http_500_and_429_retry_the_same_batch`
- `uploader_backoff_resets_after_a_success`
- `uploader_http_200_with_success_true_is_accepted_like_202`
- `uploader_fault_9_after_60_min_without_success_while_pending`
- `uploader_fault_9_counts_from_the_last_success`
- `uploader_no_fault_9_when_buffer_is_empty_or_uploads_work`
- `uploader_not_configured_sends_nothing_and_raises_fault_9_after_60_min`
- `uploader_lost_response_resends_same_batch_without_read_key`
- `uploader_readback_confirms_whole_batch_and_skips_the_resend`
- `uploader_readback_partial_resends_only_the_missing_records`
- `uploader_readback_that_proves_nothing_resends_everything`
- `uploader_readback_waits_30_s_even_when_upload_is_forced`
- `uploader_wrong_key_401_keeps_data_and_backs_off_at_the_cap`
- `uploader_poison_record_is_isolated_by_bisection_and_only_it_is_dropped`
- `uploader_two_poison_records_in_a_full_batch`
- `uploader_200_without_success_is_a_content_rejection`
- `uploader_server_rejecting_everything_drops_at_most_3_records`
- `uploader_bisection_after_a_5xx_keeps_16_s_spacing` (independent review, finding 1)
- `uploader_batch_size_grows_back_after_rejections_end` (independent review, finding 2)
- `uploader_one_off_400_does_not_lose_data`
- `uploader_duplicate_timestamps_never_reach_the_server`
- `uploader_cli_upload_now_respects_16_s_spacing`
- `uploader_seconds_until_due_for_radio_scheduling`
- `uploader_stats_snapshot`
- `uploader_waits_while_the_clock_is_not_a_real_date`

## What the edge-case tests show (by item of the task list)

| Item | Tests |
|---|---|
| Buffer wrap and overflow | `ring_wraps_many_times_without_loss_when_acked`, `ring_overflow_*`, `ring_continuous_overflow_keeps_flag_set`, `ring_full_size_buffer_holds_8_days_and_peeks_960`, `datalink_buffer_overflow_raises_fault_6_on_the_next_sample`, `datalink_ninth_day_of_outage_overwrites_oldest_and_says_so` |
| Metadata corruption and torn writes | `ring_one_corrupt_metadata_copy_falls_back_to_the_other`, `ring_torn_metadata_write_at_every_length_recovers_previous_state` (all 44 cut positions), `ring_torn_record_write_is_invisible`, `ring_power_loss_at_every_write_of_a_long_run_never_breaks_invariants` (80 cut points), `ring_both_metadata_copies_lost_rebuilds_from_records`, `ring_metadata_with_good_crc_but_broken_invariants_is_rejected` |
| CRC failures | `crc32_*`, `record_any_corrupted_byte_is_rejected`, `ring_corrupt_pending_record_is_skipped_not_blocking`, `datalink_corrupt_record_in_flash_is_skipped_by_the_uploader` |
| seq persistence | `ring_state_survives_restart`, `ring_seq_persists_across_many_restarts_and_wraps`, `datalink_state_survives_restart_and_pending_records_are_uploaded_later` |
| Minute alignment, UTC formatting, month/year/leap-day | `aggregator_minute_is_utc_aligned_not_start_aligned`, `created_at_*` (year boundary, 29 Feb 2028, 2100 not leap, every month boundary of 2026, UK clock-change days), `civil_round_trip_every_day_for_80_years` |
| JSON exactness | `json_entry_exact_text_all_fields`, `json_unknown_values_are_omitted_never_zero`, `json_number_formats`, `json_batch_exact_text_two_entries`, `json_batch_has_no_spaces_outside_strings_and_no_newlines`, `json_batch_streamed_in_any_chunk_size_equals_whole_text` |
| 960 split and spacing | `json_batch_is_capped_at_960_entries`, `uploader_backlog_split_into_960_with_16_s_spacing`, `uploader_exactly_960_pending_is_one_request`, `uploader_spacing_is_measured_from_the_end_of_the_previous_request`, `datalink_seven_day_outage_loses_nothing_and_backfills_in_order` |
| Backoff sequence, fault 9 timing | `uploader_backoff_sequence_1_2_4_8_15_minutes`, `uploader_connect_failure_retries_with_backoff_1_2_4_8_15_15`, `uploader_backoff_resets_after_a_success`, `uploader_fault_9_*`, `uploader_no_fault_9_when_buffer_is_empty_or_uploads_work`, `datalink_fault_9_appears_in_samples_after_60_min_offline_and_clears` |
| In-doubt re-send and read-back skip | `uploader_lost_response_resends_same_batch_without_read_key`, `uploader_readback_confirms_whole_batch_and_skips_the_resend`, `uploader_readback_partial_resends_only_the_missing_records`, `uploader_readback_that_proves_nothing_resends_everything`, `uploader_readback_waits_30_s_even_when_upload_is_forced` |
| RTC lost | `datalink_rtc_lost_means_no_logging_and_fault_4_until_ntp`, `datalink_rtc_with_implausible_date_is_treated_as_lost`, `datalink_rtc_missing_logs_after_ntp_but_keeps_fault_4`, `time_validity_*`, `uploader_waits_while_the_clock_is_not_a_real_date` |
| Strictly increasing, unique timestamps (lead, 13 Nov) | `ring_rejects_minute_not_after_last_logged`, `datalink_clock_stepping_back_never_logs_a_minute_twice`, `datalink_reboot_inside_the_same_minute_does_not_duplicate_it`, `json_batch_skips_duplicate_and_backward_timestamps`, `uploader_duplicate_timestamps_never_reach_the_server` |
| Poison batch (lead, 13 Nov) | `uploader_poison_record_is_isolated_by_bisection_and_only_it_is_dropped`, `uploader_two_poison_records_in_a_full_batch`, `uploader_200_without_success_is_a_content_rejection`, `uploader_server_rejecting_everything_drops_at_most_3_records`, `uploader_one_off_400_does_not_lose_data`, `uploader_wrong_key_401_keeps_data_and_backs_off_at_the_cap`, `uploader_http_500_and_429_retry_the_same_batch` |
| Energy (lead, 13 Nov) | `radio_meter_*`, `radio_policy_on_before_upload_off_in_between`, `radio_is_on_for_seconds_per_upload_over_a_day`, `uploader_seconds_until_due_for_radio_scheduling` |
| CLI parsing | `cli_*` (14 tests), `line_reader_*` |
| Simulator marks every sample | `sim_every_sample_is_marked_simulated_in_every_scenario`, `datalink_simulator_marks_every_sample_and_ignores_real_inputs`, `datalink_minute_with_mixed_sources_is_marked_simulated` |
