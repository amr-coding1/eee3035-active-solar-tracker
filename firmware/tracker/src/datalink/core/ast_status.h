// Channel-1 status string (contract section 4) and the fault-priority rule.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "ast_record.h"

namespace ast {

// "TRK;r=12;p=A;e=0" or "...;s=1" for a simulated sample.
// Longest possible text: "MOV;r=999;p=B;e=9;s=1" = 21 characters.
constexpr size_t kStatusMax = 24;
// Writes the NUL-terminated status of `r`; returns its length (0 if cap is too small).
size_t build_status(const Record& r, char* out, size_t cap);

// One code per sample (section 4):
//   1. the control system's code when it is non-zero;
//   2. otherwise a missing power sensor found by the data link (1, 2, 3; the
//      affected field is omitted as well);
//   3. otherwise the data link's own codes in the order 4, 6, 9.
uint8_t select_fault(uint8_t control_code, uint8_t sensor_code, bool clock_fault,
                     bool overflow_fault, bool upload_fault);

// 1/2/3 for the first power sensor with no valid reading in the minute, else 0.
uint8_t sensor_fault_from_flags(uint16_t record_flags);

}  // namespace ast
