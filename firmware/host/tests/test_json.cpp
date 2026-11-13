// Exact bulk-update JSON (contract 7.3 / 3.1).
#include "ast_json.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ast;
using ast_test::count_of;
using ast_test::kT0;

static Record full(uint32_t seq, uint32_t minute_index) {
  Record r;
  r.seq = seq;
  r.minute_unix = static_cast<uint32_t>(kT0) + 60 * minute_index;
  r.p_trk_dmw = 12345;       // 1234.5 mW
  r.p_fix_dmw = 9870;        // 987.0
  r.p_use_dmw = 1203;        // 120.3
  r.ang_panel_ddeg = -305;   // -30.5
  r.ang_sun_ddeg = -298;     // -29.8
  r.temp_dc = 187;           // 18.7
  r.vbat_mv = 3912;          // 3.912
  r.run = 12;
  r.flags = kHasPtrk | kHasPfix | kHasPuse | kHasAngPanel | kHasAngSun | kHasTemp | kHasVbat;
  r.state = State::TRK;
  return r;
}

static std::string entry(const Record& r) {
  char b[kJsonEntryMax];
  format_entry(r, b, sizeof b);
  return b;
}

TEST(json_entry_exact_text_all_fields) {
  CHECK_STR(entry(full(17, 0)),
            "{\"created_at\":\"2026-11-20 00:00:00 +0000\",\"field1\":1234.5,\"field2\":987.0,"
            "\"field3\":120.3,\"field4\":-30.5,\"field5\":-29.8,\"field6\":18.7,\"field7\":3.912,"
            "\"field8\":17,\"status\":\"TRK;r=12;p=A;e=0\"}");
}

TEST(json_unknown_values_are_omitted_never_zero) {
  Record r = full(18, 1);
  r.flags = static_cast<uint16_t>(r.flags & ~(kHasAngSun | kHasTemp));
  CHECK_STR(entry(r),
            "{\"created_at\":\"2026-11-20 00:01:00 +0000\",\"field1\":1234.5,\"field2\":987.0,"
            "\"field3\":120.3,\"field4\":-30.5,\"field7\":3.912,\"field8\":18,"
            "\"status\":\"TRK;r=12;p=A;e=0\"}");
  r.flags = 0;   // nothing known at all: only time, seq and status remain
  r.fault = 1;
  r.state = State::FLT;
  CHECK_STR(entry(r),
            "{\"created_at\":\"2026-11-20 00:01:00 +0000\",\"field8\":18,\"status\":\"FLT;r=12;p=A;e=1\"}");
}

TEST(json_number_formats) {
  char b[24];
  format_tenths(0, b, sizeof b);       CHECK_STR(b, "0.0");
  format_tenths(5, b, sizeof b);       CHECK_STR(b, "0.5");
  format_tenths(-5, b, sizeof b);      CHECK_STR(b, "-0.5");     // sign kept below 1
  format_tenths(10, b, sizeof b);      CHECK_STR(b, "1.0");      // always one decimal
  format_tenths(-600, b, sizeof b);    CHECK_STR(b, "-60.0");
  format_tenths(2000000000, b, sizeof b); CHECK_STR(b, "200000000.0");
  format_milli(3912, b, sizeof b);     CHECK_STR(b, "3.912");
  format_milli(4000, b, sizeof b);     CHECK_STR(b, "4.000");    // always three decimals
  format_milli(7, b, sizeof b);        CHECK_STR(b, "0.007");
  format_milli(65535, b, sizeof b);    CHECK_STR(b, "65.535");
  char tiny[3];
  CHECK_EQ(format_tenths(12345, tiny, sizeof tiny), static_cast<size_t>(0));
}

TEST(json_zero_power_is_written_as_zero_when_known) {
  Record r = full(1, 0);
  r.p_trk_dmw = 0;
  CHECK(entry(r).find("\"field1\":0.0,") != std::string::npos);
}

TEST(json_simulated_marker_and_panel_in_status) {
  Record r = full(3, 0);
  r.flags |= kSimulated | kPanelB;
  r.state = State::MOV;
  CHECK(entry(r).find("\"status\":\"MOV;r=12;p=B;e=0;s=1\"}") != std::string::npos);
}

TEST(json_batch_exact_text_two_entries) {
  Record recs[2] = {full(1, 0), full(2, 1)};
  recs[1].flags = kHasPtrk;
  JsonBatch b("WRITEKEY1001", recs, 2);
  CHECK_STR(b.to_string(),
            "{\"write_api_key\":\"WRITEKEY1001\",\"updates\":["
            "{\"created_at\":\"2026-11-20 00:00:00 +0000\",\"field1\":1234.5,\"field2\":987.0,"
            "\"field3\":120.3,\"field4\":-30.5,\"field5\":-29.8,\"field6\":18.7,\"field7\":3.912,"
            "\"field8\":1,\"status\":\"TRK;r=12;p=A;e=0\"},"
            "{\"created_at\":\"2026-11-20 00:01:00 +0000\",\"field1\":1234.5,\"field8\":2,"
            "\"status\":\"TRK;r=12;p=A;e=0\"}]}");
  CHECK_EQ(b.size(), b.to_string().size());
  CHECK_EQ(b.entries(), static_cast<size_t>(2));
}

TEST(json_batch_has_no_spaces_outside_strings_and_no_newlines) {
  Record recs[3] = {full(1, 0), full(2, 1), full(3, 2)};
  JsonBatch b("K", recs, 3);
  const std::string s = b.to_string();
  CHECK_EQ(count_of(s, "\n"), static_cast<size_t>(0));
  // The only spaces are the two inside each created_at value.
  CHECK_EQ(count_of(s, " "), static_cast<size_t>(6));
  CHECK_EQ(count_of(s, ":00 +0000\""), static_cast<size_t>(3));
}

TEST(json_batch_streamed_in_any_chunk_size_equals_whole_text) {
  std::vector<Record> recs;
  for (uint32_t i = 0; i < 40; ++i) {
    Record r = full(i + 1, i);
    if (i % 3 == 0) r.flags = static_cast<uint16_t>(r.flags & ~kHasAngSun);
    if (i % 5 == 0) r.flags |= kSimulated;
    recs.push_back(r);
  }
  JsonBatch b("ABCDEFGHIJKLMNOP", recs.data(), recs.size());
  const std::string whole = b.to_string();
  const size_t sizes[] = {1, 2, 7, 64, 255, 256, 257, 1000, 100000};
  for (size_t chunk : sizes) {
    b.rewind();
    std::string got;
    std::vector<uint8_t> buf(chunk);
    size_t n;
    while ((n = b.read(buf.data(), chunk)) > 0) got.append(reinterpret_cast<const char*>(buf.data()), n);
    CHECK(got == whole);
    CHECK_EQ(b.read(buf.data(), chunk), static_cast<size_t>(0));  // stays at the end
  }
  CHECK_EQ(b.size(), whole.size());
}

TEST(json_batch_is_capped_at_960_entries) {
  std::vector<Record> recs;
  for (uint32_t i = 0; i < 1000; ++i) recs.push_back(full(i + 1, i));
  JsonBatch b("K", recs.data(), recs.size());
  CHECK_EQ(b.count(), static_cast<size_t>(960));
  const std::string s = b.to_string();
  CHECK_EQ(count_of(s, "\"created_at\""), static_cast<size_t>(960));
  CHECK(s.find("\"field8\":960,") != std::string::npos);
  CHECK(s.find("\"field8\":961,") == std::string::npos);
  CHECK(s.size() < 220u * 1024u);          // stays a modest request
  CHECK_STR(s.substr(s.size() - 4), "\"}]}");
}

TEST(json_batch_empty_is_still_valid_json) {
  JsonBatch b("K", nullptr, 0);
  CHECK_STR(b.to_string(), "{\"write_api_key\":\"K\",\"updates\":[]}");
  CHECK_EQ(b.size(), static_cast<size_t>(34));
}

TEST(json_key_is_escaped) {
  CHECK_STR(json_escape("a\"b\\c"), "a\\\"b\\\\c");
  CHECK_STR(json_escape(std::string("x\ny")), "x\\u000ay");
  JsonBatch b("we\"ird", nullptr, 0);
  CHECK_STR(b.to_string(), "{\"write_api_key\":\"we\\\"ird\",\"updates\":[]}");
}

TEST(json_entry_longest_case_fits_buffer) {
  Record r = full(4294967295u, 0);
  r.p_trk_dmw = r.p_fix_dmw = r.p_use_dmw = 2000000000u;
  r.ang_panel_ddeg = r.ang_sun_ddeg = r.temp_dc = -32768;
  r.vbat_mv = 65535;
  r.run = 999;
  r.fault = 9;
  r.state = State::MOV;
  r.flags |= kSimulated | kPanelB;
  const std::string s = entry(r);
  CHECK(!s.empty());
  CHECK(s.size() < kJsonEntryMax);
  CHECK(s.find("\"field8\":4294967295,") != std::string::npos);
  CHECK(s.find("\"field4\":-3276.8,") != std::string::npos);
  char small[64];
  CHECK_EQ(format_entry(r, small, sizeof small), static_cast<size_t>(0));  // too small: refused, not truncated
}

TEST(json_batch_skips_duplicate_and_backward_timestamps) {
  // ThingSpeak rejects the WHOLE update if two entries share a timestamp.
  Record recs[6] = {full(1, 0), full(2, 1), full(3, 1) /* same minute */, full(4, 0) /* earlier */,
                    full(5, 2), full(6, 3)};
  JsonBatch b("K", recs, 6);
  const std::string s = b.to_string();
  CHECK_EQ(b.entries(), static_cast<size_t>(4));
  CHECK(b.included(0) && b.included(1) && !b.included(2) && !b.included(3) && b.included(4) && b.included(5));
  CHECK_EQ(count_of(s, "\"created_at\""), static_cast<size_t>(4));
  CHECK(s.find("\"field8\":3,") == std::string::npos);
  CHECK(s.find("\"field8\":4,") == std::string::npos);
  CHECK_EQ(count_of(s, "},{"), static_cast<size_t>(3));     // commas still exactly between entries
  CHECK_EQ(count_of(s, ",,"), static_cast<size_t>(0));
  CHECK_EQ(count_of(s, "[,"), static_cast<size_t>(0));
  CHECK_EQ(b.size(), s.size());
}

TEST(json_batch_after_minute_can_exclude_the_first_record) {
  Record recs[3] = {full(1, 0), full(2, 1), full(3, 2)};
  JsonBatch b("K", recs, 3, static_cast<uint32_t>(kT0));     // minute 0 already sent
  const std::string s = b.to_string();
  CHECK(!b.included(0));
  CHECK_EQ(b.entries(), static_cast<size_t>(2));
  CHECK(s.find("\"updates\":[{\"created_at\":\"2026-11-20 00:01:00 +0000\"") != std::string::npos);
  CHECK_EQ(b.size(), s.size());
}
