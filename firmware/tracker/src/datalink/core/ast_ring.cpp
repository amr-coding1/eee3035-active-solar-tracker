#include "ast_ring.h"

#include "ast_crc.h"

namespace ast {

RingBuffer::RingBuffer(Storage& storage, uint32_t capacity, uint32_t records_per_segment)
    : storage_(storage),
      capacity_(capacity == 0 ? 1 : capacity),
      per_segment_(records_per_segment == 0 ? 1 : records_per_segment),
      seg_buf_(static_cast<size_t>(per_segment_) * kRecordSize) {}

// Metadata layout (little-endian, kMetaSize = 44 bytes):
//   0 magic | 4 generation | 8 head | 12 tail | 16 ack | 20 next_seq
//   24 lost_total | 28 capacity | 32 flags (bit0 overflow pending)
//   36 last_minute | 40 CRC-32 of 0..39
bool RingBuffer::load_meta(uint16_t file_id, Meta* m) {
  uint8_t b[kMetaSize];
  if (!storage_.read(file_id, 0, b, kMetaSize)) return false;
  if (get_u32(b) != kMetaMagic) return false;
  if (get_u32(b + 40) != crc32(b, 40)) return false;
  if (get_u32(b + 28) != capacity_) return false;  // written for another buffer size
  m->generation = get_u32(b + 4);
  m->head = get_u32(b + 8);
  m->tail = get_u32(b + 12);
  m->ack = get_u32(b + 16);
  m->next_seq = get_u32(b + 20);
  m->lost_total = get_u32(b + 24);
  m->overflow_pending = (get_u32(b + 32) & 1u) != 0;
  m->last_minute = get_u32(b + 36);
  // Invariants: a copy that passes the CRC but breaks them is not trusted.
  if (m->tail > m->ack || m->ack > m->head) return false;
  if (m->head - m->tail > capacity_) return false;
  if (m->next_seq == 0) return false;
  return true;
}

bool RingBuffer::store_meta(const Meta& m) {
  uint8_t b[kMetaSize];
  put_u32(b, kMetaMagic);
  put_u32(b + 4, m.generation);
  put_u32(b + 8, m.head);
  put_u32(b + 12, m.tail);
  put_u32(b + 16, m.ack);
  put_u32(b + 20, m.next_seq);
  put_u32(b + 24, m.lost_total);
  put_u32(b + 28, capacity_);
  put_u32(b + 32, m.overflow_pending ? 1u : 0u);
  put_u32(b + 36, m.last_minute);
  put_u32(b + 40, crc32(b, 40));
  // Alternate between the two copies: the copy NOT being written always holds
  // the previous committed state.
  const uint16_t file = (m.generation & 1u) ? kFileMetaB : kFileMetaA;
  return storage_.write(file, 0, b, kMetaSize);
}

void RingBuffer::adopt(const Meta& m) {
  generation_ = m.generation;
  head_ = m.head;
  tail_ = m.tail;
  ack_ = m.ack;
  next_seq_ = m.next_seq;
  lost_total_ = m.lost_total;
  last_minute_ = m.last_minute;
  overflow_pending_ = m.overflow_pending;
}

RingBuffer::Meta RingBuffer::current() const {
  Meta m;
  m.generation = generation_;
  m.head = head_;
  m.tail = tail_;
  m.ack = ack_;
  m.next_seq = next_seq_;
  m.lost_total = lost_total_;
  m.last_minute = last_minute_;
  m.overflow_pending = overflow_pending_;
  return m;
}

RingBuffer::LoadResult RingBuffer::begin() {
  crc_errors_ = 0;
  Meta a{}, b{};
  const bool a_ok = load_meta(kFileMetaA, &a);
  const bool b_ok = load_meta(kFileMetaB, &b);
  if (a_ok || b_ok) {
    // Newest valid generation wins.
    if (a_ok && b_ok) adopt(a.generation > b.generation ? a : b);
    else adopt(a_ok ? a : b);
    return LoadResult::kLoaded;
  }
  Meta fresh{};
  fresh.next_seq = 1;
  adopt(fresh);
  if (recover_by_scan()) {
    Meta m = current();
    m.generation = generation_ + 1;
    if (store_meta(m)) generation_ = m.generation;
    return LoadResult::kRecoveredByScan;
  }
  return LoadResult::kFresh;
}

bool RingBuffer::recover_by_scan() {
  // Find the valid record with the highest seq: it was the newest one.
  bool found = false;
  uint32_t best_seq = 0, best_slot = 0, max_minute = 0;
  for (uint32_t seg_start = 0; seg_start < capacity_; seg_start += per_segment_) {
    uint32_t in_seg = per_segment_;
    if (in_seg > capacity_ - seg_start) in_seg = capacity_ - seg_start;
    const uint16_t file = static_cast<uint16_t>(seg_start / per_segment_);
    // Fast path: the whole segment in one storage call. A segment that was
    // never filled completely is shorter: fall back to record-by-record reads.
    const bool whole = storage_.read(file, 0, seg_buf_.data(), static_cast<size_t>(in_seg) * kRecordSize);
    for (uint32_t k = 0; k < in_seg; ++k) {
      Record r;
      uint8_t* raw = seg_buf_.data() + static_cast<size_t>(k) * kRecordSize;
      if (!whole && !storage_.read(file, k * static_cast<uint32_t>(kRecordSize), raw, kRecordSize)) break;
      if (!decode_record(raw, &r)) continue;
      if (r.minute_unix > max_minute) max_minute = r.minute_unix;
      if (!found || r.seq > best_seq) {
        found = true;
        best_seq = r.seq;
        best_slot = seg_start + k;
      }
    }
  }
  if (!found) return false;
  // Rebuild absolute cursors so that head % capacity is the slot after the
  // newest record. Walk backwards while seq keeps decreasing by exactly 1.
  head_ = capacity_ + best_slot + 1;
  uint32_t n = 1;
  while (n < capacity_) {
    Record r;
    if (!read_slot(head_ - 1 - n, &r) || r.seq != best_seq - n) break;
    ++n;
  }
  tail_ = head_ - n;
  // What had been uploaded is unknown: send everything again (readers
  // de-duplicate by seq) rather than risk losing data.
  ack_ = tail_;
  next_seq_ = best_seq + 1;
  last_minute_ = max_minute;
  lost_total_ = 0;
  overflow_pending_ = false;
  crc_errors_ = 0;
  return true;
}

bool RingBuffer::read_slot(uint32_t index, Record* out) {
  const uint32_t slot = index % capacity_;
  uint8_t raw[kRecordSize];
  const uint16_t file = static_cast<uint16_t>(slot / per_segment_);
  const uint32_t off = (slot % per_segment_) * static_cast<uint32_t>(kRecordSize);
  if (!storage_.read(file, off, raw, kRecordSize)) return false;
  return decode_record(raw, out);
}

bool RingBuffer::read_at(uint32_t index, Record* out) {
  if (index < tail_ || index >= head_) return false;
  return read_slot(index, out);
}

bool RingBuffer::clear() {
  Meta m = current();
  m.tail = m.ack = m.head;
  m.last_minute = 0;
  m.overflow_pending = false;
  m.generation += 1;
  if (!store_meta(m)) return false;
  adopt(m);
  return true;
}

RingBuffer::PushResult RingBuffer::push(Record rec, bool clear_overflow_flag, Record* stored) {
  if (rec.minute_unix <= last_minute_) return PushResult::kNotMonotonic;
  rec.seq = next_seq_;
  uint8_t raw[kRecordSize];
  encode_record(rec, raw);

  // 1) The record itself. Until the metadata below is committed the slot is
  //    outside [tail, head) (or is the oldest record being replaced), so a
  //    power cut here is harmless.
  const uint32_t slot = head_ % capacity_;
  const uint16_t file = static_cast<uint16_t>(slot / per_segment_);
  const uint32_t off = (slot % per_segment_) * static_cast<uint32_t>(kRecordSize);

  Meta m = current();
  bool lost_unsent = false;
  if (m.head - m.tail >= capacity_) {
    // Full: the slot we are about to write holds the oldest record.
    if (m.ack == m.tail) {          // it was never uploaded -> data loss
      m.ack = m.tail + 1;
      m.lost_total += 1;
      lost_unsent = true;
    }
    m.tail += 1;
    // The cursors must move BEFORE the old record is destroyed, otherwise a
    // power cut between the two writes would leave a slot inside [tail, head)
    // holding a record with the wrong seq. So commit the drop first.
    m.generation += 1;
    if (clear_overflow_flag) m.overflow_pending = false;
    if (lost_unsent) m.overflow_pending = true;
    if (!store_meta(m)) return PushResult::kStorageError;
    adopt(m);
    clear_overflow_flag = false;    // already applied
  }

  if (!storage_.write(file, off, raw, kRecordSize)) return PushResult::kStorageError;

  // 2) Commit: head and seq advance together in one metadata write.
  m.head += 1;
  m.next_seq += 1;
  m.last_minute = rec.minute_unix;
  m.generation += 1;
  if (clear_overflow_flag) m.overflow_pending = false;
  if (!store_meta(m)) return PushResult::kStorageError;
  adopt(m);
  if (stored != nullptr) *stored = rec;
  return PushResult::kOk;
}

size_t RingBuffer::peek_unacked(Record* out, size_t max, uint32_t* next_index) {
  size_t n = 0;
  uint32_t i = ack_;
  while (i < head_ && n < max) {
    // Read the run of slots that lies in one segment with a single storage call.
    const uint32_t slot = i % capacity_;
    const uint32_t in_seg = slot % per_segment_;
    uint32_t run = per_segment_ - in_seg;
    if (run > head_ - i) run = head_ - i;
    if (run > capacity_ - slot) run = capacity_ - slot;
    const uint16_t file = static_cast<uint16_t>(slot / per_segment_);
    const bool ok = storage_.read(file, in_seg * static_cast<uint32_t>(kRecordSize), seg_buf_.data(),
                                  static_cast<size_t>(run) * kRecordSize);
    uint32_t used = 0;
    for (; used < run && n < max; ++used) {
      Record r;
      if (ok && decode_record(seg_buf_.data() + static_cast<size_t>(used) * kRecordSize, &r)) {
        out[n++] = r;
      } else {
        ++crc_errors_;  // unreadable record: skipped, never blocks the queue
      }
    }
    i += used;
  }
  if (next_index != nullptr) *next_index = i;
  return n;
}

bool RingBuffer::ack_to(uint32_t index) {
  if (index > head_) index = head_;
  if (index <= ack_) return true;  // nothing new (e.g. overflow already moved ack past it)
  Meta m = current();
  m.ack = index;
  m.generation += 1;
  if (!store_meta(m)) return false;
  adopt(m);
  return true;
}

bool RingBuffer::ack_through_seq(uint32_t seq) {
  // Records in [tail, head) carry consecutive seq numbers ending at next_seq-1,
  // so the record with sequence number s sits at index head - (next_seq - s).
  if (seq >= next_seq_) return ack_to(head_);
  const uint32_t newer = next_seq_ - 1 - seq;  // records with a higher seq
  if (newer >= head_ - ack_) return true;      // seq is older than everything pending
  return ack_to(head_ - newer);
}

size_t RingBuffer::read_newest(Record* out, size_t max) {
  uint32_t n = count();
  if (n > max) n = static_cast<uint32_t>(max);
  size_t got = 0;
  for (uint32_t i = head_ - n; i < head_; ++i) {
    Record r;
    if (read_slot(i, &r)) out[got++] = r;
  }
  return got;
}

}  // namespace ast
