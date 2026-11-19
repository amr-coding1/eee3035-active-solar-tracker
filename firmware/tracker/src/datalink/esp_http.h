// ESP32 implementation of ast::HttpTransport (contract 7.3, 7.4):
// HTTPClient over NetworkClientSecure (the class formerly called
// WiFiClientSecure), TLS verified against the CA bundle built into the
// esp32 core (or one pinned root from secrets.h), 30 s timeout. The request
// body is streamed, so a 960-entry batch never sits in RAM as one string.
#pragma once
#include <string>

#include "core/ast_http.h"

namespace ast_esp {

class EspHttp : public ast::HttpTransport {
 public:
  ast::HttpResult post_json(const std::string& url, ast::BodyReader& body) override;
  ast::HttpResult get(const std::string& url) override;

  // Optional PEM of one root CA to pin (must stay valid for the program's
  // lifetime). nullptr = use the built-in bundle of trusted roots.
  void set_pinned_root(const char* pem) { pinned_root_ = pem; }
  // How long a request waits for the Wi-Fi association to finish.
  uint32_t wait_link_ms = 12000;
  int last_error = 0;        // last HTTPClient error code (negative) or HTTP status
  uint32_t last_duration_ms = 0;

 private:
  ast::HttpResult request(bool is_post, const std::string& url, ast::BodyReader* body);
  const char* pinned_root_ = nullptr;
};

}  // namespace ast_esp
