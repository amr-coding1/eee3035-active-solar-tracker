// sensing.cpp - see sensing.h. Owner: CO (starter code by AM).
#include "sensing.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "src/datalink/core/ast_sun.h"
#include "transmission.h"

namespace sensing {

namespace {

constexpr int kN = 4;
ast::SunArcConfig g_arc;
const char* const kPrefsNs = "ast_sun";   // NVS namespace for the calibration

const char* const kHelp =
    "Sensing (photodiode arc):\n"
    "  sun                      raw/corrected readings, estimate and why\n"
    "  sun cal dark             sensors covered: store dark offsets\n"
    "  sun cal flat             sensors under the same light (diffuser): store gains\n"
    "  sun cal clear            gain 1, offset 0\n"
    "  pot                      panel-angle pot reading (if fitted)\n";

float read_mv(uint8_t pin, int samples) {
  uint32_t acc = 0;
  for (int k = 0; k < samples; ++k) acc += analogReadMilliVolts(pin);
  return static_cast<float>(acc) / static_cast<float>(samples);
}

void read_all(float* mv, int samples) {
  for (int i = 0; i < kN; ++i) mv[i] = read_mv(cfg::kSunPins[i], samples);
}

void load_cal() {
  Preferences p;
  if (!p.begin(kPrefsNs, true)) return;   // nothing stored yet
  char key[4];
  for (int i = 0; i < kN; ++i) {
    snprintf(key, sizeof key, "g%d", i);
    g_arc.gain[i] = p.getFloat(key, 1.0f);
    snprintf(key, sizeof key, "o%d", i);
    g_arc.dark_offset_mv[i] = p.getFloat(key, 0.0f);
  }
  p.end();
}

void save_cal() {
  Preferences p;
  if (!p.begin(kPrefsNs, false)) {
    Serial.printf("sun: could not open NVS, calibration NOT stored\n");
    return;
  }
  char key[4];
  for (int i = 0; i < kN; ++i) {
    snprintf(key, sizeof key, "g%d", i);
    p.putFloat(key, g_arc.gain[i]);
    snprintf(key, sizeof key, "o%d", i);
    p.putFloat(key, g_arc.dark_offset_mv[i]);
  }
  p.end();
}

void print_cal() {
  Serial.printf("calibration (stored in NVS):");
  for (int i = 0; i < kN; ++i) {
    Serial.printf("  ch%d x%.3f -%.0f mV", i, static_cast<double>(g_arc.gain[i]), static_cast<double>(g_arc.dark_offset_mv[i]));
  }
  Serial.printf("\n");
}

void cmd_sun() {
  float mv[kN];
  read_all(mv, 64);
  const ast::SunEstimate e = ast::estimate_sun(mv, g_arc);
  Serial.printf("sensor   facing  raw mV  corrected\n");
  for (int i = 0; i < kN; ++i) {
    const float c = g_arc.gain[i] * (mv[i] - g_arc.dark_offset_mv[i]);
    Serial.printf("  A%d    %+6.1f   %6.0f  %6.0f%s\n", i, static_cast<double>(g_arc.sensor_deg[i]), static_cast<double>(mv[i]),
                  static_cast<double>(c > 0 ? c : 0), mv[i] >= g_arc.saturated_mv ? "  SATURATED" : "");
  }
  if (e.valid) {
    Serial.printf("estimate %.1f deg (%u sensors, amplitude %.0f mV, ambient %.0f mV, misfit %.0f mV)%s\n",
                  static_cast<double>(e.sun_deg), static_cast<unsigned>(e.used), static_cast<double>(e.amplitude_mv),
                  static_cast<double>(e.ambient_mv), static_cast<double>(e.residual_mv), e.saturated ? ", one channel clipped" : "");
  } else {
    Serial.printf("no estimate: %s (total %.0f mV)\n", e.why, static_cast<double>(e.total_mv));
  }
  print_cal();
}

void cmd_cal_dark() {
  float mv[kN];
  read_all(mv, 256);
  for (int i = 0; i < kN; ++i) {
    if (mv[i] > 300.0f) {
      Serial.printf("sun cal dark: A%d reads %.0f mV - cover every sensor completely, then try again\n", i, static_cast<double>(mv[i]));
      return;
    }
  }
  for (int i = 0; i < kN; ++i) g_arc.dark_offset_mv[i] = mv[i];
  save_cal();
  Serial.printf("sun cal dark: offsets stored\n");
  print_cal();
}

void cmd_cal_flat() {
  float mv[kN], c[kN];
  read_all(mv, 256);
  float mean = 0;
  for (int i = 0; i < kN; ++i) {
    if (mv[i] >= g_arc.saturated_mv) {
      Serial.printf("sun cal flat: A%d is saturated - less light, or a smaller load resistor\n", i);
      return;
    }
    c[i] = mv[i] - g_arc.dark_offset_mv[i];
    if (c[i] < 100.0f) {
      Serial.printf("sun cal flat: A%d sees only %.0f mV above dark - more light please (at least 100 mV)\n", i, static_cast<double>(c[i]));
      return;
    }
    mean += c[i] / kN;
  }
  float gain[kN];
  for (int i = 0; i < kN; ++i) {
    gain[i] = mean / c[i];
    if (gain[i] < 0.5f || gain[i] > 2.0f) {
      Serial.printf("sun cal flat: A%d would need gain %.2f (outside 0.5-2): is the light really the same on every sensor?\n", i,
                    static_cast<double>(gain[i]));
      return;
    }
  }
  for (int i = 0; i < kN; ++i) g_arc.gain[i] = gain[i];
  save_cal();
  Serial.printf("sun cal flat: gains stored\n");
  print_cal();
}

void cmd_pot() {
  if (cfg::kPanelPotPin < 0) {
    Serial.printf("pot: not fitted (config.h kPanelPotPin = -1)\n");
    return;
  }
  const float mv = read_mv(static_cast<uint8_t>(cfg::kPanelPotPin), 64);
  float deg = 0;
  panel_angle(&deg);
  Serial.printf("pot: %.0f mV -> panel %.1f deg (config: %.0f mV at -45, %.0f mV at +45)\n", static_cast<double>(mv), static_cast<double>(deg),
                static_cast<double>(cfg::kPotMvAtMinus45), static_cast<double>(cfg::kPotMvAtPlus45));
}

bool handle(const char* line) {
  char buf[48];
  strncpy(buf, line, sizeof buf - 1);
  buf[sizeof buf - 1] = '\0';
  char* save = nullptr;
  const char* w0 = strtok_r(buf, " \t", &save);
  if (w0 == nullptr) return false;
  if (strcmp(w0, "pot") == 0) {
    cmd_pot();
    return true;
  }
  if (strcmp(w0, "sun") != 0) return false;
  const char* w1 = strtok_r(nullptr, " \t", &save);
  const char* w2 = strtok_r(nullptr, " \t", &save);
  if (w1 == nullptr) {
    cmd_sun();
  } else if (strcmp(w1, "cal") == 0 && w2 != nullptr && strcmp(w2, "dark") == 0) {
    cmd_cal_dark();
  } else if (strcmp(w1, "cal") == 0 && w2 != nullptr && strcmp(w2, "flat") == 0) {
    cmd_cal_flat();
  } else if (strcmp(w1, "cal") == 0 && w2 != nullptr && strcmp(w2, "clear") == 0) {
    for (int i = 0; i < kN; ++i) {
      g_arc.gain[i] = 1.0f;
      g_arc.dark_offset_mv[i] = 0.0f;
    }
    save_cal();
    Serial.printf("sun cal clear: gain 1, offset 0 on every channel\n");
  } else {
    Serial.printf("error: try `sun`, `sun cal dark`, `sun cal flat` or `sun cal clear`\n");
  }
  return true;
}

}  // namespace

void begin() {
  analogReadResolution(12);
  for (int i = 0; i < kN; ++i) analogSetPinAttenuation(cfg::kSunPins[i], ADC_11db);   // 0-3.1 V
  if (cfg::kPanelPotPin >= 0) analogSetPinAttenuation(static_cast<uint8_t>(cfg::kPanelPotPin), ADC_11db);
  g_arc.n = kN;
  for (int i = 0; i < kN; ++i) g_arc.sensor_deg[i] = cfg::kSunSensorDeg[i];
  g_arc.dark_total_mv = cfg::kSunDarkMv;
  g_arc.saturated_mv = cfg::kSunSaturatedMv;
  g_arc.max_abs_deg = cfg::kSunMaxAbsDeg;
  load_cal();
  transmission::add_commands(handle, kHelp);
}

Sun read_sun() {
  Sun s;
  read_all(s.mv, cfg::kSunSamples);
  const ast::SunEstimate e = ast::estimate_sun(s.mv, g_arc);
  s.valid = e.valid;
  s.dark = e.dark;
  s.saturated = e.saturated;
  s.sun_deg = e.sun_deg;
  s.total_mv = e.total_mv;
  s.why = e.why;
  return s;
}

bool panel_angle(float* deg) {
  if (cfg::kPanelPotPin < 0) return false;
  const float span = cfg::kPotMvAtPlus45 - cfg::kPotMvAtMinus45;
  if (fabsf(span) < 1.0f) return false;
  const float mv = read_mv(static_cast<uint8_t>(cfg::kPanelPotPin), cfg::kSunSamples);
  *deg = -45.0f + (mv - cfg::kPotMvAtMinus45) * 90.0f / span;
  return true;
}

}  // namespace sensing
