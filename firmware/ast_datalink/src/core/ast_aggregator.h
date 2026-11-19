// 1 Hz samples -> one Record per UTC minute (contract 7.1, 3.1).
//   powers, V_supply, T_panel : mean of the valid 1 Hz readings in [t, t+60 s)
//   angles, state, run, panel : value of the LAST 1 Hz sample of the minute
//   state MOV  : if the motor ran at any time in the minute (FLT is kept)
//   control fault code : last non-zero code seen in the minute
//   a negative mean power is clamped to 0 (and counted)
//   a value with no valid reading in the minute is "unknown" (field omitted)
#pragma once
#include <stdint.h>

#include "ast_record.h"

namespace ast {

struct Sample1Hz {
  bool p_trk_valid = false, p_fix_valid = false, p_use_valid = false;
  bool vsup_valid = false, ang_panel_valid = false, ang_sun_valid = false, temp_valid = false;
  float p_trk_mw = 0, p_fix_mw = 0, p_use_mw = 0;
  float vsup_v = 0;
  float ang_panel_deg = 0, ang_sun_deg = 0;
  float temp_c = 0;
  State state = State::IDL;
  bool motor_running = false;   // true if the motor ran since the previous 1 Hz sample
  uint16_t run = 0;
  char panel = 'A';
  uint8_t ctrl_fault = 0;       // control-system fault code (0, 5, 7, 8 ...)
  bool simulated = false;
};

class Aggregator {
 public:
  // Feeds the sample taken at UTC time `unix_s`. When this sample belongs to a
  // later minute than the one being accumulated, the finished minute is
  // written to *out and true is returned (the new sample starts the next
  // minute). `out->seq` and `out->fault` (data-link part) are filled later.
  bool add(int64_t unix_s, const Sample1Hz& s, Record* out);

  // Emits the accumulated minute if `now_unix_s` is past its end (used on a
  // clean shutdown and when sampling stops). Returns true if *out was written.
  bool flush(int64_t now_unix_s, Record* out);

  void reset();                               // drop the partial minute
  bool active() const { return count_ > 0; }
  int64_t current_minute() const { return minute_; }
  int samples_in_minute() const { return count_; }
  uint32_t negative_clamps() const { return negative_clamps_; }  // quality counter

 private:
  struct Mean {
    double sum = 0;
    int n = 0;
    void add(float v) { sum += static_cast<double>(v); ++n; }
  };
  void accumulate(const Sample1Hz& s);
  void finish(Record* out);
  uint32_t power_to_dmw(const Mean& m);

  int64_t minute_ = 0;
  int count_ = 0;
  Mean p_trk_, p_fix_, p_use_, vsup_, temp_;
  Sample1Hz last_;
  bool motor_ran_ = false;
  bool any_simulated_ = false;
  uint8_t ctrl_fault_ = 0;
  uint32_t negative_clamps_ = 0;
};

// Rounds to the nearest integer, halves away from zero, saturating.
int32_t round_scaled(double value, double scale, int32_t lo, int32_t hi);

}  // namespace ast
