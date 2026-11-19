// Fakes shared by the tests: clock, HTTP transport, record source, event log.
#pragma once
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ast_datalink.h"

namespace ast_test {

constexpr int64_t kT0 = 1795132800;  // 2026-11-20 00:00:00 UTC

struct FakeClock : ast::Clock {
  int64_t t = kT0;
  int64_t now() override { return t; }
};

struct Request {
  bool is_post = false;
  std::string url, body;
  int64_t t = 0;
};

// Scripted HTTP transport. By default every POST answers 202 {"success":true}.
struct FakeTransport : ast::HttpTransport {
  explicit FakeTransport(FakeClock* c = nullptr) : clock(c) {}

  static ast::HttpResult response(int status, const std::string& body) {
    ast::HttpResult r;
    r.outcome = ast::HttpResult::kResponse;
    r.status = status;
    r.body = body;
    return r;
  }
  static ast::HttpResult ok() { return response(202, "{\"success\":true}"); }
  static ast::HttpResult not_sent() { return ast::HttpResult(); }
  static ast::HttpResult lost() {
    ast::HttpResult r;
    r.outcome = ast::HttpResult::kNoResponse;
    return r;
  }

  ast::HttpResult post_json(const std::string& url, ast::BodyReader& body) override {
    Request q;
    q.is_post = true;
    q.url = url;
    q.t = clock ? clock->t : 0;
    uint8_t buf[97];   // odd size on purpose: exercises the chunked reader
    size_t n;
    body.rewind();
    while ((n = body.read(buf, sizeof buf)) > 0) q.body.append(reinterpret_cast<const char*>(buf), n);
    declared_size_ok = declared_size_ok && (body.size() == q.body.size());
    posts.push_back(q);
    if (clock) clock->t += post_takes_s;
    if (post_handler) return post_handler(q);
    if (!post_script.empty()) {
      ast::HttpResult r = post_script.front();
      post_script.pop_front();
      return r;
    }
    return ok();
  }

  ast::HttpResult get(const std::string& url) override {
    Request q;
    q.url = url;
    q.t = clock ? clock->t : 0;
    gets.push_back(q);
    if (!get_script.empty()) {
      ast::HttpResult r = get_script.front();
      get_script.pop_front();
      return r;
    }
    return response(200, "-1");
  }

  FakeClock* clock;
  int64_t post_takes_s = 0;
  std::vector<Request> posts, gets;
  std::deque<ast::HttpResult> post_script, get_script;
  std::function<ast::HttpResult(const Request&)> post_handler;
  bool declared_size_ok = true;
};

// A queue of records with consecutive seq, standing in for the ring buffer.
struct VecSource : ast::RecordSource {
  std::vector<ast::Record> recs;
  uint32_t ack = 0;

  void add(size_t n, int64_t first_minute = kT0) {
    for (size_t i = 0; i < n; ++i) {
      ast::Record r;
      r.seq = static_cast<uint32_t>(recs.size() + 1);
      r.minute_unix = static_cast<uint32_t>(first_minute + 60 * static_cast<int64_t>(recs.size()));
      r.p_trk_dmw = 1000 + r.seq;
      r.flags = ast::kHasPtrk | ast::kHasPfix | ast::kHasPuse | ast::kHasAngPanel | ast::kHasVsup;
      r.state = ast::State::TRK;
      recs.push_back(r);
    }
  }
  uint32_t pending() override { return static_cast<uint32_t>(recs.size()) - ack; }
  size_t peek_unacked(ast::Record* out, size_t max, uint32_t* next_index) override {
    size_t n = 0;
    uint32_t i = ack;
    while (i < recs.size() && n < max) out[n++] = recs[i++];
    *next_index = i;
    return n;
  }
  bool ack_to(uint32_t next_index) override {
    if (next_index > recs.size()) next_index = static_cast<uint32_t>(recs.size());
    if (next_index > ack) ack = next_index;
    return true;
  }
  bool ack_through_seq(uint32_t seq) override { return ack_to(seq); }  // seq n sits at index n-1
};

struct EventLog : ast::EventSink {
  struct Item {
    ast::EventType type;
    int64_t t;
    uint32_t first_seq, last_seq, count, minute;
    int32_t delay_s;
    int http;
    uint8_t fault;
    std::string reason;
  };
  void on_event(const ast::Event& e) override {
    std::lock_guard<std::mutex> g(mu);
    items.push_back(Item{e.type, e.t, e.first_seq, e.last_seq, e.count, e.minute_unix, e.delay_s,
                         e.http_status, e.fault, e.reason});
  }
  int count(ast::EventType t) {
    std::lock_guard<std::mutex> g(mu);
    int n = 0;
    for (const Item& i : items) n += (i.type == t);
    return n;
  }
  std::vector<Item> of(ast::EventType t) {
    std::lock_guard<std::mutex> g(mu);
    std::vector<Item> v;
    for (const Item& i : items) if (i.type == t) v.push_back(i);
    return v;
  }
  std::mutex mu;
  std::vector<Item> items;
};

// A real 1 Hz sample with everything valid.
inline ast::Sample1Hz sample(float trk = 500.0f, float fix = 400.0f, float use = 120.0f) {
  ast::Sample1Hz s;
  s.p_trk_valid = s.p_fix_valid = s.p_use_valid = s.vsup_valid = s.ang_panel_valid = true;
  s.p_trk_mw = trk;
  s.p_fix_mw = fix;
  s.p_use_mw = use;
  s.vsup_v = 5.0f;
  s.ang_panel_deg = -10.0f;
  s.state = ast::State::TRK;
  return s;
}

// How many times `needle` occurs in `hay`.
inline size_t count_of(const std::string& hay, const std::string& needle) {
  size_t n = 0, pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
  return n;
}

}  // namespace ast_test
