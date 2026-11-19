#include "esp_ina.h"

#include <Arduino.h>
#include <Wire.h>

namespace ast_esp {

namespace {
const uint8_t kRegConfig = 0x00;
const uint8_t kRegShunt = 0x01;      // INA219 shunt voltage
const uint8_t kRegBus = 0x02;
const uint8_t kRegCurrent = 0x04;    // INA226 current
const uint8_t kRegCal = 0x05;        // INA226 calibration
const uint8_t kRegMfgId = 0xFE;      // INA226: 0x5449 ("TI")
const uint8_t kRegDieId = 0xFF;      // INA226: 0x2260
// INA226: continuous shunt + bus conversion, 64 averages x 8.244 ms each: one
// result every 1.055 s, i.e. every 1 Hz read is the average over (almost
// exactly) the second before it, so short servo bursts are not missed.
const uint16_t kIna226Config = 0x4000 | (0x3 << 9) | (0x7 << 6) | (0x7 << 3) | 0x7;
// INA219: 32 V bus range, PGA /8 (+-320 mV = +-3.2 A with 0.1 ohm), 128-sample
// averaging on bus and shunt (68.1 ms each), continuous. Shorter averaging
// than the INA226: a servo burst shorter than about 0.1 s can be missed.
const uint16_t kIna219Config = 0x3FFF;
const uint16_t kIna219PowerOn = 0x399F;   // value after reset (identification)
}  // namespace

bool InaSensor::read16(uint8_t reg, uint16_t* v) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr_, static_cast<uint8_t>(2)) != 2) return false;
  const uint8_t hi = static_cast<uint8_t>(Wire.read());
  const uint8_t lo = static_cast<uint8_t>(Wire.read());
  *v = static_cast<uint16_t>((hi << 8) | lo);
  return true;
}

bool InaSensor::write16(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(v >> 8));
  Wire.write(static_cast<uint8_t>(v & 0xFF));
  return Wire.endTransmission() == 0;
}

bool InaSensor::configure() {
  if (chip_ == ast::InaChip::kNone) {
    // Identify the chip (only addresses 0x40-0x4F are ever probed).
    if (addr_ < 0x40 || addr_ > 0x4F) return false;
    uint16_t mfg = 0, die = 0, cfg_reset = 0;
    const bool mfg_ok = read16(kRegMfgId, &mfg);
    const bool is_ti = mfg_ok && mfg == 0x5449;
    const bool die_ok = is_ti && read16(kRegDieId, &die);
    bool cfg_ok = false;
    if (!is_ti && write16(kRegConfig, 0x8000)) {   // INA219 reset bit
      delay(2);
      cfg_ok = read16(kRegConfig, &cfg_reset);
    }
    chip_ = ast::ina_identify(mfg_ok, mfg, die_ok, die, cfg_ok, cfg_reset);
    if (chip_ == ast::InaChip::kNone) return false;
  }
  if (chip_ == ast::InaChip::kIna226) {
    uint16_t id = 0;
    if (!read16(kRegMfgId, &id) || id != 0x5449) {
      chip_ = ast::InaChip::kNone;   // something else is there now
      return false;
    }
    const uint16_t cal = ast::ina226_cal_register(cal_.scale);
    if (cal == 0) return false;
    if (!write16(kRegConfig, kIna226Config) || !write16(kRegCal, cal)) return false;
  } else {
    if (!write16(kRegConfig, kIna219Config)) return false;
  }
  configured_ = true;
  return true;
}

bool InaSensor::read(ast::InaReading* out) {
  if (!configured_) {
    // Just (re)configured: the first averaged result is not ready yet, so this
    // read reports "no value" and the next one is good.
    configure();
    return false;
  }
  if (chip_ == ast::InaChip::kIna226) {
    uint16_t bus = 0, cur = 0, cal = 0;
    if (!read16(kRegBus, &bus) || !read16(kRegCurrent, &cur) || !read16(kRegCal, &cal)) {
      configured_ = false;
      return false;
    }
    if (cal != ast::ina226_cal_register(cal_.scale)) {
      // The chip was power-cycled (registers back to defaults): set it up
      // again; this reading is not usable.
      configured_ = false;
      return false;
    }
    *out = ast::ina226_convert(bus, static_cast<int16_t>(cur), cal_);
    return true;
  }
  uint16_t cfg = 0, bus = 0, shunt = 0;
  if (!read16(kRegConfig, &cfg) || !read16(kRegBus, &bus) || !read16(kRegShunt, &shunt)) {
    configured_ = false;
    return false;
  }
  if (cfg != kIna219Config) {
    // Power-cycled (config back to 0x399F) or replaced: set it up again.
    if (cfg != kIna219PowerOn) chip_ = ast::InaChip::kNone;
    configured_ = false;
    return false;
  }
  *out = ast::ina219_convert(bus, static_cast<int16_t>(shunt), cal_);
  return true;
}

int i2c_scan(uint8_t* found, int max_found) {
  int n = 0;
  for (uint8_t a = 0x08; a < 0x78 && n < max_found; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found[n++] = a;
  }
  return n;
}

const char* i2c_guess(uint8_t address) {
  if (address >= 0x40 && address <= 0x4F) return "power sensor range (INA226/INA219)";
  if (address == 0x68) return "DS3231 clock";
  if (address == 0x57) return "AT24C32 EEPROM on the DS3231 board";
  if (address == 0x3C || address == 0x3D) return "OLED display";
  return "unknown";
}

}  // namespace ast_esp
