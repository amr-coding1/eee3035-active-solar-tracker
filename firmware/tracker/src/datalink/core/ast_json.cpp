#include "ast_json.h"

#include <stdio.h>
#include <string.h>

#include "ast_config.h"
#include "ast_status.h"
#include "ast_time.h"

namespace ast {

size_t format_tenths(int64_t tenths, char* out, size_t cap) {
  const bool neg = tenths < 0;
  const unsigned long long a = static_cast<unsigned long long>(neg ? -tenths : tenths);
  const int n = snprintf(out, cap, "%s%llu.%llu", neg ? "-" : "", a / 10u, a % 10u);
  return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
}

size_t format_milli(uint32_t milli, char* out, size_t cap) {
  const int n = snprintf(out, cap, "%lu.%03lu", static_cast<unsigned long>(milli / 1000u),
                         static_cast<unsigned long>(milli % 1000u));
  return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
}

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      o.push_back('\\');
      o.push_back(ch);
    } else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else {
      o.push_back(ch);
    }
  }
  return o;
}

namespace {
// Appends `"fieldN":<text>,` style pieces with bounds checking.
struct Out {
  char* p;
  size_t cap;
  size_t len = 0;
  bool ok = true;
  void put(const char* s) {
    const size_t n = strlen(s);
    if (len + n + 1 > cap) { ok = false; return; }
    memcpy(p + len, s, n);
    len += n;
    p[len] = '\0';
  }
};
}  // namespace

size_t format_entry(const Record& r, char* out, size_t cap) {
  if (cap == 0) return 0;
  Out o{out, cap};
  char num[24];
  char ts[kCreatedAtLen + 1];
  format_created_at(r.minute_unix, ts);
  o.put("{\"created_at\":\"");
  o.put(ts);
  o.put("\"");
  if (r.has(kHasPtrk)) { format_tenths(r.p_trk_dmw, num, sizeof num); o.put(",\"field1\":"); o.put(num); }
  if (r.has(kHasPfix)) { format_tenths(r.p_fix_dmw, num, sizeof num); o.put(",\"field2\":"); o.put(num); }
  if (r.has(kHasPuse)) { format_tenths(r.p_use_dmw, num, sizeof num); o.put(",\"field3\":"); o.put(num); }
  if (r.has(kHasAngPanel)) { format_tenths(r.ang_panel_ddeg, num, sizeof num); o.put(",\"field4\":"); o.put(num); }
  if (r.has(kHasAngSun)) { format_tenths(r.ang_sun_ddeg, num, sizeof num); o.put(",\"field5\":"); o.put(num); }
  if (r.has(kHasTemp)) { format_tenths(r.temp_dc, num, sizeof num); o.put(",\"field6\":"); o.put(num); }
  if (r.has(kHasVsup)) { format_milli(r.vsup_mv, num, sizeof num); o.put(",\"field7\":"); o.put(num); }
  snprintf(num, sizeof num, "%lu", static_cast<unsigned long>(r.seq));
  o.put(",\"field8\":");
  o.put(num);
  char status[kStatusMax];
  build_status(r, status, sizeof status);
  o.put(",\"status\":\"");
  o.put(status);
  o.put("\"}");
  if (!o.ok) { out[0] = '\0'; return 0; }
  return o.len;
}

JsonBatch::JsonBatch(const std::string& write_key, const Record* records, size_t count,
                     uint32_t after_minute)
    : header_("{\"write_api_key\":\"" + json_escape(write_key) + "\",\"updates\":["),
      records_(records),
      count_(count > kMaxBatch ? kMaxBatch : count),
      include_(count_, false) {
  uint32_t prev = after_minute;
  first_included_ = count_;
  for (size_t i = 0; i < count_; ++i) {
    if (records_[i].minute_unix > prev) {
      include_[i] = true;
      prev = records_[i].minute_unix;
      if (entries_ == 0) first_included_ = i;
      ++entries_;
    }
  }
}

void JsonBatch::rewind() {
  piece_ = 0;
  chunk_len_ = chunk_pos_ = 0;
  chunk_loaded_ = false;
}

void JsonBatch::load_piece() {
  chunk_pos_ = 0;
  if (piece_ >= 1 && piece_ <= count_) {
    size_t n = 0;
    const size_t i = piece_ - 1;
    if (include_[i]) {
      if (i != first_included_) chunk_[n++] = ',';
      n += format_entry(records_[i], chunk_ + n, sizeof(chunk_) - n);
    }
    chunk_len_ = n;  // 0 for a record that is left out
  } else {  // footer
    chunk_[0] = ']';
    chunk_[1] = '}';
    chunk_len_ = 2;
  }
  chunk_loaded_ = true;
}

size_t JsonBatch::read(uint8_t* buf, size_t max) {
  size_t done = 0;
  while (done < max && piece_ <= count_ + 1) {
    if (piece_ == 0) {  // header straight from the string (the key may be long)
      const size_t left = header_.size() - chunk_pos_;
      const size_t n = left < max - done ? left : max - done;
      memcpy(buf + done, header_.data() + chunk_pos_, n);
      done += n;
      chunk_pos_ += n;
      if (chunk_pos_ == header_.size()) { ++piece_; chunk_loaded_ = false; }
      continue;
    }
    if (!chunk_loaded_) load_piece();
    const size_t left = chunk_len_ - chunk_pos_;
    const size_t n = left < max - done ? left : max - done;
    memcpy(buf + done, chunk_ + chunk_pos_, n);
    done += n;
    chunk_pos_ += n;
    if (chunk_pos_ == chunk_len_) { ++piece_; chunk_loaded_ = false; }
  }
  return done;
}

size_t JsonBatch::size() {
  if (!total_known_) {
    size_t t = header_.size() + 2;
    char tmp[kJsonEntryMax];
    for (size_t i = 0; i < count_; ++i) {
      if (include_[i]) t += format_entry(records_[i], tmp, sizeof tmp) + (i != first_included_ ? 1 : 0);
    }
    total_ = t;
    total_known_ = true;
  }
  return total_;
}

std::string JsonBatch::to_string() {
  rewind();
  std::string s;
  s.reserve(size());
  uint8_t b[512];
  size_t n;
  while ((n = read(b, sizeof b)) > 0) s.append(reinterpret_cast<const char*>(b), n);
  rewind();  // leaves the batch ready to be streamed from the start
  return s;
}

}  // namespace ast
