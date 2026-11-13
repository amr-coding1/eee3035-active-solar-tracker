// DataLink: sampler + buffer + uploader + time validity working together
// (contract section 7 end to end, with fake storage/transport/clock).
#include <set>

#include "ast_datalink.h"
#include "ast_status.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ast;
using namespace ast_test;

namespace {

struct Device {
  MemStorage* storage;
  FakeClock clock;
  FakeTransport http{&clock};
  EventLog log;
  DataLink dl;
  Sample1Hz real = sample();
  std::vector<Record> logged;

  explicit Device(MemStorage* st, uint32_t capacity = kRingCapacity, uint32_t per_seg = kRecordsPerSegment,
                  int64_t start = kT0, bool rtc_ok = true)
      : storage(st), dl(*st, http, clock, &log, capacity, per_seg) {
    clock.t = start;
    dl.begin();
    UploaderConfig c;
    c.base_url = "http://mock";
    c.channel = "1001";
    c.write_key = "WRITEKEY1001";
    dl.uploader().set_config(c);
    dl.time_begin(true, !rtc_ok, start);
  }
  // Runs the device for `seconds` seconds: sampler then uploader, once per second.
  void run(int64_t seconds, bool upload = true) {
    for (int64_t i = 0; i < seconds; ++i) {
      Record r;
      if (dl.sampler_tick(clock.t, real, &r)) logged.push_back(r);
      if (upload) dl.uploader_tick();
      clock.t += 1;
    }
  }
  void offline() { http.post_handler = [](const Request&) { return FakeTransport::not_sent(); }; }
  void online() { http.post_handler = nullptr; }
};

std::string status_of(const Record& r) {
  char b[kStatusMax];
  build_status(r, b, sizeof b);
  return b;
}

std::vector<uint32_t> seqs_in(const std::string& body) {
  std::vector<uint32_t> v;
  size_t pos = 0;
  while ((pos = body.find("\"field8\":", pos)) != std::string::npos) {
    pos += 9;
    v.push_back(static_cast<uint32_t>(strtoul(body.c_str() + pos, nullptr, 10)));
  }
  return v;
}

}  // namespace

TEST(datalink_logs_one_record_per_minute_and_uploads_every_15_min) {
  MemStorage st;
  Device d(&st);
  d.run(31 * 60);
  CHECK_EQ(d.logged.size(), static_cast<size_t>(30));
  for (size_t i = 0; i < d.logged.size(); ++i) {
    CHECK_EQ(d.logged[i].seq, static_cast<uint32_t>(i + 1));
    CHECK_EQ(d.logged[i].minute_unix, static_cast<uint32_t>(kT0 + 60 * static_cast<int64_t>(i)));
    CHECK_EQ(d.logged[i].p_trk_dmw, 5000u);
    CHECK_STR(status_of(d.logged[i]), "TRK;r=0;p=A;e=0");
  }
  // 00:00:20 nothing yet; 00:15:20 minutes 00:00-00:14; 00:30:20 minutes 00:15-00:29.
  CHECK_EQ(d.http.posts.size(), static_cast<size_t>(2));
  CHECK_EQ(d.http.posts[0].t, kT0 + 920);
  CHECK_EQ(d.http.posts[1].t, kT0 + 1820);
  CHECK_EQ(seqs_in(d.http.posts[0].body).size(), static_cast<size_t>(15));
  CHECK(d.http.posts[0].body.find("\"created_at\":\"2026-11-20 00:00:00 +0000\"") != std::string::npos);
  CHECK(d.http.posts[0].body.find("\"created_at\":\"2026-11-20 00:14:00 +0000\"") != std::string::npos);
  CHECK(d.http.posts[1].body.find("\"created_at\":\"2026-11-20 00:15:00 +0000\"") != std::string::npos);
  CHECK(d.http.posts[0].body.find("\"field1\":500.0,\"field2\":400.0,\"field3\":120.0,\"field4\":-10.0,\"field7\":3.900,\"field8\":1,") != std::string::npos);
  CHECK_EQ(d.dl.pending(), 0u);
  // F8: a sample is on the server within 16 min of the end of its minute.
  CHECK(d.http.posts[0].t - (kT0 + 60) <= 16 * 60);
}

TEST(datalink_run_and_panel_appear_in_status) {
  MemStorage st;
  Device d(&st);
  d.dl.set_run(12);
  d.dl.set_panel('B');
  d.run(61);
  CHECK_STR(status_of(d.logged[0]), "TRK;r=12;p=B;e=0");
  d.dl.set_run(5000);                       // clamped to the contract range
  d.dl.set_panel('x');                      // ignored
  d.run(60);
  CHECK_STR(status_of(d.logged[1]), "TRK;r=999;p=B;e=0");
  CHECK_EQ(d.dl.run(), 999);
  CHECK_EQ(d.dl.panel(), 'B');
}

TEST(datalink_rtc_lost_means_no_logging_and_fault_4_until_ntp) {
  MemStorage st;
  Device d(&st, kRingCapacity, kRecordsPerSegment, kT0, /*rtc_ok=*/false);
  CHECK(!d.dl.time_valid());
  d.run(10 * 60);
  CHECK_EQ(d.logged.size(), static_cast<size_t>(0));         // nothing logged without valid time
  CHECK_EQ(d.dl.status().count, 0u);
  CHECK_EQ(d.dl.status().current_fault, kFaultClock);
  const std::vector<EventLog::Item> f = d.log.of(EventType::kFault);
  CHECK_EQ(f.size(), static_cast<size_t>(1));               // announced once
  CHECK_EQ(f[0].fault, 4);
  // NTP succeeds: the RTC must be written, logging resumes at once.
  CHECK(d.dl.time_on_ntp(d.clock.t, true, 946684800 /* RTC says year 2000 */));
  CHECK(d.dl.time_valid());
  d.run(3 * 60 + 1);
  CHECK_EQ(d.logged.size(), static_cast<size_t>(3));
  CHECK_EQ(d.logged[0].minute_unix, static_cast<uint32_t>(kT0 + 600));
  CHECK_EQ(d.logged[0].seq, 1u);
  CHECK_EQ(d.logged[0].fault, 4);           // the gap is visible in the data ...
  CHECK_EQ(d.logged[1].fault, 0);           // ... once
  CHECK_EQ(d.dl.status().current_fault, 0);
}

TEST(datalink_rtc_with_implausible_date_is_treated_as_lost) {
  MemStorage st;
  FakeClock clock;
  FakeTransport http(&clock);
  DataLink dl(st, http, clock);
  dl.begin();
  dl.time_begin(true, false, 946684800);    // flag clear but the year is 2000
  CHECK(!dl.time_valid());
  dl.time_begin(true, false, kMinPlausibleUnix);
  CHECK(dl.time_valid());
}

TEST(time_validity_rtc_is_set_only_when_more_than_2_s_off) {
  TimeValidity tv;
  tv.begin(true, false, kT0);
  CHECK(tv.valid());
  CHECK(tv.source() == TimeValidity::Source::kRtc);
  CHECK(!tv.on_ntp(kT0 + 100, true, kT0 + 100));     // identical
  CHECK(!tv.on_ntp(kT0 + 100, true, kT0 + 102));     // 2 s: inside the tolerance
  CHECK(!tv.on_ntp(kT0 + 100, true, kT0 + 98));
  CHECK(tv.on_ntp(kT0 + 100, true, kT0 + 103));      // 3 s fast
  CHECK(tv.on_ntp(kT0 + 100, true, kT0 + 97));       // 3 s slow
  CHECK(tv.source() == TimeValidity::Source::kNtp);
  CHECK(tv.ntp_synced());
  CHECK(!tv.on_ntp(0, true, kT0));                    // a zero "NTP time" is not an answer
}

TEST(time_validity_lost_rtc_is_always_written_from_ntp) {
  TimeValidity tv;
  tv.begin(true, true, kT0);                // oscillator-stop flag set, time happens to be right
  CHECK(!tv.valid());
  CHECK(tv.clock_fault());
  CHECK(tv.on_ntp(kT0 + 1, true, kT0 + 1));  // must clear the flag by writing the RTC
  CHECK(tv.valid());
  CHECK(!tv.clock_fault());
  CHECK(!tv.rtc_lost());
}

TEST(time_validity_manual_set) {
  TimeValidity tv;
  tv.begin(true, true, 0);
  CHECK(!tv.valid());
  tv.on_manual_set(true);
  CHECK(tv.valid());
  CHECK(tv.source() == TimeValidity::Source::kManual);
  CHECK(!tv.clock_fault());
}

TEST(datalink_rtc_missing_logs_after_ntp_but_keeps_fault_4) {
  MemStorage st;
  Device d(&st);
  d.dl.time_begin(false, false, 0);         // no DS3231 on the bus
  CHECK(!d.dl.time_valid());
  d.run(120);
  CHECK_EQ(d.logged.size(), static_cast<size_t>(0));
  CHECK(!d.dl.time_on_ntp(d.clock.t, false, 0));   // nothing to write
  d.run(181);
  CHECK_EQ(d.logged.size(), static_cast<size_t>(3));
  for (const Record& r : d.logged) CHECK_EQ(r.fault, 4);     // "clock (RTC) missing" stays visible
  CHECK(d.dl.status().rtc_missing);
}

TEST(datalink_buffer_overflow_raises_fault_6_on_the_next_sample) {
  MemStorage st;
  Device d(&st, 5, 4);
  d.offline();
  d.run(5 * 60 + 1, /*upload=*/false);      // 5 records: full, nothing lost yet
  CHECK_EQ(d.dl.status().lost_total, 0u);
  d.run(60, false);                         // 6th record overwrites the oldest unsent
  CHECK_EQ(d.logged[5].fault, 0);
  CHECK_EQ(d.dl.status().lost_total, 1u);
  CHECK_EQ(d.dl.status().current_fault, kFaultOverflow);
  d.run(60, false);
  CHECK_EQ(d.logged[6].fault, 6);           // the NEXT sample says so
  CHECK_STR(status_of(d.logged[6]), "TRK;r=0;p=A;e=6");
  const std::vector<EventLog::Item> f = d.log.of(EventType::kFault);
  CHECK(f.size() >= 1u);
  CHECK_EQ(f[0].fault, 6);
  CHECK_STR(f[0].reason, "buffer_overflow_oldest_unsent_overwritten");
  // Make room: upload everything; the fault goes away.
  d.online();
  d.dl.uploader().request_now();
  d.run(60);
  CHECK_EQ(d.logged.back().fault, 6);       // reports the overflow caused by record 7
  d.run(60);
  CHECK_EQ(d.logged.back().fault, 0);
  CHECK_EQ(d.dl.status().lost_total, 2u);
}

TEST(datalink_fault_9_appears_in_samples_after_60_min_offline_and_clears) {
  MemStorage st;
  Device d(&st);
  d.offline();
  d.run(63 * 60);
  CHECK_EQ(d.logged[55].fault, 0);
  // First upload attempt (and failure) at 00:15:20; the reference is boot (00:00:00),
  // so fault 9 is raised at 01:00:00 and the minute logged at 01:01:00 carries it.
  CHECK_EQ(d.logged[59].fault, 0);          // minute 00:59, logged at 01:00:00 before the uploader tick
  CHECK_EQ(d.logged[60].fault, 9);
  CHECK_STR(status_of(d.logged[61]), "TRK;r=0;p=A;e=9");
  d.online();
  d.run(16 * 60);
  CHECK_EQ(d.logged.back().fault, 0);
  CHECK_EQ(d.dl.pending() <= 15u, true);
}

TEST(datalink_control_fault_wins_but_overflow_is_reported_afterwards) {
  MemStorage st;
  Device d(&st, 3, 4);
  d.run(4 * 60 + 1, false);                 // 4 records into a 3-record buffer: one lost
  CHECK_EQ(d.dl.status().lost_total, 1u);
  // From here on the operator keeps uploading, so nothing more overflows.
  auto run_uploading = [&](int seconds) {
    for (int i = 0; i < seconds; ++i) { d.dl.uploader().request_now(); d.run(1); }
  };
  run_uploading(2);
  d.real.ctrl_fault = 8;                    // motor stall reported by the control system
  d.real.state = State::FLT;
  run_uploading(120);
  CHECK_EQ(d.logged.back().fault, 8);       // control code wins ...
  CHECK(d.dl.status().overflow_pending);    // ... and the overflow is not forgotten
  d.real.ctrl_fault = 0;
  d.real.state = State::TRK;
  run_uploading(120);
  CHECK_EQ(d.dl.status().lost_total, 1u);
  CHECK_EQ(d.logged[d.logged.size() - 2].fault, 8);   // minute in which the fault was still seen
  CHECK_EQ(d.logged.back().fault, 6);
  CHECK(!d.dl.status().overflow_pending);
}

TEST(datalink_missing_power_sensor_gives_fault_code_and_omits_field) {
  MemStorage st;
  Device d(&st);
  d.real.p_fix_valid = false;
  d.run(15 * 60 + 30);
  CHECK_EQ(d.logged[0].fault, 2);
  CHECK(!d.logged[0].has(kHasPfix));
  CHECK_EQ(d.http.posts.size(), static_cast<size_t>(1));
  CHECK_EQ(count_of(d.http.posts[0].body, "\"field2\""), static_cast<size_t>(0));
  CHECK_EQ(count_of(d.http.posts[0].body, "\"field1\""), static_cast<size_t>(15));
  CHECK(d.http.posts[0].body.find("\"status\":\"TRK;r=0;p=A;e=2\"") != std::string::npos);
}

TEST(datalink_simulator_marks_every_sample_and_ignores_real_inputs) {
  MemStorage st;
  Device d(&st, kRingCapacity, kRecordsPerSegment, 1795176000 /* 2026-11-20 12:00 */);
  d.real = sample(99999.0f, 88888.0f, 77777.0f);   // absurd "real" readings
  d.dl.set_sim(true);
  d.run(16 * 60);
  CHECK_EQ(d.logged.size(), static_cast<size_t>(15));
  for (const Record& r : d.logged) {
    CHECK(r.has(kSimulated));
    CHECK(r.p_trk_dmw < 50000u);            // nothing of the real inputs leaked in
    CHECK(status_of(r).find(";s=1") != std::string::npos);
  }
  CHECK_EQ(count_of(d.http.posts[0].body, ";s=1\"}"), seqs_in(d.http.posts[0].body).size());
  CHECK(d.dl.sim_on());
}

TEST(datalink_simulator_is_off_by_default_and_real_samples_have_no_marker) {
  MemStorage st;
  Device d(&st);
  CHECK(!d.dl.sim_on());
  d.run(121);
  for (const Record& r : d.logged) CHECK(!r.has(kSimulated));
}

TEST(datalink_minute_with_mixed_sources_is_marked_simulated) {
  MemStorage st;
  Device d(&st);
  d.run(30);
  d.dl.set_sim(true);                       // switched on mid-minute
  d.run(60);
  d.dl.set_sim(false);
  d.run(91);
  CHECK_EQ(d.logged.size(), static_cast<size_t>(3));
  CHECK(d.logged[0].has(kSimulated));       // 30 real + 30 simulated seconds: flagged
  CHECK(d.logged[1].has(kSimulated));
  CHECK(!d.logged[2].has(kSimulated));      // purely real minute
}

TEST(datalink_clock_stepping_back_never_logs_a_minute_twice) {
  MemStorage st;
  Device d(&st);
  d.run(10 * 60 + 30, false);               // minutes 00:00..00:09 logged, 00:10 in progress
  CHECK_EQ(d.logged.size(), static_cast<size_t>(10));
  d.clock.t = kT0 + 5 * 60 + 10;            // NTP correction: 5 minutes back
  d.run(5 * 60 + 40, false);                // re-lives 00:05:10 .. 00:10:50
  CHECK_EQ(d.logged.size(), static_cast<size_t>(10));        // nothing new for old minutes
  const std::vector<EventLog::Item> sk = d.log.of(EventType::kSkip);
  CHECK_EQ(sk.size(), static_cast<size_t>(5));               // minutes 00:05..00:09 refused
  CHECK_EQ(sk[0].minute, static_cast<uint32_t>(kT0 + 300));
  CHECK_STR(sk[0].reason, "minute_not_after_last_logged");
  CHECK_EQ(d.dl.status().skipped_minutes, 5u);
  d.run(120, false);
  CHECK_EQ(d.logged.size(), static_cast<size_t>(12));        // 00:10 and 00:11 follow normally
  CHECK_EQ(d.logged[10].minute_unix, static_cast<uint32_t>(kT0 + 600));
  CHECK_EQ(d.logged[10].seq, 11u);          // no seq wasted on the skipped minutes
  // Timestamps strictly increasing and unique over everything stored.
  std::vector<Record> all(64);
  const size_t n = d.dl.dump_newest(all.data(), all.size());
  CHECK_EQ(n, static_cast<size_t>(12));
  for (size_t i = 1; i < n; ++i) CHECK(all[i].minute_unix > all[i - 1].minute_unix);
}

TEST(datalink_reboot_inside_the_same_minute_does_not_duplicate_it) {
  MemStorage st;
  {
    Device d(&st);
    d.run(3 * 60 + 2, false);               // minutes 0,1,2 logged; crash at 00:03:02
    CHECK_EQ(d.logged.size(), static_cast<size_t>(3));
  }
  {
    // The RTC is 70 s slow after the restart (it re-enters minute 00:01 at 00:01:52).
    Device d(&st, kRingCapacity, kRecordsPerSegment, kT0 + 3 * 60 + 2 - 70);
    d.run(70 + 120, false);
    CHECK_EQ(d.log.count(EventType::kSkip), 2);              // the second "00:01" and "00:02" were refused
    CHECK_EQ(d.logged.size(), static_cast<size_t>(2));       // 00:03 and 00:04
    CHECK_EQ(d.logged[0].minute_unix, static_cast<uint32_t>(kT0 + 180));
    CHECK_EQ(d.logged[0].seq, 4u);
  }
}

TEST(datalink_state_survives_restart_and_pending_records_are_uploaded_later) {
  MemStorage st;
  {
    Device d(&st);
    d.offline();
    d.run(20 * 60 + 5);
    CHECK_EQ(d.dl.pending(), 20u);
  }
  Device d(&st, kRingCapacity, kRecordsPerSegment, kT0 + 20 * 60 + 5);
  CHECK_EQ(d.dl.pending(), 20u);
  CHECK(d.dl.status().has_last);
  CHECK_EQ(d.dl.status().last.seq, 20u);
  d.run(11 * 60);                           // next slot 00:30:20
  CHECK_EQ(d.http.posts.size(), static_cast<size_t>(1));
  const std::vector<uint32_t> s = seqs_in(d.http.posts[0].body);
  CHECK_EQ(s.front(), 1u);
  CHECK_EQ(s.back(), 30u);                  // 20 old + minutes 00:20..00:29 (00:20 restarted mid-minute)
  CHECK_EQ(s.size(), static_cast<size_t>(30));
}

TEST(datalink_seven_day_outage_loses_nothing_and_backfills_in_order) {
  MemStorage st;
  Device d(&st);
  d.offline();
  const int64_t week = 7 * 24 * 3600;
  // One sample at the start and one at the end of each minute is enough to
  // produce the minute record; the uploader keeps trying (and failing).
  for (int64_t t = kT0; t < kT0 + week; t += 60) {
    d.clock.t = t;
    d.dl.sampler_tick(t, d.real);
    d.dl.uploader_tick();
    d.clock.t = t + 59;
    d.dl.sampler_tick(t + 59, d.real);
    d.dl.uploader_tick();
  }
  d.clock.t = kT0 + week;
  DataLinkStatus s = d.dl.status();
  CHECK_EQ(s.pending, 10079u);              // the last minute is still being accumulated
  CHECK_EQ(s.lost_total, 0u);               // 8-day buffer: a 7-day outage loses nothing
  CHECK(s.upload.fault9);
  d.online();
  std::multiset<uint32_t> got;
  std::vector<int64_t> times;
  d.http.posts.clear();
  d.run(3600);
  for (const Request& q : d.http.posts) {
    times.push_back(q.t);
    uint32_t prev = 0;
    for (uint32_t v : seqs_in(q.body)) {
      CHECK(v > prev);                      // oldest first inside a batch
      prev = v;
      got.insert(v);
    }
    CHECK(seqs_in(q.body).size() <= kMaxBatch);
  }
  CHECK(got.size() >= 10079u);
  for (uint32_t v = 1; v <= 10079; ++v) {
    if (got.count(v) != 1) { CHECK_EQ(got.count(v), static_cast<size_t>(1)); break; }
  }
  for (size_t i = 1; i < 11 && i < times.size(); ++i) CHECK(times[i] - times[i - 1] >= 16);
  CHECK(times.size() >= 11u);               // 10 full batches + remainder
  CHECK(times[10] - times[0] <= 15 * 60);   // the whole week is on the server within 15 min
  CHECK(!d.dl.status().upload.fault9);
}

TEST(datalink_ninth_day_of_outage_overwrites_oldest_and_says_so) {
  MemStorage st;
  Device d(&st, 60, 12);                    // small buffer: same logic, quicker test
  d.offline();
  for (int64_t t = kT0; t < kT0 + 100 * 60; t += 60) {
    d.clock.t = t;
    Record r;
    if (d.dl.sampler_tick(t, d.real, &r)) d.logged.push_back(r);
    d.clock.t = t + 59;
    d.dl.sampler_tick(t + 59, d.real);
  }
  DataLinkStatus s = d.dl.status();
  CHECK_EQ(s.count, 60u);
  CHECK_EQ(s.lost_total, 39u);              // 99 logged, 60 kept
  CHECK_EQ(d.logged.back().fault, 6);
  d.online();
  d.clock.t = kT0 + 100 * 60;
  d.dl.uploader().request_now();
  d.run(5);                                 // (logs minute 99 = seq 100, pushing out seq 40)
  CHECK_EQ(seqs_in(d.http.posts.back().body).front(), 41u);  // oldest surviving record first
  CHECK_EQ(seqs_in(d.http.posts.back().body).back(), 100u);
}

TEST(datalink_corrupt_record_in_flash_is_skipped_by_the_uploader) {
  MemStorage st;
  Device d(&st, 16, 4);
  d.run(6 * 60 + 1, false);
  st.files[0][1 * kRecordSize + 20] ^= 0x10;                 // damage seq 2
  d.dl.uploader().request_now();
  d.run(3);
  CHECK_EQ(d.http.posts.size(), static_cast<size_t>(1));
  const std::vector<uint32_t> s = seqs_in(d.http.posts[0].body);
  CHECK_EQ(s.size(), static_cast<size_t>(5));
  CHECK_EQ(s[0], 1u);
  CHECK_EQ(s[1], 3u);
  CHECK_EQ(d.dl.pending(), 0u);
  CHECK_EQ(d.dl.status().crc_errors, 1u);
}

TEST(datalink_storage_failure_is_reported_as_fault_6_on_next_stored_sample) {
  MemStorage st;
  Device d(&st);
  d.run(61, false);
  st.fail_all_writes = true;
  d.run(60, false);                         // this minute cannot be stored
  CHECK_EQ(d.logged.size(), static_cast<size_t>(1));
  CHECK_EQ(d.dl.status().storage_errors, 1u);
  st.power_restored();
  d.run(60, false);
  CHECK_EQ(d.logged.size(), static_cast<size_t>(2));
  CHECK_EQ(d.logged[1].fault, 6);           // data was lost: visible, never silent
  CHECK_EQ(d.logged[1].seq, 2u);
  d.run(60, false);
  CHECK_EQ(d.logged[2].fault, 0);
}

TEST(datalink_flush_stores_the_last_complete_minute_on_shutdown) {
  MemStorage st;
  Device d(&st);
  d.run(120, false);                        // t = 00:00:00 .. 00:01:59
  CHECK_EQ(d.logged.size(), static_cast<size_t>(1));
  Record r;
  CHECK(d.dl.flush(kT0 + 120, &r));
  CHECK_EQ(r.minute_unix, static_cast<uint32_t>(kT0 + 60));
  CHECK(!d.dl.flush(kT0 + 120, &r));
  CHECK_EQ(d.dl.status().count, 2u);
}

TEST(datalink_clear_buffer_keeps_seq) {
  MemStorage st;
  Device d(&st);
  d.run(5 * 60 + 1, false);
  CHECK(d.dl.clear_buffer());
  CHECK_EQ(d.dl.pending(), 0u);
  CHECK_EQ(d.dl.status().next_seq, 6u);
  d.run(60, false);
  CHECK_EQ(d.logged.back().seq, 6u);
}

TEST(datalink_status_snapshot_is_consistent) {
  MemStorage st;
  Device d(&st);
  d.dl.set_run(7);
  d.run(16 * 60);
  const DataLinkStatus s = d.dl.status();
  CHECK(s.time_valid);
  CHECK(!s.sim_on);
  CHECK_EQ(s.run, 7);
  CHECK_EQ(s.capacity, 11520u);
  CHECK_EQ(s.logged, 15u);
  CHECK_EQ(s.count, 15u);
  CHECK_EQ(s.pending, 0u);
  CHECK_EQ(s.next_seq, 16u);
  CHECK_EQ(s.upload.records_ok, 15u);
  CHECK_EQ(s.last.seq, 15u);
  CHECK_EQ(s.last_minute, static_cast<uint32_t>(kT0 + 14 * 60));
}
