#pragma once

#include "net_compat.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace hammer::testing {

struct ServerOptions {
  std::string response =
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello";
  bool close_after_response = false;
  bool close_before_response = false;
  bool never_respond = false;
  int responses_before_close = 0;
};

inline uint16_t reserve_closed_port() {
  const net::socket_t probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  ::listen(probe, 1);

  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  ::getsockname(probe, reinterpret_cast<sockaddr*>(&bound), &length);
  const uint16_t port = ::ntohs(bound.sin_port);
  net::close_socket(probe);
  return port;
}

class MockServer {
public:
  explicit MockServer(ServerOptions options) : options_(std::move(options)) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    ::listen(listener_, 64);

    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &length);
    port_ = ::ntohs(bound.sin_port);

    worker_ = std::thread([this] { serve(); });
  }

  ~MockServer() {
    running_ = false;
    wake_accept();
    if (worker_.joinable()) worker_.join();
    if (net::valid(listener_)) {
      net::close_socket(listener_);
      listener_ = net::kInvalidSocket;
    }
  }

  MockServer(const MockServer&) = delete;
  MockServer& operator=(const MockServer&) = delete;

  uint16_t port() const { return port_; }
  std::string url(const std::string& path = "/") const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
  }
  uint64_t requests_served() const { return served_.load(); }

private:
  void wake_accept() {
    const net::socket_t waker = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!net::valid(waker)) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = ::htons(port_);
    ::connect(waker, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    net::close_socket(waker);
  }

  void serve() {
    while (running_) {
      const net::socket_t client = ::accept(listener_, nullptr, nullptr);
      if (!net::valid(client)) break;
      handle(client);
      net::close_socket(client);
    }
  }

  void handle(net::socket_t client) {
    net::set_nodelay(client);
    net::set_recv_timeout(client, 1000);

    int served_here = 0;
    std::vector<char> buffer(8192);

    while (running_) {
      std::string request;
      bool got_request = false;
      while (running_) {
        const long n = net::recv_bytes(client, buffer.data(), buffer.size());
        if (n <= 0) return;
        request.append(buffer.data(), static_cast<size_t>(n));
        if (request.find("\r\n\r\n") != std::string::npos) {
          got_request = true;
          break;
        }
      }
      if (!got_request) return;

      if (options_.close_before_response) return;
      if (options_.never_respond) {
        for (int i = 0; i < 50 && running_; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return;
      }

      size_t written = 0;
      while (written < options_.response.size()) {
        const long n = net::send_bytes(client, options_.response.data() + written,
                                       options_.response.size() - written);
        if (n <= 0) return;
        written += static_cast<size_t>(n);
      }
      ++served_;
      ++served_here;

      if (options_.close_after_response) return;
      if (options_.responses_before_close > 0 && served_here >= options_.responses_before_close) {
        return;
      }
    }
  }

  ServerOptions options_;
  net::socket_t listener_ = net::kInvalidSocket;
  uint16_t port_ = 0;
  std::atomic<bool> running_{true};
  std::atomic<uint64_t> served_{0};
  std::thread worker_;
};

}
