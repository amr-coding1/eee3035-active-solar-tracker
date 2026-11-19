#include "ast_cal.h"

#include <math.h>

namespace ast {

bool two_point_cal(const CalPoint& p1, const CalPoint& p2, LinCal* out, const char** why) {
  const char* dummy;
  if (why == nullptr) why = &dummy;
  if (!p1.set || !p2.set) { *why = "both points are needed"; return false; }
  if (!isfinite(p1.raw) || !isfinite(p2.raw) || !isfinite(p1.ref) || !isfinite(p2.ref)) {
    *why = "not a number";
    return false;
  }
  const float draw = p2.raw - p1.raw;
  const float span = fabsf(p1.raw) > fabsf(p2.raw) ? fabsf(p1.raw) : fabsf(p2.raw);
  // The two points must be at least 20 % of the larger reading apart,
  // otherwise noise dominates the slope.
  if (span <= 0.0f || fabsf(draw) < 0.2f * span) { *why = "points too close together"; return false; }
  const float gain = (p2.ref - p1.ref) / draw;
  if (!(gain > 0.5f && gain < 2.0f)) { *why = "gain outside 0.5..2 (check the DMM values)"; return false; }
  out->gain = gain;
  out->offset = p1.ref - gain * p1.raw;
  *why = "ok";
  return true;
}

uint16_t ina226_cal_register(const Ina226Scale& s) {
  if (!(s.shunt_ohm > 0.0f) || !(s.current_lsb_a > 0.0f)) return 0;
  const double cal = 0.00512 / (static_cast<double>(s.current_lsb_a) * static_cast<double>(s.shunt_ohm));
  if (cal < 1.0) return 1;
  if (cal > 65535.0) return 65535;
  // Rounded to the nearest integer; the small difference to the requested
  // LSB is taken into account by ina226_effective_current_lsb().
  return static_cast<uint16_t>(cal + 0.5);
}

float ina226_effective_current_lsb(const Ina226Scale& s) {
  const uint16_t cal = ina226_cal_register(s);
  if (cal == 0) return 0.0f;
  return static_cast<float>(0.00512 / (static_cast<double>(cal) * static_cast<double>(s.shunt_ohm)));
}

float ina226_current_amps(int16_t raw, const Ina226Scale& s) {
  return static_cast<float>(raw) * ina226_effective_current_lsb(s);
}

InaReading ina226_convert(uint16_t bus_raw, int16_t current_raw, const InaChannelCal& cal) {
  InaReading r;
  r.raw_volts = ina226_bus_volts(bus_raw);
  r.raw_milliamps = ina226_current_amps(current_raw, cal.scale) * 1000.0f;
  r.volts = cal.volts.apply(r.raw_volts);
  r.milliamps = cal.milliamps.apply(r.raw_milliamps);
  r.power_mw = r.volts * r.milliamps;
  return r;
}

InaReading ina219_convert(uint16_t bus_raw, int16_t shunt_raw, const InaChannelCal& cal) {
  InaReading r;
  r.raw_volts = ina219_bus_volts(bus_raw);
  const float shunt_v = static_cast<float>(shunt_raw) * kIna219ShuntLsbV;
  r.raw_milliamps = cal.scale.shunt_ohm > 0.0f ? shunt_v / cal.scale.shunt_ohm * 1000.0f : 0.0f;
  r.volts = cal.volts.apply(r.raw_volts);
  r.milliamps = cal.milliamps.apply(r.raw_milliamps);
  r.power_mw = r.volts * r.milliamps;
  return r;
}

const char* ina_chip_name(InaChip c) {
  switch (c) {
    case InaChip::kIna226: return "INA226";
    case InaChip::kIna219: return "INA219";
    case InaChip::kNone: break;
  }
  return "none";
}

InaChip ina_identify(bool mfg_ok, uint16_t mfg_id, bool die_ok, uint16_t die_id, bool cfg_ok, uint16_t cfg_reset) {
  if (mfg_ok && mfg_id == 0x5449) {
    // A TI part. Only the INA226 die (0x226x, low nibble = revision) has the
    // register map used here; the INA3221 (0x3220), INA260 (0x2270) and others
    // also report "TI" and are refused rather than read as nonsense. If the
    // die id cannot be read, assume an INA226 (some clones do not answer it).
    if (die_ok && (die_id & 0xFFF0u) != 0x2260u) return InaChip::kNone;
    return InaChip::kIna226;
  }
  if (cfg_ok && cfg_reset == 0x399F) return InaChip::kIna219;
  return InaChip::kNone;
}

}  // namespace ast
