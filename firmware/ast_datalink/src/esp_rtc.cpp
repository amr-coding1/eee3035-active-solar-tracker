#include "esp_rtc.h"

#include <Arduino.h>
#include <Wire.h>

#include "core/ast_time.h"

namespace ast_esp {

namespace {
const uint8_t kAddr = 0x68;
const uint8_t kRegTime = 0x00;     // sec, min, hour, weekday, date, month/century, year
const uint8_t kRegControl = 0x0E;
const uint8_t kRegStatus = 0x0F;   // bit 7 = OSF (oscillator stopped at some point)
const uint8_t kRegTemp = 0x11;
uint8_t bcd2bin(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
uint8_t bin2bcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
bool bcd_ok(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }
}  // namespace

bool Ds3231::read_regs(uint8_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(kAddr, n) != n) return false;
  for (uint8_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

bool Ds3231::write_regs(uint8_t reg, const uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write(buf, n);
  return Wire.endTransmission() == 0;
}

bool Ds3231::begin() {
  Wire.beginTransmission(kAddr);
  present_ = Wire.endTransmission() == 0;
  if (present_) {
    // Control: oscillator on (EOSC = 0) also on battery, square wave off (INTCN = 1).
    const uint8_t control = 0x04;
    write_regs(kRegControl, &control, 1);
  }
  return present_;
}

bool Ds3231::read(int64_t* unix_s, bool* lost) {
  if (!present_) return false;
  uint8_t t[7], status = 0;
  if (!read_regs(kRegTime, t, 7) || !read_regs(kRegStatus, &status, 1)) return false;
  *lost = (status & 0x80) != 0;
  const uint8_t hour_reg = t[2];
  int hour;
  if (hour_reg & 0x40) {  // 12-hour mode (never written by us, but be correct)
    hour = bcd2bin(hour_reg & 0x1F) % 12 + ((hour_reg & 0x20) ? 12 : 0);
  } else {
    hour = bcd2bin(hour_reg & 0x3F);
  }
  if (!bcd_ok(t[0] & 0x7F) || !bcd_ok(t[1] & 0x7F) || !bcd_ok(t[4] & 0x3F) || !bcd_ok(t[5] & 0x1F) || !bcd_ok(t[6])) {
    return false;
  }
  ast::CivilTime c;
  c.second = bcd2bin(t[0] & 0x7F);
  c.minute = bcd2bin(t[1] & 0x7F);
  c.hour = hour;
  c.day = bcd2bin(t[4] & 0x3F);
  c.month = bcd2bin(t[5] & 0x1F);
  c.year = 2000 + bcd2bin(t[6]) + ((t[5] & 0x80) ? 100 : 0);
  if (!ast::civil_is_valid(c)) return false;
  *unix_s = ast::unix_from_civil(c);
  return true;
}

bool Ds3231::set(int64_t unix_s) {
  if (!present_) return false;
  const ast::CivilTime c = ast::civil_from_unix(unix_s);
  if (c.year < 2000 || c.year > 2199) return false;
  const int64_t days = unix_s / 86400;
  uint8_t t[7];
  t[0] = bin2bcd(c.second);
  t[1] = bin2bcd(c.minute);
  t[2] = bin2bcd(c.hour);                                  // 24-hour mode
  t[3] = static_cast<uint8_t>((days + 4) % 7 + 1);         // 1..7, 1970-01-01 was a Thursday
  t[4] = bin2bcd(c.day);
  t[5] = static_cast<uint8_t>(bin2bcd(c.month) | (c.year >= 2100 ? 0x80 : 0x00));
  t[6] = bin2bcd(c.year % 100);
  if (!write_regs(kRegTime, t, 7)) return false;
  // Clear OSF (bit 7); keep the 32 kHz output disabled (bit 3 = 0).
  const uint8_t status = 0x00;
  return write_regs(kRegStatus, &status, 1);
}

float Ds3231::temperature() {
  uint8_t b[2];
  if (!present_ || !read_regs(kRegTemp, b, 2)) return NAN;
  const int16_t raw = static_cast<int16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
  return static_cast<float>(raw >> 6) * 0.25f;
}

}  // namespace ast_esp
