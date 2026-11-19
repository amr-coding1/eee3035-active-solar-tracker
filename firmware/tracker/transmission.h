// transmission.h - everything that leaves the board. Owner: AM (data link,
// App/Website). EEE3035 Group 8, Active Solar Tracker.
//
// transmission::begin() starts the data link, which then runs on its own in
// background tasks (it is the tested firmware from firmware/ast_datalink):
//   - reads the three power sensors once a second: tracked panel, fixed panel
//     and the tracker's own use on the 5 V feed (INA226 or INA219)
//   - averages a one-minute record, timestamps it in UTC (DS3231 clock, set
//     from the internet), keeps 8 days of records in flash
//   - uploads them to ThingSpeak channel 1 over HTTPS every 15 minutes, with
//     Wi-Fi off in between; the dashboard and the nightly MATLAB job read
//     them from there (docs/CONTRACT.md)
//   - serial commands at 115200 baud (type `help`)
//
// The rest of the firmware only says what the tracker is doing, once per
// control cycle (at least once a second):
//
//   transmission::Report r;
//   r.panel_deg = servo::angle();     r.sun_deg = sun.sun_deg;
//   r.sun_valid = sun.valid;          r.state = transmission::State::kTracking;
//   r.servo_moving = servo::moving(); r.fault = 0;
//   transmission::report(r);
//
// If report() is not called for 5 s the data link treats the control code as
// silent: angles unknown, state IDL. Powers are never passed in: the data
// link reads the sensors itself.
#pragma once
#include <stdint.h>

#include <mutex>

namespace transmission {

enum class State : uint8_t {
  kTracking,   // TRK: following the light
  kHolding,    // HLD: light but no usable direction, or manual mode
  kIdle,       // IDL: dark
  kStowed,     // STW: parked at the stow angle
  kFault       // FLT: stopped because of a fault
};

struct Report {
  float panel_deg = 0.0f;     // panel angle: 0 = level, east negative, west positive
  float sun_deg = 0.0f;       // sun angle from the photodiode arc, same convention
  bool sun_valid = false;     // false: dark, cloud or sensor fault (the sun angle is not uploaded)
  State state = State::kIdle;
  bool servo_moving = false;  // true while the servo is powered for a move (the minute then shows MOV)
  uint8_t fault = 0;          // 0 none, 5 sun-sensor arc, 7 travel limit, 8 servo stall/overload/slip
};

// Call first in setup(): serial port, clock, power sensors, flash buffer,
// Wi-Fi settings and keys (secrets.h, or the `wifi` and `keys` commands).
void begin();

// Call every control cycle (from loop() or a task, never from an interrupt).
void report(const Report& r);

// Optional panel temperature (requirement F6). Without it field6 is omitted.
void set_panel_temperature(float deg_c);

// Call on every pass of loop(): reads serial commands, prints log lines.
// Returns at once.
void poll();

// Timestamped log line on the serial monitor (any task, not an interrupt).
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- read-only views for the other modules ------------------------------------------
struct Power {
  bool tracked_ok = false, fixed_ok = false, use_ok = false;   // sensor answered at the last 1 Hz read
  float tracked_mw = 0, fixed_mw = 0, use_mw = 0;              // instantaneous (1 s average)
  float supply_v = 0;                                          // 5 V feed (field7 V_supply)
};
Power power();

struct Status {
  bool time_valid = false;   // false: nothing is logged (no clock and no NTP yet)
  bool wifi_on = false;      // radio on right now (only around uploads)
  uint32_t waiting = 0;      // records not yet on ThingSpeak
  long last_upload_s_ago = -1;   // -1 = no upload since boot
  uint8_t fault = 0;         // data-link fault now: 4 clock, 6 buffer overflow, 9 upload failing
  bool simulator = false;    // `sim on`: data is simulated (marked s=1)
};
Status status();

// Other modules can add serial commands: the handler gets every line the
// data link does not know and returns true if it was one of its own.
// help_lines (static text) is printed after the data link's help.
using CommandHandler = bool (*)(const char* line);
void add_commands(CommandHandler handler, const char* help_lines);

// The I2C bus (Wire on A4/A5) is shared with the data link's tasks: hold this
// mutex around any other Wire transaction.
std::mutex& i2c_mutex();

}  // namespace transmission
