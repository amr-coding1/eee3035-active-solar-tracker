#include "ast_sun.h"

#include <math.h>

namespace ast {

namespace {

// Response of one photodiode to light arriving `off_deg` away from where it
// faces: cos^k, and nothing from behind. Single precision on purpose: the
// ESP32-S3 has a hardware FPU for float only, and the fit below calls this
// about 900 times per estimate.
float response(float off_deg, float k) {
  const float c = cosf(off_deg * 0.017453292f);
  if (c <= 0.0f) return 0.0f;
  return k == 1.0f ? c : powf(c, k);
}

struct Fit {
  bool ok = false;
  float amp = 0, ambient = 0, rms = 0;
};

// For one candidate direction: least squares of s_i = A r_i + B over the
// sensors in `use` (B fixed at 0 when fewer than three are used).
Fit fit_at(float theta, const float* s, const float* phi, const bool* use, int n, float k) {
  Fit f;
  float sc = 0, sc2 = 0, ss = 0, scs = 0;
  int m = 0;
  float r[kSunMaxSensors];
  for (int i = 0; i < n; ++i) {
    r[i] = 0.0f;
    if (!use[i]) continue;
    r[i] = response(theta - phi[i], k);
    sc += r[i];
    sc2 += r[i] * r[i];
    ss += s[i];
    scs += r[i] * s[i];
    ++m;
  }
  if (m < 2) return f;
  const float mf = static_cast<float>(m);
  if (m >= 3) {
    const float det = mf * sc2 - sc * sc;
    if (fabsf(det) < 1e-6f) return f;
    f.amp = (mf * scs - sc * ss) / det;
    f.ambient = (ss - f.amp * sc) / mf;
  } else {
    if (sc2 < 1e-6f) return f;
    f.amp = scs / sc2;
    f.ambient = 0.0f;
  }
  if (!(f.amp > 0.0f)) return f;   // light from the opposite side: not this direction
  float sq = 0;
  for (int i = 0; i < n; ++i) {
    if (!use[i]) continue;
    const float d = s[i] - (f.amp * r[i] + f.ambient);
    sq += d * d;
  }
  f.rms = sqrtf(sq / mf);
  f.ok = isfinite(f.rms) && isfinite(f.amp);
  return f;
}

}  // namespace

float angle_diff_deg(float a, float b) {
  double d = fmod(static_cast<double>(a) - static_cast<double>(b), 360.0);
  if (d <= -180.0) d += 360.0;
  if (d > 180.0) d -= 360.0;
  return static_cast<float>(d);
}

SunEstimate estimate_sun(const float* raw_mv, const SunArcConfig& cfg) {
  SunEstimate e;
  const int n = cfg.n < 0 ? 0 : (cfg.n > kSunMaxSensors ? kSunMaxSensors : cfg.n);
  if (n < 2) {
    e.why = "fewer than 2 sensors configured";
    return e;
  }
  float s[kSunMaxSensors];
  float phi[kSunMaxSensors];
  bool clipped[kSunMaxSensors];
  int n_clipped = 0;
  double total = 0;
  for (int i = 0; i < n; ++i) {
    const double raw = static_cast<double>(raw_mv[i]);
    if (!isfinite(raw)) {
      e.why = "reading is not a number";
      return e;
    }
    clipped[i] = raw >= static_cast<double>(cfg.saturated_mv);
    if (clipped[i]) ++n_clipped;
    const double c = static_cast<double>(cfg.gain[i]) * (raw - static_cast<double>(cfg.dark_offset_mv[i]));
    s[i] = c > 0.0 ? static_cast<float>(c) : 0.0f;
    phi[i] = cfg.sensor_deg[i];
    total += static_cast<double>(s[i]);
  }
  e.total_mv = static_cast<float>(total);
  e.saturated = n_clipped > 0;
  if (total < static_cast<double>(cfg.dark_total_mv)) {
    e.dark = true;
    e.why = "dark";
    return e;
  }
  if (n_clipped > 1) {
    // Two or more clipped channels hide where the peak is. The cure is on
    // the bench: a smaller photodiode load resistor (or a dimmer lamp).
    e.why = "saturated: lower the photodiode load resistor";
    return e;
  }
  int brightest = 0;
  float floor_mv = s[0];
  for (int i = 1; i < n; ++i) {
    brightest = s[i] > s[brightest] ? i : brightest;
    floor_mv = s[i] < floor_mv ? s[i] : floor_mv;
  }

  if (cfg.method == SunMethod::kCentroid) {
    // Remove the common (ambient) level first so that diffuse light does not
    // pull the estimate to the middle of the arc.
    const float peak_mv = s[brightest];
    e.amplitude_mv = peak_mv - floor_mv;
    e.ambient_mv = floor_mv;
    e.contrast = peak_mv > 0.0f ? (peak_mv - floor_mv) / peak_mv : 0.0f;
    e.used = static_cast<uint8_t>(n);
    if (e.amplitude_mv < cfg.min_amplitude_mv || e.contrast < cfg.min_contrast) {
      e.diffuse = true;
      e.why = "diffuse light: no clear direction";
      return e;
    }
    float w = 0, acc = 0;
    for (int i = 0; i < n; ++i) {
      w += s[i] - floor_mv;
      acc += (s[i] - floor_mv) * phi[i];
    }
    e.sun_deg = acc / w;
    e.valid = fabsf(e.sun_deg) <= cfg.max_abs_deg;
    e.why = e.valid ? "" : "outside the field of view";
    return e;
  }

  // Model fit: try every direction from -90 to +90 deg in 1 deg steps, then
  // refine around the best in 0.05 deg steps. For each direction the model is
  // linear in A and B, so each try is a small least-squares solve. Sensors
  // facing away read only ambient light (response 0), which the model covers,
  // so no sensor has to be dropped. A clipped channel is left out.
  bool use[kSunMaxSensors];
  int m = 0;
  for (int i = 0; i < n; ++i) {
    use[i] = !clipped[i];
    if (use[i]) ++m;
  }
  const float k = cfg.response_power > 0.0f ? cfg.response_power : 1.0f;
  Fit best;
  float best_theta = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const float lo = pass == 0 ? -90.0f : best_theta - 1.0f;
    const float step = pass == 0 ? 1.0f : 0.05f;
    const int steps = pass == 0 ? 181 : 41;
    for (int j = 0; j < steps; ++j) {
      const float theta = lo + step * static_cast<float>(j);
      const Fit f = fit_at(theta, s, phi, use, n, k);
      if (f.ok && (!best.ok || f.rms < best.rms)) {
        best = f;
        best_theta = theta;
      }
    }
    if (!best.ok) break;
  }
  e.used = static_cast<uint8_t>(m);
  if (!best.ok) {
    e.diffuse = true;
    e.why = "diffuse light: no clear direction";
    return e;
  }
  e.sun_deg = best_theta;
  e.amplitude_mv = best.amp;
  e.ambient_mv = best.ambient;
  e.residual_mv = best.rms;
  const float amb = best.ambient > 0.0f ? best.ambient : 0.0f;
  e.contrast = best.amp / (best.amp + amb);
  if (best.amp < cfg.min_amplitude_mv || e.contrast < cfg.min_contrast) {
    e.diffuse = true;
    e.why = "diffuse light: no clear direction";
    return e;
  }
  // Signs of a sensor fault rather than a light direction: a large misfit; a
  // clearly negative ambient level (the fit bending round a dead channel); or
  // a direction far from the brightest sensor.
  const float neg_limit = -(best.amp * 0.1f > 20.0f ? best.amp * 0.1f : 20.0f);
  const float from_brightest = fabsf(angle_diff_deg(best_theta, phi[brightest]));
  if (best.rms > cfg.max_misfit * best.amp || best.ambient < neg_limit || from_brightest > 60.0f) {
    e.poor_fit = true;
    e.why = "readings do not fit one light direction (sensor fault, or set the response power)";
    return e;
  }
  if (fabsf(best_theta) > cfg.max_abs_deg) {
    e.why = "outside the field of view";
    return e;
  }
  e.valid = true;
  e.why = "";
  return e;
}

}  // namespace ast
