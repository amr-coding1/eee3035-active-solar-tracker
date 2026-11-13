// Record codec and ring buffer (contract 7.2).
#include "ast_crc.h"
#include "ast_ring.h"
#include "test_framework.h"
#include "test_helpers.h"

using namespace ast;
using ast_test::kT0;

static Record make(uint32_t minute_index, uint32_t p = 1234) {
  Record r;
  r.minute_unix = static_cast<uint32_t>(kT0) + 60u * minute_index;
  r.p_trk_dmw = p;
  r.p_fix_dmw = p / 2;
  r.p_use_dmw = 1200;
  r.ang_panel_ddeg = -123;
  r.ang_sun_ddeg = -120;
  r.temp_dc = 215;
  r.vbat_mv = 3912;
  r.run = 12;
  r.flags = kHasPtrk | kHasPfix | kHasPuse | kHasAngPanel | kHasAngSun | kHasTemp | kHasVbat;
  r.state = State::TRK;
  return r;
}

static bool pushed(RingBuffer& ring, uint32_t minute_index) {
  return ring.push(make(minute_index), false) == RingBuffer::PushResult::kOk;
}

TEST(record_round_trip_all_fields) {
  Record r = make(5);
  r.seq = 4000000000u;
  r.flags |= kSimulated | kPanelB;
  r.state = State::MOV;
  r.fault = 9;
  r.ang_panel_ddeg = -600;
  r.temp_dc = -105;
  uint8_t raw[kRecordSize];
  encode_record(r, raw);
  Record back;
  CHECK(decode_record(raw, &back));
  CHECK(back == r);
  CHECK_EQ(back.panel(), 'B');
  CHECK_EQ(kRecordSize, static_cast<size_t>(40));
}

TEST(record_layout_is_fixed_little_endian) {
  Record r = make(0);
  r.seq = 0x01020304u;
  uint8_t raw[kRecordSize];
  encode_record(r, raw);
  CHECK_EQ(raw[0], 0xA5);
  CHECK_EQ(raw[1], 1);
  CHECK_EQ(raw[4], 0x04);
  CHECK_EQ(raw[7], 0x01);
  CHECK_EQ(get_u32(raw + 8), static_cast<uint32_t>(kT0));
  CHECK_EQ(get_u32(raw + 36), crc32(raw, 36));
}

TEST(record_any_corrupted_byte_is_rejected) {
  Record r = make(3);
  r.seq = 77;
  uint8_t raw[kRecordSize];
  encode_record(r, raw);
  for (size_t i = 0; i < kRecordSize; ++i) {
    uint8_t bad[kRecordSize];
    memcpy(bad, raw, kRecordSize);
    bad[i] = static_cast<uint8_t>(bad[i] ^ 0x40);
    Record out;
    out.seq = 999;
    CHECK(!decode_record(bad, &out));
    CHECK_EQ(out.seq, 999u);  // output untouched
  }
}

TEST(record_erased_flash_is_not_a_record) {
  uint8_t ff[kRecordSize], zero[kRecordSize];
  memset(ff, 0xFF, sizeof ff);
  memset(zero, 0, sizeof zero);
  Record out;
  CHECK(!decode_record(ff, &out));
  CHECK(!decode_record(zero, &out));
}

TEST(record_out_of_range_values_rejected_even_with_good_crc) {
  Record r = make(1);
  r.seq = 5;
  uint8_t raw[kRecordSize];
  encode_record(r, raw);
  raw[34] = 6;  // state beyond FLT
  put_u32(raw + 36, crc32(raw, 36));
  Record out;
  CHECK(!decode_record(raw, &out));
  encode_record(r, raw);
  raw[35] = 10;  // fault code > 9
  put_u32(raw + 36, crc32(raw, 36));
  CHECK(!decode_record(raw, &out));
  encode_record(r, raw);
  put_u32(raw + 8, static_cast<uint32_t>(kT0) + 7);  // not on a minute
  put_u32(raw + 36, crc32(raw, 36));
  CHECK(!decode_record(raw, &out));
}

TEST(state_names) {
  CHECK_STR(state_name(State::TRK), "TRK");
  CHECK_STR(state_name(State::FLT), "FLT");
  State s;
  CHECK(state_from_name("STW", &s));
  CHECK(s == State::STW);
  CHECK(!state_from_name("XYZ", &s));
}

TEST(ring_fresh_starts_empty_with_seq_1) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  CHECK(ring.begin() == RingBuffer::LoadResult::kFresh);
  CHECK_EQ(ring.count(), 0u);
  CHECK_EQ(ring.pending(), 0u);
  CHECK_EQ(ring.next_seq(), 1u);
  CHECK_EQ(kRingCapacity, 11520u);  // contract: at least 8 days at 1/min
  CHECK(static_cast<size_t>(kRecordsPerSegment) * kRecordSize <= 4096);  // one flash block
}

TEST(ring_push_assigns_consecutive_seq_and_peek_returns_oldest_first) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  for (uint32_t i = 0; i < 6; ++i) {
    Record stored;
    CHECK(ring.push(make(i, 100 + i), false, &stored) == RingBuffer::PushResult::kOk);
    CHECK_EQ(stored.seq, i + 1);
  }
  Record out[10];
  uint32_t next = 0;
  CHECK_EQ(ring.peek_unacked(out, 10, &next), static_cast<size_t>(6));
  CHECK_EQ(next, 6u);
  for (uint32_t i = 0; i < 6; ++i) {
    CHECK_EQ(out[i].seq, i + 1);
    CHECK_EQ(out[i].p_trk_dmw, 100 + i);
  }
  // A limited peek returns the oldest ones.
  CHECK_EQ(ring.peek_unacked(out, 2, &next), static_cast<size_t>(2));
  CHECK_EQ(next, 2u);
  CHECK_EQ(out[1].seq, 2u);
}

TEST(ring_ack_moves_cursor_and_is_clamped) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  for (uint32_t i = 0; i < 5; ++i) pushed(ring, i);
  CHECK(ring.ack_to(3));
  CHECK_EQ(ring.pending(), 2u);
  CHECK(ring.ack_to(1));          // going backwards is ignored
  CHECK_EQ(ring.ack(), 3u);
  CHECK(ring.ack_to(99));         // beyond head: clamped
  CHECK_EQ(ring.pending(), 0u);
  CHECK_EQ(ring.count(), 5u);     // acknowledged records are kept until overwritten
}

TEST(ring_ack_through_seq) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  for (uint32_t i = 0; i < 8; ++i) pushed(ring, i);   // seq 1..8
  CHECK(ring.ack_through_seq(3));
  CHECK_EQ(ring.pending(), 5u);
  Record out[10];
  uint32_t next;
  ring.peek_unacked(out, 10, &next);
  CHECK_EQ(out[0].seq, 4u);
  CHECK(ring.ack_through_seq(2));   // older than everything pending: no change
  CHECK_EQ(ring.pending(), 5u);
  CHECK(ring.ack_through_seq(1000));  // newer than everything: all acknowledged
  CHECK_EQ(ring.pending(), 0u);
}

TEST(ring_wraps_many_times_without_loss_when_acked) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  uint32_t expect_seq = 1;
  for (uint32_t i = 0; i < 95; ++i) {
    CHECK(pushed(ring, i));
    if (i % 3 == 2) {  // upload every 3 records
      Record out[10];
      uint32_t next;
      const size_t n = ring.peek_unacked(out, 10, &next);
      CHECK_EQ(n, static_cast<size_t>(3));
      for (size_t k = 0; k < n; ++k) CHECK_EQ(out[k].seq, expect_seq++);
      CHECK(ring.ack_to(next));
    }
  }
  CHECK_EQ(ring.lost_total(), 0u);
  CHECK(!ring.overflow_pending());
  CHECK_EQ(ring.count(), 10u);
  CHECK_EQ(ring.next_seq(), 96u);
}

TEST(ring_overflow_overwrites_oldest_unsent_and_flags_fault) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  for (uint32_t i = 0; i < 10; ++i) CHECK(pushed(ring, i));
  CHECK_EQ(ring.pending(), 10u);
  CHECK(!ring.overflow_pending());
  CHECK(pushed(ring, 10));                // 11th record: seq 1 is lost
  CHECK(ring.overflow_pending());
  CHECK_EQ(ring.lost_total(), 1u);
  CHECK_EQ(ring.pending(), 10u);
  Record out[10];
  uint32_t next;
  CHECK_EQ(ring.peek_unacked(out, 10, &next), static_cast<size_t>(10));
  CHECK_EQ(out[0].seq, 2u);               // oldest surviving
  CHECK_EQ(out[9].seq, 11u);
}

TEST(ring_overflow_of_already_sent_record_is_not_a_loss) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  for (uint32_t i = 0; i < 10; ++i) pushed(ring, i);
  ring.ack_to(4);                          // seq 1..4 uploaded
  for (uint32_t i = 10; i < 14; ++i) CHECK(pushed(ring, i));  // overwrites the 4 sent ones
  CHECK_EQ(ring.lost_total(), 0u);
  CHECK(!ring.overflow_pending());
  CHECK(pushed(ring, 14));                 // now an unsent one goes
  CHECK_EQ(ring.lost_total(), 1u);
  CHECK(ring.overflow_pending());
}

TEST(ring_overflow_flag_cleared_only_when_reported) {
  MemStorage st;
  RingBuffer ring(st, 4, 4);
  ring.begin();
  for (uint32_t i = 0; i < 5; ++i) pushed(ring, i);
  CHECK(ring.overflow_pending());
  ring.ack_to(ring.head());                // everything uploaded
  CHECK(ring.push(make(5), false) == RingBuffer::PushResult::kOk);
  CHECK(ring.overflow_pending());          // sample did not report it: still pending
  CHECK(ring.push(make(6), true) == RingBuffer::PushResult::kOk);
  CHECK(!ring.overflow_pending());
}

TEST(ring_continuous_overflow_keeps_flag_set) {
  MemStorage st;
  RingBuffer ring(st, 4, 4);
  ring.begin();
  for (uint32_t i = 0; i < 4; ++i) pushed(ring, i);
  for (uint32_t i = 4; i < 9; ++i) {
    CHECK(ring.push(make(i), true) == RingBuffer::PushResult::kOk);  // reports the previous loss, causes a new one
    CHECK(ring.overflow_pending());
  }
  CHECK_EQ(ring.lost_total(), 5u);
}

TEST(ring_state_survives_restart) {
  MemStorage st;
  {
    RingBuffer ring(st, 10, 4);
    ring.begin();
    for (uint32_t i = 0; i < 7; ++i) pushed(ring, i);
    ring.ack_to(3);
  }
  RingBuffer ring(st, 10, 4);
  CHECK(ring.begin() == RingBuffer::LoadResult::kLoaded);
  CHECK_EQ(ring.count(), 7u);
  CHECK_EQ(ring.pending(), 4u);
  CHECK_EQ(ring.next_seq(), 8u);
  CHECK_EQ(ring.last_minute(), static_cast<uint32_t>(kT0) + 6 * 60);
  Record stored;
  CHECK(ring.push(make(7), false, &stored) == RingBuffer::PushResult::kOk);
  CHECK_EQ(stored.seq, 8u);                // seq continues, never reused
}

TEST(ring_seq_persists_across_many_restarts_and_wraps) {
  MemStorage st;
  uint32_t expect = 1;
  for (int boot = 0; boot < 12; ++boot) {
    RingBuffer ring(st, 10, 4);
    ring.begin();
    for (int i = 0; i < 7; ++i) {
      Record stored;
      CHECK(ring.push(make(expect), false, &stored) == RingBuffer::PushResult::kOk);
      CHECK_EQ(stored.seq, expect);
      ++expect;
    }
    ring.ack_to(ring.head());
  }
}

TEST(ring_metadata_alternates_between_two_copies) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  pushed(ring, 0);
  const std::vector<uint8_t> a1 = st.files[kFileMetaA], b1 = st.files[kFileMetaB];
  pushed(ring, 1);
  const std::vector<uint8_t> a2 = st.files[kFileMetaA], b2 = st.files[kFileMetaB];
  CHECK((a1 != a2) != (b1 != b2));         // exactly one copy changed
  pushed(ring, 2);
  const std::vector<uint8_t> a3 = st.files[kFileMetaA], b3 = st.files[kFileMetaB];
  CHECK((a2 != a3) != (b2 != b3));
  CHECK((a1 != a2) != (a2 != a3));         // and it was the other one this time
  CHECK_EQ(a3.size(), kMetaSize);
}

TEST(ring_one_corrupt_metadata_copy_falls_back_to_the_other) {
  for (int which = 0; which < 2; ++which) {
    MemStorage st;
    {
      RingBuffer ring(st, 10, 4);
      ring.begin();
      for (uint32_t i = 0; i < 6; ++i) pushed(ring, i);
    }
    // Find the newest copy and destroy it.
    RingBuffer probe(st, 10, 4);
    probe.begin();
    const uint16_t newest = (probe.generation() & 1u) ? kFileMetaB : kFileMetaA;
    const uint16_t victim = which == 0 ? newest : (newest == kFileMetaA ? kFileMetaB : kFileMetaA);
    st.files[victim][10] ^= 0xFF;
    RingBuffer ring(st, 10, 4);
    CHECK(ring.begin() == RingBuffer::LoadResult::kLoaded);
    if (victim == newest) {
      CHECK_EQ(ring.count(), 5u);          // last push undone, nothing else
      CHECK_EQ(ring.next_seq(), 6u);
    } else {
      CHECK_EQ(ring.count(), 6u);
      CHECK_EQ(ring.next_seq(), 7u);
    }
  }
}

TEST(ring_torn_metadata_write_at_every_length_recovers_previous_state) {
  // Power fails during the metadata write of the 4th push, after 0..43 bytes.
  for (size_t torn = 0; torn < kMetaSize; ++torn) {
    MemStorage st;
    RingBuffer ring(st, 10, 4);
    ring.begin();
    for (uint32_t i = 0; i < 3; ++i) pushed(ring, i);
    st.fail_after_writes = 1;              // record write succeeds, metadata write is torn
    st.torn_bytes = torn;
    CHECK(ring.push(make(3), false) == RingBuffer::PushResult::kStorageError);
    st.power_restored();
    RingBuffer again(st, 10, 4);
    CHECK(again.begin() == RingBuffer::LoadResult::kLoaded);
    CHECK_EQ(again.count(), 3u);
    CHECK_EQ(again.next_seq(), 4u);
    Record stored;
    CHECK(again.push(make(3), false, &stored) == RingBuffer::PushResult::kOk);
    CHECK_EQ(stored.seq, 4u);
    Record out[10];
    uint32_t next;
    CHECK_EQ(again.peek_unacked(out, 10, &next), static_cast<size_t>(4));
    for (uint32_t i = 0; i < 4; ++i) CHECK_EQ(out[i].seq, i + 1);
  }
}

TEST(ring_torn_record_write_is_invisible) {
  for (size_t torn = 0; torn < kRecordSize; torn += 7) {
    MemStorage st;
    RingBuffer ring(st, 10, 4);
    ring.begin();
    for (uint32_t i = 0; i < 3; ++i) pushed(ring, i);
    st.fail_after_writes = 0;              // the record write itself is torn
    st.torn_bytes = torn;
    CHECK(ring.push(make(3), false) == RingBuffer::PushResult::kStorageError);
    CHECK_EQ(ring.count(), 3u);            // RAM state unchanged as well
    st.power_restored();
    RingBuffer again(st, 10, 4);
    again.begin();
    CHECK_EQ(again.count(), 3u);
    CHECK_EQ(again.next_seq(), 4u);
  }
}

TEST(ring_power_loss_at_every_write_of_a_long_run_never_breaks_invariants) {
  // Cut the power at the k-th storage write, restart, and check that what is
  // readable is always a clean run of consecutive seq with no duplicates.
  for (int cut = 0; cut < 80; ++cut) {
    MemStorage st;
    uint32_t minute = 0;
    {
      RingBuffer ring(st, 6, 4);
      ring.begin();
      st.fail_after_writes = cut;
      st.torn_bytes = static_cast<size_t>(cut % 40);
      for (int i = 0; i < 30; ++i) {
        ring.push(make(minute++), ring.overflow_pending());
        if (i % 4 == 3) {
          Record out[6];
          uint32_t next;
          ring.peek_unacked(out, 3, &next);
          ring.ack_to(next);
        }
      }
    }
    st.power_restored();
    RingBuffer ring(st, 6, 4);
    const RingBuffer::LoadResult lr = ring.begin();
    // A cut during the very first writes leaves no committed metadata at all:
    // then the buffer is new (or rebuilt from the one record that got through).
    if (cut >= 2) CHECK(lr == RingBuffer::LoadResult::kLoaded);
    CHECK(ring.count() <= 6u);
    CHECK(ring.pending() <= ring.count());
    Record out[6];
    const size_t n = ring.read_newest(out, 6);
    CHECK_EQ(n, static_cast<size_t>(ring.count()));     // every slot in range decodes
    for (size_t i = 1; i < n; ++i) {
      CHECK_EQ(out[i].seq, out[i - 1].seq + 1);
      CHECK(out[i].minute_unix > out[i - 1].minute_unix);
    }
    if (n > 0) CHECK_EQ(out[n - 1].seq + 1, ring.next_seq());
    Record stored;
    CHECK(ring.push(make(minute + 100), false, &stored) == RingBuffer::PushResult::kOk);
    CHECK_EQ(stored.seq + 1, ring.next_seq());
  }
}

TEST(ring_both_metadata_copies_lost_rebuilds_from_records) {
  MemStorage st;
  {
    RingBuffer ring(st, 10, 4);
    ring.begin();
    for (uint32_t i = 0; i < 27; ++i) pushed(ring, i);   // wrapped: seq 18..27 present
    ring.ack_to(ring.head());
  }
  st.files.erase(kFileMetaA);
  st.files[kFileMetaB].assign(kMetaSize, 0xFF);
  RingBuffer ring(st, 10, 4);
  CHECK(ring.begin() == RingBuffer::LoadResult::kRecoveredByScan);
  CHECK_EQ(ring.next_seq(), 28u);          // seq is still never reused
  CHECK_EQ(ring.count(), 10u);
  CHECK_EQ(ring.pending(), 10u);           // upload state unknown: send again (de-duplicated by seq)
  CHECK_EQ(ring.last_minute(), static_cast<uint32_t>(kT0) + 26 * 60);
  Record out[10];
  uint32_t next;
  CHECK_EQ(ring.peek_unacked(out, 10, &next), static_cast<size_t>(10));
  for (uint32_t i = 0; i < 10; ++i) CHECK_EQ(out[i].seq, 18 + i);
  // The rebuilt state was written back: a second restart loads it normally.
  RingBuffer again(st, 10, 4);
  CHECK(again.begin() == RingBuffer::LoadResult::kLoaded);
  CHECK_EQ(again.next_seq(), 28u);
}

TEST(ring_scan_recovery_stops_at_a_gap) {
  MemStorage st;
  {
    RingBuffer ring(st, 10, 4);
    ring.begin();
    for (uint32_t i = 0; i < 8; ++i) pushed(ring, i);
  }
  st.files.erase(kFileMetaA);
  st.files.erase(kFileMetaB);
  st.files[1][0] ^= 0xFF;                  // slot 4 (seq 5) unreadable
  RingBuffer ring(st, 10, 4);
  CHECK(ring.begin() == RingBuffer::LoadResult::kRecoveredByScan);
  CHECK_EQ(ring.next_seq(), 9u);
  CHECK_EQ(ring.count(), 3u);              // seq 6, 7, 8
}

TEST(ring_metadata_with_good_crc_but_broken_invariants_is_rejected) {
  MemStorage st;
  {
    RingBuffer ring(st, 10, 4);
    ring.begin();
    pushed(ring, 0);
  }
  // Rewrite copy A with ack > head and a valid CRC.
  uint8_t b[kMetaSize];
  put_u32(b, kMetaMagic);
  put_u32(b + 4, 1000);  // very new generation
  put_u32(b + 8, 5);     // head
  put_u32(b + 12, 0);    // tail
  put_u32(b + 16, 9);    // ack > head
  put_u32(b + 20, 6);
  put_u32(b + 24, 0);
  put_u32(b + 28, 10);
  put_u32(b + 32, 0);
  put_u32(b + 36, 0);
  put_u32(b + 40, crc32(b, 40));
  st.files[kFileMetaA].assign(b, b + kMetaSize);
  RingBuffer ring(st, 10, 4);
  ring.begin();
  CHECK_EQ(ring.count(), 1u);              // the other (sane) copy was used
  CHECK_EQ(ring.next_seq(), 2u);
}

TEST(ring_metadata_for_other_capacity_is_ignored) {
  MemStorage st;
  {
    RingBuffer ring(st, 8, 4);
    ring.begin();
    for (uint32_t i = 0; i < 3; ++i) pushed(ring, i);
  }
  RingBuffer ring(st, 12, 4);
  CHECK(ring.begin() == RingBuffer::LoadResult::kRecoveredByScan);
  CHECK_EQ(ring.next_seq(), 4u);
}

TEST(ring_corrupt_pending_record_is_skipped_not_blocking) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  for (uint32_t i = 0; i < 6; ++i) pushed(ring, i);
  st.files[0][2 * kRecordSize + 12] ^= 0x01;   // damage seq 3
  Record out[10];
  uint32_t next;
  CHECK_EQ(ring.peek_unacked(out, 10, &next), static_cast<size_t>(5));
  CHECK_EQ(next, 6u);
  CHECK_EQ(out[1].seq, 2u);
  CHECK_EQ(out[2].seq, 4u);
  CHECK_EQ(ring.crc_errors(), 1u);
  CHECK(ring.ack_to(next));
  CHECK_EQ(ring.pending(), 0u);
}

TEST(ring_rejects_minute_not_after_last_logged) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  CHECK(pushed(ring, 5));
  CHECK(ring.push(make(5), false) == RingBuffer::PushResult::kNotMonotonic);   // same minute
  CHECK(ring.push(make(4), false) == RingBuffer::PushResult::kNotMonotonic);   // earlier
  CHECK_EQ(ring.count(), 1u);
  CHECK_EQ(ring.next_seq(), 2u);           // no seq consumed
  // ... also after a restart (the last minute is persisted) ...
  RingBuffer again(st, 10, 4);
  again.begin();
  CHECK(again.push(make(5), false) == RingBuffer::PushResult::kNotMonotonic);
  CHECK(again.push(make(6), false) == RingBuffer::PushResult::kOk);
  // ... and even when every record has been uploaded and overwritten.
  again.ack_to(again.head());
  CHECK(again.push(make(6), false) == RingBuffer::PushResult::kNotMonotonic);
}

TEST(ring_clear_keeps_seq_and_allows_earlier_time) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  for (uint32_t i = 10; i < 15; ++i) pushed(ring, i);
  CHECK(ring.clear());
  CHECK_EQ(ring.count(), 0u);
  CHECK_EQ(ring.pending(), 0u);
  CHECK_EQ(ring.next_seq(), 6u);
  Record stored;
  CHECK(ring.push(make(2), false, &stored) == RingBuffer::PushResult::kOk);
  CHECK_EQ(stored.seq, 6u);
}

TEST(ring_full_size_buffer_holds_8_days_and_peeks_960) {
  MemStorage st;
  RingBuffer ring(st);
  ring.begin();
  for (uint32_t i = 0; i < kRingCapacity + 5; ++i) {
    if (ring.push(make(i, i), false) != RingBuffer::PushResult::kOk) { CHECK(false); break; }
  }
  CHECK_EQ(ring.count(), 11520u);
  CHECK_EQ(ring.lost_total(), 5u);
  std::vector<Record> out(kMaxBatch);
  uint32_t next;
  CHECK_EQ(ring.peek_unacked(out.data(), kMaxBatch, &next), kMaxBatch);
  CHECK_EQ(out[0].seq, 6u);
  CHECK_EQ(out[959].seq, 965u);
  CHECK_EQ(st.files.size(), static_cast<size_t>(kSegments + 2));
  CHECK_EQ(st.files[0].size(), static_cast<size_t>(3840));
}

TEST(ring_storage_failure_changes_nothing) {
  MemStorage st;
  RingBuffer ring(st, 10, 4);
  ring.begin();
  pushed(ring, 0);
  st.fail_all_writes = true;
  CHECK(ring.push(make(1), false) == RingBuffer::PushResult::kStorageError);
  CHECK(!ring.ack_to(1));
  CHECK_EQ(ring.count(), 1u);
  CHECK_EQ(ring.pending(), 1u);
  st.power_restored();
  CHECK(pushed(ring, 1));
  CHECK_EQ(ring.count(), 2u);
}
