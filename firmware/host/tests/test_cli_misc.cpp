// Serial CLI parser (7.6), calibration maths, telemetry API (7.7), radio
// accounting, and the PC HTTP client's parsers.
#include "ast_cal.h"
#include "ast_cli.h"
#include "ast_power.h"
#include "ast_telemetry.h"
#include "host_http.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ast;

TEST(cli_simple_commands) {
  CHECK(cli_parse("help").type == CmdType::kHelp);
  CHECK(cli_parse("?").type == CmdType::kHelp);
  CHECK(cli_parse("status").type == CmdType::kStatus);
  CHECK(cli_parse("time").type == CmdType::kTime);
  CHECK(cli_parse("buffer").type == CmdType::kBuffer);
  CHECK(cli_parse("upload").type == CmdType::kUploadNow);
  CHECK(cli_parse("upload now").type == CmdType::kUploadNow);
  CHECK(cli_parse("reboot").type == CmdType::kReboot);
  CHECK(cli_parse("power").type == CmdType::kPower);
  CHECK(cli_parse("cal").type == CmdType::kCalShow);
}

TEST(cli_is_case_insensitive_and_tolerates_white_space) {
  CHECK(cli_parse("  STATUS \r\n").type == CmdType::kStatus);
  CHECK(cli_parse("\tHelp").type == CmdType::kHelp);
  CHECK(cli_parse("").type == CmdType::kEmpty);
  CHECK(cli_parse("   \t ").type == CmdType::kEmpty);
  const Command c = cli_parse("Panel b");
  CHECK(c.type == CmdType::kPanel);
  CHECK_EQ(c.panel, 'B');
}

TEST(cli_unknown_command_and_extra_arguments_are_errors) {
  Command c = cli_parse("frobnicate");
  CHECK(c.type == CmdType::kError);
  CHECK(c.error.find("unknown command") != std::string::npos);
  CHECK(cli_parse("status now").type == CmdType::kError);
  CHECK(cli_parse("reboot please").type == CmdType::kError);
  CHECK(cli_parse("power on").type == CmdType::kError);
}

TEST(cli_run_id_range) {
  Command c = cli_parse("run 12");
  CHECK(c.type == CmdType::kRun);
  CHECK_EQ(c.num, 12u);
  CHECK_EQ(cli_parse("run 0").num, 0u);
  CHECK_EQ(cli_parse("run 999").num, 999u);       // calibration runs are 900-999
  CHECK(cli_parse("run 1000").type == CmdType::kError);
  CHECK(cli_parse("run -1").type == CmdType::kError);
  CHECK(cli_parse("run").type == CmdType::kError);
  CHECK(cli_parse("run abc").type == CmdType::kError);
  CHECK(cli_parse("run 1 2").type == CmdType::kError);
  CHECK(cli_parse("run 12.5").type == CmdType::kError);
}

TEST(cli_panel_letter) {
  CHECK_EQ(cli_parse("panel A").panel, 'A');
  CHECK_EQ(cli_parse("panel B").panel, 'B');
  CHECK(cli_parse("panel C").type == CmdType::kError);
  CHECK(cli_parse("panel").type == CmdType::kError);
  CHECK(cli_parse("panel AB").type == CmdType::kError);
}

TEST(cli_sim_on_off_and_scenario) {
  Command c = cli_parse("sim on");
  CHECK(c.type == CmdType::kSim);
  CHECK(c.flag);
  CHECK(!c.has_scenario);
  c = cli_parse("sim off");
  CHECK(c.type == CmdType::kSim);
  CHECK(!c.flag);
  c = cli_parse("sim on lamp");
  CHECK(c.type == CmdType::kSim && c.flag && c.has_scenario);
  CHECK(c.scenario == Scenario::kLamp);
  CHECK(cli_parse("sim").type == CmdType::kError);
  CHECK(cli_parse("sim maybe").type == CmdType::kError);
  CHECK(cli_parse("sim on tornado").type == CmdType::kError);
  CHECK(cli_parse("sim off lamp").type == CmdType::kError);
}

TEST(cli_wifi_with_quotes_and_validation) {
  Command c = cli_parse("wifi MyPhone secretpass");
  CHECK(c.type == CmdType::kWifi);
  CHECK_STR(c.s1, "MyPhone");
  CHECK_STR(c.s2, "secretpass");
  c = cli_parse("wifi \"Abdul's iPhone\" \"pass word 123\"");
  CHECK(c.type == CmdType::kWifi);
  CHECK_STR(c.s1, "Abdul's iPhone");
  CHECK_STR(c.s2, "pass word 123");
  c = cli_parse("wifi \"quo\\\"te\" 12345678");
  CHECK_STR(c.s1, "quo\"te");
  c = cli_parse("wifi OpenNet \"\"");               // open network
  CHECK(c.type == CmdType::kWifi);
  CHECK_STR(c.s2, "");
  CHECK(cli_parse("wifi OnlySsid").type == CmdType::kError);
  CHECK(cli_parse("wifi ssid short").type == CmdType::kError);                 // < 8 characters
  CHECK(cli_parse("wifi \"unterminated pass12345").type == CmdType::kError);
  CHECK(cli_parse("wifi 123456789012345678901234567890123 password1").type == CmdType::kError);  // 33-char SSID
  CHECK(cli_parse("wifi My Phone password1").type == CmdType::kError);         // forgot the quotes
}

TEST(cli_keys) {
  Command c = cli_parse("keys 3578277 ABCDEFGH12345678");
  CHECK(c.type == CmdType::kKeys);
  CHECK_STR(c.s1, "3578277");
  CHECK_STR(c.s2, "ABCDEFGH12345678");
  CHECK_STR(c.s3, "");
  c = cli_parse("keys 1001 WRITEKEY1001 READKEY1001");
  CHECK_STR(c.s3, "READKEY1001");
  CHECK(cli_parse("keys").type == CmdType::kError);
  CHECK(cli_parse("keys 1001").type == CmdType::kError);
  CHECK(cli_parse("keys abc KEY").type == CmdType::kError);
  CHECK(cli_parse("keys 0 KEY").type == CmdType::kError);
  CHECK(cli_parse("keys 1001 bad\"key").type == CmdType::kError);
  CHECK(cli_parse("keys 1001 K R extra").type == CmdType::kError);
}

TEST(cli_upload_every) {
  Command c = cli_parse("upload every 5");
  CHECK(c.type == CmdType::kUploadEvery);
  CHECK_EQ(c.num, 5u);
  CHECK_EQ(cli_parse("upload every 60").num, 60u);
  CHECK(cli_parse("upload every 4").type == CmdType::kError);
  CHECK(cli_parse("upload every 61").type == CmdType::kError);
  CHECK(cli_parse("upload every").type == CmdType::kError);
  CHECK(cli_parse("upload later").type == CmdType::kError);
}

TEST(cli_time_set) {
  Command c = cli_parse("time 2026-11-20T10:30:00Z");
  CHECK(c.type == CmdType::kTimeSet);
  CHECK_EQ(c.unix_time, 1795132800 + 10 * 3600 + 30 * 60);
  c = cli_parse("time 2026-11-20 10:30:00");
  CHECK(c.type == CmdType::kTimeSet);
  CHECK_EQ(c.unix_time, 1795132800 + 10 * 3600 + 30 * 60);
  CHECK(cli_parse("time yesterday").type == CmdType::kError);
  CHECK(cli_parse("time 2020-01-01T00:00:00Z").type == CmdType::kError);   // before 2026: a typo
  CHECK(cli_parse("time 2026-02-30T00:00:00Z").type == CmdType::kError);
}

TEST(cli_buffer_subcommands) {
  Command c = cli_parse("buffer dump");
  CHECK(c.type == CmdType::kBufferDump);
  CHECK_EQ(c.num, 20u);
  CHECK_EQ(cli_parse("buffer dump 100").num, 100u);
  CHECK(cli_parse("buffer dump 0").type == CmdType::kError);
  CHECK(cli_parse("buffer dump 99999").type == CmdType::kError);
  CHECK(cli_parse("buffer clear").type == CmdType::kError);     // needs the confirmation word
  CHECK(cli_parse("buffer clear yes").type == CmdType::kBufferClear);
  CHECK(cli_parse("buffer erase").type == CmdType::kError);
}

TEST(cli_cal_commands) {
  Command c = cli_parse("cal trk v 1 5.012");
  CHECK(c.type == CmdType::kCalPoint);
  CHECK(c.cal_channel == CalChannel::kTrk);
  CHECK(!c.cal_is_current);
  CHECK_EQ(c.cal_point, 1);
  CHECK_NEAR(c.value, 5.012, 1e-9);
  c = cli_parse("cal use i 2 250.5");
  CHECK(c.type == CmdType::kCalPoint);
  CHECK(c.cal_channel == CalChannel::kUse);
  CHECK(c.cal_is_current);
  CHECK_EQ(c.cal_point, 2);
  c = cli_parse("cal fix i reset");
  CHECK(c.type == CmdType::kCalReset);
  CHECK(c.cal_channel == CalChannel::kFix && c.cal_is_current);
  c = cli_parse("cal fix shunt 0.1");
  CHECK(c.type == CmdType::kCalShunt);
  CHECK_NEAR(c.value, 0.1, 1e-9);
  CHECK_NEAR(c.value2, 0.0, 1e-12);
  c = cli_parse("cal fix shunt 0.05 0.05");
  CHECK_NEAR(c.value2, 0.05, 1e-9);
  CHECK(cli_parse("cal show").type == CmdType::kCalShow);
  CHECK(cli_parse("cal abc v 1 5").type == CmdType::kError);
  CHECK(cli_parse("cal trk w 1 5").type == CmdType::kError);
  CHECK(cli_parse("cal trk v 3 5").type == CmdType::kError);
  CHECK(cli_parse("cal trk v 1").type == CmdType::kError);
  CHECK(cli_parse("cal trk v 1 five").type == CmdType::kError);
  CHECK(cli_parse("cal trk v 1 nan").type == CmdType::kError);
  CHECK(cli_parse("cal trk shunt 0").type == CmdType::kError);
  CHECK(cli_parse("cal trk shunt 100").type == CmdType::kError);
  CHECK_STR(cal_channel_name(CalChannel::kUse), "use");
}

TEST(cli_help_mentions_every_contract_command) {
  const std::string h = cli_help();
  for (const char* word : {"help", "status", "time", "buffer", "upload", "run", "panel", "sim", "cal", "wifi", "keys", "reboot", "power"}) {
    CHECK(h.find(std::string("  ") + word) != std::string::npos);
  }
}

TEST(cli_tokenizer) {
  std::vector<std::string> w;
  CHECK(cli_tokenize("a  b\tc", &w));
  CHECK_EQ(w.size(), static_cast<size_t>(3));
  CHECK(cli_tokenize("say \"hello world\" x", &w));
  CHECK_EQ(w.size(), static_cast<size_t>(3));
  CHECK_STR(w[1], "hello world");
  CHECK(cli_tokenize("a \"\" b", &w));
  CHECK_EQ(w.size(), static_cast<size_t>(3));
  CHECK_STR(w[1], "");
  CHECK(cli_tokenize("pre\"fix ed\"post", &w));
  CHECK_EQ(w.size(), static_cast<size_t>(1));
  CHECK_STR(w[0], "prefix edpost");
  CHECK(!cli_tokenize("\"open", &w));
}

TEST(line_reader_handles_cr_lf_crlf_and_backspace) {
  LineReader lr;
  std::string line;
  std::vector<std::string> lines;
  const std::string input = "status\r\nrun 12\nhelp\rpanex\bl A\n\n";
  for (char c : input) {
    if (lr.feed(c, &line)) lines.push_back(line);
  }
  CHECK_EQ(lines.size(), static_cast<size_t>(5));
  CHECK_STR(lines[0], "status");
  CHECK_STR(lines[1], "run 12");
  CHECK_STR(lines[2], "help");
  CHECK_STR(lines[3], "panel A");
  CHECK_STR(lines[4], "");
}

TEST(line_reader_discards_over_long_lines_and_control_characters) {
  LineReader lr(16);
  std::string line;
  bool got = false;
  for (int i = 0; i < 100; ++i) got = lr.feed('x', &line) || got;
  got = lr.feed('\n', &line) || got;
  CHECK(!got);                               // the long line is thrown away, not truncated and run
  for (char c : std::string("st\x01" "at\x1b" "us")) lr.feed(c, &line);
  CHECK(lr.feed('\n', &line));
  CHECK_STR(line, "status");
}

TEST(cal_two_point_solves_gain_and_offset) {
  // INA226 reads 4.950 V and 11.880 V when the DMM shows 5.000 V and 12.000 V.
  CalPoint p1{true, 4.950f, 5.000f}, p2{true, 11.880f, 12.000f};
  LinCal c;
  const char* why = nullptr;
  CHECK(two_point_cal(p1, p2, &c, &why));
  CHECK_NEAR(c.gain, 7.0 / 6.93, 1e-5);
  CHECK_NEAR(c.apply(4.950f), 5.000, 1e-4);
  CHECK_NEAR(c.apply(11.880f), 12.000, 1e-4);
  CHECK_NEAR(c.apply(8.0f), 5.0 + (8.0 - 4.95) * 7.0 / 6.93, 1e-4);
  // Order of the points does not matter.
  LinCal c2;
  CHECK(two_point_cal(p2, p1, &c2, &why));
  CHECK_NEAR(c2.gain, c.gain, 1e-6);
  CHECK_NEAR(c2.offset, c.offset, 1e-5);
}

TEST(cal_two_point_refuses_bad_input) {
  LinCal c;
  c.gain = 1.5f;
  const char* why = nullptr;
  CalPoint a{true, 5.0f, 5.0f}, b{true, 5.2f, 5.3f}, unset{};
  CHECK(!two_point_cal(a, b, &c, &why));     // points too close
  CHECK(std::string(why).find("close") != std::string::npos);
  CHECK(!two_point_cal(a, unset, &c, &why)); // second point missing
  CalPoint far{true, 10.0f, 30.0f};
  CHECK(!two_point_cal(a, far, &c, &why));   // gain 5: wrong DMM value typed
  CalPoint nan_pt{true, NAN, 1.0f};
  CHECK(!two_point_cal(a, nan_pt, &c, &why));
  CHECK_NEAR(c.gain, 1.5, 1e-9);             // untouched on failure
  LinCal ident;
  CHECK_NEAR(ident.apply(3.3f), 3.3, 1e-6);  // default is "no correction"
}

TEST(ina226_scaling_matches_datasheet) {
  Ina226Scale s;                             // 0.1 ohm, 100 uA/bit
  CHECK_EQ(ina226_cal_register(s), 512);     // 0.00512 / (0.0001 * 0.1)
  CHECK_NEAR(ina226_effective_current_lsb(s), 0.0001, 1e-9);
  CHECK_NEAR(ina226_bus_volts(4000), 5.0, 1e-6);          // 4000 * 1.25 mV
  CHECK_NEAR(ina226_current_amps(1000, s), 0.1, 1e-7);
  CHECK_NEAR(ina226_current_amps(-250, s), -0.025, 1e-7); // reverse current is signed
  s.shunt_ohm = 0.01f;
  s.current_lsb_a = 0.001f;
  CHECK_EQ(ina226_cal_register(s), 512);
  s.shunt_ohm = 0.0f;
  CHECK_EQ(ina226_cal_register(s), 0);       // unusable scale is refused
  s.shunt_ohm = 0.1f;
  s.current_lsb_a = 1e-9f;
  CHECK_EQ(ina226_cal_register(s), 65535);   // clamped
  s.shunt_ohm = 0.003f;
  s.current_lsb_a = 0.0001f;                 // CAL = 17066.67 -> 17067; effective LSB differs slightly
  CHECK_EQ(ina226_cal_register(s), 17067);
  CHECK_NEAR(ina226_effective_current_lsb(s) / 0.0001, 17066.67 / 17067.0, 1e-5);
}

TEST(ina226_convert_applies_calibration_and_gives_milliwatts) {
  InaChannelCal cal;
  InaReading r = ina226_convert(4000, 1000, cal);         // 5 V, 100 mA
  CHECK_NEAR(r.volts, 5.0, 1e-5);
  CHECK_NEAR(r.milliamps, 100.0, 1e-3);
  CHECK_NEAR(r.power_mw, 500.0, 1e-2);
  cal.volts.gain = 1.01f;
  cal.milliamps.offset = -0.5f;
  r = ina226_convert(4000, 1000, cal);
  CHECK_NEAR(r.volts, 5.05, 1e-5);
  CHECK_NEAR(r.milliamps, 99.5, 1e-3);
  CHECK_NEAR(r.power_mw, 5.05 * 99.5, 1e-2);
  CHECK_NEAR(r.raw_volts, 5.0, 1e-5);        // raw values stay available for `cal`
  CHECK_NEAR(r.raw_milliamps, 100.0, 1e-3);
}

TEST(telemetry_snapshot_reflects_setters) {
  Telemetry t;
  t.set_angles(1000, -12.5f, -11.0f, true);
  t.set_state(1000, State::TRK);
  t.set_fault(1000, 5);
  t.set_temperature(21.5f);
  const TelemetrySnapshot s = t.take(1500);
  CHECK(s.fresh);
  CHECK(s.panel_valid && s.sun_valid && s.temp_valid);
  CHECK_NEAR(s.panel_deg, -12.5, 1e-6);
  CHECK_NEAR(s.sun_deg, -11.0, 1e-6);
  CHECK(s.state == State::TRK);
  CHECK_EQ(s.fault, 5);
  CHECK_NEAR(s.temp_c, 21.5, 1e-6);
  CHECK(!s.motor_ran);
  CHECK(!s.has_run && !s.has_panel);
}

TEST(telemetry_stale_control_system_gives_unknown_angles_and_idle) {
  Telemetry t;
  TelemetrySnapshot s = t.take(0);
  CHECK(!s.fresh);                           // never called
  CHECK(!s.panel_valid);
  CHECK(s.state == State::IDL);
  t.set_angles(1000, 10.0f, 10.0f, true);
  t.set_state(1000, State::TRK);
  CHECK(t.take(6000).fresh);                 // exactly 5 s old: still accepted
  s = t.take(6001);
  CHECK(!s.fresh);
  CHECK(!s.panel_valid && !s.sun_valid);     // old angles are never reported as current
  CHECK(s.state == State::IDL);
  t.set_state(7000, State::HLD);
  CHECK(t.take(7001).fresh);
}

TEST(telemetry_motor_flag_is_latched_until_sampled) {
  Telemetry t;
  t.set_state(100, State::TRK);
  t.set_motor_running(100, true);
  t.set_motor_running(300, false);           // 200 ms burst between two samples
  CHECK(t.take(1000).motor_ran);
  CHECK(!t.take(2000).motor_ran);            // reported once
  t.set_motor_running(2500, true);
  CHECK(t.take(3000).motor_ran);
  CHECK(t.take(4000).motor_ran);             // still running
  t.set_motor_running(4500, false);
  CHECK(t.take(5000).motor_ran);             // ran during this second
  CHECK(!t.take(6000).motor_ran);
}

TEST(telemetry_sun_invalid_and_temperature_clear) {
  Telemetry t;
  t.set_angles(10, 5.0f, 99.0f, false);
  t.set_temperature(30.0f);
  t.clear_temperature();
  const TelemetrySnapshot s = t.take(20);
  CHECK(s.panel_valid);
  CHECK(!s.sun_valid);
  CHECK(!s.temp_valid);
}

TEST(telemetry_run_and_panel_requests_are_delivered_once) {
  Telemetry t;
  t.set_run(905);
  t.set_panel('b');
  TelemetrySnapshot s = t.take(0);
  CHECK(s.has_run && s.has_panel);
  CHECK_EQ(s.run, 905);
  CHECK_EQ(s.panel, 'B');
  s = t.take(1);
  CHECK(!s.has_run && !s.has_panel);
  t.set_panel('Z');                          // ignored
  t.set_run(5000);                           // clamped
  s = t.take(2);
  CHECK(!s.has_panel);
  CHECK_EQ(s.run, 999);
}

TEST(telemetry_out_of_range_inputs_are_made_safe) {
  Telemetry t;
  t.set_state(0, static_cast<State>(77));
  t.set_fault(0, 42);
  const TelemetrySnapshot s = t.take(1);
  CHECK(s.state == State::FLT);
  CHECK_EQ(s.fault, 0);
}

TEST(telemetry_free_functions_use_the_shared_instance) {
  telemetry_set_angles(3.0f, 4.0f, true);
  telemetry_set_state(State::STW);
  telemetry_set_motor_running(true);
  telemetry_set_motor_running(false);
  telemetry_set_fault(7);
  telemetry_set_temperature(18.0f);
  telemetry_set_run(3);
  telemetry_set_panel('B');
  const TelemetrySnapshot s = telemetry_instance().take(telemetry_now_ms());
  CHECK(s.fresh);
  CHECK(s.state == State::STW);
  CHECK(s.motor_ran);
  CHECK_EQ(s.fault, 7);
  CHECK_EQ(s.run, 3);
  CHECK_EQ(s.panel, 'B');
  telemetry_clear_temperature();
  telemetry_set_fault(0);
}

TEST(radio_meter_counts_sessions_and_today) {
  RadioMeter m;
  const int64_t t0 = ast_test::kT0 + 900;
  CHECK(!m.is_on());
  m.on(t0 + 12);
  m.on(t0 + 13);                             // second call is ignored
  CHECK(m.is_on());
  CHECK_EQ(m.session_seconds(t0 + 20), 8u);
  CHECK_EQ(m.today_seconds(t0 + 20), 8u);    // running session counts
  m.off(t0 + 21);
  m.off(t0 + 30);
  CHECK_EQ(m.last_session_seconds(), 9u);
  CHECK_EQ(m.session_seconds(t0 + 40), 0u);
  m.on(t0 + 912);
  m.off(t0 + 922);
  CHECK_EQ(m.today_seconds(t0 + 1000), 19u);
  CHECK_EQ(m.sessions(), 2u);
  CHECK_EQ(m.total_seconds(t0 + 1000), static_cast<uint64_t>(19));
}

TEST(radio_meter_today_resets_at_utc_midnight_and_splits_a_session) {
  RadioMeter m;
  const int64_t midnight = ast_test::kT0 + 86400;
  m.on(midnight - 100);
  m.off(midnight - 90);
  CHECK_EQ(m.today_seconds(midnight - 1), 10u);
  CHECK_EQ(m.today_seconds(midnight + 5), 0u);       // new day
  m.on(midnight + 86400 - 4);                        // session across the next midnight
  CHECK_EQ(m.today_seconds(midnight + 86400 + 6), 6u);
  m.off(midnight + 86400 + 6);
  CHECK_EQ(m.today_seconds(midnight + 86400 + 7), 6u);   // only the part after 00:00 counts for today
  CHECK_EQ(m.last_session_seconds(), 10u);
}

TEST(radio_policy_on_before_upload_off_in_between) {
  CHECK(!radio_needed(false, 600, true));            // 10 min to go: off
  CHECK(!radio_needed(false, 9, true));
  CHECK(radio_needed(false, 8, true));               // lead time reached: on
  CHECK(radio_needed(true, 0, true));                // upload due
  CHECK(radio_needed(true, 16, true));               // backlog batch in 16 s: stay on
  CHECK(!radio_needed(false, 16, true));             // ... but do not switch on that early
  CHECK(!radio_needed(true, 21, true));
  CHECK(!radio_needed(true, Uploader::kNeverDue, true));
  CHECK(radio_needed(false, Uploader::kNeverDue, false));   // no valid time: NTP needed
}

TEST(radio_is_on_for_seconds_per_upload_over_a_day) {
  // Whole-day run of the uploader with the radio policy: 96 uploads, the
  // radio is on for the lead time only.
  ast_test::FakeClock clock;
  ast_test::FakeTransport http(&clock);
  ast_test::VecSource src;
  Uploader up(src, http, clock);
  UploaderConfig c;
  c.channel = "1";
  c.write_key = "K";
  up.set_config(c);
  RadioMeter radio;
  for (int64_t s = 0; s < 86400; ++s) {
    if (s % 60 == 0 && s > 0) src.add(1);
    const bool need = radio_needed(radio.is_on(), up.seconds_until_due(), true);
    if (need) radio.on(clock.t);
    if (radio.is_on() || !need) up.tick();
    if (!need) radio.off(clock.t);
    clock.t += 1;
  }
  CHECK_EQ(http.posts.size(), static_cast<size_t>(95));      // 00:15:20 ... 23:45:20
  CHECK_EQ(radio.sessions(), 95u);
  CHECK(radio.total_seconds(clock.t) <= 95u * 10u);          // about 9 s each
  CHECK(radio.total_seconds(clock.t) >= 95u * 8u);
  for (const ast_test::Request& q : http.posts) CHECK_EQ(q.t % 60, 20);
}

TEST(host_http_url_parsing) {
  ast_host::ParsedUrl u;
  CHECK(ast_host::parse_http_url("http://127.0.0.1:18766/channels/1001/bulk_update.json", &u));
  CHECK_STR(u.host, "127.0.0.1");
  CHECK_EQ(u.port, 18766);
  CHECK_STR(u.path, "/channels/1001/bulk_update.json");
  CHECK(ast_host::parse_http_url("http://localhost", &u));
  CHECK_EQ(u.port, 80);
  CHECK_STR(u.path, "/");
  CHECK(ast_host::parse_http_url("http://h:8080/a?b=c", &u));
  CHECK_STR(u.path, "/a?b=c");
  CHECK(!ast_host::parse_http_url("https://api.thingspeak.com/x", &u));   // TLS is the ESP32 adapter's job
  CHECK(!ast_host::parse_http_url("http://:80/x", &u));
  CHECK(!ast_host::parse_http_url("http://h:99999/x", &u));
  CHECK(!ast_host::parse_http_url("http://h:8o/x", &u));
  CHECK(!ast_host::parse_http_url("ftp://h/x", &u));
}

TEST(host_http_response_parsing) {
  int status = 0;
  std::string body;
  CHECK(ast_host::parse_http_response("HTTP/1.0 202 Accepted\r\nContent-Length: 17\r\n\r\n{\"success\": true}", &status, &body));
  CHECK_EQ(status, 202);
  CHECK_STR(body, "{\"success\": true}");
  CHECK(ast_host::parse_http_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n{\"su\r\nc\r\nccess\":true}\r\n0\r\n\r\n", &status, &body));
  CHECK_STR(body, "{\"success\":true}");
  CHECK(ast_host::parse_http_response("HTTP/1.1 429 Too Many Requests\r\n\r\nslow down", &status, &body));
  CHECK_EQ(status, 429);
  CHECK_STR(body, "slow down");
  // Truncated or missing responses are "no response" (in doubt), never a success.
  CHECK(!ast_host::parse_http_response("", &status, &body));
  CHECK(!ast_host::parse_http_response("HTTP/1.1 202 Accepted\r\nContent-Le", &status, &body));
  CHECK(!ast_host::parse_http_response("HTTP/1.1 202 Accepted\r\nContent-Length: 17\r\n\r\n{\"succ", &status, &body));
  CHECK(!ast_host::parse_http_response("garbage\r\n\r\n", &status, &body));
  CHECK(!ast_host::parse_http_response("HTTP/1.1 2x2 Odd\r\n\r\n", &status, &body));
}
