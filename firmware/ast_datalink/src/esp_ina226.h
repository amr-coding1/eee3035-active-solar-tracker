// Minimal INA226 driver. Addresses (contract / README wiring table):
//   0x40 tracked panel, 0x41 fixed panel, 0x44 tracker consumption.
// The scaling and calibration maths is in core/ast_cal.* (unit-tested on the PC).
#pragma once
#include <stdint.h>

#include "core/ast_cal.h"

namespace ast_esp {

class Ina226 {
 public:
  explicit Ina226(uint8_t address) : addr_(address) {}
  // New calibration. The chip is only set up again when the shunt scaling
  // changed (gain/offset corrections are applied in software).
  void set_cal(const ast::InaChannelCal& cal) {
    const bool scale_changed = cal.scale.shunt_ohm != cal_.scale.shunt_ohm ||
                               cal.scale.current_lsb_a != cal_.scale.current_lsb_a;
    cal_ = cal;
    if (scale_changed) configured_ = false;
  }
  const ast::InaChannelCal& cal() const { return cal_; }
  uint8_t address() const { return addr_; }
  // Reads bus voltage and current. Returns false when the chip does not
  // answer (sensor missing -> fault 1/2/3). (Re)configures the chip whenever
  // it has just appeared, so a sensor plugged in later works without a reboot.
  bool read(ast::InaReading* out);
  bool ok() const { return configured_; }

 private:
  bool configure();
  bool read16(uint8_t reg, uint16_t* v);
  bool write16(uint8_t reg, uint16_t v);
  uint8_t addr_;
  bool configured_ = false;
  ast::InaChannelCal cal_;
};

}  // namespace ast_esp
