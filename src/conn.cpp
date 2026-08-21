#include "conn.h"

#include <cstring>
#include <string_view>

namespace hammer {
namespace {

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
    const char cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
    if (ca != cb) return false;
  }
  return true;
}

bool has_header(const std::vector<std::string>& headers, std::string_view name) {
  for (const std::string& header : headers) {
    const size_t colon = header.find(':');
    if (colon == std::string::npos) continue;
    if (iequals(std::string_view(header).substr(0, colon), name)) return true;
  }
  return false;
}

}

bool resolve_endpoint(const Url& url, Endpoint& out, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  const std::string port = std::to_string(url.port);
  addrinfo* results = nullptr;
  const int status = ::getaddrinfo(url.host.c_str(), port.c_str(), &hints, &results);
  if (status != 0 || results == nullptr) {
    error = "cannot resolve host \"" + url.host + "\"";
    return false;
  }

  std::memcpy(&out.storage, results->ai_addr, results->ai_addrlen);
  out.length = static_cast<socklen_t>(results->ai_addrlen);
  out.family = results->ai_family;
  ::freeaddrinfo(results);
  return true;
}

std::string build_request(const Config& config) {
  std::string request;
  request.reserve(256 + config.body.size());

  request += config.method;
  request += ' ';
  request += config.url.path;
  request += " HTTP/1.1\r\n";

  if (!has_header(config.headers, "Host")) {
    request += "Host: ";
    request += config.url.host;
    if (config.url.port != 80) {
      request += ':';
      request += std::to_string(config.url.port);
    }
    request += "\r\n";
  }
  if (!has_header(config.headers, "User-Agent")) {
    request += "User-Agent: hammer/";
    request += version_string();
    request += "\r\n";
  }
  if (!has_header(config.headers, "Accept")) {
    request += "Accept: */*\r\n";
  }
  for (const std::string& header : config.headers) {
    request += header;
    request += "\r\n";
  }
  if (!config.body.empty() && !has_header(config.headers, "Content-Length")) {
    request += "Content-Length: ";
    request += std::to_string(config.body.size());
    request += "\r\n";
  }

  request += "\r\n";
  request += config.body;
  return request;
}

net::socket_t open_socket(const Endpoint& endpoint) {
  const net::socket_t handle = ::socket(endpoint.family, SOCK_STREAM, IPPROTO_TCP);
  if (!net::valid(handle)) return net::kInvalidSocket;
  net::set_nodelay(handle);
  return handle;
}

}
