#include "ast_power.h"

namespace ast {

void RadioMeter::roll_day(int64_t now) {
  const int64_t d = now / 86400;
  if (d != day_) {
    day_ = d;
    today_ = 0;
  }
}

void RadioMeter::on(int64_t now) {
  if (on_) return;
  roll_day(now);
  on_ = true;
  on_since_ = now;
  ++sessions_;
}

void RadioMeter::off(int64_t now) {
  if (!on_) return;
  on_ = false;
  if (now < on_since_) now = on_since_;   // clock stepped back during the session
  const int64_t day_start = (now / 86400) * 86400;
  roll_day(now);
  // Only the part of the session that lies in today's UTC day counts for today.
  const int64_t from = on_since_ > day_start ? on_since_ : day_start;
  today_ += static_cast<uint32_t>(now - from);
  last_session_ = static_cast<uint32_t>(now - on_since_);
  total_ += static_cast<uint64_t>(now - on_since_);
}

uint32_t RadioMeter::session_seconds(int64_t now) const {
  if (!on_ || now < on_since_) return 0;
  return static_cast<uint32_t>(now - on_since_);
}

uint32_t RadioMeter::today_seconds(int64_t now) {
  roll_day(now);
  uint32_t s = today_;
  if (on_ && now > on_since_) {
    const int64_t day_start = (now / 86400) * 86400;
    const int64_t from = on_since_ > day_start ? on_since_ : day_start;
    s += static_cast<uint32_t>(now - from);
  }
  return s;
}

uint64_t RadioMeter::total_seconds(int64_t now) const {
  return total_ + session_seconds(now);
}

bool radio_needed(bool radio_is_on, int64_t seconds_until_due, bool time_valid, int lead_s, int linger_s) {
  if (!time_valid) return true;
  if (seconds_until_due <= lead_s) return true;
  if (radio_is_on && seconds_until_due <= linger_s) return true;
  return false;
}

}  // namespace ast
