#include "host_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace ast_host {

bool parse_http_url(const std::string& url, ParsedUrl* out) {
  const std::string scheme = "http://";
  if (url.compare(0, scheme.size(), scheme) != 0) return false;   // https is not supported here
  const size_t host_start = scheme.size();
  size_t path_start = url.find('/', host_start);
  if (path_start == std::string::npos) path_start = url.size();
  std::string hostport = url.substr(host_start, path_start - host_start);
  if (hostport.empty()) return false;
  ParsedUrl u;
  const size_t colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    const std::string port = hostport.substr(colon + 1);
    if (port.empty() || port.size() > 5) return false;
    for (char c : port) if (c < '0' || c > '9') return false;
    u.port = atoi(port.c_str());
    if (u.port < 1 || u.port > 65535) return false;
    hostport = hostport.substr(0, colon);
    if (hostport.empty()) return false;
  }
  u.host = hostport;
  u.path = path_start < url.size() ? url.substr(path_start) : "/";
  *out = u;
  return true;
}

static std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

bool parse_http_response(const std::string& raw, int* status, std::string* body) {
  const size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) return false;
  if (raw.compare(0, 5, "HTTP/") != 0) return false;
  const size_t sp = raw.find(' ');
  if (sp == std::string::npos || sp + 4 > head_end + 2) return false;
  int code = 0;
  for (size_t i = sp + 1; i < sp + 4; ++i) {
    if (raw[i] < '0' || raw[i] > '9') return false;
    code = code * 10 + (raw[i] - '0');
  }
  const std::string head = lower(raw.substr(0, head_end + 2));
  const std::string payload = raw.substr(head_end + 4);
  if (head.find("transfer-encoding: chunked") != std::string::npos) {
    std::string out;
    size_t pos = 0;
    for (;;) {
      const size_t eol = payload.find("\r\n", pos);
      if (eol == std::string::npos) return false;            // truncated
      const unsigned long n = strtoul(payload.substr(pos, eol - pos).c_str(), nullptr, 16);
      if (n == 0) break;
      if (eol + 2 + n > payload.size()) return false;        // truncated chunk
      out.append(payload, eol + 2, n);
      pos = eol + 2 + n + 2;
    }
    *body = out;
  } else {
    const size_t cl = head.find("content-length:");
    if (cl != std::string::npos) {
      const unsigned long n = strtoul(head.c_str() + cl + 15, nullptr, 10);
      if (payload.size() < n) return false;                  // response cut short
      *body = payload.substr(0, n);
    } else {
      *body = payload;                                        // delimited by connection close
    }
  }
  *status = code;
  return true;
}

static bool send_all(int fd, const char* p, size_t n) {
  while (n > 0) {
    const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

ast::HttpResult HostHttp::request(const char* method, const std::string& url, ast::BodyReader* body) {
  ast::HttpResult res;   // kNotSent
  ++requests;
  if (link_up && !link_up()) return res;
  ParsedUrl u;
  if (!parse_http_url(url, &u)) return res;

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* ai = nullptr;
  char port[8];
  snprintf(port, sizeof port, "%d", u.port);
  if (getaddrinfo(u.host.c_str(), port, &hints, &ai) != 0 || ai == nullptr) return res;
  const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0) { freeaddrinfo(ai); return res; }
  timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
  freeaddrinfo(ai);
  if (rc != 0) { ::close(fd); return res; }

  std::string head = std::string(method) + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + port +
                     "\r\nUser-Agent: ast-datalink/1.5 (host)\r\nAccept: */*\r\nConnection: close\r\n";
  if (sim_time) head += "X-Sim-Time: " + std::to_string(sim_time()) + "\r\n";
  if (body != nullptr) {
    head += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body->size()) + "\r\n";
  }
  head += "\r\n";
  bool sent = send_all(fd, head.data(), head.size());
  if (sent && body != nullptr) {
    body->rewind();
    uint8_t buf[4096];
    size_t n;
    while (sent && (n = body->read(buf, sizeof buf)) > 0) {
      sent = send_all(fd, reinterpret_cast<const char*>(buf), n);
    }
  }
  if (!sent) {   // the server cannot have processed an incomplete request
    ::close(fd);
    return res;
  }

  // From here on the server may have acted on the request.
  res.outcome = ast::HttpResult::kNoResponse;
  std::string raw;
  char rbuf[4096];
  for (;;) {
    const ssize_t r = ::recv(fd, rbuf, sizeof rbuf, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;            // closed, reset or timed out
    raw.append(rbuf, static_cast<size_t>(r));
    if (raw.size() > 1024 * 1024) break;
  }
  ::close(fd);
  int status = 0;
  std::string payload;
  if (parse_http_response(raw, &status, &payload)) {
    res.outcome = ast::HttpResult::kResponse;
    res.status = status;
    res.body = payload.substr(0, 512);
  }
  return res;
}

ast::HttpResult HostHttp::post_json(const std::string& url, ast::BodyReader& body) {
  return request("POST", url, &body);
}

ast::HttpResult HostHttp::get(const std::string& url) { return request("GET", url, nullptr); }

}  // namespace ast_host
