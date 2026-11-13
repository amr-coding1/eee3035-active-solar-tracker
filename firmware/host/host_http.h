// Plain HTTP/1.1 client over POSIX sockets for http://host:port URLs (the
// mock ThingSpeak server). Implements ast::HttpTransport for ast_host_device.
#pragma once
#include <functional>
#include <string>

#include "ast_http.h"

namespace ast_host {

struct ParsedUrl {
  std::string host;
  int port = 80;
  std::string path;   // starts with '/', includes the query
};
bool parse_http_url(const std::string& url, ParsedUrl* out);

// Parses a raw HTTP response (status line, headers, body; handles
// Content-Length and chunked bodies). False if no complete status line/headers.
bool parse_http_response(const std::string& raw, int* status, std::string* body);

class HostHttp : public ast::HttpTransport {
 public:
  ast::HttpResult post_json(const std::string& url, ast::BodyReader& body) override;
  ast::HttpResult get(const std::string& url) override;

  int timeout_ms = 30000;                    // contract 7.4
  std::function<bool()> link_up;             // false = Wi-Fi unavailable (request not sent)
  std::function<long long()> sim_time;       // when set: adds "X-Sim-Time: <unix seconds>"
  unsigned long requests = 0;

 private:
  ast::HttpResult request(const char* method, const std::string& url, ast::BodyReader* body);
};

}  // namespace ast_host
