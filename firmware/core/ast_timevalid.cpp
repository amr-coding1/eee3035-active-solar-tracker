#include "ast_timevalid.h"

namespace ast {

void TimeValidity::begin(bool rtc_present, bool rtc_lost_flag, int64_t rtc_unix) {
  rtc_present_ = rtc_present;
  // An RTC that claims a date before this firmware existed has lost time too,
  // whatever its oscillator-stop flag says (e.g. flag cleared by other code).
  rtc_lost_ = !rtc_present || rtc_lost_flag || rtc_unix < kMinPlausibleUnix;
  ntp_synced_ = false;
  valid_ = !rtc_lost_;
  source_ = valid_ ? Source::kRtc : Source::kNone;
}

bool TimeValidity::on_ntp(int64_t ntp_unix, bool rtc_present, int64_t rtc_unix) {
  if (ntp_unix < kMinPlausibleUnix) return false;  // not a real NTP answer
  rtc_present_ = rtc_present;
  int64_t diff = rtc_unix - ntp_unix;
  if (diff < 0) diff = -diff;
  const bool must_set = rtc_present && (rtc_lost_ || diff > kRtcSetThresholdS);
  valid_ = true;
  ntp_synced_ = true;
  source_ = Source::kNtp;
  if (rtc_present) rtc_lost_ = false;  // the caller writes the RTC when we return true
  return must_set;
}

void TimeValidity::on_manual_set(bool rtc_present) {
  rtc_present_ = rtc_present;
  valid_ = true;
  if (rtc_present) rtc_lost_ = false;
  source_ = Source::kManual;
}

}  // namespace ast
