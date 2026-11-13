// Power-loss-safe ring buffer of Records over an abstract Storage (contract 7.2).
//
// Cursors are ABSOLUTE record counts (they only ever grow); the slot of a
// record is `index % capacity`:
//
//      tail <= ack <= head          head - tail <= capacity
//      [tail, ack)   uploaded and acknowledged, kept until overwritten
//      [ack, head)   waiting for upload ("pending", "unsent")
//
// Metadata (cursors + next seq) is written alternately to two copies, each
// with a generation number and a CRC-32. A write that is cut short by a power
// loss damages at most the copy being written; the other copy still holds the
// previous consistent state, so at worst the last operation is undone:
//   - undone push: the record was never visible, its seq is used again for
//     the next sample (it was never uploaded, so seq stays unique);
//   - undone ack : the batch is uploaded again; readers de-duplicate by seq
//     (contract 6.1).
// If BOTH copies are unreadable the buffer is rebuilt by scanning every slot
// (highest seq wins) so that seq is still never reused.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "ast_config.h"
#include "ast_record.h"
#include "ast_storage.h"

namespace ast {

constexpr size_t kMetaSize = 44;
constexpr uint32_t kMetaMagic = 0x4D545341u;  // "ASTM" little-endian

class RingBuffer {
 public:
  enum class LoadResult {
    kLoaded,          // a valid metadata copy was found
    kFresh,           // nothing on the medium: new empty buffer, seq starts at 1
    kRecoveredByScan  // both metadata copies bad: rebuilt from the records
  };

  explicit RingBuffer(Storage& storage, uint32_t capacity = kRingCapacity,
                      uint32_t records_per_segment = kRecordsPerSegment);

  LoadResult begin();

  // Appends a record. `rec.seq` is assigned here (next_seq, then +1).
  // If the buffer is full the oldest record is dropped; when that record had
  // not been uploaded yet, the loss is counted and the overflow flag is set
  // (fault 6 on the NEXT sample). `clear_overflow_flag` = this record reports
  // the earlier overflow, so the flag may be cleared in the same commit.
  // The minute of every record must be later than the minute of the record
  // logged before it (persisted in the metadata as last_minute): ThingSpeak
  // rejects a whole bulk update that contains a duplicate timestamp, so a
  // clock that steps back must never produce a second record for a minute.
  enum class PushResult {
    kOk,
    kStorageError,   // nothing has changed
    kNotMonotonic    // rec.minute_unix <= last_minute(): refused, nothing has changed
  };
  PushResult push(Record rec, bool clear_overflow_flag, Record* stored = nullptr);

  // Operator command `buffer clear yes`: forgets every stored record and the
  // last-minute guard. seq is NOT reset (never reused). False on storage error.
  bool clear();

  // Copies up to `max` pending records (oldest first) into `out`. Records that
  // fail their CRC are skipped and counted. `*next_index` is the absolute index
  // just after the last slot examined: pass it to ack_to() after a successful
  // upload. Returns the number of records copied.
  size_t peek_unacked(Record* out, size_t max, uint32_t* next_index);

  // Moves the ack cursor forward to `index` (clamped to [ack, head]) and
  // persists it. Returns false when storage failed (ack unchanged).
  bool ack_to(uint32_t index);
  // Acknowledges every pending record with seq <= `seq`. Returns false on a
  // storage failure.
  bool ack_through_seq(uint32_t seq);

  // Newest `max` records in chronological order (for `buffer dump` / CSV).
  size_t read_newest(Record* out, size_t max);
  bool read_at(uint32_t index, Record* out);

  uint32_t capacity() const { return capacity_; }
  uint32_t head() const { return head_; }
  uint32_t tail() const { return tail_; }
  uint32_t ack() const { return ack_; }
  uint32_t count() const { return head_ - tail_; }
  uint32_t pending() const { return head_ - ack_; }
  uint32_t next_seq() const { return next_seq_; }
  uint32_t last_minute() const { return last_minute_; }      // minute_unix of the newest record ever logged
  uint32_t lost_total() const { return lost_total_; }        // unsent records overwritten, ever
  bool overflow_pending() const { return overflow_pending_; }
  uint32_t crc_errors() const { return crc_errors_; }        // since boot
  uint32_t generation() const { return generation_; }

 private:
  struct Meta {
    uint32_t generation, head, tail, ack, next_seq, lost_total, last_minute;
    bool overflow_pending;
  };
  bool load_meta(uint16_t file_id, Meta* m);
  bool store_meta(const Meta& m);
  void adopt(const Meta& m);
  Meta current() const;
  bool read_slot(uint32_t index, Record* out);
  bool recover_by_scan();

  Storage& storage_;
  const uint32_t capacity_;
  const uint32_t per_segment_;
  uint32_t generation_ = 0;
  uint32_t head_ = 0, tail_ = 0, ack_ = 0;
  uint32_t next_seq_ = 1;
  uint32_t lost_total_ = 0;
  uint32_t last_minute_ = 0;
  bool overflow_pending_ = false;
  uint32_t crc_errors_ = 0;
  std::vector<uint8_t> seg_buf_;  // one segment, so a batch needs few storage calls
};

}  // namespace ast
