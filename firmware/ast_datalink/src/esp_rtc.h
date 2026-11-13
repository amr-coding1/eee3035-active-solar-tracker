// Minimal DS3231 driver (I2C 0x68): UTC time and the oscillator-stop flag
// that tells us the clock lost power (contract 7.1).
#pragma once
#include <stdint.h>

namespace ast_esp {

class Ds3231 {
 public:
  // True if a chip answers at 0x68.
  bool begin();
  bool present() const { return present_; }
  // Reads the time as unix seconds (UTC). *lost = oscillator-stop flag (the
  // time is not trustworthy). Returns false on an I2C error or nonsense BCD.
  bool read(int64_t* unix_s, bool* lost);
  // Writes the time and clears the oscillator-stop flag.
  bool set(int64_t unix_s);
  // Die temperature in degC (0.25 degC steps); NAN on error. Handy on the bench.
  float temperature();

 private:
  bool read_regs(uint8_t reg, uint8_t* buf, uint8_t n);
  bool write_regs(uint8_t reg, const uint8_t* buf, uint8_t n);
  bool present_ = false;
};

}  // namespace ast_esp
