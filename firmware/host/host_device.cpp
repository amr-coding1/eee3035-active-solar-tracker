// ast_host_device - the AST data link running on a PC (docs/PLAN.md, "Shared
// interfaces"). It runs the SAME portable core as the ESP32 (firmware/core):
// simulator -> minute aggregator -> ring buffer -> upload state machine, with a
// simulated clock, files in --state-dir as "flash" and plain HTTP as transport.
//
// Exit codes: 0 ok, 2 usage error, 3 simulated crash (--crash-at).
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "ast_datalink.h"
#include "ast_json.h"
#include "ast_power.h"
#include "ast_status.h"
#include "ast_time.h"
#include "host_http.h"
#include "host_storage.h"

namespace {

const char* kUsage =
    "ast_host_device --base http://127.0.0.1:PORT --ch1 ID --write-key KEY [--read-key KEY]\n"
    "  --state-dir DIR                 persistent \"flash\": ring buffer, metadata, seq, clock\n"
    "  --start 2026-11-20T00:00:00Z    virtual UTC start (ignored when DIR already holds a clock, unless --set-time)\n"
    "  --set-time                      use --start even if DIR holds a clock\n"
    "  --minutes N                     run N virtual minutes, then exit 0 (graceful)\n"
    "  --upload-min 15                 upload interval (5-60)\n"
    "  --scenario clear|cloudy|overcast|night|lamp     --seed S\n"
    "  --wifi-down START/END           Wi-Fi unavailable in this ISO-8601 UTC window (repeatable)\n"
    "  --crash-at ISO                  abrupt stop at that virtual time -> exit 3\n"
    "  --rtc-lost                      RTC oscillator-stop flag set, no NTP until the first connection\n"
    "  --run ID --panel A|B            run id and panel letter for the status string\n"
    "  --virtual                       no real sleeping; requests carry X-Sim-Time: <unix seconds>\n"
    "  --log FILE                      JSON lines (appended)\n"
    "extras (not in PLAN.md):\n"
    "  --drain                         after --minutes, keep the uploader running (no sampling) until the\n"
    "                                  buffer is empty, at most 3 virtual hours\n"
    "  --http-timeout-ms N             HTTP timeout, default 30000\n"
    "  --radio-lead-s N                switch the radio on N s before an upload is due (default 8)\n"
    "  --selftest                      built-in offline check, exit 0 on success\n"
    "exit codes: 0 ok, 2 usage error, 3 simulated crash\n";

struct Window { int64_t start, end; };

struct Options {
  std::string base, ch1, write_key, read_key, state_dir, log_file;
  bool have_start = false, set_time = false, is_virtual = false, rtc_lost = false, drain = false;
  int64_t start = 0;
  long minutes = -1;
  int upload_min = ast::kUploadMinDefault;
  ast::Scenario scenario = ast::Scenario::kClear;
  uint32_t seed = 1;
  std::vector<Window> wifi_down;
  bool have_crash = false;
  int64_t crash_at = 0;
  unsigned run = 0;
  char panel = 'A';
  int http_timeout_ms = ast::kHttpTimeoutMs;
  int radio_lead_s = ast::kRadioLeadS;
};

[[noreturn]] void usage_error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "ast_host_device: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n\n%s", kUsage);
  va_end(ap);
  exit(2);
}

bool parse_long(const char* s, long lo, long hi, long* out) {
  if (s == nullptr || *s == '\0') return false;
  char* end = nullptr;
  errno = 0;
  const long v = strtol(s, &end, 10);
  if (errno != 0 || *end != '\0' || v < lo || v > hi) return false;
  *out = v;
  return true;
}

Options parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) usage_error("%s needs a value", a.c_str());
      return argv[++i];
    };
    long n = 0;
    if (a == "--base") o.base = value();
    else if (a == "--ch1") o.ch1 = value();
    else if (a == "--write-key") o.write_key = value();
    else if (a == "--read-key") o.read_key = value();
    else if (a == "--state-dir") o.state_dir = value();
    else if (a == "--log") o.log_file = value();
    else if (a == "--start") {
      if (!ast::parse_iso(value(), &o.start)) usage_error("--start: not an ISO-8601 UTC time");
      o.have_start = true;
    } else if (a == "--set-time") o.set_time = true;
    else if (a == "--minutes") {
      if (!parse_long(value(), 0, 100000000, &n)) usage_error("--minutes: not a number");
      o.minutes = n;
    } else if (a == "--upload-min") {
      if (!parse_long(value(), ast::kUploadMinLow, ast::kUploadMinHigh, &n)) usage_error("--upload-min: 5 to 60");
      o.upload_min = static_cast<int>(n);
    } else if (a == "--scenario") {
      if (!ast::scenario_from_name(value(), &o.scenario)) usage_error("--scenario: clear|cloudy|overcast|night|lamp");
    } else if (a == "--seed") {
      if (!parse_long(value(), 0, 2147483647L, &n)) usage_error("--seed: not a number");
      o.seed = static_cast<uint32_t>(n);
    } else if (a == "--wifi-down") {
      const std::string w = value();
      const size_t slash = w.find('/');
      Window win{};
      if (slash == std::string::npos || !ast::parse_iso(w.substr(0, slash).c_str(), &win.start) ||
          !ast::parse_iso(w.substr(slash + 1).c_str(), &win.end) || win.end <= win.start) {
        usage_error("--wifi-down: expected START/END in ISO-8601 UTC with END after START");
      }
      o.wifi_down.push_back(win);
    } else if (a == "--crash-at") {
      if (!ast::parse_iso(value(), &o.crash_at)) usage_error("--crash-at: not an ISO-8601 UTC time");
      o.have_crash = true;
    } else if (a == "--rtc-lost") o.rtc_lost = true;
    else if (a == "--run") {
      if (!parse_long(value(), 0, ast::kRunMax, &n)) usage_error("--run: 0 to 999");
      o.run = static_cast<unsigned>(n);
    } else if (a == "--panel") {
      const std::string p = value();
      if (p == "A" || p == "a") o.panel = 'A';
      else if (p == "B" || p == "b") o.panel = 'B';
      else usage_error("--panel: A or B");
    } else if (a == "--virtual") o.is_virtual = true;
    else if (a == "--drain") o.drain = true;
    else if (a == "--http-timeout-ms") {
      if (!parse_long(value(), 100, 600000, &n)) usage_error("--http-timeout-ms: 100 to 600000");
      o.http_timeout_ms = static_cast<int>(n);
    } else if (a == "--radio-lead-s") {
      if (!parse_long(value(), 0, 120, &n)) usage_error("--radio-lead-s: 0 to 120");
      o.radio_lead_s = static_cast<int>(n);
    } else if (a == "--help" || a == "-h") {
      printf("%s", kUsage);
      exit(0);
    } else {
      usage_error("unknown option %s", a.c_str());
    }
  }
  if (o.base.empty()) usage_error("--base is required");
  ast_host::ParsedUrl u;
  if (!ast_host::parse_http_url(o.base, &u)) usage_error("--base must look like http://127.0.0.1:PORT");
  if (o.ch1.empty()) usage_error("--ch1 is required");
  if (o.write_key.empty()) usage_error("--write-key is required");
  if (o.state_dir.empty()) usage_error("--state-dir is required");
  if (o.minutes < 0) usage_error("--minutes is required");
  return o;
}

struct VirtualClock : ast::Clock {
  int64_t t = 0;
  int64_t now() override { return t; }
};

std::string iso(int64_t t) {
  char b[ast::kIsoLen + 1];
  ast::format_iso(t, b);
  return b;
}

// JSON-lines logger: one object per event, flushed line by line so that the
// log is complete up to a simulated crash.
class JsonlLog : public ast::EventSink {
 public:
  ~JsonlLog() override { if (f_ != nullptr) fclose(f_); }
  bool open(const std::string& path) {
    f_ = fopen(path.c_str(), "a");
    return f_ != nullptr;
  }
  void line(int64_t t, const char* ev, const std::string& rest) {
    if (f_ == nullptr) return;
    fprintf(f_, "{\"t\":%lld,\"utc\":\"%s\",\"ev\":\"%s\"%s%s}\n", static_cast<long long>(t), iso(t).c_str(), ev,
            rest.empty() ? "" : ",", rest.c_str());
    fflush(f_);
  }
  static std::string kv(const char* k, long long v) {
    return std::string("\"") + k + "\":" + std::to_string(v);
  }
  static std::string ks(const char* k, const std::string& v) {
    return std::string("\"") + k + "\":\"" + ast::json_escape(v) + "\"";
  }

  void on_event(const ast::Event& e) override {
    using ast::EventType;
    std::string r;
    const char* ev = "event";
    const long long radio_s = radio != nullptr ? radio->session_seconds(e.t) : 0;
    switch (e.type) {
      case EventType::kSample: {
        ev = "sample";
        char entry[ast::kJsonEntryMax];
        ast::format_entry(*e.record, entry, sizeof entry);
        // The entry is exactly what will be uploaded for this sample.
        r = kv("seq", e.record->seq) + "," + kv("pending", e.pending) + ",\"entry\":" + entry;
        ++samples;
        break;
      }
      case EventType::kUploadOk:
        ev = "upload_ok";
        r = kv("n", e.count) + "," + kv("first_seq", e.first_seq) + "," + kv("last_seq", e.last_seq) + "," +
            kv("http", e.http_status) + "," + kv("pending", e.pending) + "," + kv("radio_on_s", radio_s) + "," +
            ks("reason", e.reason);
        ++uploads_ok;
        break;
      case EventType::kUploadFail:
        ev = "upload_fail";
        r = kv("n", e.count) + "," + kv("first_seq", e.first_seq) + "," + kv("last_seq", e.last_seq) + "," +
            kv("http", e.http_status) + "," + kv("pending", e.pending) + "," + kv("radio_on_s", radio_s) + "," +
            ks("reason", e.reason);
        break;
      case EventType::kInDoubt:
        ev = "in_doubt";
        r = kv("n", e.count) + "," + kv("first_seq", e.first_seq) + "," + kv("last_seq", e.last_seq) + "," +
            kv("pending", e.pending) + "," + kv("radio_on_s", radio_s) + "," + ks("reason", e.reason);
        break;
      case EventType::kFault:
        ev = "fault";
        r = kv("code", e.fault) + "," + kv("pending", e.pending) + "," + ks("reason", e.reason);
        break;
      case EventType::kBackoff:
        ev = "backoff";
        r = kv("delay_s", e.delay_s) + "," + kv("failures", e.count) + "," + kv("pending", e.pending);
        break;
      case EventType::kReadback:
        ev = "readback";
        r = kv("http", e.http_status) + "," + kv("last_seq", e.last_seq) + "," + kv("skipped", e.count) + "," +
            kv("pending", e.pending) + "," + ks("reason", e.reason);
        break;
      case EventType::kSkip:
        ev = "skip";
        r = kv("minute", e.minute_unix) + "," + ks("created_at", iso(e.minute_unix)) + "," + ks("reason", e.reason);
        break;
      case EventType::kDrop:
        ev = "drop";
        r = kv("seq", e.first_seq) + "," + kv("minute", e.minute_unix) + "," + kv("http", e.http_status) + "," +
            ks("reason", e.reason);
        break;
    }
    line(e.t, ev, r);
  }

  const ast::RadioMeter* radio = nullptr;
  unsigned long samples = 0, uploads_ok = 0;

 private:
  FILE* f_ = nullptr;
};

bool read_clock_file(const std::string& path, int64_t* t) {
  FILE* f = fopen(path.c_str(), "r");
  if (f == nullptr) return false;
  long long v = 0;
  const bool ok = fscanf(f, "%lld", &v) == 1 && v > 0;
  fclose(f);
  if (ok) *t = v;
  return ok;
}

void write_clock_file(const std::string& path, int64_t t) {
  // Written to a temporary name and renamed, so the clock file is never half-written.
  const std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (f == nullptr) return;
  fprintf(f, "%lld\n", static_cast<long long>(t));
  fclose(f);
  rename(tmp.c_str(), path.c_str());
}

void sleep_until_next_second() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timespec d;
  d.tv_sec = 0;
  d.tv_nsec = 1000000000L - ts.tv_nsec;
  nanosleep(&d, nullptr);
}

int run_device(const Options& o) {
  ast_host::DirStorage storage(o.state_dir);
  const std::string clock_path = o.state_dir + "/clock.txt";

  // ---- clock: the state directory holds the "RTC" -----------------------------
  VirtualClock clock;
  int64_t stored = 0;
  const bool have_stored = read_clock_file(clock_path, &stored);
  if (have_stored && !(o.set_time && o.have_start)) clock.t = stored;
  else if (o.have_start) clock.t = o.start;
  else clock.t = static_cast<int64_t>(time(nullptr));

  JsonlLog log;
  if (!o.log_file.empty() && !log.open(o.log_file)) {
    fprintf(stderr, "ast_host_device: cannot open log file %s\n", o.log_file.c_str());
    return 2;
  }

  auto wifi_available = [&](int64_t t) {
    for (const Window& w : o.wifi_down) {
      if (t >= w.start && t < w.end) return false;
    }
    return true;
  };

  ast::RadioMeter radio;
  log.radio = &radio;
  ast_host::HostHttp http;
  http.timeout_ms = o.http_timeout_ms;
  http.link_up = [&]() { return radio.is_on() && wifi_available(clock.t); };
  if (o.is_virtual) http.sim_time = [&]() { return static_cast<long long>(clock.t); };

  ast::DataLink dl(storage, http, clock, &log);
  const ast::RingBuffer::LoadResult lr = dl.begin();
  ast::UploaderConfig uc;
  uc.base_url = o.base;
  uc.channel = o.ch1;
  uc.write_key = o.write_key;
  uc.read_key = o.read_key;
  uc.upload_min = o.upload_min;
  dl.uploader().set_config(uc);
  dl.set_run(static_cast<uint16_t>(o.run));
  dl.set_panel(o.panel);
  ast::SimConfig sc;
  sc.scenario = o.scenario;
  sc.seed = o.seed;
  dl.set_sim_config(sc);
  dl.set_sim(true);   // this program has no real sensors: every sample is marked s=1
  dl.time_begin(true, o.rtc_lost, clock.t);

  {
    const ast::DataLinkStatus st = dl.status();
    const char* how = lr == ast::RingBuffer::LoadResult::kLoaded ? "loaded"
                      : lr == ast::RingBuffer::LoadResult::kFresh ? "fresh" : "recovered_by_scan";
    log.line(clock.t, "boot",
             JsonlLog::ks("contract", ast::kContractVersion) + "," + JsonlLog::ks("buffer", how) + "," +
                 JsonlLog::kv("next_seq", st.next_seq) + "," + JsonlLog::kv("pending", st.pending) + "," +
                 JsonlLog::kv("count", st.count) + "," + JsonlLog::kv("time_valid", st.time_valid ? 1 : 0) + "," +
                 JsonlLog::ks("scenario", ast::scenario_name(o.scenario)) + "," + JsonlLog::kv("seed", o.seed) + "," +
                 JsonlLog::kv("upload_min", o.upload_min) + "," + JsonlLog::kv("virtual", o.is_virtual ? 1 : 0));
  }

  const ast::Sample1Hz no_real_sensors;
  auto radio_and_upload = [&](int64_t t) {
    // NTP: possible as soon as the radio is on and the hotspot is reachable.
    if (!dl.time_valid() && radio.is_on() && wifi_available(t)) {
      dl.time_on_ntp(t, true, 0);
      log.line(t, "ntp", JsonlLog::ks("reason", "time_valid_rtc_set"));
    }
    const int64_t due = dl.uploader().seconds_until_due();
    const bool need = ast::radio_needed(radio.is_on(), due, dl.time_valid(), o.radio_lead_s);
    if (need && !radio.is_on()) {
      radio.on(t);
      log.line(t, "radio", JsonlLog::ks("state", "on"));
    }
    dl.uploader_tick();
    if (!need && radio.is_on()) {
      radio.off(t);
      log.line(t, "radio", JsonlLog::ks("state", "off") + "," + JsonlLog::kv("radio_on_s", radio.last_session_seconds()) +
                               "," + JsonlLog::kv("radio_today_s", radio.today_seconds(t)));
    }
  };

  const int64_t end = clock.t + static_cast<int64_t>(o.minutes) * 60;
  int64_t t = clock.t;
  for (; t < end; ++t) {
    clock.t = t;
    if (o.have_crash && t >= o.crash_at) {
      // Abrupt stop: no flush, no clean-up. Only the battery-backed clock
      // (a separate chip on the real device) keeps the time.
      write_clock_file(clock_path, t);
      log.line(t, "crash", "");
      _exit(3);
    }
    dl.sampler_tick(t, no_real_sensors);
    radio_and_upload(t);
    if (t % 60 == 59) write_clock_file(clock_path, t + 1);
    if (!o.is_virtual) sleep_until_next_second();
  }

  // ---- graceful end ---------------------------------------------------------------
  clock.t = end;
  dl.flush(end);   // the last complete minute is stored before "power off"
  if (o.drain) {
    const int64_t limit = end + 3 * 3600;
    for (t = end; t < limit && dl.pending() > 0; ++t) {
      clock.t = t;
      radio_and_upload(t);
      if (!o.is_virtual) sleep_until_next_second();
    }
    clock.t = t;
  }
  if (radio.is_on()) {
    radio.off(clock.t);
    log.line(clock.t, "radio", JsonlLog::ks("state", "off") + "," + JsonlLog::kv("radio_on_s", radio.last_session_seconds()) +
                                   "," + JsonlLog::kv("radio_today_s", radio.today_seconds(clock.t)));
  }
  write_clock_file(clock_path, clock.t);
  const ast::DataLinkStatus st = dl.status();
  log.line(clock.t, "exit",
           JsonlLog::kv("pending", st.pending) + "," + JsonlLog::kv("next_seq", st.next_seq) + "," +
               JsonlLog::kv("logged", st.logged) + "," + JsonlLog::kv("uploaded", st.upload.records_ok) + "," +
               JsonlLog::kv("lost", st.lost_total) + "," + JsonlLog::kv("radio_total_s", static_cast<long long>(radio.total_seconds(clock.t))) +
               "," + JsonlLog::kv("http_requests", static_cast<long long>(http.requests)));
  printf("ast_host_device: %s -> %s, logged %u, uploaded %u, pending %u, next seq %u, radio on %llu s\n",
         iso(end - static_cast<int64_t>(o.minutes) * 60).c_str(), iso(clock.t).c_str(), st.logged, st.upload.records_ok,
         st.pending, st.next_seq, static_cast<unsigned long long>(radio.total_seconds(clock.t)));
  return 0;
}

// Offline self-check used by ctest: 45 virtual minutes with nothing listening,
// then a restart; records and seq must persist.
int selftest() {
  char tmpl[] = "/tmp/ast_selftest_XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 1;
  Options o;
  o.base = "http://127.0.0.1:9";   // discard port: connection refused
  o.ch1 = "1";
  o.write_key = "K";
  o.state_dir = tmpl;
  o.have_start = true;
  o.start = 1795132800;
  o.minutes = 45;
  o.is_virtual = true;
  o.http_timeout_ms = 500;
  if (run_device(o) != 0) return 1;
  ast_host::DirStorage storage(tmpl);
  ast::RingBuffer ring(storage);
  if (ring.begin() != ast::RingBuffer::LoadResult::kLoaded) return 1;
  const bool ok1 = ring.count() == 45 && ring.pending() == 45 && ring.next_seq() == 46;
  o.minutes = 15;                    // restart: continues from the stored clock and seq
  o.have_start = false;
  if (run_device(o) != 0) return 1;
  ast::RingBuffer ring2(storage);
  ring2.begin();
  const bool ok2 = ring2.count() == 60 && ring2.next_seq() == 61 &&
                   ring2.last_minute() == 1795132800u + 59u * 60u;
  const std::string cmd = std::string("rm -rf ") + tmpl;
  if (system(cmd.c_str()) != 0) return 1;
  printf("selftest %s\n", ok1 && ok2 ? "ok" : "FAILED");
  return ok1 && ok2 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  if (argc == 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
  if (argc < 2) {
    fprintf(stderr, "%s", kUsage);
    return 2;
  }
  const Options o = parse_args(argc, argv);
  return run_device(o);
}
