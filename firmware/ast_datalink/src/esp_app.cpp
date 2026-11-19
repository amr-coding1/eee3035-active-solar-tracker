// AST data link - ESP32 application (Arduino Nano ESP32, core esp32:esp32 3.3.x).
// Everything that decides WHAT happens is in the portable core (src/core,
// unit-tested on the PC); this file only connects it to the hardware.
//
// Tasks (contract 7.4):
//   sampler  (priority 3): once per second, vTaskDelayUntil - reads the three
//            INA226 and the telemetry of the control system, feeds DataLink.
//   uploader (priority 2): once per second - switches the radio on shortly
//            before an upload is due, runs the upload state machine, NTP -> RTC,
//            switches the radio off again.
//   loop()   (Arduino task): serial CLI and log output only.
// Both tasks are subscribed to the task watchdog (180 s).
#include "esp_app.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>
#include <deque>
#include <string>
#include <vector>

#include "core/ast_cli.h"
#include "core/ast_datalink.h"
#include "core/ast_json.h"
#include "core/ast_power.h"
#include "core/ast_status.h"
#include "core/ast_telemetry.h"
#include "core/ast_time.h"
#include "esp_http.h"
#include "esp_ina226.h"
#include "esp_rtc.h"
#include "esp_settings.h"
#include "esp_storage.h"

namespace ast_esp {

namespace {

// ---- energy settings -----------------------------------------------------------
const uint32_t kCpuMhzIdle = 80;      // lowest frequency that keeps Wi-Fi/USB clocks valid
const uint32_t kCpuMhzRadio = 160;    // during an upload: a shorter TLS handshake = less radio time
const uint32_t kNtpWindowOnMs = 60000;     // no valid time: try NTP for 60 s ...
const uint32_t kNtpWindowOffMs = 240000;   // ... then radio off for 4 min before the next try

// ---- hardware --------------------------------------------------------------------
const uint8_t kInaAddr[3] = {0x40, 0x41, 0x44};   // tracked, fixed, consumption

struct SystemClock : ast::Clock {
  int64_t now() override { return static_cast<int64_t>(time(nullptr)); }
};

std::string iso(int64_t t) {
  char b[ast::kIsoLen + 1];
  ast::format_iso(t, b);
  return b;
}

// Log lines are queued here by any task and printed by loop(), so that a
// stalled USB port can never block sampling or uploading.
class LineQueue {
 public:
  void push(const std::string& line) {
    std::lock_guard<std::mutex> g(mu_);
    if (lines_.size() >= 48) {
      ++dropped_;
      return;
    }
    lines_.push_back(line);
  }
  bool pop(std::string* line, unsigned* dropped) {
    std::lock_guard<std::mutex> g(mu_);
    *dropped = dropped_;
    dropped_ = 0;
    if (lines_.empty()) return false;
    *line = lines_.front();
    lines_.pop_front();
    return true;
  }

 private:
  std::mutex mu_;
  std::deque<std::string> lines_;
  unsigned dropped_ = 0;
};

LineQueue g_lines;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  g_lines.push(std::string("[") + iso(static_cast<int64_t>(time(nullptr))) + "] " + buf);
}

std::atomic<uint32_t> g_radio_session_start_ms{0};
std::atomic<bool> g_radio_on{false};

uint32_t radio_session_s() {
  return g_radio_on.load() ? (millis() - g_radio_session_start_ms.load()) / 1000u : 0u;
}

class SerialSink : public ast::EventSink {
 public:
  void on_event(const ast::Event& e) override {
    using ast::EventType;
    switch (e.type) {
      case EventType::kSample: {
        char entry[ast::kJsonEntryMax];
        ast::format_entry(*e.record, entry, sizeof entry);
        logf("sample pending=%lu %s", static_cast<unsigned long>(e.pending), entry);
        break;
      }
      case EventType::kUploadOk:
        logf("upload_ok n=%lu seq=%lu..%lu http=%d pending=%lu radio_on_s=%lu (%s)", static_cast<unsigned long>(e.count),
             static_cast<unsigned long>(e.first_seq), static_cast<unsigned long>(e.last_seq), e.http_status,
             static_cast<unsigned long>(e.pending), static_cast<unsigned long>(radio_session_s()), e.reason);
        break;
      case EventType::kUploadFail:
        logf("upload_fail n=%lu seq=%lu..%lu http=%d reason=%s pending=%lu radio_on_s=%lu", static_cast<unsigned long>(e.count),
             static_cast<unsigned long>(e.first_seq), static_cast<unsigned long>(e.last_seq), e.http_status, e.reason,
             static_cast<unsigned long>(e.pending), static_cast<unsigned long>(radio_session_s()));
        break;
      case EventType::kInDoubt:
        logf("in_doubt n=%lu seq=%lu..%lu (request sent, no response) radio_on_s=%lu", static_cast<unsigned long>(e.count),
             static_cast<unsigned long>(e.first_seq), static_cast<unsigned long>(e.last_seq),
             static_cast<unsigned long>(radio_session_s()));
        break;
      case EventType::kFault:
        logf("FAULT %u %s", static_cast<unsigned>(e.fault), e.reason);
        break;
      case EventType::kBackoff:
        logf("backoff retry in %ld s (failure %lu)", static_cast<long>(e.delay_s), static_cast<unsigned long>(e.count));
        break;
      case EventType::kReadback:
        logf("readback http=%d last_seq=%lu skipped=%lu (%s)", e.http_status, static_cast<unsigned long>(e.last_seq),
             static_cast<unsigned long>(e.count), e.reason);
        break;
      case EventType::kSkip:
        logf("skip minute %s not logged (%s)", iso(e.minute_unix).c_str(), e.reason);
        break;
      case EventType::kDrop:
        logf("DROP seq=%lu minute=%s http=%d (%s)", static_cast<unsigned long>(e.first_seq), iso(e.minute_unix).c_str(),
             e.http_status, e.reason);
        break;
    }
  }
};

// ---- global objects (created in app_setup) -------------------------------------
LittleFsStorage g_storage;
EspHttp g_http;
SystemClock g_clock;
SerialSink g_sink;
Ds3231 g_rtc;
Ina226 g_ina[3] = {Ina226(kInaAddr[0]), Ina226(kInaAddr[1]), Ina226(kInaAddr[2])};
ast::DataLink* g_dl = nullptr;

std::mutex g_i2c_mu;                 // Wire bus
std::mutex g_cfg_mu;                 // g_settings, g_cal_points, g_last_reading, g_radio
Settings g_settings;
ast::CalPoint g_cal_points[3][2][2]; // [channel][0 = V, 1 = I][point 1, 2]
ast::InaReading g_last_reading[3];
bool g_last_valid[3] = {false, false, false};
ast::RadioMeter g_radio;

std::atomic<bool> g_ntp_flag{false};
std::atomic<bool> g_save_misc{false};
std::atomic<uint32_t> g_last_ntp_ms{0};
std::atomic<bool> g_ntp_ever{false};
uint32_t g_ntp_window_start_ms = 0;  // uploader task only
bool g_ntp_window_started = false;

void ntp_callback(struct timeval*) { g_ntp_flag.store(true); }

// ---- sensors -----------------------------------------------------------------------
void read_sensors(ast::Sample1Hz* s) {
  ast::InaReading r[3];
  bool ok[3];
  {
    std::lock_guard<std::mutex> g(g_i2c_mu);
    for (int i = 0; i < 3; ++i) ok[i] = g_ina[i].read(&r[i]);
  }
  {
    std::lock_guard<std::mutex> g(g_cfg_mu);
    for (int i = 0; i < 3; ++i) {
      g_last_valid[i] = ok[i];
      if (ok[i]) g_last_reading[i] = r[i];
    }
  }
  s->p_trk_valid = ok[0];
  s->p_trk_mw = r[0].power_mw;
  s->p_fix_valid = ok[1];
  s->p_fix_mw = r[1].power_mw;
  s->p_use_valid = ok[2];
  s->p_use_mw = r[2].power_mw;
  // The consumption sensor sits in the 5 V feed to the tracker (19 Nov wiring
  // correction): its bus voltage is the supply rail, field7 V_supply.
  s->vsup_valid = ok[2];
  s->vsup_v = r[2].volts;
}

void sampler_task(void*) {
  esp_task_wdt_add(nullptr);
  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));   // sleeps; no busy waiting
    esp_task_wdt_reset();
    const int64_t now = static_cast<int64_t>(time(nullptr));

    // Control-system state (contract 7.7). Taken every second even while the
    // simulator is on, so that the motor latch does not go stale.
    const ast::TelemetrySnapshot tel = ast::telemetry_instance().take(ast::telemetry_now_ms());
    if (tel.has_run || tel.has_panel) {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      if (tel.has_run && tel.run != g_settings.run) { g_settings.run = tel.run; g_dl->set_run(tel.run); g_save_misc.store(true); }
      if (tel.has_panel && tel.panel != g_settings.panel) { g_settings.panel = tel.panel; g_dl->set_panel(tel.panel); g_save_misc.store(true); }
    }

    ast::Sample1Hz s;
    if (!g_dl->sim_on()) {
      read_sensors(&s);
      s.ang_panel_valid = tel.panel_valid;
      s.ang_panel_deg = tel.panel_deg;
      s.ang_sun_valid = tel.sun_valid;
      s.ang_sun_deg = tel.sun_deg;
      s.temp_valid = tel.temp_valid;
      s.temp_c = tel.temp_c;
      s.state = tel.state;
      s.motor_running = tel.motor_ran;
      s.ctrl_fault = tel.fault;
    }
    g_dl->sampler_tick(now, s);
  }
}

// ---- radio, NTP, upload ------------------------------------------------------------
void radio_start() {
  std::string ssid, pass;
  {
    std::lock_guard<std::mutex> g(g_cfg_mu);
    ssid = g_settings.wifi_ssid;
    pass = g_settings.wifi_pass;
  }
  setCpuFrequencyMhz(kCpuMhzRadio);
  WiFi.persistent(false);            // credentials live in our NVS namespace only
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.empty() ? nullptr : pass.c_str());
  // UTC, no time zone, no DST (contract 7.1). SNTP sets the system clock and
  // calls ntp_callback(); the RTC is disciplined in handle_ntp().
  sntp_set_time_sync_notification_cb(ntp_callback);
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  g_radio_session_start_ms.store(millis());
  g_radio_on.store(true);
}

void radio_stop() {
  WiFi.disconnect(true /* radio off */, false);
  WiFi.mode(WIFI_OFF);
  setCpuFrequencyMhz(kCpuMhzIdle);
  const uint32_t dur = (millis() - g_radio_session_start_ms.load()) / 1000u;
  g_radio_on.store(false);
  const int64_t now = static_cast<int64_t>(time(nullptr));
  {
    // Booked with the session length measured by millis(), so a clock step
    // (NTP) during the session cannot distort the count.
    std::lock_guard<std::mutex> g(g_cfg_mu);
    g_radio.on(now - static_cast<int64_t>(dur));
    g_radio.off(now);
  }
  logf("radio off after %lu s", static_cast<unsigned long>(dur));
}

void handle_ntp() {
  if (!g_ntp_flag.exchange(false)) return;
  const int64_t ntp = static_cast<int64_t>(time(nullptr));   // system clock was just set by SNTP
  int64_t rtc_unix = 0;
  bool lost = true, read_ok = false;
  {
    std::lock_guard<std::mutex> g(g_i2c_mu);
    read_ok = g_rtc.present() && g_rtc.read(&rtc_unix, &lost);
  }
  const bool was_valid = g_dl->time_valid();
  const bool must_set = g_dl->time_on_ntp(ntp, g_rtc.present(), read_ok ? rtc_unix : 0);
  bool set_ok = false;
  if (must_set) {
    // Write on a second boundary so the RTC is within a few ms of UTC.
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    delay(static_cast<uint32_t>((1000000L - tv.tv_usec) / 1000L));
    std::lock_guard<std::mutex> g(g_i2c_mu);
    set_ok = g_rtc.set(static_cast<int64_t>(time(nullptr)));
  }
  g_last_ntp_ms.store(millis());
  g_ntp_ever.store(true);
  if (must_set) {
    logf("ntp sync: RTC was %s (diff %lld s) -> RTC %s", read_ok ? (lost ? "lost" : "off") : "unreadable",
         static_cast<long long>(read_ok ? rtc_unix - ntp : 0), set_ok ? "set" : "WRITE FAILED");
  } else if (!was_valid) {
    logf("ntp sync: time valid%s", g_rtc.present() ? "" : " (no RTC fitted: fault 4 stays)");
  }
}

void uploader_task(void*) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_task_wdt_reset();
    handle_ntp();

    bool have_wifi;
    {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      have_wifi = !g_settings.wifi_ssid.empty();
    }
    const bool time_valid = g_dl->time_valid();
    bool need = false;
    if (have_wifi) {
      if (time_valid) {
        g_ntp_window_started = false;
        need = ast::radio_needed(g_radio_on.load(), g_dl->uploader().seconds_until_due(), true);
      } else {
        // No valid time: NTP is the only way to start logging. Try in
        // windows so that a missing hotspot does not keep the radio on.
        const uint32_t ms = millis();
        if (!g_ntp_window_started) { g_ntp_window_started = true; g_ntp_window_start_ms = ms; }
        const uint32_t phase = (ms - g_ntp_window_start_ms) % (kNtpWindowOnMs + kNtpWindowOffMs);
        need = phase < kNtpWindowOnMs;
      }
    }
    if (need && !g_radio_on.load()) radio_start();
    if (time_valid) g_dl->uploader_tick();      // may block up to the HTTP timeouts; never holds the buffer mutex meanwhile
    handle_ntp();
    if (!need && g_radio_on.load()) radio_stop();
  }
}

// ---- CLI ---------------------------------------------------------------------------------
const char* wifi_state_text() {
  if (!g_radio_on.load()) return "off";
  switch (WiFi.status()) {
    case WL_CONNECTED: return "connected";
    case WL_NO_SSID_AVAIL: return "hotspot not found";
    case WL_CONNECT_FAILED: return "connect failed (password?)";
    case WL_DISCONNECTED: return "connecting";
    default: return "starting";
  }
}

const char* time_source_text(ast::TimeValidity::Source s) {
  switch (s) {
    case ast::TimeValidity::Source::kRtc: return "RTC";
    case ast::TimeValidity::Source::kNtp: return "NTP";
    case ast::TimeValidity::Source::kManual: return "manual";
    case ast::TimeValidity::Source::kNone: break;
  }
  return "none";
}

std::string mask(const std::string& secret) {
  if (secret.empty()) return "(not set)";
  if (secret.size() <= 4) return "****";
  return secret.substr(0, 2) + "****" + secret.substr(secret.size() - 2);
}

void cmd_status() {
  const ast::DataLinkStatus st = g_dl->status();
  const int64_t now = static_cast<int64_t>(time(nullptr));
  Settings cfg;
  {
    std::lock_guard<std::mutex> g(g_cfg_mu);
    cfg = g_settings;
  }
  Serial.printf("AST data link, contract %s\n", ast::kContractVersion);
  Serial.printf("time      %s UTC  valid=%s source=%s rtc=%s\n", iso(now).c_str(), st.time_valid ? "yes" : "NO (not logging)",
                time_source_text(st.time_source), st.rtc_missing ? "MISSING" : "ok");
  Serial.printf("run       r=%u p=%c  simulator=%s%s%s\n", static_cast<unsigned>(st.run), st.panel, st.sim_on ? "ON (" : "off",
                st.sim_on ? ast::scenario_name(st.scenario) : "", st.sim_on ? ", samples marked s=1)" : "");
  Serial.printf("fault     data-link code now: %u\n", static_cast<unsigned>(st.current_fault));
  Serial.printf("buffer    %lu stored, %lu waiting for upload, capacity %lu, next seq %lu, lost %lu\n",
                static_cast<unsigned long>(st.count), static_cast<unsigned long>(st.pending), static_cast<unsigned long>(st.capacity),
                static_cast<unsigned long>(st.next_seq), static_cast<unsigned long>(st.lost_total));
  if (st.has_last) {
    char entry[ast::kJsonEntryMax];
    ast::format_entry(st.last, entry, sizeof entry);
    Serial.printf("last      %s\n", entry);
  }
  Serial.printf("wifi      ssid=\"%s\" state=%s\n", cfg.wifi_ssid.c_str(), wifi_state_text());
  Serial.printf("thingspeak ch1=%s write=%s read=%s every %d min\n", cfg.ch1.empty() ? "(not set)" : cfg.ch1.c_str(),
                mask(cfg.write_key).c_str(), mask(cfg.read_key).c_str(), cfg.upload_min);
  Serial.printf("upload    last ok %s, batches %lu, records %lu, failures in a row %d, last http %d%s%s%s\n",
                st.upload.last_success > 0 ? iso(st.upload.last_success).c_str() : "never", static_cast<unsigned long>(st.upload.batches_ok),
                static_cast<unsigned long>(st.upload.records_ok), st.upload.fail_count, st.upload.last_http_status,
                st.upload.in_doubt ? ", batch in doubt" : "", st.upload.bisecting ? ", narrowing down a rejected batch" : "",
                st.upload.fault9 ? ", FAULT 9" : "");
  if (st.upload.next_retry > 0) Serial.printf("          next retry %s\n", iso(st.upload.next_retry).c_str());
  if (st.upload.dropped > 0) Serial.printf("          records dropped (rejected by server): %lu\n", static_cast<unsigned long>(st.upload.dropped));
  Serial.printf("counters  logged %lu, skipped minutes %lu, storage errors %lu, crc errors %lu, negative means clamped %lu\n",
                static_cast<unsigned long>(st.logged), static_cast<unsigned long>(st.skipped_minutes), static_cast<unsigned long>(st.storage_errors),
                static_cast<unsigned long>(st.crc_errors), static_cast<unsigned long>(st.negative_clamps));
  Serial.printf("system    heap free %lu B (min %lu), flash fs %lu/%lu kB, uptime %lu s\n", static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMinFreeHeap()), static_cast<unsigned long>(g_storage.used_bytes() / 1024),
                static_cast<unsigned long>(g_storage.total_bytes() / 1024), static_cast<unsigned long>(millis() / 1000));
}

void cmd_time() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  int64_t rtc_unix = 0;
  bool lost = true, ok = false;
  float temp = NAN;
  {
    std::lock_guard<std::mutex> g(g_i2c_mu);
    ok = g_rtc.present() && g_rtc.read(&rtc_unix, &lost);
    if (ok) temp = g_rtc.temperature();
  }
  const ast::DataLinkStatus st = g_dl->status();
  Serial.printf("system %s UTC, valid=%s, source=%s\n", iso(now).c_str(), st.time_valid ? "yes" : "NO", time_source_text(st.time_source));
  if (ok) {
    Serial.printf("RTC    %s UTC, oscillator-stop flag=%s, diff to system %lld s, chip %.2f C\n", iso(rtc_unix).c_str(),
                  lost ? "SET (time lost)" : "clear", static_cast<long long>(rtc_unix - now), static_cast<double>(temp));
  } else {
    Serial.printf("RTC    %s\n", g_rtc.present() ? "read error" : "not found at 0x68");
  }
  if (g_ntp_ever.load()) Serial.printf("NTP    last sync %lu s ago\n", static_cast<unsigned long>((millis() - g_last_ntp_ms.load()) / 1000u));
  else Serial.printf("NTP    no sync since boot\n");
}

void cmd_time_set(int64_t unix_s) {
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(unix_s);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  bool ok;
  {
    std::lock_guard<std::mutex> g(g_i2c_mu);
    ok = g_rtc.set(unix_s);
  }
  g_dl->time_on_manual_set(g_rtc.present());
  Serial.printf("time set to %s UTC, RTC %s\n", iso(unix_s).c_str(), ok ? "written" : (g_rtc.present() ? "WRITE FAILED" : "not fitted"));
}

void cmd_buffer() {
  const ast::DataLinkStatus st = g_dl->status();
  Serial.printf("capacity %lu records (%lu days at 1/min), stored %lu, pending %lu\n", static_cast<unsigned long>(st.capacity),
                static_cast<unsigned long>(st.capacity / 1440), static_cast<unsigned long>(st.count), static_cast<unsigned long>(st.pending));
  Serial.printf("cursors head=%lu tail=%lu ack=%lu, next seq %lu, newest minute %s\n", static_cast<unsigned long>(st.head),
                static_cast<unsigned long>(st.tail), static_cast<unsigned long>(st.ack), static_cast<unsigned long>(st.next_seq),
                st.last_minute ? iso(st.last_minute).c_str() : "-");
  Serial.printf("lost (overwritten before upload) %lu, overflow fault pending=%s, crc errors %lu\n", static_cast<unsigned long>(st.lost_total),
                st.overflow_pending ? "yes" : "no", static_cast<unsigned long>(st.crc_errors));
}

void cmd_buffer_dump(uint32_t n) {
  if (n > 500) {
    Serial.printf("(showing the newest 500)\n");
    n = 500;
  }
  std::vector<ast::Record> recs(n);
  const size_t got = g_dl->dump_newest(recs.data(), n);
  Serial.printf("seq,created_at_utc,p_trk_mw,p_fix_mw,p_use_mw,ang_panel_deg,ang_sun_deg,temp_c,v_supply_v,status\n");
  for (size_t i = 0; i < got; ++i) {
    const ast::Record& r = recs[i];
    char f[7][16];
    const bool has[7] = {r.has(ast::kHasPtrk), r.has(ast::kHasPfix), r.has(ast::kHasPuse), r.has(ast::kHasAngPanel),
                         r.has(ast::kHasAngSun), r.has(ast::kHasTemp), r.has(ast::kHasVsup)};
    ast::format_tenths(r.p_trk_dmw, f[0], sizeof f[0]);
    ast::format_tenths(r.p_fix_dmw, f[1], sizeof f[1]);
    ast::format_tenths(r.p_use_dmw, f[2], sizeof f[2]);
    ast::format_tenths(r.ang_panel_ddeg, f[3], sizeof f[3]);
    ast::format_tenths(r.ang_sun_ddeg, f[4], sizeof f[4]);
    ast::format_tenths(r.temp_dc, f[5], sizeof f[5]);
    ast::format_milli(r.vsup_mv, f[6], sizeof f[6]);
    char status[ast::kStatusMax];
    ast::build_status(r, status, sizeof status);
    Serial.printf("%lu,%s", static_cast<unsigned long>(r.seq), iso(r.minute_unix).c_str());
    for (int k = 0; k < 7; ++k) Serial.printf(",%s", has[k] ? f[k] : "");
    Serial.printf(",%s\n", status);
  }
  Serial.printf("(%u records)\n", static_cast<unsigned>(got));
}

void cmd_power() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  uint32_t today, sessions, last;
  {
    std::lock_guard<std::mutex> g(g_cfg_mu);
    today = g_radio.today_seconds(now);
    sessions = g_radio.sessions();
    last = g_radio.last_session_seconds();
  }
  const uint32_t running = radio_session_s();
  Serial.printf("cpu       %lu MHz (idle %lu, during upload %lu)\n", static_cast<unsigned long>(getCpuFrequencyMhz()),
                static_cast<unsigned long>(kCpuMhzIdle), static_cast<unsigned long>(kCpuMhzRadio));
  Serial.printf("wifi      %s\n", wifi_state_text());
  Serial.printf("radio on  today (UTC) %lu s, sessions since boot %lu, last session %lu s%s\n", static_cast<unsigned long>(today + running),
                static_cast<unsigned long>(sessions), static_cast<unsigned long>(last), running ? " (session running)" : "");
  Serial.printf("leds      off (power LED is hard-wired)\n");
  Serial.printf("note      P_use (INA226 0x44) is the measured truth; see the energy budget in firmware/README.md\n");
}

void cmd_cal_show() {
  static const char* const names[3] = {"trk 0x40", "fix 0x41", "use 0x44"};
  for (int i = 0; i < 3; ++i) {
    ast::InaReading r;
    bool ok;
    ast::InaChannelCal cal;
    {
      std::lock_guard<std::mutex> g(g_i2c_mu);
      ok = g_ina[i].read(&r);
      cal = g_ina[i].cal();
    }
    if (ok) {
      Serial.printf("%s  raw %.4f V %.3f mA | calibrated %.4f V %.3f mA %.2f mW\n", names[i], static_cast<double>(r.raw_volts),
                    static_cast<double>(r.raw_milliamps), static_cast<double>(r.volts), static_cast<double>(r.milliamps),
                    static_cast<double>(r.power_mw));
    } else {
      Serial.printf("%s  NOT ANSWERING (or just configured: repeat in 2 s)\n", names[i]);
    }
    Serial.printf("          shunt %.4f ohm, current LSB %.4f mA, V: x%.5f %+.5f V, I: x%.5f %+.4f mA\n", static_cast<double>(cal.scale.shunt_ohm),
                  static_cast<double>(cal.scale.current_lsb_a * 1000.0f), static_cast<double>(cal.volts.gain), static_cast<double>(cal.volts.offset),
                  static_cast<double>(cal.milliamps.gain), static_cast<double>(cal.milliamps.offset));
  }
}

void apply_cal(int ch) {   // g_cfg_mu held by the caller
  std::lock_guard<std::mutex> g(g_i2c_mu);
  g_ina[ch].set_cal(g_settings.cal[ch]);
}

void cmd_cal_point(const ast::Command& c) {
  const int ch = static_cast<int>(c.cal_channel);
  const int q = c.cal_is_current ? 1 : 0;
  ast::InaReading r;
  bool ok;
  {
    std::lock_guard<std::mutex> g(g_i2c_mu);
    ok = g_ina[ch].read(&r);
  }
  if (!ok) {
    Serial.printf("cal: sensor %s is not answering\n", ast::cal_channel_name(c.cal_channel));
    return;
  }
  const float raw = c.cal_is_current ? r.raw_milliamps : r.raw_volts;
  std::lock_guard<std::mutex> g(g_cfg_mu);
  ast::CalPoint& p = g_cal_points[ch][q][c.cal_point - 1];
  p.set = true;
  p.raw = raw;
  p.ref = static_cast<float>(c.value);
  Serial.printf("cal %s %s point %d: sensor %.5f, DMM %.5f %s\n", ast::cal_channel_name(c.cal_channel), q ? "i" : "v", c.cal_point,
                static_cast<double>(raw), c.value, q ? "mA" : "V");
  if (g_cal_points[ch][q][0].set && g_cal_points[ch][q][1].set) {
    ast::LinCal lin;
    const char* why = "";
    if (ast::two_point_cal(g_cal_points[ch][q][0], g_cal_points[ch][q][1], &lin, &why)) {
      if (q) g_settings.cal[ch].milliamps = lin; else g_settings.cal[ch].volts = lin;
      apply_cal(ch);
      settings_save_cal(g_settings, ch);
      Serial.printf("cal %s %s: gain %.5f offset %+.5f - stored in NVS\n", ast::cal_channel_name(c.cal_channel), q ? "i" : "v",
                    static_cast<double>(lin.gain), static_cast<double>(lin.offset));
    } else {
      Serial.printf("cal NOT changed: %s\n", why);
    }
    g_cal_points[ch][q][0].set = g_cal_points[ch][q][1].set = false;
  } else {
    Serial.printf("now set the other operating point and enter point %d\n", c.cal_point == 1 ? 2 : 1);
  }
}

void handle_command(const std::string& line) {
  const ast::Command c = ast::cli_parse(line);
  using ast::CmdType;
  switch (c.type) {
    case CmdType::kEmpty: break;
    case CmdType::kError: Serial.printf("error: %s\n", c.error.c_str()); break;
    case CmdType::kHelp: Serial.print(ast::cli_help()); break;
    case CmdType::kStatus: cmd_status(); break;
    case CmdType::kTime: cmd_time(); break;
    case CmdType::kTimeSet: cmd_time_set(c.unix_time); break;
    case CmdType::kBuffer: cmd_buffer(); break;
    case CmdType::kBufferDump: cmd_buffer_dump(c.num); break;
    case CmdType::kBufferClear:
      Serial.printf(g_dl->clear_buffer() ? "buffer cleared (seq continues)\n" : "buffer clear FAILED (storage)\n");
      break;
    case CmdType::kUploadNow:
      g_dl->uploader().request_now();
      Serial.printf("upload requested (%lu records waiting)\n", static_cast<unsigned long>(g_dl->pending()));
      break;
    case CmdType::kUploadEvery: {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      g_settings.upload_min = static_cast<int>(c.num);
      g_dl->uploader().set_upload_min(g_settings.upload_min);
      settings_save_misc(g_settings);
      Serial.printf("upload every %d min, at second 20\n", g_settings.upload_min);
      break;
    }
    case CmdType::kRun: {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      g_settings.run = static_cast<uint16_t>(c.num);
      g_dl->set_run(g_settings.run);
      settings_save_misc(g_settings);
      Serial.printf("run id %u%s\n", static_cast<unsigned>(g_settings.run),
                    c.num == 0 ? " (no test running)" : (c.num >= 900 ? " (calibration run: both panels fixed)" : " (tracking test)"));
      break;
    }
    case CmdType::kPanel: {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      g_settings.panel = c.panel;
      g_dl->set_panel(c.panel);
      settings_save_misc(g_settings);
      Serial.printf("panel %c is on the tracker\n", c.panel);
      break;
    }
    case CmdType::kSim: {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      g_settings.sim_on = c.flag;
      if (c.has_scenario) g_settings.scenario = c.scenario;
      ast::SimConfig sc;
      sc.scenario = g_settings.scenario;
      g_dl->set_sim_config(sc);
      g_dl->set_sim(c.flag);
      settings_save_misc(g_settings);
      if (c.flag) Serial.printf("SIMULATOR ON (%s): samples are marked s=1 and are NOT measurements\n", ast::scenario_name(g_settings.scenario));
      else Serial.printf("simulator off: real sensors\n");
      break;
    }
    case CmdType::kCalShow: cmd_cal_show(); break;
    case CmdType::kCalPoint: cmd_cal_point(c); break;
    case CmdType::kCalReset: {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      const int ch = static_cast<int>(c.cal_channel);
      if (c.cal_is_current) g_settings.cal[ch].milliamps = ast::LinCal(); else g_settings.cal[ch].volts = ast::LinCal();
      apply_cal(ch);
      settings_save_cal(g_settings, ch);
      Serial.printf("cal %s %s reset to gain 1, offset 0\n", ast::cal_channel_name(c.cal_channel), c.cal_is_current ? "i" : "v");
      break;
    }
    case CmdType::kCalShunt: {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      const int ch = static_cast<int>(c.cal_channel);
      g_settings.cal[ch].scale.shunt_ohm = static_cast<float>(c.value);
      if (c.value2 > 0.0) g_settings.cal[ch].scale.current_lsb_a = static_cast<float>(c.value2 / 1000.0);
      apply_cal(ch);
      settings_save_cal(g_settings, ch);
      const float lsb = ast::ina226_effective_current_lsb(g_settings.cal[ch].scale);
      Serial.printf("cal %s: shunt %.4f ohm, current LSB %.4f mA, range +/-%.0f mA (shunt limit %.0f mA)\n", ast::cal_channel_name(c.cal_channel),
                    c.value, static_cast<double>(lsb * 1000.0f), static_cast<double>(lsb * 32767.0f * 1000.0f), 81.92 / c.value);
      break;
    }
    case CmdType::kWifi: {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      g_settings.wifi_ssid = c.s1;
      g_settings.wifi_pass = c.s2;
      settings_save_wifi(g_settings);
      Serial.printf("wifi \"%s\" stored in NVS; used at the next connection\n", c.s1.c_str());
      break;
    }
    case CmdType::kKeys: {
      std::lock_guard<std::mutex> g(g_cfg_mu);
      g_settings.ch1 = c.s1;
      g_settings.write_key = c.s2;
      g_settings.read_key = c.s3;
      settings_save_keys(g_settings);
      ast::UploaderConfig uc = g_dl->uploader().config();
      uc.channel = c.s1;
      uc.write_key = c.s2;
      uc.read_key = c.s3;
      g_dl->uploader().set_config(uc);
      Serial.printf("keys stored in NVS: channel %s, write %s, read %s\n", c.s1.c_str(), mask(c.s2).c_str(), mask(c.s3).c_str());
      break;
    }
    case CmdType::kPower: cmd_power(); break;
    case CmdType::kReboot:
      Serial.printf("rebooting\n");
      Serial.flush();
      delay(200);
      ESP.restart();
      break;
  }
}

void leds_off() {
#if defined(ARDUINO_NANO_ESP32)
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);     // yellow LED, active high
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_RED, HIGH);        // RGB LED, active low
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_BLUE, HIGH);
#elif defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
#endif
}

}  // namespace

std::mutex& i2c_mutex() { return g_i2c_mu; }

void app_setup(const Defaults& d) {
  Serial.begin(115200);
  leds_off();
  setCpuFrequencyMhz(kCpuMhzIdle);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);                // radio off until an upload is due
  delay(300);
  Serial.printf("\nAST data link - EEE3035 Group 8 - contract %s\n", ast::kContractVersion);

  // Settings: secrets.h defaults first, NVS on top.
  g_settings.wifi_ssid = d.wifi_ssid ? d.wifi_ssid : "";
  g_settings.wifi_pass = d.wifi_pass ? d.wifi_pass : "";
  g_settings.ch1 = d.ch1 ? d.ch1 : "";
  g_settings.write_key = d.write_key ? d.write_key : "";
  g_settings.read_key = d.read_key ? d.read_key : "";
  settings_load(&g_settings);
  g_http.set_pinned_root(d.root_ca_pem);

  // I2C on A4 (SDA) / A5 (SCL), 100 kHz, 50 ms bus timeout so a stuck device
  // cannot stall the sampler.
  Wire.begin(SDA, SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  const bool fs_ok = g_storage.begin();
  Serial.printf("flash file system: %s\n", fs_ok ? "mounted" : "NOT AVAILABLE (select partition scheme 'With SPIFFS partition')");

  g_dl = new ast::DataLink(g_storage, g_http, g_clock, &g_sink);
  const ast::RingBuffer::LoadResult lr = g_dl->begin();
  const ast::DataLinkStatus st = g_dl->status();
  Serial.printf("buffer: %s, %lu stored, %lu waiting, next seq %lu\n",
                lr == ast::RingBuffer::LoadResult::kLoaded ? "loaded" : (lr == ast::RingBuffer::LoadResult::kFresh ? "new" : "REBUILT from records"),
                static_cast<unsigned long>(st.count), static_cast<unsigned long>(st.pending), static_cast<unsigned long>(st.next_seq));

  ast::UploaderConfig uc;
  uc.channel = g_settings.ch1;
  uc.write_key = g_settings.write_key;
  uc.read_key = g_settings.read_key;
  uc.upload_min = g_settings.upload_min;
  g_dl->uploader().set_config(uc);
  g_dl->set_run(g_settings.run);
  g_dl->set_panel(g_settings.panel);
  ast::SimConfig sc;
  sc.scenario = g_settings.scenario;
  g_dl->set_sim_config(sc);
  g_dl->set_sim(g_settings.sim_on);
  if (g_settings.sim_on) Serial.printf("SIMULATOR ON (%s): samples are marked s=1\n", ast::scenario_name(g_settings.scenario));

  // Clock: RTC -> system time (UTC). Lost time => no logging until NTP (7.1).
  int64_t rtc_unix = 0;
  bool lost = true;
  const bool rtc_present = g_rtc.begin();
  const bool rtc_read = rtc_present && g_rtc.read(&rtc_unix, &lost);
  g_dl->time_begin(rtc_present, lost || !rtc_read, rtc_unix);
  if (g_dl->time_valid()) {
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(rtc_unix);
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    Serial.printf("RTC ok: %s UTC\n", iso(rtc_unix).c_str());
  } else {
    Serial.printf("RTC %s: FAULT 4, not logging until NTP (or `time <ISO>`)\n", rtc_present ? "lost time" : "not found");
  }

  for (int i = 0; i < 3; ++i) {
    g_ina[i].set_cal(g_settings.cal[i]);
    ast::InaReading r;
    g_ina[i].read(&r);   // configures the chip
    Serial.printf("INA226 0x%02X %s\n", kInaAddr[i], g_ina[i].ok() ? "found" : "NOT FOUND");
  }

  // Task watchdog: 180 s, panic (= reboot) on expiry (contract 7.4).
  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = static_cast<uint32_t>(ast::kWatchdogS) * 1000u;
  wdt.idle_core_mask = 0;
  wdt.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);

  xTaskCreatePinnedToCore(sampler_task, "ast_sampler", 8192, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(uploader_task, "ast_uploader", 16384, nullptr, 2, nullptr, 1);
  Serial.printf("type 'help' for commands\n");
}

void app_loop() {
  static ast::LineReader reader;
  std::string line;
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch < 0) break;
    if (reader.feed(static_cast<char>(ch), &line)) handle_command(line);
  }
  unsigned dropped = 0;
  while (g_lines.pop(&line, &dropped)) Serial.println(line.c_str());
  if (dropped > 0) Serial.printf("(%u log lines dropped)\n", dropped);
  if (g_save_misc.exchange(false)) {
    std::lock_guard<std::mutex> g(g_cfg_mu);
    settings_save_misc(g_settings);
  }
  delay(20);   // yields to the other tasks; the CLI does not need to be faster
}

}  // namespace ast_esp
