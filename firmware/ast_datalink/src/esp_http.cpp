#include "esp_http.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include "core/ast_config.h"

namespace ast_esp {

namespace {

// Presents an ast::BodyReader as an Arduino Stream for HTTPClient::sendRequest().
class BodyStream : public Stream {
 public:
  explicit BodyStream(ast::BodyReader& body) : body_(body), left_(body.size()) { body_.rewind(); }
  int available() override { return left_ > 0x7FFF ? 0x7FFF : static_cast<int>(left_); }
  int read() override {
    uint8_t b;
    return readBytes(reinterpret_cast<char*>(&b), 1) == 1 ? b : -1;
  }
  int peek() override { return -1; }   // not used by HTTPClient
  size_t readBytes(char* buffer, size_t length) override {
    const size_t n = body_.read(reinterpret_cast<uint8_t*>(buffer), length);
    left_ -= n < left_ ? n : left_;
    return n;
  }
  size_t write(uint8_t) override { return 0; }   // read-only stream
  void flush() override {}

 private:
  ast::BodyReader& body_;
  size_t left_;
};

bool wait_for_link(uint32_t max_ms) {
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start >= max_ms) return false;
    delay(100);
  }
  return true;
}

}  // namespace

ast::HttpResult EspHttp::request(bool is_post, const std::string& url, ast::BodyReader* body) {
  ast::HttpResult res;   // kNotSent
  const uint32_t t0 = millis();
  // Radio switched off (energy saving) or still associating: wait a little, then give up.
  if (WiFi.getMode() == WIFI_OFF || !wait_for_link(wait_link_ms)) {
    last_error = -100;   // no Wi-Fi
    return res;
  }

  const bool tls = url.compare(0, 8, "https://") == 0;
  NetworkClientSecure secure;
  NetworkClient plain;     // plain http:// is only for a mock server on the bench LAN
  HTTPClient http;
  if (tls) {
    // The server certificate is ALWAYS verified (never setInsecure()).
    if (pinned_root_ != nullptr) secure.setCACert(pinned_root_);
    else secure.useBuiltinCACertBundle();
    secure.setHandshakeTimeout(20);                       // seconds
  }
  http.setReuse(false);
  http.setConnectTimeout(10000);                          // TCP connect
  http.setTimeout(static_cast<uint16_t>(ast::kHttpTimeoutMs));   // 30 s for the response (7.4)
  const bool begun = tls ? http.begin(secure, url.c_str()) : http.begin(plain, url.c_str());
  if (!begun) {
    last_error = -101;   // URL not understood
    return res;
  }
  http.setUserAgent("ast-datalink/1.5 (ESP32-S3)");

  int code;
  if (is_post) {
    http.addHeader("Content-Type", "application/json");
    BodyStream stream(*body);
    code = http.sendRequest("POST", &stream, body->size());
  } else {
    code = http.GET();
  }
  last_error = code;

  if (code > 0) {
    res.outcome = ast::HttpResult::kResponse;
    res.status = code;
    // ThingSpeak answers are tiny ({"success":true}); never buffer a big page.
    const int size = http.getSize();
    if (size < 0 || size <= 4096) {
      const String payload = http.getString();
      res.body.assign(payload.c_str(), payload.length() > 512 ? 512 : payload.length());
    }
  } else if (code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_SEND_HEADER_FAILED ||
             code == HTTPC_ERROR_SEND_PAYLOAD_FAILED || code == HTTPC_ERROR_NOT_CONNECTED) {
    // Connect/TLS failed or the request could not be sent completely: the
    // server has not processed it.
    res.outcome = ast::HttpResult::kNotSent;
  } else {
    // Request went out, no usable answer (timeout, connection lost ...):
    // the batch may or may not be stored -> "in doubt".
    res.outcome = ast::HttpResult::kNoResponse;
  }
  http.end();
  last_duration_ms = millis() - t0;
  return res;
}

ast::HttpResult EspHttp::post_json(const std::string& url, ast::BodyReader& body) {
  return request(true, url, &body);
}

ast::HttpResult EspHttp::get(const std::string& url) { return request(false, url, nullptr); }

}  // namespace ast_esp
