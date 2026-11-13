#include "ast_datalink.h"

#include "ast_status.h"

namespace ast {

DataLink::DataLink(Storage& storage, HttpTransport& transport, Clock& clock, EventSink* sink,
                   uint32_t capacity, uint32_t records_per_segment)
    : ring_(storage, capacity, records_per_segment),
      sink_(sink),
      uploader_(*this, transport, clock, sink) {}

void DataLink::emit(const Event& e) {
  if (sink_ != nullptr) sink_->on_event(e);
}

RingBuffer::LoadResult DataLink::begin() {
  std::lock_guard<std::mutex> g(mu_);
  const RingBuffer::LoadResult r = ring_.begin();
  Record newest;
  has_last_ = ring_.read_newest(&newest, 1) == 1;
  if (has_last_) last_ = newest;
  return r;
}

// ---- time ---------------------------------------------------------------------
void DataLink::time_begin(bool rtc_present, bool rtc_lost_flag, int64_t rtc_unix) {
  std::lock_guard<std::mutex> g(mu_);
  time_.begin(rtc_present, rtc_lost_flag, rtc_unix);
}
bool DataLink::time_on_ntp(int64_t ntp_unix, bool rtc_present, int64_t rtc_unix) {
  std::lock_guard<std::mutex> g(mu_);
  return time_.on_ntp(ntp_unix, rtc_present, rtc_unix);
}
void DataLink::time_on_manual_set(bool rtc_present) {
  std::lock_guard<std::mutex> g(mu_);
  time_.on_manual_set(rtc_present);
}
bool DataLink::time_valid() {
  std::lock_guard<std::mutex> g(mu_);
  return time_.valid();
}

// ---- settings -------------------------------------------------------------------
void DataLink::set_run(uint16_t run) {
  std::lock_guard<std::mutex> g(mu_);
  run_ = run > kRunMax ? kRunMax : run;
}
void DataLink::set_panel(char panel) {
  std::lock_guard<std::mutex> g(mu_);
  if (panel == 'A' || panel == 'a') panel_ = 'A';
  else if (panel == 'B' || panel == 'b') panel_ = 'B';
}
uint16_t DataLink::run() { std::lock_guard<std::mutex> g(mu_); return run_; }
char DataLink::panel() { std::lock_guard<std::mutex> g(mu_); return panel_; }
void DataLink::set_sim(bool on) { std::lock_guard<std::mutex> g(mu_); sim_on_ = on; }
void DataLink::set_sim_config(const SimConfig& cfg) { std::lock_guard<std::mutex> g(mu_); sim_.configure(cfg); }
bool DataLink::sim_on() { std::lock_guard<std::mutex> g(mu_); return sim_on_; }

// ---- sampling ---------------------------------------------------------------------
uint8_t DataLink::datalink_fault_locked(uint16_t record_flags, uint8_t control_code,
                                        bool* used_clock, bool* used_overflow) {
  const bool clock_fault = clock_fault_latched_ || time_.clock_fault();
  const bool overflow_fault = ring_.overflow_pending() || storage_fault_latched_;
  const uint8_t code = select_fault(control_code, sensor_fault_from_flags(record_flags), clock_fault,
                                    overflow_fault, uploader_.fault9());
  *used_clock = (code == kFaultClock);
  *used_overflow = (code == kFaultOverflow);
  return code;
}

bool DataLink::store_record(Record rec, int64_t now, Record* logged) {
  // (1) One record per UTC minute, strictly increasing (contract 3.1; a
  // duplicate timestamp makes ThingSpeak reject the whole bulk update). The
  // newest logged minute is persisted with the cursors, so this also holds
  // after a reboot inside the same minute or an RTC/NTP correction backwards.
  if (rec.minute_unix <= ring_.last_minute()) {
    ++skipped_minutes_;
    Event e;
    e.type = EventType::kSkip;
    e.t = now;
    e.minute_unix = rec.minute_unix;
    e.pending = ring_.pending();
    e.reason = "minute_not_after_last_logged";
    emit(e);
    return false;
  }
  rec.run = run_;
  if (panel_ == 'B') rec.flags |= kPanelB; else rec.flags = static_cast<uint16_t>(rec.flags & ~kPanelB);

  // (2) One fault code per sample (section 4). The latched data-link faults
  // (4 after a period without valid time, 6 after an overflow) are cleared
  // only when a stored sample actually carried them, so they are never lost
  // behind a control-system code.
  bool used_clock = false, used_overflow = false;
  rec.fault = datalink_fault_locked(rec.flags, rec.fault, &used_clock, &used_overflow);

  const uint32_t lost_before = ring_.lost_total();
  Record stored;
  const RingBuffer::PushResult pr = ring_.push(rec, used_overflow, &stored);
  if (pr != RingBuffer::PushResult::kOk) {
    ++storage_errors_;
    storage_fault_latched_ = true;   // data lost: visible as e=6 on the next stored sample
    Event e;
    e.type = EventType::kFault;
    e.t = now;
    e.fault = kFaultOverflow;
    e.minute_unix = rec.minute_unix;
    e.reason = "storage_write_failed";
    emit(e);
    return false;
  }
  if (used_clock) clock_fault_latched_ = false;
  if (used_overflow) storage_fault_latched_ = false;
  ++logged_;
  has_last_ = true;
  last_ = stored;
  if (logged != nullptr) *logged = stored;

  Event e;
  e.type = EventType::kSample;
  e.t = now;
  e.record = &stored;
  e.first_seq = e.last_seq = stored.seq;
  e.minute_unix = stored.minute_unix;
  e.pending = ring_.pending();
  e.fault = stored.fault;
  emit(e);
  if (ring_.lost_total() != lost_before) {
    Event f;
    f.type = EventType::kFault;
    f.t = now;
    f.fault = kFaultOverflow;
    f.count = ring_.lost_total();
    f.pending = ring_.pending();
    f.reason = "buffer_overflow_oldest_unsent_overwritten";
    emit(f);
  }
  return true;
}

bool DataLink::sampler_tick(int64_t now, const Sample1Hz& real, Record* logged) {
  std::lock_guard<std::mutex> g(mu_);
  if (!time_.valid()) {
    // 7.1: without valid time nothing is logged, and fault 4 is raised.
    agg_.reset();
    clock_fault_latched_ = true;
    if (!clock_fault_announced_) {
      clock_fault_announced_ = true;
      Event e;
      e.type = EventType::kFault;
      e.t = now;
      e.fault = kFaultClock;
      e.reason = "time_not_valid_no_logging";
      emit(e);
    }
    return false;
  }
  clock_fault_announced_ = false;
  Sample1Hz s = sim_on_ ? sim_.step(now) : real;
  s.simulated = sim_on_;   // the marker follows the SOURCE of the data, nothing else
  Record rec;
  if (!agg_.add(now, s, &rec)) return false;
  return store_record(rec, now, logged);
}

bool DataLink::flush(int64_t now, Record* logged) {
  std::lock_guard<std::mutex> g(mu_);
  Record rec;
  if (!time_.valid() || !agg_.flush(now, &rec)) return false;
  return store_record(rec, now, logged);
}

// ---- upload -----------------------------------------------------------------------
Uploader::Tick DataLink::uploader_tick() { return uploader_.tick(); }

uint32_t DataLink::pending() {
  std::lock_guard<std::mutex> g(mu_);
  return ring_.pending();
}
size_t DataLink::peek_unacked(Record* out, size_t max, uint32_t* next_index) {
  std::lock_guard<std::mutex> g(mu_);
  return ring_.peek_unacked(out, max, next_index);
}
bool DataLink::ack_to(uint32_t next_index) {
  std::lock_guard<std::mutex> g(mu_);
  return ring_.ack_to(next_index);
}
bool DataLink::ack_through_seq(uint32_t seq) {
  std::lock_guard<std::mutex> g(mu_);
  return ring_.ack_through_seq(seq);
}

// ---- CLI ------------------------------------------------------------------------------
size_t DataLink::dump_newest(Record* out, size_t max) {
  std::lock_guard<std::mutex> g(mu_);
  return ring_.read_newest(out, max);
}

bool DataLink::clear_buffer() {
  std::lock_guard<std::mutex> g(mu_);
  agg_.reset();
  return ring_.clear();
}

DataLinkStatus DataLink::status() {
  DataLinkStatus s;
  {
    std::lock_guard<std::mutex> g(mu_);
    s.time_valid = time_.valid();
    s.rtc_missing = time_.rtc_missing();
    s.time_source = time_.source();
    s.sim_on = sim_on_;
    s.scenario = sim_.config().scenario;
    s.run = run_;
    s.panel = panel_;
    s.capacity = ring_.capacity();
    s.count = ring_.count();
    s.pending = ring_.pending();
    s.head = ring_.head();
    s.tail = ring_.tail();
    s.ack = ring_.ack();
    s.next_seq = ring_.next_seq();
    s.lost_total = ring_.lost_total();
    s.crc_errors = ring_.crc_errors();
    s.last_minute = ring_.last_minute();
    s.overflow_pending = ring_.overflow_pending();
    s.logged = logged_;
    s.storage_errors = storage_errors_;
    s.skipped_minutes = skipped_minutes_;
    s.negative_clamps = agg_.negative_clamps();
    bool a = false, b = false;
    s.current_fault = datalink_fault_locked(kHasPtrk | kHasPfix | kHasPuse, 0, &a, &b);
    s.has_last = has_last_;
    s.last = last_;
  }
  s.upload = uploader_.stats();   // has its own lock
  return s;
}

}  // namespace ast
