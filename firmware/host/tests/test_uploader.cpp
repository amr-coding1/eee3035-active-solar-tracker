// Upload state machine (contract 7.3) against a scripted transport.
#include <set>

#include "ast_uploader.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ast;
using namespace ast_test;

namespace {

struct Rig {
  FakeClock clock;
  FakeTransport http{&clock};
  VecSource src;
  EventLog log;
  Uploader up{src, http, clock, &log};

  explicit Rig(const char* read_key = "") {
    UploaderConfig c;
    c.base_url = "http://mock";
    c.channel = "1001";
    c.write_key = "WRITEKEY1001";
    c.read_key = read_key;
    up.set_config(c);
  }
  // One tick per second for `seconds` seconds.
  void run(int64_t seconds) {
    for (int64_t i = 0; i < seconds; ++i) {
      up.tick();
      clock.t += 1;
    }
  }
  void run_until(int64_t t) { while (clock.t < t) { up.tick(); clock.t += 1; } }
};

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

TEST(uploader_slot_times_are_second_20_of_every_upload_min) {
  CHECK_EQ(latest_slot(kT0 + 20, 15), kT0 + 20);
  CHECK_EQ(latest_slot(kT0 + 19, 15), kT0 - 900 + 20);
  CHECK_EQ(latest_slot(kT0 + 899, 15), kT0 + 20);
  CHECK_EQ(latest_slot(kT0 + 920, 15), kT0 + 920);
  CHECK_EQ(latest_slot(kT0 + 3599, 60), kT0 + 20);
  CHECK_EQ(latest_slot(kT0 + 320, 5), kT0 + 320);
  for (int m = 5; m <= 60; ++m) {
    for (int64_t t = kT0; t < kT0 + 7200; t += 37) {
      const int64_t s = latest_slot(t, m);
      CHECK(s <= t && t - s < m * 60);
      CHECK_EQ(s % 60, 20);
      CHECK_EQ((s / 60) % m, 0);
    }
  }
  CHECK_EQ(latest_slot(kT0 + 20, 0), kT0 + 20);   // invalid interval falls back to 15
}

TEST(uploader_backoff_sequence_1_2_4_8_15_minutes) {
  const int want[] = {60, 120, 240, 480, 900, 900, 900};
  for (int i = 0; i < 7; ++i) CHECK_EQ(backoff_seconds(i + 1), want[i]);
  CHECK_EQ(backoff_seconds(0), 60);
  CHECK_EQ(backoff_seconds(1000), 900);
}

TEST(uploader_success_body_detection) {
  CHECK(body_reports_success("{\"success\":true}"));
  CHECK(body_reports_success("{\"success\": true}"));          // python json.dumps style
  CHECK(body_reports_success("{ \"channel_id\": 1, \"success\" :\ttrue }"));
  CHECK(!body_reports_success("{\"success\":false}"));
  CHECK(!body_reports_success("{\"status\":\"401\",\"error\":{}}"));
  CHECK(!body_reports_success(""));
  CHECK(!body_reports_success("success true"));
  CHECK(!body_reports_success("{\"success\":\"true\"}"));        // a string is not the boolean
  CHECK(body_reports_success("{\"success\":false,\"x\":{\"success\":true}}"));
}

TEST(uploader_last_field8_parsing) {
  uint32_t v = 0;
  CHECK(parse_last_field8("{\"created_at\":\"2026-11-20T00:14:00Z\",\"entry_id\":15,\"field8\":\"15\"}", &v));
  CHECK_EQ(v, 15u);
  CHECK(parse_last_field8("{\"field8\": 4294967295}", &v));
  CHECK_EQ(v, 4294967295u);
  CHECK(parse_last_field8("{\"field8\":\"77.0\"}", &v));
  CHECK_EQ(v, 77u);
  v = 5;
  CHECK(!parse_last_field8("-1", &v));
  CHECK(!parse_last_field8("{\"field8\":null}", &v));
  CHECK(!parse_last_field8("{\"field8\":\"\"}", &v));
  CHECK(!parse_last_field8("{\"field8\":\"12abc\"}", &v));
  CHECK(!parse_last_field8("{\"field8\":\"4294967296\"}", &v));   // overflow
  CHECK(!parse_last_field8("{\"field7\":\"3.9\"}", &v));
  CHECK_EQ(v, 5u);
}

TEST(uploader_posts_every_15_min_at_second_20_with_exact_url) {
  Rig r;
  for (int q = 0; q < 4; ++q) {
    r.src.add(15, kT0 + q * 900);
    r.run_until(kT0 + (q + 1) * 900);
  }
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(4));
  for (size_t i = 0; i < r.http.posts.size(); ++i) {
    CHECK_EQ(r.http.posts[i].t, kT0 + 20 + static_cast<int64_t>(i) * 900);
    CHECK_STR(r.http.posts[i].url, "http://mock/channels/1001/bulk_update.json");
    CHECK_EQ(seqs_in(r.http.posts[i].body).size(), static_cast<size_t>(15));
  }
  CHECK(r.http.posts[0].body.rfind("{\"write_api_key\":\"WRITEKEY1001\",\"updates\":[{\"created_at\":\"2026-11-20 00:00:00 +0000\"", 0) == 0);
  CHECK(r.http.declared_size_ok);          // Content-Length matched the streamed body
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(r.log.count(EventType::kUploadOk), 4);
  CHECK_EQ(r.http.gets.size(), static_cast<size_t>(0));
}

TEST(uploader_sends_nothing_when_nothing_is_pending) {
  Rig r;
  r.run(3600);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(0));
  // A record that appears after a slot waits for the NEXT slot (no stale firing).
  r.src.add(1);
  r.run(1);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(0));
  r.run_until(kT0 + 3600 + 30);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(1));
  CHECK_EQ(r.http.posts[0].t, kT0 + 3600 + 20);
}

TEST(uploader_interval_5_and_60_minutes) {
  {
    Rig r;
    r.up.set_upload_min(5);
    for (int q = 0; q < 6; ++q) { r.src.add(5); r.run_until(kT0 + (q + 1) * 300); }
    CHECK_EQ(r.http.posts.size(), static_cast<size_t>(6));
    for (size_t i = 0; i < 6; ++i) CHECK_EQ(r.http.posts[i].t, kT0 + 20 + static_cast<int64_t>(i) * 300);
  }
  {
    Rig r;
    r.up.set_upload_min(60);
    r.src.add(60);
    r.run(3 * 3600);
    CHECK_EQ(r.http.posts.size(), static_cast<size_t>(1));
    CHECK_EQ(r.http.posts[0].t, kT0 + 20);
  }
}

TEST(uploader_upload_min_limits) {
  Rig r;
  CHECK(!r.up.set_upload_min(4));
  CHECK(!r.up.set_upload_min(61));
  CHECK(r.up.set_upload_min(5));
  CHECK(r.up.set_upload_min(60));
  CHECK_EQ(r.up.config().upload_min, 60);
  CHECK(upload_min_is_valid(15));
  UploaderConfig c = r.up.config();
  c.upload_min = 99;                        // an invalid value in a config keeps the old one
  c.base_url = "http://mock///";
  r.up.set_config(c);
  CHECK_EQ(r.up.config().upload_min, 60);
  CHECK_STR(r.up.config().base_url, "http://mock");
}

TEST(uploader_backlog_split_into_960_with_16_s_spacing) {
  Rig r;
  r.src.add(2000);
  r.run(200);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(3));
  CHECK_EQ(seqs_in(r.http.posts[0].body).size(), static_cast<size_t>(960));
  CHECK_EQ(seqs_in(r.http.posts[1].body).size(), static_cast<size_t>(960));
  CHECK_EQ(seqs_in(r.http.posts[2].body).size(), static_cast<size_t>(80));
  CHECK_EQ(r.http.posts[0].t, kT0 + 20);
  CHECK_EQ(r.http.posts[1].t, kT0 + 36);
  CHECK_EQ(r.http.posts[2].t, kT0 + 52);
  // Oldest first, no gaps, no overlaps.
  CHECK_EQ(seqs_in(r.http.posts[0].body).front(), 1u);
  CHECK_EQ(seqs_in(r.http.posts[1].body).front(), 961u);
  CHECK_EQ(seqs_in(r.http.posts[2].body).back(), 2000u);
  CHECK_EQ(r.src.pending(), 0u);
}

TEST(uploader_exactly_960_pending_is_one_request) {
  Rig r;
  r.src.add(960);
  r.run(200);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(1));
}

TEST(uploader_spacing_is_measured_from_the_end_of_the_previous_request) {
  Rig r;
  r.http.post_takes_s = 10;                 // slow link: each POST takes 10 s
  r.src.add(1000);
  r.run(200);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(2));
  CHECK(r.http.posts[1].t >= r.http.posts[0].t + 10 + 16);
}

TEST(uploader_record_arriving_during_upload_waits_for_next_slot) {
  Rig r;
  r.src.add(15);
  r.http.post_handler = [&](const Request&) {
    if (r.src.recs.size() == 15) r.src.add(1);     // the sampler logs a record meanwhile
    return FakeTransport::ok();
  };
  r.run(600);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(1));   // no extra 1-record request 16 s later
  CHECK_EQ(r.src.pending(), 1u);
  r.run_until(kT0 + 930);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(2));
  CHECK_EQ(r.http.posts[1].t, kT0 + 920);
}

TEST(uploader_connect_failure_retries_with_backoff_1_2_4_8_15_15) {
  Rig r;
  r.src.add(15);
  r.http.post_handler = [](const Request&) { return FakeTransport::not_sent(); };
  r.run(4000);
  const int64_t want[] = {20, 80, 200, 440, 920, 1820, 2720, 3620};
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(8));
  for (size_t i = 0; i < r.http.posts.size() && i < 8; ++i) CHECK_EQ(r.http.posts[i].t - kT0, want[i]);
  CHECK_EQ(r.src.pending(), 15u);           // records stay in the buffer
  const std::vector<EventLog::Item> b = r.log.of(EventType::kBackoff);
  CHECK_EQ(b.size(), static_cast<size_t>(8));
  CHECK_EQ(b[0].delay_s, 60);
  CHECK_EQ(b[4].delay_s, 900);
  CHECK_EQ(b[7].delay_s, 900);
  CHECK_STR(r.log.of(EventType::kUploadFail)[0].reason, "not_sent");
}

TEST(uploader_http_500_and_429_retry_the_same_batch) {
  for (int status : {500, 503, 429, 302}) {
    Rig r;
    r.src.add(15);
    r.http.post_script.push_back(FakeTransport::response(status, "{\"status\":\"x\"}"));
    r.run(100);
    CHECK_EQ(r.http.posts.size(), static_cast<size_t>(2));
    CHECK_EQ(r.http.posts[1].t, kT0 + 80);
    CHECK(r.http.posts[0].body == r.http.posts[1].body);
    CHECK_EQ(r.src.pending(), 0u);
    CHECK_STR(r.log.of(EventType::kUploadFail)[0].reason, "http_status");
    CHECK_EQ(r.log.of(EventType::kUploadFail)[0].http, status);
  }
}

TEST(uploader_backoff_resets_after_a_success) {
  Rig r;
  r.src.add(15);
  for (int i = 0; i < 3; ++i) r.http.post_script.push_back(FakeTransport::not_sent());
  r.run(900);                               // fails at 20, 80, 200; succeeds at 440
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(4));
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(r.up.stats().fail_count, 0);
  r.src.add(15);
  r.http.post_script.push_back(FakeTransport::not_sent());
  r.run(200);                               // next slot kT0+920 fails -> retry after 60 s again
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(6));
  CHECK_EQ(r.http.posts[4].t, kT0 + 920);
  CHECK_EQ(r.http.posts[5].t, kT0 + 980);
}

TEST(uploader_http_200_with_success_true_is_accepted_like_202) {
  Rig r;
  r.src.add(3);
  r.http.post_script.push_back(FakeTransport::response(200, "{\"success\": true}"));
  r.run(30);
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(r.log.count(EventType::kUploadOk), 1);
}

TEST(uploader_fault_9_after_60_min_without_success_while_pending) {
  Rig r;
  r.src.add(15);
  r.http.post_handler = [](const Request&) { return FakeTransport::not_sent(); };
  r.run_until(kT0 + 3600);                  // ticks up to kT0+3599 done
  CHECK(!r.up.fault9());
  r.run(1);                                 // tick at exactly 60 min
  CHECK(r.up.fault9());
  CHECK(r.up.stats().fault9);
  r.run(3600);
  const std::vector<EventLog::Item> f = r.log.of(EventType::kFault);
  CHECK_EQ(f.size(), static_cast<size_t>(1));   // raised once, not every tick
  CHECK_EQ(f[0].fault, 9);
  CHECK_EQ(f[0].t, kT0 + 3600);
  // First success clears it.
  r.http.post_handler = nullptr;
  r.run(1000);
  CHECK(!r.up.fault9());
  CHECK_EQ(r.src.pending(), 0u);
}

TEST(uploader_fault_9_counts_from_the_last_success) {
  Rig r;
  r.src.add(15);
  r.run(100);                               // success at kT0+20
  CHECK_EQ(r.up.stats().last_success, kT0 + 20);
  r.http.post_handler = [](const Request&) { return FakeTransport::not_sent(); };
  r.src.add(15);
  r.run_until(kT0 + 20 + 3600);
  CHECK(!r.up.fault9());
  r.run(1);
  CHECK(r.up.fault9());
}

TEST(uploader_no_fault_9_when_buffer_is_empty_or_uploads_work) {
  {
    Rig r;                                  // offline but nothing to send
    r.http.post_handler = [](const Request&) { return FakeTransport::not_sent(); };
    r.run(3 * 3600);
    CHECK(!r.up.fault9());
  }
  {
    Rig r;                                  // hourly uploads that work: never "failing"
    r.up.set_upload_min(60);
    for (int h = 0; h < 5; ++h) {
      for (int m = 0; m < 60; ++m) { r.src.add(1); r.run(60); CHECK(!r.up.fault9()); }
    }
    CHECK_EQ(r.http.posts.size(), static_cast<size_t>(5));
  }
}

TEST(uploader_not_configured_sends_nothing_and_raises_fault_9_after_60_min) {
  Rig r;
  UploaderConfig c = r.up.config();
  c.write_key = "";
  r.up.set_config(c);
  r.src.add(5);
  CHECK(r.up.tick() == Uploader::Tick::kNotConfigured);
  r.run(3599);
  CHECK(!r.up.fault9());
  r.run(2);
  CHECK(r.up.fault9());
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(0));
  CHECK(!r.up.stats().configured);
}

TEST(uploader_lost_response_resends_same_batch_without_read_key) {
  Rig r;
  r.src.add(15);
  r.http.post_script.push_back(FakeTransport::lost());
  r.run(100);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(2));
  CHECK_EQ(r.http.gets.size(), static_cast<size_t>(0));      // no read key -> no read-back
  CHECK(r.http.posts[0].body == r.http.posts[1].body);       // duplicates: readers de-duplicate by seq
  CHECK_EQ(r.http.posts[1].t, kT0 + 80);
  CHECK_EQ(r.log.count(EventType::kInDoubt), 1);
  CHECK_EQ(r.src.pending(), 0u);
  CHECK(!r.up.stats().in_doubt);
}

TEST(uploader_readback_confirms_whole_batch_and_skips_the_resend) {
  Rig r("READKEY1001");
  r.src.add(15);
  r.http.post_script.push_back(FakeTransport::lost());
  r.http.get_script.push_back(FakeTransport::response(200, "{\"created_at\":\"2026-11-20T00:14:00Z\",\"entry_id\":15,\"field8\":\"15\"}"));
  r.run(600);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(1));     // never sent twice
  CHECK_EQ(r.http.gets.size(), static_cast<size_t>(1));
  CHECK_STR(r.http.gets[0].url, "http://mock/channels/1001/fields/8/last.json?api_key=READKEY1001");
  CHECK(r.http.gets[0].t >= r.http.posts[0].t + 30);         // >= 30 s after the doubtful request
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(r.up.stats().readback_skipped, 15u);
  CHECK_EQ(r.up.stats().fail_count, 0);
  const std::vector<EventLog::Item> ok = r.log.of(EventType::kUploadOk);
  CHECK_EQ(ok.size(), static_cast<size_t>(1));
  CHECK_STR(ok[0].reason, "confirmed_by_readback");
}

TEST(uploader_readback_partial_resends_only_the_missing_records) {
  Rig r("READKEY1001");
  r.src.add(15);
  r.http.post_script.push_back(FakeTransport::lost());
  r.http.get_script.push_back(FakeTransport::response(200, "{\"field8\":\"10\"}"));
  r.run(600);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(2));
  const std::vector<uint32_t> s = seqs_in(r.http.posts[1].body);
  CHECK_EQ(s.size(), static_cast<size_t>(5));
  CHECK_EQ(s.front(), 11u);
  CHECK_EQ(s.back(), 15u);
  CHECK_EQ(r.src.pending(), 0u);
}

TEST(uploader_readback_that_proves_nothing_resends_everything) {
  // Older seq, unreadable answer, HTTP error, GET failure, or a seq beyond the
  // batch (another device / erased buffer): always re-send the whole batch.
  const ast::HttpResult answers[] = {
      FakeTransport::response(200, "{\"field8\":\"99\"}"),      // batch is 101..115
      FakeTransport::response(200, "-1"),
      FakeTransport::response(400, "-1"),
      FakeTransport::not_sent(),
      FakeTransport::response(200, "{\"field8\":\"5000\"}"),
      FakeTransport::response(200, "{\"field8\":null}"),
  };
  for (const ast::HttpResult& a : answers) {
    Rig r("READKEY1001");
    r.src.add(115);
    r.src.ack = 100;
    r.http.post_script.push_back(FakeTransport::lost());
    r.http.get_script.push_back(a);
    r.run(600);
    CHECK_EQ(r.http.gets.size(), static_cast<size_t>(1));
    CHECK_EQ(r.http.posts.size(), static_cast<size_t>(2));
    CHECK(r.http.posts[0].body == r.http.posts[1].body);
    CHECK_EQ(r.src.pending(), 0u);
  }
}

TEST(uploader_readback_waits_30_s_even_when_upload_is_forced) {
  Rig r("READKEY1001");
  r.src.add(15);
  r.http.post_script.push_back(FakeTransport::lost());
  r.run_until(kT0 + 25);                    // doubtful POST at kT0+20
  r.up.request_now();                       // operator types "upload" 5 s later
  r.run_until(kT0 + 49);
  CHECK_EQ(r.http.gets.size(), static_cast<size_t>(0));
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(1));
  r.run_until(kT0 + 52);
  CHECK_EQ(r.http.gets.size(), static_cast<size_t>(1));
  CHECK_EQ(r.http.gets[0].t, kT0 + 50);
}

TEST(uploader_wrong_key_401_keeps_data_and_backs_off_at_the_cap) {
  for (int status : {401, 403, 404}) {
    Rig r;
    r.src.add(15);
    r.http.post_handler = [status](const Request&) { return FakeTransport::response(status, "{\"status\":\"401\"}"); };
    r.run(2000);
    CHECK_EQ(r.http.posts.size(), static_cast<size_t>(3));   // 20, 920, 1820: 15 min apart at once
    CHECK_EQ(r.http.posts[1].t, kT0 + 920);
    CHECK_EQ(r.src.pending(), 15u);
    CHECK_STR(r.log.of(EventType::kUploadFail)[0].reason, "keys_or_channel_wrong");
    CHECK_EQ(r.log.count(EventType::kDrop), 0);
    r.run(1700);
    CHECK(r.up.fault9());                    // after 60 min as usual
  }
}

TEST(uploader_poison_record_is_isolated_by_bisection_and_only_it_is_dropped) {
  Rig r;
  r.src.add(64);
  std::multiset<uint32_t> stored;
  r.http.post_handler = [&](const Request& q) {
    if (q.body.find("\"field8\":37,") != std::string::npos) return FakeTransport::response(400, "{\"status\":\"400\"}");
    for (uint32_t s : seqs_in(q.body)) stored.insert(s);
    return FakeTransport::ok();
  };
  r.run(600);
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(stored.size(), static_cast<size_t>(63));
  for (uint32_t s = 1; s <= 64; ++s) CHECK_EQ(stored.count(s), static_cast<size_t>(s == 37 ? 0 : 1));
  const std::vector<EventLog::Item> d = r.log.of(EventType::kDrop);
  CHECK_EQ(d.size(), static_cast<size_t>(1));
  CHECK_EQ(d[0].first_seq, 37u);
  CHECK_STR(d[0].reason, "rejected_by_server");
  CHECK_EQ(d[0].http, 400);
  CHECK_EQ(r.up.stats().dropped, 1u);
  // Every request respected the 16 s spacing, and it took few requests.
  for (size_t i = 1; i < r.http.posts.size(); ++i) CHECK(r.http.posts[i].t - r.http.posts[i - 1].t >= 16);
  CHECK(r.http.posts.size() <= static_cast<size_t>(16));
  CHECK(!r.up.fault9());
}

TEST(uploader_two_poison_records_in_a_full_batch) {
  Rig r;
  r.src.add(1000);
  std::multiset<uint32_t> stored;
  r.http.post_handler = [&](const Request& q) {
    if (q.body.find("\"field8\":1,") != std::string::npos || q.body.find("\"field8\":960,") != std::string::npos) {
      return FakeTransport::response(400, "bad");
    }
    for (uint32_t s : seqs_in(q.body)) stored.insert(s);
    return FakeTransport::ok();
  };
  r.run(2000);
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(stored.size(), static_cast<size_t>(998));
  CHECK_EQ(stored.count(1), static_cast<size_t>(0));
  CHECK_EQ(stored.count(960), static_cast<size_t>(0));
  CHECK_EQ(stored.count(2), static_cast<size_t>(1));
  CHECK_EQ(stored.count(1000), static_cast<size_t>(1));
  CHECK_EQ(r.log.count(EventType::kDrop), 2);
  for (size_t i = 1; i < r.http.posts.size(); ++i) CHECK(r.http.posts[i].t - r.http.posts[i - 1].t >= 16);
}

TEST(uploader_200_without_success_is_a_content_rejection) {
  Rig r;
  r.src.add(4);
  std::multiset<uint32_t> stored;
  r.http.post_handler = [&](const Request& q) {
    if (q.body.find("\"field8\":2,") != std::string::npos) return FakeTransport::response(200, "0");
    for (uint32_t s : seqs_in(q.body)) stored.insert(s);
    return FakeTransport::ok();
  };
  r.run(300);
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(stored.size(), static_cast<size_t>(3));
  CHECK_EQ(r.log.of(EventType::kDrop)[0].first_seq, 2u);
  CHECK_STR(r.log.of(EventType::kUploadFail)[0].reason, "rejected_content");
}

TEST(uploader_server_rejecting_everything_drops_at_most_3_records) {
  Rig r;
  r.src.add(40);
  r.http.post_handler = [](const Request&) { return FakeTransport::response(400, "{\"status\":\"400\"}"); };
  r.run(6 * 3600);
  CHECK_EQ(r.log.count(EventType::kDrop), kMaxDropsWithoutSuccess);
  CHECK_EQ(r.src.pending(), 37u);           // the rest is kept
  CHECK(r.up.fault9());                     // and the problem is visible
  // Once dropping stopped, retries are 15 min apart.
  const size_t n = r.http.posts.size();
  CHECK(n > 4);
  CHECK_EQ(r.http.posts[n - 1].t - r.http.posts[n - 2].t, 900);
  // Server recovers: everything left goes through, nothing more is dropped.
  r.http.post_handler = nullptr;
  r.run(2000);
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(r.log.count(EventType::kDrop), kMaxDropsWithoutSuccess);
}

// Review finding 1 (13 Nov 2026): a bisection step that follows an earlier
// 5xx must still wait 16 s (next_retry_ is already in the past there).
TEST(uploader_bisection_after_a_5xx_keeps_16_s_spacing) {
  Rig r;
  r.src.add(64);
  int calls = 0;
  std::multiset<uint32_t> stored;
  r.http.post_handler = [&](const Request& q) {
    if (calls++ == 0) return FakeTransport::response(500, "busy");
    if (q.body.find("\"field8\":37,") != std::string::npos) return FakeTransport::response(400, "{\"status\":\"400\"}");
    for (uint32_t s : seqs_in(q.body)) stored.insert(s);
    return FakeTransport::ok();
  };
  r.run(1200);
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(stored.size(), static_cast<size_t>(63));
  CHECK_EQ(r.log.count(EventType::kDrop), 1);
  CHECK(r.http.posts.size() > static_cast<size_t>(4));
  for (size_t i = 1; i < r.http.posts.size(); ++i) CHECK(r.http.posts[i].t - r.http.posts[i - 1].t >= 16);
}

// Review finding 2 (13 Nov 2026): after a rejection episode (here the 3-drop
// limit was reached and the batch size had shrunk to 1) the batch size must
// grow back once the server accepts data, or a day of backlog would take hours.
TEST(uploader_batch_size_grows_back_after_rejections_end) {
  Rig r;
  r.src.add(1500);
  r.http.post_handler = [](const Request&) { return FakeTransport::response(400, "{\"status\":\"400\"}"); };
  r.run(2 * 3600);
  CHECK_EQ(r.log.count(EventType::kDrop), kMaxDropsWithoutSuccess);
  CHECK_EQ(r.src.pending(), 1497u);
  r.http.post_handler = nullptr;               // server recovers
  const size_t before = r.http.posts.size();
  r.run(1800);                                 // the next retry is at most 15 min away
  CHECK_EQ(r.src.pending(), 0u);
  CHECK_EQ(r.log.count(EventType::kDrop), kMaxDropsWithoutSuccess);
  // 1, 2, 4, ... 512, then 960-record batches: a handful of requests, not ~1500.
  const size_t after = r.http.posts.size() - before;
  CHECK(after <= static_cast<size_t>(14));
  CHECK(seqs_in(r.http.posts.back().body).size() > static_cast<size_t>(100));
  for (size_t i = before + 1; i < r.http.posts.size(); ++i) CHECK(r.http.posts[i].t - r.http.posts[i - 1].t >= 16);
  CHECK(!r.up.stats().bisecting);
}

TEST(uploader_one_off_400_does_not_lose_data) {
  Rig r;
  r.src.add(30);
  r.http.post_script.push_back(FakeTransport::response(400, "glitch"));
  std::multiset<uint32_t> stored;
  int n = 0;
  r.http.post_handler = [&](const Request& q) {
    if (n++ == 0) return FakeTransport::response(400, "glitch");
    for (uint32_t s : seqs_in(q.body)) stored.insert(s);
    return FakeTransport::ok();
  };
  r.run(300);
  CHECK_EQ(stored.size(), static_cast<size_t>(30));
  CHECK_EQ(r.log.count(EventType::kDrop), 0);
  CHECK_EQ(r.src.pending(), 0u);
}

TEST(uploader_duplicate_timestamps_never_reach_the_server) {
  Rig r;
  r.src.add(6);
  r.src.recs[3].minute_unix = r.src.recs[2].minute_unix;     // seq 4 repeats the minute of seq 3
  r.run(60);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(1));
  const std::vector<uint32_t> s = seqs_in(r.http.posts[0].body);
  CHECK_EQ(s.size(), static_cast<size_t>(5));
  CHECK_EQ(count_of(r.http.posts[0].body, "2026-11-20 00:02:00 +0000"), static_cast<size_t>(1));
  CHECK_EQ(r.src.pending(), 0u);
  const std::vector<EventLog::Item> d = r.log.of(EventType::kDrop);
  CHECK_EQ(d.size(), static_cast<size_t>(1));
  CHECK_EQ(d[0].first_seq, 4u);
  CHECK_STR(d[0].reason, "timestamp_not_increasing");
  CHECK_EQ(r.up.stats().records_ok, 5u);
}

TEST(uploader_cli_upload_now_respects_16_s_spacing) {
  Rig r;
  r.src.add(3);
  r.run_until(kT0 + 300);                   // regular upload happened at +20
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(1));
  r.src.add(2);
  r.up.request_now();
  r.run(2);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(2));     // off-schedule, immediately
  CHECK_EQ(r.http.posts[1].t, kT0 + 300);
  r.src.add(1);
  r.up.request_now();
  r.run(10);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(2));     // too soon after the previous write
  r.run(10);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(3));
  CHECK_EQ(r.http.posts[2].t, kT0 + 316);
}

TEST(uploader_seconds_until_due_for_radio_scheduling) {
  Rig r;
  CHECK_EQ(r.up.seconds_until_due(), Uploader::kNeverDue);   // before the first tick
  r.up.tick();
  CHECK_EQ(r.up.seconds_until_due(), Uploader::kNeverDue);   // nothing pending
  r.src.add(15);
  CHECK_EQ(r.up.seconds_until_due(), 20);                    // kT0 -> slot at kT0+20
  r.run_until(kT0 + 12);
  CHECK_EQ(r.up.seconds_until_due(), 8);
  r.http.post_script.push_back(FakeTransport::not_sent());
  r.run_until(kT0 + 21);                                     // failed at +20
  CHECK_EQ(r.up.seconds_until_due(), 59);                    // retry at +80
  r.run_until(kT0 + 81);                                     // success at +80
  CHECK_EQ(r.up.seconds_until_due(), Uploader::kNeverDue);
  r.src.add(1);
  CHECK_EQ(r.up.seconds_until_due(), 920 - 81);              // next slot
}

TEST(uploader_stats_snapshot) {
  Rig r;
  r.src.add(15);
  r.http.post_script.push_back(FakeTransport::response(500, ""));
  r.run(30);
  UploaderStats s = r.up.stats();
  CHECK_EQ(s.fail_count, 1);
  CHECK_EQ(s.last_http_status, 500);
  CHECK_EQ(s.next_retry, kT0 + 80);
  CHECK_EQ(s.failures, 1u);
  CHECK(s.configured);
  r.run(100);
  s = r.up.stats();
  CHECK_EQ(s.batches_ok, 1u);
  CHECK_EQ(s.records_ok, 15u);
  CHECK_EQ(s.last_success, kT0 + 80);
  CHECK_EQ(s.next_retry, 0);
  CHECK_EQ(s.last_http_status, 202);
}

TEST(uploader_waits_while_the_clock_is_not_a_real_date) {
  Rig r;
  r.clock.t = 100;                          // 1970: RTC lost, NTP not yet done
  r.src.add(5);
  for (int i = 0; i < 100; ++i) { CHECK(r.up.tick() == Uploader::Tick::kWaiting); r.clock.t += 1; }
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(0));
  CHECK(!r.up.fault9());
  r.clock.t = kT0 + 5;                      // NTP sets the clock
  r.run(30);
  CHECK_EQ(r.http.posts.size(), static_cast<size_t>(1));
  CHECK_EQ(r.http.posts[0].t, kT0 + 20);
  CHECK(!r.up.fault9());                    // the 60 min count starts at the first real time
}
