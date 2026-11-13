// CRC and UTC calendar (contract 3.1 created_at format).
#include "ast_crc.h"
#include "ast_time.h"
#include "test_framework.h"

using namespace ast;

static std::string created(int64_t t) {
  char b[kCreatedAtLen + 1];
  format_created_at(t, b);
  return b;
}

TEST(crc32_matches_standard_check_value) {
  CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
  CHECK_EQ(crc32(nullptr, 0), 0u);
}

TEST(crc32_detects_single_bit_flip) {
  uint8_t buf[36];
  for (size_t i = 0; i < sizeof buf; ++i) buf[i] = static_cast<uint8_t>(i * 7);
  const uint32_t good = crc32(buf, sizeof buf);
  for (size_t byte = 0; byte < sizeof buf; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      buf[byte] = static_cast<uint8_t>(buf[byte] ^ (1u << bit));
      CHECK(crc32(buf, sizeof buf) != good);
      buf[byte] = static_cast<uint8_t>(buf[byte] ^ (1u << bit));
    }
  }
}

TEST(little_endian_helpers_round_trip) {
  uint8_t b[4];
  put_u32(b, 0x12345678u);
  CHECK_EQ(b[0], 0x78);
  CHECK_EQ(b[3], 0x12);
  CHECK_EQ(get_u32(b), 0x12345678u);
  put_u16(b, 0xBEEF);
  CHECK_EQ(get_u16(b), 0xBEEF);
}

TEST(created_at_exact_format) {
  CHECK_STR(created(1795132800), "2026-11-20 00:00:00 +0000");
  CHECK_EQ(strlen(created(1795132800).c_str()), kCreatedAtLen);
}

TEST(created_at_seconds_are_always_zero) {
  // 2026-11-13 14:07:33 -> start of that minute
  CHECK_STR(created(1794578853), "2026-11-13 14:07:00 +0000");
  CHECK_STR(created(1795132800 + 59), "2026-11-20 00:00:00 +0000");
  CHECK_STR(created(1795132800 + 60), "2026-11-20 00:01:00 +0000");
}

TEST(created_at_year_boundary) {
  CHECK_STR(created(1798761599), "2026-12-31 23:59:00 +0000");
  CHECK_STR(created(1798761600), "2027-01-01 00:00:00 +0000");
}

TEST(created_at_leap_day_2028) {
  CHECK_STR(created(1835481540), "2028-02-29 23:59:00 +0000");
  CHECK_STR(created(1835481600), "2028-03-01 00:00:00 +0000");
  CHECK_STR(created(1709164800), "2024-02-29 00:00:00 +0000");
}

TEST(created_at_2100_is_not_a_leap_year) {
  CHECK_STR(created(4107542340), "2100-02-28 23:59:00 +0000");
  CHECK_STR(created(4107542400), "2100-03-01 00:00:00 +0000");
}

TEST(created_at_month_boundaries_through_a_year) {
  // First minute of every month of 2026, and the minute before it.
  const char* first[12] = {"2026-01-01", "2026-02-01", "2026-03-01", "2026-04-01", "2026-05-01", "2026-06-01",
                           "2026-07-01", "2026-08-01", "2026-09-01", "2026-10-01", "2026-11-01", "2026-12-01"};
  const char* before[12] = {"2025-12-31", "2026-01-31", "2026-02-28", "2026-03-31", "2026-04-30", "2026-05-31",
                            "2026-06-30", "2026-07-31", "2026-08-31", "2026-09-30", "2026-10-31", "2026-11-30"};
  for (int m = 1; m <= 12; ++m) {
    const int64_t t = days_from_civil(2026, m, 1) * 86400;
    CHECK_STR(created(t), std::string(first[m - 1]) + " 00:00:00 +0000");
    CHECK_STR(created(t - 1), std::string(before[m - 1]) + " 23:59:00 +0000");
  }
}

TEST(created_at_ignores_uk_clock_changes) {
  // UK clocks change on 2026-03-29 01:00 UTC and 2026-10-25 01:00 UTC. UTC
  // text must run straight through with no repeated or missing hour.
  CHECK_STR(created(1774746000 - 60), "2026-03-29 00:59:00 +0000");
  CHECK_STR(created(1774746000), "2026-03-29 01:00:00 +0000");
  CHECK_STR(created(1774746000 + 3600), "2026-03-29 02:00:00 +0000");
  CHECK_STR(created(1792891800), "2026-10-25 01:30:00 +0000");
  CHECK_STR(created(1792891800 - 3600), "2026-10-25 00:30:00 +0000");
}

TEST(civil_round_trip_every_day_for_80_years) {
  int64_t prev_day_number = days_from_civil(2020, 1, 1) - 1;
  for (int64_t d = days_from_civil(2020, 1, 1); d < days_from_civil(2100, 12, 31); ++d) {
    const CivilTime c = civil_from_unix(d * 86400 + 12 * 3600 + 34 * 60 + 56);
    CHECK(civil_is_valid(c));
    if (days_from_civil(c.year, c.month, c.day) != d) { CHECK(false); break; }
    if (unix_from_civil(c) != d * 86400 + 12 * 3600 + 34 * 60 + 56) { CHECK(false); break; }
    CHECK_EQ(d, prev_day_number + 1);
    prev_day_number = d;
  }
}

TEST(iso_format_and_parse) {
  char b[kIsoLen + 1];
  format_iso(1794578853, b);
  CHECK_STR(b, "2026-11-13T14:07:33Z");
  int64_t t = 0;
  CHECK(parse_iso("2026-11-13T14:07:33Z", &t));
  CHECK_EQ(t, 1794578853);
  CHECK(parse_iso("2026-11-13 14:07:33", &t));
  CHECK_EQ(t, 1794578853);
  CHECK(parse_iso("2026-11-20T00:00", &t));
  CHECK_EQ(t, 1795132800);
  CHECK(parse_iso("2028-02-29T23:59:00Z", &t));
  CHECK_EQ(t, 1835481540);
}

TEST(iso_parse_rejects_bad_input) {
  int64_t t = 7;
  CHECK(!parse_iso("", &t));
  CHECK(!parse_iso(nullptr, &t));
  CHECK(!parse_iso("2026-11-13", &t));
  CHECK(!parse_iso("2026-13-01T00:00:00Z", &t));   // month 13
  CHECK(!parse_iso("2026-02-29T00:00:00Z", &t));   // not a leap year
  CHECK(!parse_iso("2026-11-13T24:00:00Z", &t));
  CHECK(!parse_iso("2026-11-13T14:07:60Z", &t));
  CHECK(!parse_iso("2026-11-13T14:07:33+01:00", &t));  // offsets are not accepted: UTC only
  CHECK(!parse_iso("2026-11-13T14:07:33Zjunk", &t));
  CHECK(!parse_iso("2026-1a-13T14:07:33Z", &t));
  CHECK_EQ(t, 7);  // untouched on failure
}

TEST(minute_floor_handles_all_seconds) {
  for (int s = 0; s < 60; ++s) CHECK_EQ(minute_floor(1795132800 + s), 1795132800);
  CHECK_EQ(minute_floor(-1), -60);
}
