#include "esp_settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include "core/ast_config.h"
#include "core/ast_uploader.h"

namespace ast_esp {

namespace {
const char* kNs = "ast";

std::string get_str(Preferences& p, const char* key, const std::string& def) {
  if (!p.isKey(key)) return def;
  const String v = p.getString(key, def.c_str());
  return std::string(v.c_str());
}

// Calibration as six floats: shunt ohm, current LSB (A), V gain, V offset, I gain, I offset.
struct CalBlob { float v[6]; };
const char* const kCalKeys[3] = {"cal_trk", "cal_fix", "cal_use"};
}  // namespace

void settings_load(Settings* s) {
  Preferences p;
  if (!p.begin(kNs, true)) return;   // namespace not created yet: keep defaults
  s->wifi_ssid = get_str(p, "ssid", s->wifi_ssid);
  s->wifi_pass = get_str(p, "pass", s->wifi_pass);
  s->ch1 = get_str(p, "ch1", s->ch1);
  s->write_key = get_str(p, "wkey", s->write_key);
  s->read_key = get_str(p, "rkey", s->read_key);
  const int up = p.getInt("upmin", s->upload_min);
  if (ast::upload_min_is_valid(up)) s->upload_min = up;
  const unsigned run = p.getUShort("run", s->run);
  s->run = run > ast::kRunMax ? 0 : static_cast<uint16_t>(run);
  s->panel = p.getUChar("panel", 'A') == 'B' ? 'B' : 'A';
  s->sim_on = p.getBool("sim", false);
  const uint8_t sc = p.getUChar("simsc", 0);
  s->scenario = sc <= static_cast<uint8_t>(ast::Scenario::kLamp) ? static_cast<ast::Scenario>(sc) : ast::Scenario::kClear;
  for (int i = 0; i < 3; ++i) {
    CalBlob b;
    if (p.getBytesLength(kCalKeys[i]) == sizeof b && p.getBytes(kCalKeys[i], &b, sizeof b) == sizeof b) {
      // Accept only sane numbers: a corrupted blob must not poison the readings.
      const bool sane = b.v[0] > 0.0005f && b.v[0] < 20.0f && b.v[1] > 1e-7f && b.v[1] < 0.02f &&
                        b.v[2] > 0.5f && b.v[2] < 2.0f && b.v[4] > 0.5f && b.v[4] < 2.0f &&
                        isfinite(b.v[3]) && isfinite(b.v[5]);
      if (sane) {
        s->cal[i].scale.shunt_ohm = b.v[0];
        s->cal[i].scale.current_lsb_a = b.v[1];
        s->cal[i].volts.gain = b.v[2];
        s->cal[i].volts.offset = b.v[3];
        s->cal[i].milliamps.gain = b.v[4];
        s->cal[i].milliamps.offset = b.v[5];
      }
    }
  }
  p.end();
}

void settings_save_wifi(const Settings& s) {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putString("ssid", s.wifi_ssid.c_str());
  p.putString("pass", s.wifi_pass.c_str());
  p.end();
}

void settings_save_keys(const Settings& s) {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putString("ch1", s.ch1.c_str());
  p.putString("wkey", s.write_key.c_str());
  p.putString("rkey", s.read_key.c_str());
  p.end();
}

void settings_save_misc(const Settings& s) {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putInt("upmin", s.upload_min);
  p.putUShort("run", s.run);
  p.putUChar("panel", static_cast<uint8_t>(s.panel));
  p.putBool("sim", s.sim_on);
  p.putUChar("simsc", static_cast<uint8_t>(s.scenario));
  p.end();
}

void settings_save_cal(const Settings& s, int channel) {
  if (channel < 0 || channel > 2) return;
  Preferences p;
  if (!p.begin(kNs, false)) return;
  CalBlob b;
  b.v[0] = s.cal[channel].scale.shunt_ohm;
  b.v[1] = s.cal[channel].scale.current_lsb_a;
  b.v[2] = s.cal[channel].volts.gain;
  b.v[3] = s.cal[channel].volts.offset;
  b.v[4] = s.cal[channel].milliamps.gain;
  b.v[5] = s.cal[channel].milliamps.offset;
  p.putBytes(kCalKeys[channel], &b, sizeof b);
  p.end();
}

}  // namespace ast_esp
