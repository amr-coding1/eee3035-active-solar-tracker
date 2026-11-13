// Energy awareness. The tracker only pays off if its own consumption (P_use,
// INA226 #3) stays well below the tracking gain of 1.5-2.5 Wh/day, so the
// Wi-Fi radio is OFF except for a few seconds around each upload. RadioMeter
// counts how long it was on; RadioPolicy decides when it has to be on.
#pragma once
#include <stdint.h>

namespace ast {

class RadioMeter {
 public:
  void on(int64_t now_unix);        // no effect if already on
  void off(int64_t now_unix);       // no effect if already off
  bool is_on() const { return on_; }
  // Seconds the radio has been on in the current session (0 when off).
  uint32_t session_seconds(int64_t now_unix) const;
  // Seconds on during the UTC day that contains now_unix (running session included).
  uint32_t today_seconds(int64_t now_unix);
  uint32_t last_session_seconds() const { return last_session_; }
  uint32_t sessions() const { return sessions_; }
  uint64_t total_seconds(int64_t now_unix) const;

 private:
  void roll_day(int64_t now_unix);
  bool on_ = false;
  int64_t on_since_ = 0;
  int64_t day_ = -1;                // unix day number of today_
  uint32_t today_ = 0;              // finished-session seconds booked to day_
  uint32_t last_session_ = 0;
  uint32_t sessions_ = 0;
  uint64_t total_ = 0;
};

// Radio needed?  `seconds_until_due` comes from Uploader::seconds_until_due().
//   - on  `lead_s` before the next request is due (time to join the hotspot,
//     so the request itself still starts at second 20);
//   - stays on while more requests follow within `linger_s` (backlog batches
//     are 16 s apart: reconnecting each time would cost more than waiting);
//   - on while the device has no valid time (it needs NTP to start logging).
constexpr int kRadioLeadS = 8;
constexpr int kRadioLingerS = 20;
bool radio_needed(bool radio_is_on, int64_t seconds_until_due, bool time_valid,
                  int lead_s = kRadioLeadS, int linger_s = kRadioLingerS);

}  // namespace ast
