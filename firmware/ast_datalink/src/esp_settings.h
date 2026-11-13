// Persistent settings in NVS (Preferences, namespace "ast"): Wi-Fi
// credentials, ThingSpeak keys, upload interval, run/panel, simulator switch
// and the INA226 calibration (contract 7.3, 7.6).
#pragma once
#include <string>

#include "core/ast_cal.h"
#include "core/ast_sim.h"

namespace ast_esp {

struct Settings {
  std::string wifi_ssid, wifi_pass;
  std::string ch1, write_key, read_key;
  int upload_min = 15;
  uint16_t run = 0;
  char panel = 'A';
  bool sim_on = false;                       // contract 7.5: OFF by default
  ast::Scenario scenario = ast::Scenario::kClear;
  ast::InaChannelCal cal[3];                 // 0 trk, 1 fix, 2 use
};

// Loads everything; values missing in NVS keep what `s` already holds (the
// defaults from secrets.h).
void settings_load(Settings* s);
void settings_save_wifi(const Settings& s);
void settings_save_keys(const Settings& s);
void settings_save_misc(const Settings& s);      // upload_min, run, panel, sim
void settings_save_cal(const Settings& s, int channel);

}  // namespace ast_esp
