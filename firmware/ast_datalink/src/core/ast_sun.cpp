#include "ast_sun.h"

#include <math.h>

namespace ast {

namespace {

constexpr double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * kPi / 180.0; }
inline double deg_of(double r) { return r * 180.0 / kPi; }

// Solves the 3x3 system m * x = v by Gaussian elimination with partial
// pivoting. Returns false when the matrix is (nearly) singular.
bool solve3(double m[3][3], double v[3], double x[3]) {
  for (int col = 0; col < 3; ++col) {
    int piv = col;
    for (int r = col + 1; r < 3; ++r) {
      if (fabs(m[r][col]) > fabs(m[piv][col])) piv = r;
    }
    if (fabs(m[piv][col]) < 1e-9) return false;
    if (piv != col) {
      for (int k = 0; k < 3; ++k) {
        const double t = m[col][k];
        m[col][k] = m[piv][k];
        m[piv][k] = t;
      }
      const double t = v[col];
      v[col] = v[piv];
      v[piv] = t;
    }
    for (int r = col + 1; r < 3; ++r) {
      const double f = m[r][col] / m[col][col];
      for (int k = col; k < 3; ++k) m[r][k] -= f * m[col][k];
      v[r] -= f * v[col];
    }
  }
  for (int r = 2; r >= 0; --r) {
    double acc = v[r];
    for (int k = r + 1; k < 3; ++k) acc -= m[r][k] * x[k];
    x[r] = acc / m[r][r];
  }
  return true;
}

struct Fit {
  bool ok = false;
  double theta_deg = 0, amp = 0, ambient = 0, rms = 0;
};

// Least-squares fit of s = a cos(phi) + b sin(phi) + c over the sensors in
// `use` (c fixed at 0 when with_ambient is false).
Fit fit_cosine(const double* s, const double* phi_deg, const bool* use, int n, bool with_ambient) {
  Fit f;
  double m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  double v[3] = {0, 0, 0};
  int count = 0;
  for (int i = 0; i < n; ++i) {
    if (!use[i]) continue;
    const double row[3] = {cos(rad(phi_deg[i])), sin(rad(phi_deg[i])), with_ambient ? 1.0 : 0.0};
    for (int r = 0; r < 3; ++r) {
      for (int k = 0; k < 3; ++k) m[r][k] += row[r] * row[k];
      v[r] += row[r] * s[i];
    }
    ++count;
  }
  if (!with_ambient) {
    // Two unknowns: make the third equation trivial (c = 0).
    m[2][0] = m[2][1] = m[0][2] = m[1][2] = 0.0;
    m[2][2] = 1.0;
    v[2] = 0.0;
  }
  if (count < (with_ambient ? 3 : 2)) return f;
  double x[3] = {0, 0, 0};
  if (!solve3(m, v, x)) return f;
  f.amp = sqrt(x[0] * x[0] + x[1] * x[1]);
  if (!(f.amp > 0.0) || !isfinite(f.amp)) return f;
  f.theta_deg = deg_of(atan2(x[1], x[0]));
  f.ambient = x[2];
  double sq = 0;
  for (int i = 0; i < n; ++i) {
    if (!use[i]) continue;
    const double model = x[0] * cos(rad(phi_deg[i])) + x[1] * sin(rad(phi_deg[i])) + x[2];
    sq += (s[i] - model) * (s[i] - model);
  }
  f.rms = sqrt(sq / count);
  f.ok = isfinite(f.theta_deg);
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
  double s[kSunMaxSensors];
  double phi[kSunMaxSensors];
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
    s[i] = c > 0.0 ? c : 0.0;
    phi[i] = static_cast<double>(cfg.sensor_deg[i]);
    total += s[i];
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

  if (cfg.method == SunMethod::kCentroid) {
    // Remove the common (ambient) level first so that diffuse light does not
    // pull the estimate to the middle of the arc.
    double floor_mv = s[0];
    for (int i = 1; i < n; ++i) floor_mv = s[i] < floor_mv ? s[i] : floor_mv;
    double w = 0, acc = 0;
    for (int i = 0; i < n; ++i) {
      w += s[i] - floor_mv;
      acc += (s[i] - floor_mv) * phi[i];
    }
    if (!(w > 0.0)) {
      // Every sensor sees the same light: the source is straight above the
      // middle of the arc (or the light is completely diffuse).
      double mid = 0;
      for (int i = 0; i < n; ++i) mid += phi[i];
      e.sun_deg = static_cast<float>(mid / n);
    } else {
      e.sun_deg = static_cast<float>(acc / w);
    }
    e.used = static_cast<uint8_t>(n);
    e.valid = fabs(static_cast<double>(e.sun_deg)) <= static_cast<double>(cfg.max_abs_deg);
    e.why = e.valid ? "" : "outside the field of view";
    return e;
  }

  // Cosine fit. Start from the brightest sensor's direction and use the
  // sensors that face it (clipped ones only if too few are left), fit, then
  // repeat with the sensors that face the fitted direction until the set
  // stops changing. With only two sensors left, the ambient level is taken as
  // the smallest signal (a sensor facing away sees only ambient light) and
  // the two-parameter fit is used.
  int brightest = 0;
  for (int i = 1; i < n; ++i) brightest = s[i] > s[brightest] ? i : brightest;
  double ambient_floor = s[0];
  for (int i = 1; i < n; ++i) ambient_floor = s[i] < ambient_floor ? s[i] : ambient_floor;
  double theta = phi[brightest];
  bool use[kSunMaxSensors];
  int count = 0;
  Fit f;
  bool have_fit = false;
  for (int iter = 0; iter < 5; ++iter) {
    bool next[kSunMaxSensors];
    int kept = 0, kept_unclipped = 0;
    for (int i = 0; i < n; ++i) {
      next[i] = fabs(static_cast<double>(angle_diff_deg(static_cast<float>(theta), static_cast<float>(phi[i])))) <
                static_cast<double>(cfg.face_limit_deg);
      if (next[i]) {
        ++kept;
        if (!clipped[i]) ++kept_unclipped;
      }
    }
    if (kept_unclipped >= 2) {   // leave clipped sensors out when possible
      for (int i = 0; i < n; ++i) next[i] = next[i] && !clipped[i];
      kept = kept_unclipped;
    }
    if (kept < 2) break;
    bool changed = !have_fit;
    for (int i = 0; i < n; ++i) {
      if (have_fit && next[i] != use[i]) changed = true;
      use[i] = next[i];
    }
    count = kept;
    if (!changed) break;
    Fit g;
    if (count >= 3) {
      g = fit_cosine(s, phi, use, n, true);
    } else {
      double s2[kSunMaxSensors];
      for (int i = 0; i < n; ++i) s2[i] = s[i] - ambient_floor;
      g = fit_cosine(s2, phi, use, n, false);
      g.ambient = ambient_floor;
    }
    if (!g.ok) break;
    f = g;
    have_fit = true;
    theta = f.theta_deg;
  }
  if (!have_fit) {
    // Degenerate geometry (e.g. only one sensor lit): fall back to the
    // brightest sensor's direction.
    e.sun_deg = static_cast<float>(phi[brightest]);
    e.used = 1;
    e.valid = fabs(static_cast<double>(e.sun_deg)) <= static_cast<double>(cfg.max_abs_deg);
    e.why = e.valid ? "" : "outside the field of view";
    return e;
  }
  e.sun_deg = static_cast<float>(f.theta_deg);
  e.amplitude_mv = static_cast<float>(f.amp);
  e.ambient_mv = static_cast<float>(f.ambient);
  e.residual_mv = static_cast<float>(f.rms);
  e.used = static_cast<uint8_t>(count);
  if (fabs(f.theta_deg) > static_cast<double>(cfg.max_abs_deg)) {
    e.why = "outside the field of view";
    return e;
  }
  e.valid = true;
  e.why = "";
  return e;
}

}  // namespace ast
