// Upload state machine (contract 7.3) over an abstract HttpTransport.
//
//  IDLE --slot (every upload_min at hh:mm:20)--> POST batch (<= 960, oldest first)
//    success (HTTP 200/202 + "success":true)  -> ack; if the batch was full and
//                                                 more is waiting: next batch >= 16 s later
//    failure (not sent, HTTP 429/5xx/other)   -> records stay; retry after 1, 2, 4, 8, 15 min
//    no response (request sent, reply lost)   -> batch "in doubt"; same backoff, then:
//         with a READ key : GET fields/8/last.json (>= 30 s after the doubtful
//                           request); records of that batch with seq <= last
//                           are acknowledged without being sent again
//         always          : whatever is still pending is sent (duplicates are
//                           removed by the readers, contract 6.1)
//    content rejected (HTTP 400, or 200/202 without "success":true)
//                                             -> halve the batch and retry >= 16 s later until the
//                                                offending record is alone; drop only that record
//                                                (logged with its seq). At most 3 records are dropped
//                                                without a success in between; after that the data
//                                                is kept and retried every 15 min.
//    HTTP 401/403/404 (key or channel wrong)  -> records stay; straight to the 15 min retry cap
//  Fault 9: uploads are failing, records are pending and the last success (or
//  the start-up) is 60 min or more ago.
//
// tick() is called about once per second from the uploader task only; the
// small accessors are safe from other threads.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "ast_config.h"
#include "ast_http.h"
#include "ast_record.h"

namespace ast {

// The uploader's view of the ring buffer (implemented by DataLink with the
// shared mutex held inside each call, never across an HTTP request).
class RecordSource {
 public:
  virtual ~RecordSource() {}
  virtual uint32_t pending() = 0;
  virtual size_t peek_unacked(Record* out, size_t max, uint32_t* next_index) = 0;
  virtual bool ack_to(uint32_t next_index) = 0;
  virtual bool ack_through_seq(uint32_t seq) = 0;
};

struct UploaderConfig {
  std::string base_url = "https://api.thingspeak.com";
  std::string channel;     // TS_CH1
  std::string write_key;   // TS_WRITE_KEY
  std::string read_key;    // TS_READ_KEY, optional (enables the read-back)
  int upload_min = kUploadMinDefault;
};

struct UploaderStats {
  int64_t last_success = 0;      // 0 = none since boot
  int64_t last_attempt = 0;
  int64_t next_retry = 0;        // 0 = not in backoff
  int fail_count = 0;            // consecutive failures
  int last_http_status = 0;
  bool in_doubt = false;
  bool fault9 = false;
  bool configured = false;
  uint32_t batches_ok = 0;
  uint32_t records_ok = 0;
  uint32_t failures = 0;
  uint32_t readback_skipped = 0;
  uint32_t dropped = 0;          // records discarded (rejected by the server / bad timestamp)
  bool bisecting = false;        // a rejected batch is being narrowed down
};

constexpr int kMaxDropsWithoutSuccess = 3;

// Pure helpers (unit-tested on their own).
int backoff_seconds(int consecutive_failures);                  // 1 -> 60 ... >=5 -> 900
int64_t latest_slot(int64_t now_unix, int upload_min);          // newest slot time <= now
bool body_reports_success(const std::string& body);             // contains "success":true
bool parse_last_field8(const std::string& body, uint32_t* seq); // {"field8":"123"} -> 123
bool upload_min_is_valid(int minutes);

class Uploader {
 public:
  enum class Tick { kIdle, kWaiting, kNotConfigured, kUploaded, kFailed, kInDoubt };

  Uploader(RecordSource& source, HttpTransport& transport, Clock& clock, EventSink* sink = nullptr);

  void set_config(const UploaderConfig& cfg);   // any thread
  UploaderConfig config();                      // any thread
  bool set_upload_min(int minutes);             // any thread; false if outside 5..60
  void request_now() { force_.store(true); }    // CLI "upload"; any thread
  bool fault9() const { return fault9_.load(); }
  UploaderStats stats();                        // any thread

  Tick tick();                                  // uploader thread only

  // Energy saving (the radio is OFF between uploads): seconds until the next
  // request may start; 0 = due now; kNeverDue = nothing to send or not
  // configured. The adapter switches Wi-Fi on a few seconds before this
  // reaches 0 and off again when it is large. Uploader thread only.
  static constexpr int64_t kNeverDue = 0x7FFFFFFF;
  int64_t seconds_until_due();

 private:
  void on_failure(int64_t now, bool straight_to_cap);
  int64_t due_time(int64_t now, const UploaderConfig& cfg) const;
  void emit(const Event& e);
  void publish_stats(const UploaderConfig& cfg);
  void update_fault9(int64_t now, uint32_t pending, bool configured);
  void do_readback(const UploaderConfig& cfg);

  RecordSource& src_;
  HttpTransport& http_;
  Clock& clock_;
  EventSink* sink_;

  std::mutex cfg_mu_;        // protects cfg_ and stats_
  UploaderConfig cfg_;
  UploaderStats stats_;
  std::atomic<bool> force_{false};
  std::atomic<bool> fault9_{false};

  // --- state below is touched by tick() only ---
  std::vector<Record> batch_;
  bool started_ = false;
  int64_t reference_ = 0;      // last success, or the first tick after boot
  int64_t last_success_ = 0;
  int64_t last_attempt_ = 0;
  int64_t last_post_end_ = 0;  // 0 = no POST yet
  int64_t last_slot_ = 0;
  int64_t next_retry_ = 0;
  int fail_count_ = 0;
  bool backlog_ = false;
  bool in_doubt_ = false;
  bool readback_done_ = false;
  int64_t doubt_time_ = 0;
  uint32_t doubt_first_seq_ = 0, doubt_last_seq_ = 0;
  int last_http_status_ = 0;
  uint32_t batches_ok_ = 0, records_ok_ = 0, failures_ = 0, readback_skipped_ = 0, dropped_ = 0;
  // Poison-batch handling (see tick()).
  size_t batch_limit_ = kMaxBatch;
  bool rejecting_ = false;
  int drops_since_success_ = 0;
};

}  // namespace ast
