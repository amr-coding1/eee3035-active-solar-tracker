// Power-sensor driver for INA226 and INA219 (the 19 Nov block diagram shows a
// four-channel INA219 board, the BOM three INA226 boards: either works). The
// chip at each address is identified on first contact (core/ast_cal.h,
// ina_identify) and set up for a 1 Hz reading:
//   INA226: 64 averages x 8.244 ms, current from the calibration register.
//   INA219: 128-sample averaging (68 ms), 32 V / +-320 mV range, current from
//           the shunt voltage and the shunt value (cal <ch> shunt <ohms>).
// The scaling and calibration maths is in core/ast_cal.* (unit-tested on the PC).
#pragma once
#include <stdint.h>

#include "core/ast_cal.h"

namespace ast_esp {

class InaSensor {
 public:
  explicit InaSensor(uint8_t address = 0x40) : addr_(address) {}
  // Changing the address forgets the chip found at the old one.
  void set_address(uint8_t address) {
    if (address != addr_) {
      addr_ = address;
      configured_ = false;
      chip_ = ast::InaChip::kNone;
    }
  }
  // New calibration. An INA226 is only set up again when the shunt scaling
  // changed (gain/offset corrections are applied in software).
  void set_cal(const ast::InaChannelCal& cal) {
    const bool scale_changed = cal.scale.shunt_ohm != cal_.scale.shunt_ohm ||
                               cal.scale.current_lsb_a != cal_.scale.current_lsb_a;
    cal_ = cal;
    if (scale_changed) configured_ = false;
  }
  const ast::InaChannelCal& cal() const { return cal_; }
  uint8_t address() const { return addr_; }
  ast::InaChip chip() const { return chip_; }
  // Reads bus voltage and current. Returns false when no supported chip
  // answers (sensor missing -> fault 1/2/3). (Re)configures the chip whenever
  // it has just appeared or was power-cycled, so a sensor plugged in later
  // works without a reboot; that first call returns false.
  bool read(ast::InaReading* out);
  bool ok() const { return configured_; }

 private:
  bool configure();
  bool read16(uint8_t reg, uint16_t* v);
  bool write16(uint8_t reg, uint16_t v);
  uint8_t addr_;
  bool configured_ = false;
  ast::InaChip chip_ = ast::InaChip::kNone;
  ast::InaChannelCal cal_;
};

// I2C bus scan (caller holds the bus mutex): fills `found` with every address
// that acknowledges, returns how many.
int i2c_scan(uint8_t* found, int max_found);
// What usually sits at an address on this project ("power sensor", "DS3231 clock", ...).
const char* i2c_guess(uint8_t address);

}  // namespace ast_esp
