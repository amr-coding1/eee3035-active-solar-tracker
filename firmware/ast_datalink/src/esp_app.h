// The ESP32 application around the portable core: tasks, sensors, Wi-Fi,
// NTP/RTC, energy saving and the serial CLI. Called from ast_datalink.ino.
#pragma once
#include <stdint.h>

#include <mutex>
#include <string>

#include "core/ast_cal.h"

namespace ast_esp {

// Defaults from secrets.h (may be empty strings / nullptr). NVS values win.
struct Defaults {
  const char* wifi_ssid;
  const char* wifi_pass;
  const char* ch1;
  const char* write_key;
  const char* read_key;
  const char* root_ca_pem;   // nullptr = built-in CA bundle
  // I2C addresses of the power sensors (INA226 or INA219, found automatically):
  // tracked panel, fixed panel, tracker consumption (5 V feed).
  uint8_t ina_addr[3] = {0x40, 0x41, 0x44};
  // Optional second consumption branch, added to P_use (e.g. the servo branch
  // on its own channel of a four-channel INA219 board). 0 = not fitted.
  uint8_t ina_addr_use2 = 0;
  const char* banner = "AST data link";   // first line printed at boot
};

void app_setup(const Defaults& defaults);   // call once from setup()
void app_loop();                            // app_poll() + delay(20): the data-link-only sketch
void app_poll();                            // serial CLI + log output; returns at once (call often)

// Queues a log line (timestamped, printed by app_poll). Any task, not an ISR.
void app_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Newest 1 Hz readings of the three power sensors (read-only copy).
struct PowerSnapshot {
  bool ok[3] = {false, false, false};          // sensor answered at the last read
  float power_mw[3] = {0, 0, 0};
  float volts[3] = {0, 0, 0};
  float milliamps[3] = {0, 0, 0};
  ast::InaChip chip[3] = {ast::InaChip::kNone, ast::InaChip::kNone, ast::InaChip::kNone};
  uint8_t address[3] = {0, 0, 0};
};
PowerSnapshot app_latest_power();

// Short status for other modules (and the control code's live line).
struct Brief {
  bool time_valid = false;
  bool radio_on = false;
  bool wifi_connected = false;
  uint32_t pending = 0;          // records waiting for upload
  int64_t last_upload = 0;       // UTC of the last successful upload, 0 = never
  int fail_count = 0;            // failed uploads in a row
  uint8_t fault = 0;             // data-link fault code now (4, 6, 9 or 0)
  bool sim_on = false;
};
Brief app_brief();

// Commands the data link does not know are offered to this hook first
// (return true if handled). help_text is appended to `help`.
using CommandHook = bool (*)(const std::string& line);
void app_set_command_hook(CommandHook hook, const char* help_text);

// The I2C bus (Wire, A4/A5) is shared by the data link's tasks. Control-system
// code that also talks I2C must hold this mutex around each transaction:
//   { std::lock_guard<std::mutex> lock(ast_esp::i2c_mutex()); Wire....; }
std::mutex& i2c_mutex();

}  // namespace ast_esp
