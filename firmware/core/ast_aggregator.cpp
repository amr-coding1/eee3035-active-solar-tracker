#include "ast_aggregator.h"

#include <math.h>

#include "ast_config.h"
#include "ast_time.h"

namespace ast {

int32_t round_scaled(double value, double scale, int32_t lo, int32_t hi) {
  double x = value * scale;
  x = (x >= 0.0) ? floor(x + 0.5) : -floor(-x + 0.5);
  if (x < static_cast<double>(lo)) return lo;
  if (x > static_cast<double>(hi)) return hi;
  return static_cast<int32_t>(x);
}

void Aggregator::reset() {
  minute_ = 0;
  count_ = 0;
  p_trk_ = p_fix_ = p_use_ = vbat_ = temp_ = Mean();
  motor_ran_ = false;
  any_simulated_ = false;
  ctrl_fault_ = 0;
}

void Aggregator::accumulate(const Sample1Hz& s) {
  // A reading that is not a finite number is treated as "no reading".
  if (s.p_trk_valid && isfinite(s.p_trk_mw)) p_trk_.add(s.p_trk_mw);
  if (s.p_fix_valid && isfinite(s.p_fix_mw)) p_fix_.add(s.p_fix_mw);
  if (s.p_use_valid && isfinite(s.p_use_mw)) p_use_.add(s.p_use_mw);
  if (s.vbat_valid && isfinite(s.vbat_v)) vbat_.add(s.vbat_v);
  if (s.temp_valid && isfinite(s.temp_c)) temp_.add(s.temp_c);
  if (s.motor_running) motor_ran_ = true;
  if (s.simulated) any_simulated_ = true;
  if (s.ctrl_fault != 0) ctrl_fault_ = s.ctrl_fault;
  last_ = s;
  ++count_;
}

uint32_t Aggregator::power_to_dmw(const Mean& m) {
  const double mean = m.sum / m.n;
  if (mean < 0.0) {
    ++negative_clamps_;   // contract 3.1: clamped to 0, e unchanged
    return 0;
  }
  // 0.1 mW units; 400 W would still fit, far above any panel on this rig.
  return static_cast<uint32_t>(round_scaled(mean, 10.0, 0, 2000000000));
}

void Aggregator::finish(Record* out) {
  Record r;
  r.minute_unix = static_cast<uint32_t>(minute_);
  if (p_trk_.n > 0) { r.p_trk_dmw = power_to_dmw(p_trk_); r.flags |= kHasPtrk; }
  if (p_fix_.n > 0) { r.p_fix_dmw = power_to_dmw(p_fix_); r.flags |= kHasPfix; }
  if (p_use_.n > 0) { r.p_use_dmw = power_to_dmw(p_use_); r.flags |= kHasPuse; }
  if (vbat_.n > 0) {
    r.vbat_mv = static_cast<uint16_t>(round_scaled(vbat_.sum / vbat_.n, 1000.0, 0, 65535));
    r.flags |= kHasVbat;
  }
  if (temp_.n > 0) {
    r.temp_dc = static_cast<int16_t>(round_scaled(temp_.sum / temp_.n, 10.0, -32768, 32767));
    r.flags |= kHasTemp;
  }
  if (last_.ang_panel_valid && isfinite(last_.ang_panel_deg)) {
    r.ang_panel_ddeg = static_cast<int16_t>(round_scaled(last_.ang_panel_deg, 10.0, -32768, 32767));
    r.flags |= kHasAngPanel;
  }
  if (last_.ang_sun_valid && isfinite(last_.ang_sun_deg)) {
    r.ang_sun_ddeg = static_cast<int16_t>(round_scaled(last_.ang_sun_deg, 10.0, -32768, 32767));
    r.flags |= kHasAngSun;
  }
  r.state = last_.state;
  if (static_cast<uint8_t>(r.state) >= kStateCount) r.state = State::FLT;
  if (motor_ran_ && r.state != State::FLT) r.state = State::MOV;
  r.run = last_.run > kRunMax ? kRunMax : last_.run;
  if (last_.panel == 'B' || last_.panel == 'b') r.flags |= kPanelB;
  if (any_simulated_) r.flags |= kSimulated;
  r.fault = ctrl_fault_ > 9 ? 0 : ctrl_fault_;
  *out = r;
}

bool Aggregator::add(int64_t unix_s, const Sample1Hz& s, Record* out) {
  const int64_t m = minute_floor(unix_s);
  bool emitted = false;
  if (count_ > 0 && m != minute_) {
    if (m > minute_) {
      finish(out);
      emitted = true;
    }
    // m < minute_: the clock stepped back over a minute boundary. The partial
    // minute is dropped; DataLink additionally refuses to log a minute twice.
    reset();
  }
  if (count_ == 0) minute_ = m;
  accumulate(s);
  return emitted;
}

bool Aggregator::flush(int64_t now_unix_s, Record* out) {
  if (count_ == 0 || now_unix_s < minute_ + 60) return false;
  finish(out);
  reset();
  return true;
}

}  // namespace ast
