#include "ast_sim.h"

#include <math.h>
#include <string.h>

namespace ast {

namespace {
const double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double wrap360(double a) {
  a = fmod(a, 360.0);
  return a < 0 ? a + 360.0 : a;
}
const char* const kScenarioNames[] = {"clear", "cloudy", "overcast", "night", "lamp"};

// Lamp scenario: an indoor halogen lamp is carried along an arc from east to
// west in 2 h; the cycle repeats every 3 h (lamp off in the third hour),
// aligned to UTC so that a restart continues the same sweep.
const int64_t kLampCycleS = 3 * 3600;
const int64_t kLampSweepS = 2 * 3600;
const double kSensorMarginDeg = 10.0;
const double kLampIrradiance = 180.0;   // W/m2-equivalent on a panel facing the lamp
}  // namespace

const char* scenario_name(Scenario s) {
  const uint8_t i = static_cast<uint8_t>(s);
  return i < 5 ? kScenarioNames[i] : "clear";
}

bool scenario_from_name(const char* name, Scenario* s) {
  if (name == nullptr) return false;
  for (uint8_t i = 0; i < 5; ++i) {
    if (strcmp(name, kScenarioNames[i]) == 0) {
      *s = static_cast<Scenario>(i);
      return true;
    }
  }
  return false;
}

SunPosition sun_position(int64_t unix_s, double latitude_deg, double longitude_deg) {
  // Days since J2000.0 (2000-01-01 12:00 UTC = unix 946728000).
  const double n = (static_cast<double>(unix_s) - 946728000.0) / 86400.0;
  const double L = wrap360(280.460 + 0.9856474 * n);          // mean longitude
  const double g = rad(wrap360(357.528 + 0.9856003 * n));     // mean anomaly
  const double lambda = rad(L + 1.915 * sin(g) + 0.020 * sin(2 * g));  // ecliptic longitude
  const double eps = rad(23.439 - 0.0000004 * n);             // obliquity
  const double ra = atan2(cos(eps) * sin(lambda), cos(lambda));
  const double dec = asin(sin(eps) * sin(lambda));
  const double gmst_h = fmod(18.697374558 + 24.06570982441908 * n, 24.0);
  const double lst = rad(wrap360(gmst_h * 15.0 + longitude_deg));
  const double H = lst - ra;                                   // hour angle
  const double lat = rad(latitude_deg);
  const double sin_el = sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(H);
  SunPosition p;
  p.elevation_deg = deg(asin(clampd(sin_el, -1.0, 1.0)));
  p.azimuth_deg = wrap360(deg(atan2(-sin(H), tan(dec) * cos(lat) - sin(lat) * cos(H))));
  return p;
}

namespace {
// Sun unit vector components: e (east), n (north), u (up).
struct Vec { double e, n, u; };
Vec sun_vector(const SunPosition& s) {
  const double el = rad(s.elevation_deg), az = rad(s.azimuth_deg);
  return Vec{cos(el) * sin(az), cos(el) * cos(az), sin(el)};
}
}  // namespace

double sun_axis_angle_deg(const SunPosition& sun, double axis_tilt_deg) {
  const Vec s = sun_vector(sun);
  const double b = rad(axis_tilt_deg);
  // Panel normal at angle 0 (facing south, tilted b): n0 = (0, -sin b, cos b).
  // Rotating about the axis towards the west adds the west unit vector (-1,0,0).
  const double along_n0 = -s.n * sin(b) + s.u * cos(b);
  const double along_west = -s.e;
  return deg(atan2(along_west, along_n0));
}

double Simulator::hash01(uint32_t seed, int64_t key, uint32_t channel) {
  // splitmix64 finaliser: good avalanche, identical on every platform.
  uint64_t z = static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ull + (static_cast<uint64_t>(seed) << 32) +
               channel * 0xD1B54A32D192ED03ull;
  z ^= z >> 30; z *= 0xBF58476D1CE4E5B9ull;
  z ^= z >> 27; z *= 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

Simulator::Simulator(const SimConfig& cfg) : cfg_(cfg) {}

void Simulator::configure(const SimConfig& cfg) {
  cfg_ = cfg;
  initialised_ = false;
  moving_ = false;
}

double Simulator::beam_fraction(int64_t t) const {
  switch (cfg_.scenario) {
    case Scenario::kClear: return 1.0;
    case Scenario::kOvercast: return 0.03;
    case Scenario::kNight: return 0.0;
    case Scenario::kLamp: return 1.0;
    case Scenario::kCloudy: break;
  }
  // Value noise: one random number every 4 minutes, smoothly interpolated,
  // then sharpened so clouds have edges (sun in / sun out within a minute).
  const int64_t cell = 240;
  const int64_t k = t >= 0 ? t / cell : -((-t + cell - 1) / cell);
  const double f = static_cast<double>(t - k * cell) / static_cast<double>(cell);
  const double a = hash01(cfg_.seed, k, 1), b = hash01(cfg_.seed, k + 1, 1);
  const double w = f * f * (3.0 - 2.0 * f);
  const double v = a + (b - a) * w;                 // 0..1
  const double x = clampd((v - 0.35) / 0.30, 0.0, 1.0);   // below 0.35: thick cloud
  return 0.05 + 0.95 * x * x * (3.0 - 2.0 * x);
}

Sample1Hz Simulator::step(int64_t t) {
  const double noise_a = hash01(cfg_.seed, t, 2) - 0.5;   // +/-0.5, new every second
  const double noise_b = hash01(cfg_.seed, t, 3) - 0.5;
  const double noise_c = hash01(cfg_.seed, t, 4) - 0.5;

  // ---- light source ---------------------------------------------------------
  double beam = 0.0;       // W/m2 on a surface facing the source
  double diffuse = 0.0;    // W/m2 on any panel
  double source_angle = 0.0;   // angle of the source about the tracking axis
  bool source_up = false;      // there is light at all
  bool lamp = cfg_.scenario == Scenario::kLamp;
  double ambient_c = 12.0;

  if (lamp) {
    int64_t ph = t % kLampCycleS;
    if (ph < 0) ph += kLampCycleS;
    ambient_c = 21.0;
    if (ph < kLampSweepS) {
      source_up = true;
      source_angle = -cfg_.limit_deg + 2.0 * cfg_.limit_deg * static_cast<double>(ph) / static_cast<double>(kLampSweepS);
      beam = kLampIrradiance;
      diffuse = 4.0;      // room lighting
    } else {
      diffuse = 2.0;
    }
  } else if (cfg_.scenario != Scenario::kNight) {
    const SunPosition sun = sun_position(t, cfg_.latitude_deg, cfg_.longitude_deg);
    const double day_frac = static_cast<double>(((t % 86400) + 86400) % 86400) / 86400.0;
    ambient_c = 10.0 + 4.0 * sin(2.0 * kPi * (day_frac - 0.375));   // warmest about 15:00 UTC
    if (sun.elevation_deg > 0.0) {
      source_up = true;
      source_angle = sun_axis_angle_deg(sun, cfg_.axis_tilt_deg);
      // Clear-sky direct normal irradiance (Meinel) with Kasten-Young air mass.
      const double el = sun.elevation_deg;
      const double am = 1.0 / (sin(rad(el)) + 0.50572 * pow(el + 6.07995, -1.6364));
      const double dni = 1353.0 * pow(0.7, pow(am, 0.678));
      const double frac = beam_fraction(t);
      beam = dni * frac;
      // Diffuse sky light: small under a clear sky, larger under cloud.
      diffuse = (0.10 * dni + 0.25 * dni * (1.0 - frac)) * sin(rad(el));
    }
  }

  // ---- what the sun-sensor arc reports -----------------------------------------
  // The arc of light sensors only covers the mechanical travel plus a margin;
  // outside it (early morning / late evening in summer) there is no sun angle.
  const bool sun_seen = source_up && beam > 80.0 && fabs(source_angle) < cfg_.limit_deg + kSensorMarginDeg &&
                        (lamp || sun_position(t, cfg_.latitude_deg, cfg_.longitude_deg).elevation_deg > 2.0);
  const double sun_meas = source_angle + 0.3 * noise_a;   // sensor noise +/-0.15 deg

  // ---- tracker controller --------------------------------------------------------
  if (!initialised_) {
    initialised_ = true;
    moving_ = false;
    // Start where a tracker that had been running would be.
    panel_deg_ = sun_seen ? clampd(source_angle, -cfg_.limit_deg, cfg_.limit_deg) : 0.0;
  }
  State state;
  const bool dark = !source_up || (beam + diffuse) < 5.0;
  if (!moving_) {
    if (dark) {
      // Night: park level (ready for either direction in the morning).
      if (fabs(panel_deg_) > 0.05) { moving_ = true; target_deg_ = 0.0; }
    } else if (sun_seen) {
      const double want = clampd(sun_meas, -cfg_.limit_deg, cfg_.limit_deg);
      if (fabs(want - panel_deg_) > cfg_.deadband_deg) {
        // Move slightly past the sun (it travels west) so that the error runs
        // from about -0.85 x dead-band to +dead-band before the next move:
        // 4.6 deg per move, i.e. one move about every 20 minutes at 14-15 deg/h.
        // (0.85, not 1.0: the sensor noise must not trigger a move straight back.)
        moving_ = true;
        target_deg_ = clampd(want + 0.85 * cfg_.deadband_deg, -cfg_.limit_deg, cfg_.limit_deg);
      }
    }
  }
  bool motor = false;
  if (moving_) {
    motor = true;
    const double stepd = cfg_.motor_speed_dps;   // one second of travel
    const double d = target_deg_ - panel_deg_;
    if (fabs(d) <= stepd) {
      panel_deg_ = target_deg_;
      moving_ = false;
    } else {
      panel_deg_ += d > 0 ? stepd : -stepd;
    }
    state = State::MOV;
  } else if (dark) {
    state = State::IDL;
  } else if (sun_seen) {
    state = State::TRK;
  } else {
    state = State::HLD;   // light but no clear sun direction: hold position
  }

  // ---- panel powers ---------------------------------------------------------------
  double cos_trk, cos_fix;
  if (lamp) {
    cos_trk = cos(rad(source_angle - panel_deg_));
    cos_fix = cos(rad(source_angle));
  } else {
    // In the plane perpendicular to the axis the sun is at source_angle; the
    // component along the axis is lost for both panels alike.
    const SunPosition sun = sun_position(t, cfg_.latitude_deg, cfg_.longitude_deg);
    const Vec s = sun_vector(sun);
    const double b = rad(cfg_.axis_tilt_deg);
    const double n0 = -s.n * sin(b) + s.u * cos(b);
    const double west = -s.e;
    cos_trk = n0 * cos(rad(panel_deg_)) + west * sin(rad(panel_deg_));
    cos_fix = n0;
  }
  const double g_trk = beam * (cos_trk > 0 ? cos_trk : 0.0) + diffuse;
  const double g_fix = beam * (cos_fix > 0 ? cos_fix : 0.0) + diffuse;
  const double temp_c = ambient_c + 0.03 * g_trk;
  const double derate = 1.0 - 0.004 * (temp_c - 25.0);

  Sample1Hz s;
  s.simulated = true;
  s.p_trk_valid = s.p_fix_valid = s.p_use_valid = s.vsup_valid = true;
  s.p_trk_mw = static_cast<float>(cfg_.panel_rated_mw * g_trk / 1000.0 * derate * (1.0 + 0.01 * noise_b));
  s.p_fix_mw = static_cast<float>(cfg_.panel_rated_mw * g_fix / 1000.0 * derate * (1.0 + 0.01 * noise_c));
  // Own use: quiescent draw with a slow ripple (Wi-Fi beacons, regulator) plus
  // the motor while it runs. Stays within 100-150 mW when the motor is off.
  const double ripple = 12.0 * sin(2.0 * kPi * static_cast<double>(((t % 900) + 900) % 900) / 900.0);
  s.p_use_mw = static_cast<float>(cfg_.base_use_mw + ripple + 16.0 * noise_a + (motor ? cfg_.motor_use_mw : 0.0));
  // Supply rail (field7 V_supply): the 5 V feed to the tracker, measured by the
  // consumption sensor. Sags slightly with load (cable and power-bank output
  // resistance, about 0.15 ohm here) and a lot while the actuator moves.
  s.vsup_v = static_cast<float>(5.05 - 0.00015 * static_cast<double>(s.p_use_mw) + 0.004 * noise_b);
  s.ang_panel_valid = true;
  s.ang_panel_deg = static_cast<float>(panel_deg_);
  s.ang_sun_valid = sun_seen;
  s.ang_sun_deg = static_cast<float>(sun_meas);
  s.temp_valid = true;
  s.temp_c = static_cast<float>(temp_c);
  s.state = state;
  s.motor_running = motor;
  s.ctrl_fault = 0;
  return s;
}

}  // namespace ast
