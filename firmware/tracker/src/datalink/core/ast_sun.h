// Sun direction from the photodiode arc (Solar Sensing; owner CO, starter code
// by AM). Portable maths only: the ADC reads are in tracker/sensing.cpp.
//
// Each photodiode faces a known direction on the tracking axis (east
// negative). Its signal is modelled as
//     s_i = A * cos(theta - phi_i) + B        (B = diffuse/ambient light)
// for the sensors that face the light (|theta - phi_i| < 85 deg). Writing
// a = A cos(theta), b = A sin(theta) makes this linear in (a, b, B), so three
// or more sensors give theta = atan2(b, a) by least squares. Sensors that
// face away from the light are dropped and the fit repeated.
//
// Accuracy (unit tests, 4 sensors 30 deg apart, cosine response): exact with
// matched sensors; mean 0.6 deg, worst 4 deg with 1 % gain mismatch; worst
// about 14 deg near the ends of the arc with 3 % mismatch. So match the
// channels first (`sun cal flat` under a diffuser, see tracker/README.md), and
// expect real photodiodes to differ from a cosine: check against the
// inclinometer. Mounting the arc ON the panel (it then only has to find
// zero, where it is most accurate) avoids most of this.
#pragma once
#include <stdint.h>

namespace ast {

constexpr int kSunMaxSensors = 8;

enum class SunMethod : uint8_t {
  kCosineFit = 0,   // least squares as above (default)
  kCentroid = 1     // signal-weighted mean of the sensor directions (simple, biased to the middle)
};

struct SunArcConfig {
  int n = 4;
  // Direction each photodiode faces, in the order of the ADC readings.
  float sensor_deg[kSunMaxSensors] = {-45.0f, -15.0f, 15.0f, 45.0f, 0, 0, 0, 0};
  // Per-channel correction: corrected = gain * (raw - dark_offset), never below 0.
  float gain[kSunMaxSensors] = {1, 1, 1, 1, 1, 1, 1, 1};
  float dark_offset_mv[kSunMaxSensors] = {0, 0, 0, 0, 0, 0, 0, 0};
  float dark_total_mv = 40.0f;   // sum of corrected signals below this = dark (night, lamp off)
  float saturated_mv = 3000.0f;  // raw reading at or above this is clipped by the ADC
  float max_abs_deg = 70.0f;     // an estimate further out than this is not trusted
  float face_limit_deg = 85.0f;  // sensors further than this from the estimate are dropped
  SunMethod method = SunMethod::kCosineFit;
};

struct SunEstimate {
  bool valid = false;       // sun_deg can be used for tracking
  bool dark = false;        // too little light
  bool saturated = false;   // at least one channel was clipped
  float sun_deg = 0.0f;     // direction of the light on the tracking axis
  float total_mv = 0.0f;    // sum of corrected signals
  float amplitude_mv = 0.0f;  // fitted A (cosine fit)
  float ambient_mv = 0.0f;    // fitted B (cosine fit)
  float residual_mv = 0.0f;   // rms misfit of the sensors used
  uint8_t used = 0;           // sensors in the final fit
  const char* why = "";       // why it is not valid ("" when valid)
};

// raw_mv: one reading per sensor (cfg.n values), in millivolts.
SunEstimate estimate_sun(const float* raw_mv, const SunArcConfig& cfg);

// Smallest signed difference a - b in degrees, in (-180, 180].
float angle_diff_deg(float a, float b);

}  // namespace ast
