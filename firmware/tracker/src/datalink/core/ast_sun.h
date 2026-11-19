// Sun direction from the photodiode arc (Solar Sensing; owner CO, starter code
// by AM). Portable maths only: the ADC reads are in tracker/sensing.cpp.
//
// Each photodiode faces a known direction phi_i on the tracking axis (east
// negative). Its signal is modelled as
//     s_i = A * r(theta - phi_i) + B,   r(x) = cos(x)^k for |x| < 90 deg, else 0
// (B = diffuse/ambient light, k = response_power, 1 for a bare photodiode).
// For every candidate direction theta the model is linear in A and B, so the
// estimate is the theta (1 deg grid, then 0.05 deg) with the smallest
// least-squares misfit. Sensors facing away read B only, which the model
// covers, so none has to be dropped.
//
// Accuracy (unit tests, 4 sensors 30 deg apart, cosine response): exact with
// matched sensors; mean under 1 deg, worst under 5 deg with 1 % gain mismatch
// and 5 mV noise; worst about 14 deg near the ends of the arc with 3 %
// mismatch. So match the channels first (`sun cal flat` under a diffuser, see
// tracker/README.md), and check the real angular response against the
// inclinometer: a narrower response than a cosine (a shroud, a lens) shows up
// as "readings do not fit" until response_power is raised (try 2). Mounting
// the arc ON the panel (it then only has to find zero, where it is most
// accurate) avoids most of this.
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
  float response_power = 1.0f;   // k in cos^k: 1 = bare photodiode; raise if the response is narrower
  // A direction is only reported when the light has one: a beam weaker than
  // min_amplitude_mv, or weak against the ambient level (contrast), is diffuse
  // light (overcast, room light) and gives no direction - otherwise noise
  // would send the panel back and forth. A fit that leaves a large misfit
  // means the readings do not look like one light source (a dead or stuck
  // photodiode, a shadow on one sensor): also no direction, and the control
  // code raises fault 5 if it lasts.
  float min_amplitude_mv = 30.0f;
  float min_contrast = 0.15f;      // amplitude / (amplitude + ambient)
  float max_misfit = 0.30f;        // rms misfit / amplitude
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
  float contrast = 0.0f;      // amplitude / (amplitude + ambient), 0..1
  bool diffuse = false;       // light without a clear direction (cloud, room light)
  bool poor_fit = false;      // readings do not fit one light direction (sensor fault?)
  uint8_t used = 0;           // sensors in the final fit
  const char* why = "";       // why it is not valid ("" when valid)
};

// raw_mv: one reading per sensor (cfg.n values), in millivolts.
SunEstimate estimate_sun(const float* raw_mv, const SunArcConfig& cfg);

// Smallest signed difference a - b in degrees, in (-180, 180].
float angle_diff_deg(float a, float b);

}  // namespace ast
