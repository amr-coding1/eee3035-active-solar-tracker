#include "esp_ina226.h"

#include <Arduino.h>
#include <Wire.h>

namespace ast_esp {

namespace {
const uint8_t kRegConfig = 0x00;
const uint8_t kRegBus = 0x02;
const uint8_t kRegCurrent = 0x04;
const uint8_t kRegCal = 0x05;
const uint8_t kRegMfgId = 0xFE;      // reads 0x5449 ("TI")
// Continuous shunt + bus conversion, 64 averages x 8.244 ms each: one result
// every 1.055 s, i.e. every 1 Hz read is the average over (almost exactly)
// the second before it, so short motor bursts are not missed.
const uint16_t kConfig = 0x4000 | (0x3 << 9) | (0x7 << 6) | (0x7 << 3) | 0x7;
}  // namespace

bool Ina226::read16(uint8_t reg, uint16_t* v) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr_, static_cast<uint8_t>(2)) != 2) return false;
  const uint8_t hi = static_cast<uint8_t>(Wire.read());
  const uint8_t lo = static_cast<uint8_t>(Wire.read());
  *v = static_cast<uint16_t>((hi << 8) | lo);
  return true;
}

bool Ina226::write16(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(v >> 8));
  Wire.write(static_cast<uint8_t>(v & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Ina226::configure() {
  uint16_t id = 0;
  if (!read16(kRegMfgId, &id) || id != 0x5449) return false;
  const uint16_t cal = ast::ina226_cal_register(cal_.scale);
  if (cal == 0) return false;
  if (!write16(kRegConfig, kConfig) || !write16(kRegCal, cal)) return false;
  configured_ = true;
  return true;
}

bool Ina226::read(ast::InaReading* out) {
  if (!configured_) {
    // Just (re)configured: the first averaged result is ready in about 1 s,
    // so this read reports "no value" and the next one is good.
    configure();
    return false;
  }
  uint16_t bus = 0, cur = 0, cal = 0;
  if (!read16(kRegBus, &bus) || !read16(kRegCurrent, &cur) || !read16(kRegCal, &cal)) {
    configured_ = false;
    return false;
  }
  if (cal != ast::ina226_cal_register(cal_.scale)) {
    // The chip was power-cycled (registers back to defaults): set it up again;
    // this reading is not usable.
    configured_ = false;
    return false;
  }
  *out = ast::ina226_convert(bus, static_cast<int16_t>(cur), cal_);
  return true;
}

}  // namespace ast_esp
