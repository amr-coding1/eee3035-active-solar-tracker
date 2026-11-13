#include "ast_record.h"

#include <string.h>

#include "ast_crc.h"

namespace ast {

static const char* const kStateNames[kStateCount] = {"TRK", "MOV", "HLD", "IDL", "STW", "FLT"};

const char* state_name(State s) {
  const uint8_t i = static_cast<uint8_t>(s);
  return i < kStateCount ? kStateNames[i] : "FLT";
}

bool state_from_name(const char* name, State* s) {
  if (name == nullptr) return false;
  for (uint8_t i = 0; i < kStateCount; ++i) {
    if (strcmp(name, kStateNames[i]) == 0) {
      *s = static_cast<State>(i);
      return true;
    }
  }
  return false;
}

bool operator==(const Record& a, const Record& b) {
  return a.seq == b.seq && a.minute_unix == b.minute_unix && a.p_trk_dmw == b.p_trk_dmw &&
         a.p_fix_dmw == b.p_fix_dmw && a.p_use_dmw == b.p_use_dmw &&
         a.ang_panel_ddeg == b.ang_panel_ddeg && a.ang_sun_ddeg == b.ang_sun_ddeg &&
         a.temp_dc == b.temp_dc && a.vbat_mv == b.vbat_mv && a.run == b.run &&
         a.flags == b.flags && a.state == b.state && a.fault == b.fault;
}

void encode_record(const Record& r, uint8_t* out) {
  out[0] = kRecordMagic;
  out[1] = kRecordFormat;
  put_u16(out + 2, static_cast<uint16_t>(r.flags & kFlagsMask));
  put_u32(out + 4, r.seq);
  put_u32(out + 8, r.minute_unix);
  put_u32(out + 12, r.p_trk_dmw);
  put_u32(out + 16, r.p_fix_dmw);
  put_u32(out + 20, r.p_use_dmw);
  put_u16(out + 24, static_cast<uint16_t>(r.ang_panel_ddeg));
  put_u16(out + 26, static_cast<uint16_t>(r.ang_sun_ddeg));
  put_u16(out + 28, static_cast<uint16_t>(r.temp_dc));
  put_u16(out + 30, r.vbat_mv);
  put_u16(out + 32, r.run);
  out[34] = static_cast<uint8_t>(r.state);
  out[35] = r.fault;
  put_u32(out + 36, crc32(out, 36));
}

bool decode_record(const uint8_t* in, Record* r) {
  if (in[0] != kRecordMagic || in[1] != kRecordFormat) return false;
  if (get_u32(in + 36) != crc32(in, 36)) return false;
  Record t;
  t.flags = get_u16(in + 2);
  t.seq = get_u32(in + 4);
  t.minute_unix = get_u32(in + 8);
  t.p_trk_dmw = get_u32(in + 12);
  t.p_fix_dmw = get_u32(in + 16);
  t.p_use_dmw = get_u32(in + 20);
  t.ang_panel_ddeg = static_cast<int16_t>(get_u16(in + 24));
  t.ang_sun_ddeg = static_cast<int16_t>(get_u16(in + 26));
  t.temp_dc = static_cast<int16_t>(get_u16(in + 28));
  t.vbat_mv = get_u16(in + 30);
  t.run = get_u16(in + 32);
  if (in[34] >= kStateCount || in[35] > 9) return false;
  if ((t.flags & ~kFlagsMask) != 0 || t.seq == 0 || t.minute_unix % 60u != 0) return false;
  t.state = static_cast<State>(in[34]);
  t.fault = in[35];
  *r = t;
  return true;
}

}  // namespace ast
