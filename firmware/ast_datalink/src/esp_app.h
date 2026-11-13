// The ESP32 application around the portable core: tasks, sensors, Wi-Fi,
// NTP/RTC, energy saving and the serial CLI. Called from ast_datalink.ino.
#pragma once
#include <mutex>

namespace ast_esp {

// Defaults from secrets.h (may be empty strings / nullptr). NVS values win.
struct Defaults {
  const char* wifi_ssid;
  const char* wifi_pass;
  const char* ch1;
  const char* write_key;
  const char* read_key;
  const char* root_ca_pem;   // nullptr = built-in CA bundle
};

void app_setup(const Defaults& defaults);   // call once from setup()
void app_loop();                            // call from loop(): serial CLI + log output

// The I2C bus (Wire, A4/A5) is shared by the data link's tasks. Control-system
// code that also talks I2C must hold this mutex around each transaction:
//   { std::lock_guard<std::mutex> lock(ast_esp::i2c_mutex()); Wire....; }
std::mutex& i2c_mutex();

}  // namespace ast_esp
