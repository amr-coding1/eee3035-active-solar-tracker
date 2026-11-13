#include "ast_uploader.h"

#include <ctype.h>

#include "ast_json.h"

namespace ast {

int backoff_seconds(int consecutive_failures) {
  const int n = static_cast<int>(sizeof(kBackoffS) / sizeof(kBackoffS[0]));
  if (consecutive_failures < 1) consecutive_failures = 1;
  if (consecutive_failures > n) consecutive_failures = n;
  return kBackoffS[consecutive_failures - 1];
}

bool upload_min_is_valid(int minutes) {
  return minutes >= kUploadMinLow && minutes <= kUploadMinHigh;
}

int64_t latest_slot(int64_t now, int upload_min) {
  if (!upload_min_is_valid(upload_min)) upload_min = kUploadMinDefault;
  // Slots are the UTC minutes whose index is a multiple of upload_min, at
  // second 20. For 15 min that is hh:00:20, hh:15:20, hh:30:20, hh:45:20.
  const int64_t minute_index = now / 60;
  int64_t slot = (minute_index - minute_index % upload_min) * 60 + kUploadSecond;
  if (slot > now) slot -= static_cast<int64_t>(upload_min) * 60;
  return slot;
}

static size_t skip_ws(const std::string& s, size_t i) {
  while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
  return i;
}

bool body_reports_success(const std::string& body) {
  // "success":true, tolerating white space around the colon.
  size_t pos = 0;
  while ((pos = body.find("\"success\"", pos)) != std::string::npos) {
    size_t i = skip_ws(body, pos + 9);
    if (i < body.size() && body[i] == ':') {
      i = skip_ws(body, i + 1);
      if (body.compare(i, 4, "true") == 0) return true;
    }
    pos += 9;
  }
  return false;
}

bool parse_last_field8(const std::string& body, uint32_t* seq) {
  const size_t pos = body.find("\"field8\"");
  if (pos == std::string::npos) return false;
  size_t i = skip_ws(body, pos + 8);
  if (i >= body.size() || body[i] != ':') return false;
  i = skip_ws(body, i + 1);
  if (i < body.size() && body[i] == '"') ++i;
  uint64_t v = 0;
  size_t digits = 0;
  while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
    v = v * 10 + static_cast<uint64_t>(body[i] - '0');
    if (v > 0xFFFFFFFFull) return false;
    ++i;
    ++digits;
  }
  if (digits == 0) return false;
  // Accept "123" and "123.0" style values, nothing else (e.g. not "12abc").
  if (i < body.size() && body[i] == '.') {
    ++i;
    while (i < body.size() && body[i] == '0') ++i;
  }
  if (i < body.size() && body[i] != '"' && body[i] != ',' && body[i] != '}' &&
      !isspace(static_cast<unsigned char>(body[i]))) {
    return false;
  }
  *seq = static_cast<uint32_t>(v);
  return true;
}

Uploader::Uploader(RecordSource& source, HttpTransport& transport, Clock& clock, EventSink* sink)
    : src_(source), http_(transport), clock_(clock), sink_(sink) {
  batch_.resize(kMaxBatch);
}

void Uploader::set_config(const UploaderConfig& cfg) {
  std::lock_guard<std::mutex> g(cfg_mu_);
  const int keep = cfg_.upload_min;
  cfg_ = cfg;
  if (!upload_min_is_valid(cfg_.upload_min)) cfg_.upload_min = keep;
  // Strip a trailing '/' so that base + "/channels/..." never has "//".
  while (!cfg_.base_url.empty() && cfg_.base_url.back() == '/') cfg_.base_url.pop_back();
}

UploaderConfig Uploader::config() {
  std::lock_guard<std::mutex> g(cfg_mu_);
  return cfg_;
}

bool Uploader::set_upload_min(int minutes) {
  if (!upload_min_is_valid(minutes)) return false;
  std::lock_guard<std::mutex> g(cfg_mu_);
  cfg_.upload_min = minutes;
  return true;
}

UploaderStats Uploader::stats() {
  std::lock_guard<std::mutex> g(cfg_mu_);
  return stats_;
}

void Uploader::emit(const Event& e) {
  if (sink_ != nullptr) sink_->on_event(e);
}

void Uploader::publish_stats(const UploaderConfig& cfg) {
  std::lock_guard<std::mutex> g(cfg_mu_);
  stats_.last_success = last_success_;
  stats_.last_attempt = last_attempt_;
  stats_.next_retry = fail_count_ > 0 ? next_retry_ : 0;
  stats_.fail_count = fail_count_;
  stats_.last_http_status = last_http_status_;
  stats_.in_doubt = in_doubt_;
  stats_.fault9 = fault9_.load();
  stats_.configured = !cfg.channel.empty() && !cfg.write_key.empty();
  stats_.batches_ok = batches_ok_;
  stats_.records_ok = records_ok_;
  stats_.failures = failures_;
  stats_.readback_skipped = readback_skipped_;
  stats_.dropped = dropped_;
  stats_.bisecting = rejecting_;
}

void Uploader::update_fault9(int64_t now, uint32_t pending, bool configured) {
  // "Failing" = at least one failed attempt since the last success, or no
  // channel/key configured at all (nothing can ever be uploaded).
  const bool failing = fail_count_ > 0 || rejecting_ || !configured;
  const bool f9 = pending > 0 && failing && (now - reference_) >= kFault9AfterS;
  const bool was = fault9_.exchange(f9);
  if (f9 && !was) {
    Event e;
    e.type = EventType::kFault;
    e.t = now;
    e.fault = kFaultUpload;
    e.pending = pending;
    e.reason = "upload_failing_60min";
    emit(e);
  }
}

void Uploader::on_failure(int64_t now, bool straight_to_cap) {
  ++fail_count_;
  const int cap_index = static_cast<int>(sizeof(kBackoffS) / sizeof(kBackoffS[0]));
  if (straight_to_cap && fail_count_ < cap_index) fail_count_ = cap_index;
  ++failures_;
  backlog_ = false;
  const int delay = backoff_seconds(fail_count_);
  next_retry_ = now + delay;
  Event e;
  e.type = EventType::kBackoff;
  e.t = now;
  e.delay_s = delay;
  e.count = static_cast<uint32_t>(fail_count_);
  e.pending = src_.pending();
  e.reason = "retry_scheduled";
  emit(e);
}

void Uploader::do_readback(const UploaderConfig& cfg) {
  // Optional (contract 7.3 "MAY"): ask the channel for the newest seq it holds.
  readback_done_ = true;  // at most one read-back per doubtful request
  const std::string url =
      cfg.base_url + "/channels/" + cfg.channel + "/fields/8/last.json?api_key=" + cfg.read_key;
  const HttpResult res = http_.get(url);
  const int64_t now = clock_.now();
  Event e;
  e.type = EventType::kReadback;
  e.t = now;
  e.http_status = res.outcome == HttpResult::kResponse ? res.status : 0;
  e.first_seq = doubt_first_seq_;
  e.reason = "readback_failed";
  uint32_t last = 0;
  if (res.outcome == HttpResult::kResponse && res.status == 200 && parse_last_field8(res.body, &last)) {
    e.last_seq = last;
    // Only trust a value that lies inside the doubtful batch. Anything else
    // (channel holds older data only, or seq from another device / an erased
    // buffer) means "send the whole batch again", which is always safe.
    if (last >= doubt_first_seq_ && last <= doubt_last_seq_) {
      if (src_.ack_through_seq(last)) {
        e.count = last - doubt_first_seq_ + 1;
        readback_skipped_ += e.count;
        e.reason = "skipped_already_stored";
        if (last == doubt_last_seq_) {
          // The whole batch had arrived: this was a successful upload after all.
          in_doubt_ = false;
          fail_count_ = 0;
          next_retry_ = 0;
          last_success_ = now;
          reference_ = now;
          last_slot_ = latest_slot(now, cfg.upload_min);
          records_ok_ += e.count;
          ++batches_ok_;
        }
      }
    } else {
      e.reason = "nothing_to_skip";
    }
  }
  e.pending = src_.pending();
  emit(e);
  if (!in_doubt_) {
    Event ok;
    ok.type = EventType::kUploadOk;
    ok.t = now;
    ok.count = doubt_last_seq_ - doubt_first_seq_ + 1;
    ok.first_seq = doubt_first_seq_;
    ok.last_seq = doubt_last_seq_;
    ok.http_status = last_http_status_;
    ok.pending = e.pending;
    ok.reason = "confirmed_by_readback";
    emit(ok);
  }
}

int64_t Uploader::due_time(int64_t now, const UploaderConfig& cfg) const {
  // Earliest moment the next request may start (<= now means "due").
  const int64_t spacing = last_post_end_ == 0 ? 0 : last_post_end_ + kBatchSpacingS;
  int64_t t;
  if (force_.load()) {
    t = spacing;                           // CLI "upload": as soon as the 16 s rule allows
  } else if (fail_count_ > 0) {
    t = next_retry_;                       // backoff 1, 2, 4, 8, 15 min
  } else if (backlog_ || rejecting_) {
    t = spacing;                           // next backlog / bisection batch >= 16 s after the previous
  } else {
    int64_t slot = latest_slot(now, cfg.upload_min);
    if (slot <= last_slot_) slot += static_cast<int64_t>(cfg.upload_min) * 60;  // already served: next one
    t = slot > spacing ? slot : spacing;
  }
  // Never sooner than 16 s after the previous request, whatever the branch above
  // chose (e.g. a bisection step after an earlier 5xx: next_retry_ is already in
  // the past there). Review finding 1, 13 Nov 2026.
  if (t < spacing) t = spacing;
  const bool want_readback = in_doubt_ && !readback_done_ && !cfg.read_key.empty();
  if (want_readback && t < doubt_time_ + kReadbackDelayS) t = doubt_time_ + kReadbackDelayS;
  return t;
}

int64_t Uploader::seconds_until_due() {
  const UploaderConfig cfg = config();
  const int64_t now = clock_.now();
  if (!started_ || cfg.channel.empty() || cfg.write_key.empty()) return kNeverDue;
  if (src_.pending() == 0) return kNeverDue;
  const int64_t t = due_time(now, cfg);
  return t <= now ? 0 : t - now;
}

Uploader::Tick Uploader::tick() {
  const UploaderConfig cfg = config();
  int64_t now = clock_.now();
  // Without a real date there is no schedule (the device is waiting for NTP).
  if (now < kMinPlausibleUnix) return Tick::kWaiting;
  if (!started_) {
    started_ = true;
    reference_ = now;
    last_slot_ = latest_slot(now, cfg.upload_min);  // wait for the next slot
  }
  const bool configured = !cfg.channel.empty() && !cfg.write_key.empty();
  uint32_t pending = src_.pending();
  update_fault9(now, pending, configured);

  if (pending == 0) {
    // Nothing to send: consume slots silently so that a stale one does not
    // fire the moment the next record appears.
    const int64_t slot = latest_slot(now, cfg.upload_min);
    if (slot > last_slot_) last_slot_ = slot;
    backlog_ = false;
    rejecting_ = false;
    batch_limit_ = kMaxBatch;
    force_.store(false);
    publish_stats(cfg);
    return Tick::kIdle;
  }
  if (!configured) {
    publish_stats(cfg);
    return Tick::kNotConfigured;
  }

  // ---- is an attempt due? -------------------------------------------------
  const bool want_readback = in_doubt_ && !readback_done_ && !cfg.read_key.empty();
  if (now < due_time(now, cfg)) {
    publish_stats(cfg);
    return Tick::kWaiting;
  }
  force_.store(false);
  last_attempt_ = now;
  last_slot_ = latest_slot(now, cfg.upload_min);   // this slot is being served now

  // ---- optional read-back for a doubtful batch ------------------------------
  if (want_readback) {
    do_readback(cfg);
    now = clock_.now();
    pending = src_.pending();
    if (!in_doubt_) {
      // The doubtful batch is confirmed stored. Anything logged since then
      // goes out with the next regular slot.
      update_fault9(now, pending, configured);
      publish_stats(cfg);
      return Tick::kUploaded;
    }
  }
  if (pending == 0) {
    publish_stats(cfg);
    return Tick::kIdle;
  }

  // ---- POST one batch --------------------------------------------------------
  uint32_t next_index = 0;
  const size_t n = src_.peek_unacked(batch_.data(), batch_limit_, &next_index);
  if (n == 0) {
    // Every pending slot examined was unreadable (CRC): step over them.
    src_.ack_to(next_index);
    publish_stats(cfg);
    return Tick::kIdle;
  }
  JsonBatch body(cfg.write_key, batch_.data(), n);
  const std::string url = cfg.base_url + "/channels/" + cfg.channel + "/bulk_update.json";
  const HttpResult res = http_.post_json(url, body);
  now = clock_.now();        // the request may have taken up to 30 s
  last_post_end_ = now;
  last_http_status_ = res.outcome == HttpResult::kResponse ? res.status : 0;

  Event e;
  e.t = now;
  e.count = static_cast<uint32_t>(n);
  e.first_seq = batch_[0].seq;
  e.last_seq = batch_[n - 1].seq;
  e.http_status = last_http_status_;
  e.pending = pending;

  Tick result = Tick::kFailed;
  const bool got = res.outcome == HttpResult::kResponse;
  const bool http_ok = got && (res.status == 200 || res.status == 202);
  if (http_ok && body_reports_success(res.body)) {
    // ---------------- success ----------------
    // If the ack cannot be persisted the batch is simply sent again later.
    src_.ack_to(next_index);
    in_doubt_ = false;
    fail_count_ = 0;
    next_retry_ = 0;
    drops_since_success_ = 0;
    last_success_ = now;
    reference_ = now;
    ++batches_ok_;
    records_ok_ += static_cast<uint32_t>(body.entries());
    last_slot_ = latest_slot(now, cfg.upload_min);
    pending = src_.pending();
    // More to send right away (>= 16 s later) when this was a full batch, or
    // when a rejected batch is being narrowed down (see below).
    const bool was_rejecting = rejecting_;
    backlog_ = pending > 0 && (n == kMaxBatch || was_rejecting);
    // After a rejection episode the batch size grows back (doubling on every
    // success, 16 s apart), so a backlog is not sent one record at a time once
    // the server accepts data again. Review finding 2, 13 Nov 2026.
    if (batch_limit_ < kMaxBatch) batch_limit_ = batch_limit_ * 2 < kMaxBatch ? batch_limit_ * 2 : kMaxBatch;
    if (batch_limit_ == kMaxBatch) rejecting_ = false;
    e.type = EventType::kUploadOk;
    e.pending = pending;
    e.reason = "ok";
    emit(e);
    // Records the batch builder had to leave out (timestamp not increasing)
    // are gone now: say so, with their seq.
    for (size_t i = 0; i < n; ++i) {
      if (!body.included(i)) {
        ++dropped_;
        Event d;
        d.type = EventType::kDrop;
        d.t = now;
        d.first_seq = d.last_seq = batch_[i].seq;
        d.minute_unix = batch_[i].minute_unix;
        d.count = 1;
        d.pending = pending;
        d.reason = "timestamp_not_increasing";
        emit(d);
      }
    }
    result = Tick::kUploaded;
  } else if (res.outcome == HttpResult::kNoResponse) {
    // ---------------- lost response: in doubt ----------------
    in_doubt_ = true;
    readback_done_ = false;
    doubt_time_ = now;
    doubt_first_seq_ = batch_[0].seq;
    doubt_last_seq_ = batch_[n - 1].seq;
    e.type = EventType::kInDoubt;
    e.reason = "no_response";
    emit(e);
    on_failure(now, false);
    result = Tick::kInDoubt;
  } else if (got && (res.status == 400 || http_ok)) {
    // ---------------- content rejected ("poison batch") ----------------
    // HTTP 400, or 200/202 without "success":true: the server understood the
    // request and refused what is in it. Retrying the same bytes can never
    // work, so halve the batch (16 s apart) until the offending record is
    // alone, drop only that one, and carry on.
    in_doubt_ = false;
    rejecting_ = true;
    backlog_ = false;
    e.type = EventType::kUploadFail;
    e.reason = "rejected_content";
    emit(e);
    if (n > 1) {
      batch_limit_ = (n + 1) / 2;
    } else if (drops_since_success_ < kMaxDropsWithoutSuccess) {
      if (src_.ack_to(next_index)) {
        ++drops_since_success_;
        ++dropped_;
        batch_limit_ = kMaxBatch;
        rejecting_ = false;
        backlog_ = src_.pending() > 0;   // continue with the rest 16 s later
        Event d;
        d.type = EventType::kDrop;
        d.t = now;
        d.first_seq = d.last_seq = batch_[0].seq;
        d.minute_unix = batch_[0].minute_unix;
        d.count = 1;
        d.http_status = last_http_status_;
        d.pending = src_.pending();
        d.reason = "rejected_by_server";
        emit(d);
      }
    } else {
      // Several single records in a row were refused and nothing was accepted
      // in between: this is not one bad record, the server refuses everything
      // (wrong channel settings?). Stop dropping, keep the data, retry slowly.
      on_failure(now, true);
    }
    result = Tick::kFailed;
  } else {
    // ---------------- definite failure, nothing stored ----------------
    in_doubt_ = false;
    e.type = EventType::kUploadFail;
    const bool keys_wrong = got && (res.status == 401 || res.status == 403 || res.status == 404);
    if (res.outcome == HttpResult::kNotSent) e.reason = "not_sent";
    else if (keys_wrong) e.reason = "keys_or_channel_wrong";
    else e.reason = "http_status";       // 429, 5xx, anything else: retry the same batch
    emit(e);
    // Wrong key / channel: no point hammering the server, go straight to the
    // 15 min cap (data stays in the buffer, fault 9 after 60 min as usual).
    on_failure(now, keys_wrong);
    result = Tick::kFailed;
  }
  update_fault9(now, src_.pending(), configured);
  publish_stats(cfg);
  return result;
}

}  // namespace ast
