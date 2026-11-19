// transmission.cpp - see transmission.h. Owner: AM.
// A thin layer over the data link in src/datalink (generated copy of
// firmware/core + firmware/ast_datalink/src: never edit it there, run
// firmware/sync_core.sh after changing the sources).
#include "transmission.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <string>

#include "config.h"
#include "src/datalink/core/ast_telemetry.h"
#include "src/datalink/esp_app.h"

#if __has_include("secrets.h")
#include "secrets.h"   // git-ignored; copy secrets.example.h
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef TS_CH1
#define TS_CH1 ""
#endif
#ifndef TS_WRITE_KEY
#define TS_WRITE_KEY ""
#endif
#ifndef TS_READ_KEY
#define TS_READ_KEY ""
#endif
#ifndef TS_ROOT_CA_PEM
#define TS_ROOT_CA_PEM nullptr   // nullptr = verify with the built-in CA bundle
#endif

namespace transmission {

namespace {

constexpr int kMaxHandlers = 6;
CommandHandler g_handlers[kMaxHandlers];
int g_handler_count = 0;
std::string g_help;   // help text of all added commands (built in setup())

bool dispatch(const std::string& line) {
  for (int i = 0; i < g_handler_count; ++i) {
    if (g_handlers[i](line.c_str())) return true;
  }
  return false;
}

ast::State to_contract(State s) {
  switch (s) {
    case State::kTracking: return ast::State::TRK;
    case State::kHolding: return ast::State::HLD;
    case State::kIdle: return ast::State::IDL;
    case State::kStowed: return ast::State::STW;
    case State::kFault: return ast::State::FLT;
  }
  return ast::State::IDL;
}

}  // namespace

void begin() {
  ast_esp::Defaults d = {WIFI_SSID, WIFI_PASS, TS_CH1, TS_WRITE_KEY, TS_READ_KEY, TS_ROOT_CA_PEM};
  d.ina_addr[0] = cfg::kInaTracked;
  d.ina_addr[1] = cfg::kInaFixed;
  d.ina_addr[2] = cfg::kInaUse;
  d.ina_addr_use2 = cfg::kInaUse2;
  d.banner = "AST tracker firmware (sensing, control, servo, data link)";
  ast_esp::app_set_command_hook(dispatch, g_help.c_str());
  ast_esp::app_setup(d);
}

void report(const Report& r) {
  ast::telemetry_set_angles(r.panel_deg, r.sun_deg, r.sun_valid);
  ast::telemetry_set_state(to_contract(r.state));
  ast::telemetry_set_motor_running(r.servo_moving);
  ast::telemetry_set_fault(r.fault);
}

void set_panel_temperature(float deg_c) { ast::telemetry_set_temperature(deg_c); }

void poll() { ast_esp::app_poll(); }

void log(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  ast_esp::app_log("%s", buf);
}

Power power() {
  const ast_esp::PowerSnapshot p = ast_esp::app_latest_power();
  Power out;
  out.tracked_ok = p.ok[0];
  out.fixed_ok = p.ok[1];
  out.use_ok = p.ok[2];
  out.tracked_mw = p.power_mw[0];
  out.fixed_mw = p.power_mw[1];
  out.use_mw = p.power_mw[2];
  out.supply_v = p.volts[2];
  return out;
}

Status status() {
  const ast_esp::Brief b = ast_esp::app_brief();
  Status s;
  s.time_valid = b.time_valid;
  s.wifi_on = b.radio_on;
  s.waiting = b.pending;
  s.fault = b.fault;
  s.simulator = b.sim_on;
  const int64_t now = static_cast<int64_t>(time(nullptr));
  s.last_upload_s_ago = (b.last_upload > 0 && b.time_valid && now >= b.last_upload) ? static_cast<long>(now - b.last_upload) : -1;
  return s;
}

void add_commands(CommandHandler handler, const char* help_lines) {
  if (handler == nullptr || g_handler_count >= kMaxHandlers) return;
  g_handlers[g_handler_count++] = handler;
  if (help_lines != nullptr) g_help += help_lines;
  ast_esp::app_set_command_hook(dispatch, g_help.c_str());   // the text moved: hand over the new pointer
}

std::mutex& i2c_mutex() { return ast_esp::i2c_mutex(); }

}  // namespace transmission
