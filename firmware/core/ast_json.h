// Exact ThingSpeak bulk-update body (contract 7.3, 3.1).
//
//   {"write_api_key":"KEY","updates":[{"created_at":"2026-11-20 00:00:00 +0000",
//    "field1":123.4,...,"field8":17,"status":"TRK;r=1;p=A;e=0"},...]}
//
// Compact JSON, keys in the order created_at, field1..field8, status. Unknown
// values are omitted. field1-6 one decimal, field7 three decimals, field8 integer.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "ast_http.h"
#include "ast_record.h"

namespace ast {

constexpr size_t kJsonEntryMax = 256;  // longest possible entry is about 215 characters

// Formats one update object (no trailing comma, NUL-terminated). Returns its length.
size_t format_entry(const Record& r, char* out, size_t cap);

// Fixed-point helpers (exposed for tests): value in tenths -> "-12.3";
// millivolts -> "3.912".
size_t format_tenths(int64_t tenths, char* out, size_t cap);
size_t format_milli(uint32_t milli, char* out, size_t cap);

std::string json_escape(const std::string& s);

class JsonBatch : public BodyReader {
 public:
  // `records` must stay valid while the batch is being read. At most
  // kMaxBatch (960) records are used.
  // Defensive rule: created_at must be strictly increasing inside one bulk
  // update (ThingSpeak rejects the WHOLE update when it contains a duplicate
  // timestamp). A record whose minute is not later than the previous record
  // written (or than `after_minute`) is left out; see included().
  JsonBatch(const std::string& write_key, const Record* records, size_t count,
            uint32_t after_minute = 0);

  // True when record i is part of the body. entries() = how many are.
  bool included(size_t i) const { return i < count_ && include_[i]; }
  size_t entries() const { return entries_; }

  size_t size() override;
  size_t read(uint8_t* buf, size_t max) override;
  void rewind() override;

  size_t count() const { return count_; }
  std::string to_string();  // whole body (PC, tests); rewinds the read position

 private:
  void load_piece();  // puts piece number `piece_` into chunk_

  std::string header_;
  const Record* records_;
  size_t count_;
  std::vector<bool> include_;
  size_t entries_ = 0;
  size_t first_included_ = 0;
  size_t total_ = 0;
  bool total_known_ = false;
  // Pieces: 0 = header, 1..count = entries (with separating comma), count+1 = "]}".
  size_t piece_ = 0;
  char chunk_[kJsonEntryMax + 2];
  size_t chunk_len_ = 0, chunk_pos_ = 0;
  bool chunk_loaded_ = false;
};

}  // namespace ast
