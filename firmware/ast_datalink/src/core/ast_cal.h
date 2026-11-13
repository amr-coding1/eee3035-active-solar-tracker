// INA226 scaling and two-point calibration maths (portable part of the
// sensor driver; the I2C access is in ast_datalink/src/esp_ina226.*).
#pragma once
#include <stdint.h>

namespace ast {

// true_value = gain * raw_value + offset
struct LinCal {
  float gain = 1.0f;
  float offset = 0.0f;
  float apply(float raw) const { return gain * raw + offset; }
};

struct CalPoint {
  bool set = false;
  float raw = 0;   // what the INA226 reported (uncalibrated, V or mA)
  float ref = 0;   // what the bench DMM showed (same unit)
};

// Solves the straight line through two points. Refuses (returns false and
// sets *why) when the points are too close together or the result is far from
// an ideal sensor (gain outside 0.5..2), which means a typing mistake.
bool two_point_cal(const CalPoint& p1, const CalPoint& p2, LinCal* out, const char** why);

struct Ina226Scale {
  float shunt_ohm = 0.1f;        // breakout boards carry R100; the power lead supplies the real value
  float current_lsb_a = 0.0001f; // 100 uA per bit -> +/-3.2767 A range in the register
};

constexpr float kInaBusLsbV = 0.00125f;      // bus voltage register: 1.25 mV/bit
constexpr float kInaShuntLsbV = 0.0000025f;  // shunt voltage register: 2.5 uV/bit

// Calibration register value, datasheet eq. 1: CAL = 0.00512 / (I_lsb * R_shunt),
// limited to 1..65535. Returns 0 when the scale is not usable.
uint16_t ina226_cal_register(const Ina226Scale& s);
// Because CAL is rounded to an integer the real LSB differs slightly from
// the requested one; this is the value to multiply the current register by.
float ina226_effective_current_lsb(const Ina226Scale& s);
inline float ina226_bus_volts(uint16_t raw) { return static_cast<float>(raw) * kInaBusLsbV; }
float ina226_current_amps(int16_t raw, const Ina226Scale& s);

struct InaChannelCal {
  Ina226Scale scale;
  LinCal volts;      // applied to the bus voltage in V
  LinCal milliamps;  // applied to the current in mA
};

struct InaReading {
  float volts = 0;       // calibrated
  float milliamps = 0;   // calibrated
  float power_mw = 0;    // volts * milliamps
  float raw_volts = 0;   // before two-point correction (used by `cal`)
  float raw_milliamps = 0;
};
InaReading ina226_convert(uint16_t bus_raw, int16_t current_raw, const InaChannelCal& cal);

}  // namespace ast
