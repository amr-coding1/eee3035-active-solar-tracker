// Time validity (contract 7.1): the device may only log when it knows UTC.
//
//   boot: RTC present, oscillator-stop flag clear, plausible date -> VALID
//         otherwise                                                -> INVALID (fault 4, no logging)
//   NTP success (or a manual `time` set)                           -> VALID
//   RTC is (re)written from NTP when it lost time, or |RTC - NTP| > 2 s.
// While the RTC chip is missing altogether fault 4 stays raised even after
// NTP made the time valid (the next power cycle without Wi-Fi would be blind).
#pragma once
#include <stdint.h>

#include "ast_config.h"

namespace ast {

class TimeValidity {
 public:
  enum class Source { kNone, kRtc, kNtp, kManual };

  void begin(bool rtc_present, bool rtc_lost_flag, int64_t rtc_unix);
  // Call when NTP delivered `ntp_unix`. Returns true when the RTC must be set
  // to the NTP time (contract: lost time, or more than 2 s apart).
  bool on_ntp(int64_t ntp_unix, bool rtc_present, int64_t rtc_unix);
  // CLI `time <ISO>`: the operator typed the time (bench use without Wi-Fi).
  void on_manual_set(bool rtc_present);

  bool valid() const { return valid_; }
  bool rtc_missing() const { return !rtc_present_; }
  bool rtc_lost() const { return rtc_lost_; }
  // Fault 4 condition right now (section 4: "clock (RTC) missing or lost time").
  bool clock_fault() const { return !valid_ || !rtc_present_; }
  Source source() const { return source_; }
  bool ntp_synced() const { return ntp_synced_; }

 private:
  bool valid_ = false;
  bool rtc_present_ = false;
  bool rtc_lost_ = true;
  bool ntp_synced_ = false;
  Source source_ = Source::kNone;
};

}  // namespace ast
