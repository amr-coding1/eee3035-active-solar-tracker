// Firmware simulator (contract 7.5): sun model for Guildford, clouds, a
// single-axis tracker with a dead-band controller, and the tracker's own
// consumption. It produces the same Sample1Hz the real sensors would, always
// with simulated = true so that every logged sample carries "s=1".
//
// Geometry: the tracking axis runs north-south and is tilted up towards the
// north by `axis_tilt_deg`, so at angle 0 the panel faces south at that tilt.
// The FIXED panel is identical and stays at angle 0. Angles follow the
// contract: east negative, west positive.
#pragma once
#include <stdint.h>

#include "ast_aggregator.h"

namespace ast {

enum class Scenario : uint8_t { kClear = 0, kCloudy, kOvercast, kNight, kLamp };
const char* scenario_name(Scenario s);
bool scenario_from_name(const char* name, Scenario* s);

struct SimConfig {
  Scenario scenario = Scenario::kClear;
  uint32_t seed = 1;
  double latitude_deg = 51.24;      // Guildford
  double longitude_deg = -0.57;     // east positive
  double axis_tilt_deg = 35.0;
  double limit_deg = 60.0;          // mechanical travel either side of level
  double deadband_deg = 2.5;        // move when the pointing error exceeds this
  double motor_speed_dps = 2.0;     // degrees per second while moving
  double panel_rated_mw = 2000.0;   // at 1000 W/m2, 25 degC
  double base_use_mw = 120.0;       // ESP32-S3 + sensors + driver quiescent, average
  double motor_use_mw = 1500.0;     // extra while the motor runs
};

struct SunPosition {
  double elevation_deg;   // above the horizon
  double azimuth_deg;     // from north, clockwise (east = 90)
};
// Low-precision solar position (Astronomical Almanac formulae, about 0.01 deg).
SunPosition sun_position(int64_t unix_s, double latitude_deg, double longitude_deg);
// Sun angle about the tilted N-S axis (contract field5 convention).
double sun_axis_angle_deg(const SunPosition& sun, double axis_tilt_deg);

class Simulator {
 public:
  explicit Simulator(const SimConfig& cfg = SimConfig());
  void configure(const SimConfig& cfg);
  const SimConfig& config() const { return cfg_; }

  // One step per second, in time order. The tracker angle is carried from
  // step to step; everything else is a pure function of (seed, time).
  Sample1Hz step(int64_t unix_s);

  double panel_angle_deg() const { return panel_deg_; }
  bool moving() const { return moving_; }

  // Deterministic noise in [0, 1) from (seed, key, channel) - exposed for tests.
  static double hash01(uint32_t seed, int64_t key, uint32_t channel);
  // Fraction of the direct beam that gets through the clouds at time t, 0..1.
  double beam_fraction(int64_t unix_s) const;

 private:
  SimConfig cfg_;
  bool initialised_ = false;
  double panel_deg_ = 0.0;
  bool moving_ = false;
  double target_deg_ = 0.0;
};

}  // namespace ast
