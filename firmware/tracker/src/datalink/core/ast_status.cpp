#include "ast_status.h"

#include <stdio.h>

#include "ast_config.h"

namespace ast {

size_t build_status(const Record& r, char* out, size_t cap) {
  const unsigned run = r.run > kRunMax ? kRunMax : r.run;
  const unsigned fault = r.fault > 9 ? 9u : r.fault;
  const int n = snprintf(out, cap, "%s;r=%u;p=%c;e=%u%s", state_name(r.state), run, r.panel(),
                         fault, r.has(kSimulated) ? ";s=1" : "");
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    if (cap > 0) out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

uint8_t select_fault(uint8_t control_code, uint8_t sensor_code, bool clock_fault,
                     bool overflow_fault, bool upload_fault) {
  if (control_code != 0 && control_code <= 9) return control_code;
  if (sensor_code >= kFaultSensorTrk && sensor_code <= kFaultSensorUse) return sensor_code;
  if (clock_fault) return kFaultClock;
  if (overflow_fault) return kFaultOverflow;
  if (upload_fault) return kFaultUpload;
  return kFaultNone;
}

uint8_t sensor_fault_from_flags(uint16_t flags) {
  if ((flags & kHasPtrk) == 0) return kFaultSensorTrk;
  if ((flags & kHasPfix) == 0) return kFaultSensorFix;
  if ((flags & kHasPuse) == 0) return kFaultSensorUse;
  return kFaultNone;
}

}  // namespace ast
