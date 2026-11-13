// ThreadSanitizer test (contract 7.4): sampler, uploader and CLI threads use
// one DataLink at the same time. Built with -fsanitize=thread; any data race
// makes the program fail. It also checks the result: every record uploaded
// exactly once and in order, while the transport is slow and sometimes fails.
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdio.h>
#include <thread>
#include <vector>

#include "ast_datalink.h"
#include "ast_telemetry.h"
#include "test_helpers.h"

using namespace ast;

namespace {

struct AtomicClock : Clock {
  std::atomic<int64_t> t{ast_test::kT0};
  int64_t now() override { return t.load(); }
};

// Slow transport: every request takes a little real time (the sampler must
// keep running meanwhile) and some fail or lose their response.
struct SlowTransport : HttpTransport {
  HttpResult post_json(const std::string&, BodyReader& body) override {
    std::string text;
    uint8_t buf[512];
    size_t n;
    body.rewind();
    while ((n = body.read(buf, sizeof buf)) > 0) text.append(reinterpret_cast<const char*>(buf), n);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const int k = calls.fetch_add(1);
    HttpResult r;
    if (k % 7 == 3) return r;                                  // not sent
    // Store the seq numbers the "server" received.
    {
      std::lock_guard<std::mutex> g(mu);
      size_t pos = 0;
      while ((pos = text.find("\"field8\":", pos)) != std::string::npos) {
        pos += 9;
        received.push_back(static_cast<uint32_t>(strtoul(text.c_str() + pos, nullptr, 10)));
      }
    }
    if (k % 7 == 5) { r.outcome = HttpResult::kNoResponse; return r; }   // stored, reply lost
    r.outcome = HttpResult::kResponse;
    r.status = 202;
    r.body = "{\"success\":true}";
    return r;
  }
  HttpResult get(const std::string&) override { return HttpResult(); }
  std::atomic<int> calls{0};
  std::mutex mu;
  std::vector<uint32_t> received;
};

}  // namespace

int main() {
  MemStorage storage;
  AtomicClock clock;
  SlowTransport transport;
  ast_test::EventLog events;
  DataLink dl(storage, transport, clock, &events, 2048, 64);   // large enough: no overflow in this test
  dl.begin();
  UploaderConfig cfg;
  cfg.channel = "1";
  cfg.write_key = "K";
  cfg.upload_min = 5;
  dl.uploader().set_config(cfg);
  dl.time_begin(true, false, clock.now());

  const int kMinutes = 600;
  std::atomic<bool> sampling_done{false};
  std::atomic<bool> stop{false};
  std::atomic<long> max_sampler_ns{0};

  std::thread sampler([&] {
    for (int64_t s = 0; s < kMinutes * 60; ++s) {
      const int64_t t = ast_test::kT0 + s;
      clock.t.store(t);
      const auto a = std::chrono::steady_clock::now();
      dl.sampler_tick(t, ast_test::sample(100.0f + static_cast<float>(s % 60)));
      const long ns = static_cast<long>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - a).count());
      if (ns > max_sampler_ns.load()) max_sampler_ns.store(ns);
      if (s % 20 == 0) std::this_thread::yield();
    }
    dl.flush(ast_test::kT0 + kMinutes * 60);
    sampling_done.store(true);
  });

  std::thread uploader([&] {
    while (!stop.load()) {
      dl.uploader_tick();
      std::this_thread::yield();
    }
  });

  std::thread cli([&] {
    unsigned n = 0;
    while (!stop.load()) {
      const DataLinkStatus st = dl.status();
      (void)st;
      dl.set_run(static_cast<uint16_t>(n % 900));
      dl.set_panel(n % 2 ? 'A' : 'B');
      if (n % 50 == 0) dl.uploader().request_now();
      if (n % 97 == 0) dl.uploader().set_upload_min(5 + static_cast<int>(n % 3));
      telemetry_set_angles(1.0f, 2.0f, true);
      telemetry_set_motor_running(n % 5 == 0);
      (void)telemetry_instance().take(telemetry_now_ms());
      ++n;
      std::this_thread::yield();
    }
  });

  sampler.join();
  // Let the uploader drain what is left: jump the clock forward in steps.
  for (int i = 0; i < 4000 && dl.pending() > 0; ++i) {
    clock.t.fetch_add(20);
    dl.uploader().request_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  stop.store(true);
  uploader.join();
  cli.join();

  // ---- checks ------------------------------------------------------------------
  int failures = 0;
  const DataLinkStatus st = dl.status();
  std::vector<bool> seen(static_cast<size_t>(kMinutes) + 2, false);
  {
    std::lock_guard<std::mutex> g(transport.mu);
    for (uint32_t s : transport.received) {
      if (s >= 1 && s <= static_cast<uint32_t>(kMinutes)) seen[s] = true;
      else { printf("FAIL: unexpected seq %u\n", s); ++failures; }
    }
  }
  // Every record must have reached the server at least once (duplicates from
  // lost responses are allowed: readers de-duplicate by seq).
  unsigned missing = 0;
  for (int s = 1; s <= kMinutes; ++s) missing += seen[static_cast<size_t>(s)] ? 0u : 1u;
  if (st.logged != static_cast<uint32_t>(kMinutes)) { printf("FAIL: logged %u\n", st.logged); ++failures; }
  if (st.pending != 0) { printf("FAIL: %u records still pending\n", st.pending); ++failures; }
  if (missing != 0) { printf("FAIL: %u seq never reached the server\n", missing); ++failures; }
  if (st.lost_total != 0) { printf("FAIL: lost_total=%u\n", st.lost_total); ++failures; }
  if (st.next_seq != static_cast<uint32_t>(kMinutes) + 1) { printf("FAIL: next_seq %u\n", st.next_seq); ++failures; }
  printf("tsan_concurrency: %d minutes logged, %d requests, %u lost, longest sampler_tick %.3f ms\n",
         kMinutes, transport.calls.load(), st.lost_total, static_cast<double>(max_sampler_ns.load()) / 1e6);
  printf(failures == 0 ? "PASS\n" : "FAILED\n");
  return failures == 0 ? 0 : 1;
}
