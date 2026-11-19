// DataLink: the whole device-side behaviour of contract section 7 in one
// portable object. The ESP32 sketch and the PC "ast_host_device" both run
// exactly this code; only Storage, HttpTransport, Clock and the sensor
// readings differ.
//
// Threads (contract 7.4):
//   sampler thread  : sampler_tick() once per second
//   uploader thread : uploader_tick() about once per second
//   CLI thread      : setters and status()
// The ring buffer is protected by one mutex that is held only for short
// flash operations and NEVER across an HTTP request, so an upload (up to
// 30 s) cannot delay sampling.
#pragma once
#include <stdint.h>

#include <mutex>

#include "ast_aggregator.h"
#include "ast_http.h"
#include "ast_ring.h"
#include "ast_sim.h"
#include "ast_timevalid.h"
#include "ast_uploader.h"

namespace ast {

struct DataLinkStatus {
  bool time_valid = false;
  bool rtc_missing = false;
  TimeValidity::Source time_source = TimeValidity::Source::kNone;
  bool sim_on = false;
  Scenario scenario = Scenario::kClear;
  uint16_t run = 0;
  char panel = 'A';
  // ring buffer
  uint32_t capacity = 0, count = 0, pending = 0, head = 0, tail = 0, ack = 0;
  uint32_t next_seq = 1, lost_total = 0, crc_errors = 0, last_minute = 0;
  bool overflow_pending = false;
  // counters since boot
  uint32_t logged = 0, storage_errors = 0, skipped_minutes = 0, negative_clamps = 0;
  uint8_t current_fault = 0;     // code the next sample would carry (control code not included)
  bool has_last = false;
  Record last;                   // newest logged record
  UploaderStats upload;
};

class DataLink : public RecordSource {
 public:
  DataLink(Storage& storage, HttpTransport& transport, Clock& clock, EventSink* sink = nullptr,
           uint32_t capacity = kRingCapacity, uint32_t records_per_segment = kRecordsPerSegment);

  // Loads the ring buffer from storage. Call once before the tasks start.
  RingBuffer::LoadResult begin();

  // ---- time validity (7.1); call from the task that owns RTC/NTP -------------
  void time_begin(bool rtc_present, bool rtc_lost_flag, int64_t rtc_unix);
  bool time_on_ntp(int64_t ntp_unix, bool rtc_present, int64_t rtc_unix);  // true: write the RTC
  void time_on_manual_set(bool rtc_present);
  bool time_valid();

  // ---- settings (any thread) --------------------------------------------------------
  void set_run(uint16_t run);
  void set_panel(char panel);
  uint16_t run();
  char panel();
  void set_sim(bool on);
  void set_sim_config(const SimConfig& cfg);
  bool sim_on();
  Uploader& uploader() { return uploader_; }

  // ---- sampler thread ------------------------------------------------------------------
  // `now` = UTC unix seconds of this sample; `real` = the sensor readings
  // (ignored completely while the simulator is on: real and simulated data
  // are never mixed inside a sample). Returns true when a minute record was
  // stored; it is copied to *logged when given.
  bool sampler_tick(int64_t now, const Sample1Hz& real, Record* logged = nullptr);
  // Stores the minute in progress if it is complete at `now` (clean shutdown).
  bool flush(int64_t now, Record* logged = nullptr);

  // ---- uploader thread -----------------------------------------------------------------
  Uploader::Tick uploader_tick();

  // ---- CLI ---------------------------------------------------------------------------------
  DataLinkStatus status();
  size_t dump_newest(Record* out, size_t max);
  bool clear_buffer();

  // ---- RecordSource (used by the Uploader; each call takes the mutex) --------
  uint32_t pending() override;
  size_t peek_unacked(Record* out, size_t max, uint32_t* next_index) override;
  bool ack_to(uint32_t next_index) override;
  bool ack_through_seq(uint32_t seq) override;

 private:
  bool store_record(Record rec, int64_t now, Record* logged);  // mu_ held by caller
  uint8_t datalink_fault_locked(uint16_t record_flags, uint8_t control_code, bool* used_clock,
                                bool* used_overflow);
  void emit(const Event& e);

  std::mutex mu_;                // protects everything below except uploader_
  RingBuffer ring_;
  Aggregator agg_;
  Simulator sim_;
  TimeValidity time_;
  EventSink* sink_;
  Uploader uploader_;

  bool sim_on_ = false;
  uint16_t run_ = 0;
  char panel_ = 'A';
  bool clock_fault_latched_ = false;    // time was invalid: first sample afterwards carries e=4
  bool clock_fault_announced_ = false;
  bool storage_fault_latched_ = false;  // a record could not be stored: next sample carries e=6
  uint32_t logged_ = 0, storage_errors_ = 0, skipped_minutes_ = 0;
  bool has_last_ = false;
  Record last_;
};

}  // namespace ast
