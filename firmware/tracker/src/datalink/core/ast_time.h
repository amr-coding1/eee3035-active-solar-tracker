// UTC calendar helpers (no time zones, no DST, no libc time functions).
// Contract 3.1: created_at is "YYYY-MM-DD HH:MM:00 +0000", start of the minute.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ast {

struct CivilTime {
  int year; int month; int day; int hour; int minute; int second;
};

// Days since 1970-01-01 for a proleptic Gregorian date (valid for any year).
int64_t days_from_civil(int year, int month, int day);
CivilTime civil_from_unix(int64_t unix_s);
int64_t unix_from_civil(const CivilTime& c);
bool civil_is_valid(const CivilTime& c);

inline int64_t minute_floor(int64_t unix_s) {
  int64_t r = unix_s % 60;
  if (r < 0) r += 60;
  return unix_s - r;
}

constexpr size_t kCreatedAtLen = 25;  // strlen("2026-11-20 00:00:00 +0000")
// Writes the contract timestamp of the minute containing unix_s (seconds forced
// to 00) plus a terminating NUL; `out` must hold kCreatedAtLen + 1 bytes.
void format_created_at(int64_t unix_s, char* out);

constexpr size_t kIsoLen = 20;        // strlen("2026-11-20T00:00:00Z")
void format_iso(int64_t unix_s, char* out);  // out must hold kIsoLen + 1 bytes

// Accepts "YYYY-MM-DDTHH:MM:SSZ", "YYYY-MM-DD HH:MM:SS" (UTC), with or without
// the trailing Z, and "YYYY-MM-DDTHH:MM" (seconds = 0). Returns false otherwise.
bool parse_iso(const char* s, int64_t* unix_s);

}  // namespace ast
