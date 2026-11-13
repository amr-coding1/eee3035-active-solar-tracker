// AST data link - EEE3035 Group 8 "Active Solar Tracker" (AM: data manager).
// Board: Arduino Nano ESP32, core esp32:esp32 3.3.x, FQBN
// esp32:esp32:nano_nora:PartitionScheme=spiffs (contract 7.1).
// Build: firmware/build.sh      Docs: firmware/README.md, firmware/INTERFACE.md
//
// This sketch contains the data link only. The control system (sun sensor,
// motor) hands its state over with the ast::telemetry_set_*() functions
// (firmware/INTERFACE.md); with no control code present the angles are simply
// reported as unknown.
#include "src/core/ast_telemetry.h"
#include "src/esp_app.h"

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

void setup() {
  const ast_esp::Defaults defaults = {WIFI_SSID, WIFI_PASS, TS_CH1, TS_WRITE_KEY, TS_READ_KEY, TS_ROOT_CA_PEM};
  ast_esp::app_setup(defaults);
}

void loop() {
  ast_esp::app_loop();
}
