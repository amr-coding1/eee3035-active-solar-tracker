#include "ast_tracker.h"

#include <math.h>

namespace ast {

namespace {
float clampf(float v, float lim) {
  if (v > lim) return lim;
  if (v < -lim) return -lim;
  return v;
}
}  // namespace

void Tracker::set_manual(float panel_deg) {
  manual_ = true;
  manual_pending_ = true;
  manual_deg_ = panel_deg;
}

void Tracker::set_auto() {
  manual_ = false;
  manual_pending_ = false;
  have_filter_ = false;   // start the filter afresh from the next estimate
}

TrackerDecision Tracker::step(const TrackerInput& in) {
  TrackerDecision d;

  // Sun estimate: smoothed while it stays valid; restarted after a gap.
  if (in.sun_valid && isfinite(in.sun_deg)) {
    const float a = cfg_.filter_alpha > 0.0f && cfg_.filter_alpha <= 1.0f ? cfg_.filter_alpha : 1.0f;
    filtered_ = have_filter_ ? filtered_ + a * (in.sun_deg - filtered_) : in.sun_deg;
    have_filter_ = true;
  } else {
    have_filter_ = false;
  }
  d.sun_valid = have_filter_;
  d.sun_deg = filtered_;

  // Darkness has to last before the panel is stowed (a passing shadow must
  // not send it east).
  if (in.sun_dark) {
    if (!dark_) {
      dark_ = true;
      dark_since_ms_ = in.now_ms;
    }
  } else {
    dark_ = false;
  }

  const bool gap_ok = !moved_once_ || in.now_ms - last_move_ms_ >= cfg_.min_move_gap_ms;
  auto request = [&](float target) {
    d.move = true;
    d.target_deg = target;
    moved_once_ = true;
    last_move_ms_ = in.now_ms;
  };

  if (in.fault != 0) {
    d.state = State::FLT;
    return d;
  }

  if (manual_) {
    d.state = State::HLD;
    if (manual_pending_ && !in.servo_busy) {
      manual_pending_ = false;
      request(manual_deg_);
    }
    return d;
  }

  if (dark_) {
    const bool stow_due = in.now_ms - dark_since_ms_ >= cfg_.dark_to_stow_ms;
    const float stow = clampf(cfg_.stow_deg, cfg_.track_limit_deg);
    const bool at_stow = fabsf(in.panel_deg - stow) <= 0.5f;
    if (stow_due && !at_stow && !in.servo_busy) request(stow);
    d.state = stow_due && at_stow && !in.servo_busy ? State::STW : State::IDL;
    return d;
  }

  if (!have_filter_) {
    d.state = State::HLD;   // light, but no usable direction: keep the panel where it is
    return d;
  }

  d.state = State::TRK;
  const float target = clampf(filtered_, cfg_.track_limit_deg);
  if (fabsf(target - in.panel_deg) > cfg_.deadband_deg && !in.servo_busy && gap_ok) request(target);
  return d;
}

bool StallGuard::update(uint32_t now_ms, bool servo_busy, bool p_use_valid, float p_use_mw) {
  if (!servo_busy || !p_use_valid || !(p_use_mw > cfg_.limit_mw)) {
    above_ = false;
    return false;
  }
  if (!above_) {
    above_ = true;
    since_ms_ = now_ms;
    return false;
  }
  return now_ms - since_ms_ >= cfg_.max_ms;
}

}  // namespace ast
