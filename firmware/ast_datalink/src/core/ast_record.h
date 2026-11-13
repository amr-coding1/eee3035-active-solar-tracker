// One logged sample (one UTC minute) and its fixed-size on-flash encoding.
// Contract 3.1 (fields), 4 (state / fault), 7.2 (fixed-size record with CRC).
//
// Values are stored as scaled integers in exactly the resolution the channel
// uses (0.1 mW, 0.1 deg, 0.1 degC, 1 mV), so the JSON text is produced with
// integer arithmetic only: the same record always gives the same characters on
// the ESP32 and on the PC.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ast {

enum class State : uint8_t { TRK = 0, MOV = 1, HLD = 2, IDL = 3, STW = 4, FLT = 5 };
constexpr uint8_t kStateCount = 6;
const char* state_name(State s);                    // "TRK", "MOV", ...
bool state_from_name(const char* name, State* s);

// Record::flags bits. A cleared "has" bit means the value is unknown and the
// field is omitted from the upload (contract 3.1: never written as 0).
constexpr uint16_t kHasPtrk = 1u << 0;      // field1
constexpr uint16_t kHasPfix = 1u << 1;      // field2
constexpr uint16_t kHasPuse = 1u << 2;      // field3
constexpr uint16_t kHasAngPanel = 1u << 3;  // field4
constexpr uint16_t kHasAngSun = 1u << 4;    // field5
constexpr uint16_t kHasTemp = 1u << 5;      // field6
constexpr uint16_t kHasVbat = 1u << 6;      // field7
constexpr uint16_t kSimulated = 1u << 7;    // status key s=1
constexpr uint16_t kPanelB = 1u << 8;       // status key p=B (clear: p=A)
constexpr uint16_t kFlagsMask = 0x01FFu;

struct Record {
  uint32_t seq = 0;            // field8, starts at 1
  uint32_t minute_unix = 0;    // start of the UTC minute, unix seconds (multiple of 60)
  uint32_t p_trk_dmw = 0;      // 0.1 mW
  uint32_t p_fix_dmw = 0;
  uint32_t p_use_dmw = 0;
  int16_t ang_panel_ddeg = 0;  // 0.1 deg, east negative
  int16_t ang_sun_ddeg = 0;
  int16_t temp_dc = 0;         // 0.1 degC
  uint16_t vbat_mv = 0;        // 1 mV
  uint16_t run = 0;            // status key r
  uint16_t flags = 0;
  State state = State::IDL;
  uint8_t fault = 0;           // status key e, 0..9

  bool has(uint16_t bit) const { return (flags & bit) != 0; }
  char panel() const { return (flags & kPanelB) ? 'B' : 'A'; }
};

bool operator==(const Record& a, const Record& b);
inline bool operator!=(const Record& a, const Record& b) { return !(a == b); }

// On-flash layout (little-endian), 40 bytes:
//   0 magic 0xA5 | 1 format version 1 | 2-3 flags | 4-7 seq | 8-11 minute_unix
//   12-15 p_trk | 16-19 p_fix | 20-23 p_use | 24-25 ang_panel | 26-27 ang_sun
//   28-29 temp | 30-31 vbat | 32-33 run | 34 state | 35 fault | 36-39 CRC-32 of 0..35
constexpr size_t kRecordSize = 40;
constexpr uint8_t kRecordMagic = 0xA5;
constexpr uint8_t kRecordFormat = 1;

void encode_record(const Record& r, uint8_t* out /* kRecordSize bytes */);
// Returns false (and leaves *r untouched) on bad magic, version, CRC, or a
// field that is out of range (state > FLT, fault > 9, minute not on :00).
bool decode_record(const uint8_t* in /* kRecordSize bytes */, Record* r);

}  // namespace ast
