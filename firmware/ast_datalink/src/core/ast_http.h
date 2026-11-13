// Abstract interfaces between the portable upload logic and the outside
// world: HTTP transport, wall clock, request body, event log.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>

#include "ast_record.h"

namespace ast {

// A request body that can be produced piece by piece, so that a 960-entry
// batch (about 150 kB of JSON) never has to exist in RAM at once on the ESP32.
class BodyReader {
 public:
  virtual ~BodyReader() {}
  virtual size_t size() = 0;                           // total bytes (Content-Length)
  virtual size_t read(uint8_t* buf, size_t max) = 0;   // next bytes; 0 at the end
  virtual void rewind() = 0;
};

struct HttpResult {
  enum Outcome {
    kNotSent,     // no connection / TLS failed / request could not be sent: the server has NOT seen it
    kNoResponse,  // request went out, no (complete) response came back: outcome unknown ("in doubt")
    kResponse     // an HTTP response was received: see status and body
  };
  Outcome outcome = kNotSent;
  int status = 0;
  std::string body;  // adapters keep at most the first 512 bytes
};

class HttpTransport {
 public:
  virtual ~HttpTransport() {}
  // POST with "Content-Type: application/json". Must return within about
  // kHttpTimeoutMs (contract 7.4).
  virtual HttpResult post_json(const std::string& url, BodyReader& body) = 0;
  virtual HttpResult get(const std::string& url) = 0;
};

class Clock {
 public:
  virtual ~Clock() {}
  virtual int64_t now() = 0;  // UTC, unix seconds
};

// Events for the serial console (ESP32) and the JSON-lines log (PC).
enum class EventType {
  kSample, kUploadOk, kUploadFail, kInDoubt, kFault, kBackoff, kReadback,
  kSkip,   // a minute was NOT logged (clock stepped back); Event::minute_unix says which
  kDrop    // a stored record was discarded without upload (first_seq = its seq, reason says why)
};

struct Event {
  EventType type = EventType::kSample;
  int64_t t = 0;                 // UTC unix seconds
  const Record* record = nullptr;  // kSample only (valid during the call)
  int http_status = 0;           // upload events
  uint32_t count = 0;            // records in the batch / records skipped (kReadback)
  uint32_t first_seq = 0, last_seq = 0;
  uint32_t pending = 0;          // records still waiting after the event
  int32_t delay_s = 0;           // kBackoff
  uint8_t fault = 0;             // kFault
  uint32_t minute_unix = 0;      // kSkip / kDrop: the minute concerned
  const char* reason = "";       // short machine-readable word
};

class EventSink {
 public:
  virtual ~EventSink() {}
  // May be called from the sampler and the uploader thread; must be thread-safe.
  virtual void on_event(const Event& e) = 0;
};

}  // namespace ast
