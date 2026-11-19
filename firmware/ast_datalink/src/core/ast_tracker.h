// Tracking decisions (Control; owner CO, starter code by AM). Portable logic
// only, unit-tested on the PC: given the sun estimate and where the panel is,
// decide whether to move, where to, and which state to report to the data
// link. The glue that reads the sensors and drives the servo is
// tracker/control.cpp.
//
// Rules (requirements F2, F3 in the design brief):
//   - re-point only when |sun - panel| > deadband (5 deg: costs at most 0.4 %
//     of the beam power, and fewer moves cost less energy)
//   - never command beyond +/-track_limit (the sun outside it parks the panel
//     at the limit, which is not a fault)
//   - dark for dark_to_stow: go to the stow angle (east, ready for sunrise)
//   - sun not seen but not dark (cloud, odd light): hold where it is (HLD)
//   - any fault: stop moving (FLT) until it clears
#pragma once
#include <stdint.h>

#include "ast_record.h"

namespace ast {

struct TrackerConfig {
  float deadband_deg = 5.0f;          // F2a
  float track_limit_deg = 45.0f;      // F2b
  float stow_deg = -45.0f;            // F3b idle position (east)
  uint32_t dark_to_stow_ms = 600000;  // 10 min outdoors; set much shorter for lamp tests
  uint32_t min_move_gap_ms = 10000;   // never re-point more often than this
  float filter_alpha = 0.3f;          // weight of a new sun estimate (1 = no smoothing)
  uint32_t lost_reset_ms = 2000;      // the smoothing restarts only after this long without an estimate
  float at_stow_deg = 2.0f;           // within this of the stow angle counts as stowed (pot noise)
};

struct TrackerInput {
  uint32_t now_ms = 0;
  bool sun_valid = false;   // the estimate can be used
  bool sun_dark = false;    // too little light
  float sun_deg = 0.0f;     // estimated sun angle (absolute, same axis as the panel)
  float panel_deg = 0.0f;   // where the panel is (commanded, or measured if a pot is fitted)
  bool servo_busy = false;  // a move is still in progress
  uint8_t fault = 0;        // a fault found elsewhere (5, 7, 8); 0 = none
};

struct TrackerDecision {
  bool move = false;          // start a move to target_deg now
  float target_deg = 0.0f;
  State state = State::IDL;   // to report (MOV is added by the data link while the servo moves)
  bool sun_valid = false;     // filtered sun angle available
  float sun_deg = 0.0f;       // filtered sun angle (for telemetry and the pointing error)
};

class Tracker {
 public:
  explicit Tracker(const TrackerConfig& cfg) : cfg_(cfg) {}
  void set_config(const TrackerConfig& cfg) { cfg_ = cfg; }
  const TrackerConfig& config() const { return cfg_; }

  TrackerDecision step(const TrackerInput& in);

  // Manual mode (bench tests, command-to-angle calibration): the tracker
  // stops tracking and moves once to the given angle; reports HLD.
  void set_manual(float panel_deg);
  // Manual mode without a move: stay where it is (after `servo off`).
  void hold() {
    manual_ = true;
    manual_pending_ = false;
  }
  void set_auto();
  bool manual() const { return manual_; }
  float manual_deg() const { return manual_deg_; }

 private:
  TrackerConfig cfg_;
  bool manual_ = false, manual_pending_ = false;
  float manual_deg_ = 0.0f;
  bool have_filter_ = false;
  float filtered_ = 0.0f;
  uint32_t last_valid_ms_ = 0;
  bool dark_ = false;
  uint32_t dark_since_ms_ = 0;
  bool moved_once_ = false;
  uint32_t last_move_ms_ = 0;
};

// Servo stall/overload guard. A jammed panel or a slipping clutch makes the
// servo draw its stall current (about 1.4 A at 5 V, 7 W) for the whole move,
// where a free move takes a few tenths of a joule. P_use comes from the
// consumption sensor only once a second (an INA226 averages over that second;
// an INA219 is a 68 ms snapshot), and a 5 deg move lasts under a second, so the
// guard adds up the energy above the level measured just before the move,
// over the move and a tail after it (so the 1 Hz reading that covers the move
// is counted), and compares it with an allowance that grows with the move:
//   allowance = base_j + j_per_deg * |move|
// Example with the defaults: a jammed 5 deg move costs about 5 J against
// 1.8 J allowed (stall); a free 90 deg stow move about 2-5 J against 6.9 J.
struct StallConfig {
  float base_j = 1.5f;
  float j_per_deg = 0.06f;
  uint32_t tail_ms = 1500;
};

class StallGuard {
 public:
  explicit StallGuard(const StallConfig& cfg) : cfg_(cfg) {}
  void set_config(const StallConfig& cfg) { cfg_ = cfg; }
  // Call every control step. move_deg: size of the move in progress (the
  // angle it set out to cover). Returns true once the excess energy of the
  // current move window passes its allowance (once per window).
  bool update(uint32_t now_ms, bool servo_busy, float move_deg, bool p_use_valid, float p_use_mw);
  void reset() { active_ = false; }
  float excess_j() const { return excess_j_; }      // of the current or last window
  float allowance_j() const { return allow_j_; }

 private:
  StallConfig cfg_;
  bool active_ = false;
  bool baseline_set_ = false;
  float baseline_mw_ = 0, excess_j_ = 0, allow_j_ = 0;
  uint32_t last_ms_ = 0, tail_until_ms_ = 0;
};

}  // namespace ast
