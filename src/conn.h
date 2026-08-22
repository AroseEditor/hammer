#pragma once

#include "cli.h"
#include "http_parser.h"
#include "net_compat.h"

#include <chrono>
#include <string>

namespace hammer {

struct Endpoint {
  sockaddr_storage storage{};
  socklen_t length = 0;
  int family = 0;
};

bool resolve_endpoint(const Url& url, Endpoint& out, std::string& error);

std::string build_request(const Config& config);

net::socket_t open_socket(const Endpoint& endpoint);

enum class ConnState { Closed, Connecting, Scheduled, Writing, Reading };

enum class ConnectStart { Established, InProgress, Failed };

ConnectStart start_connect(const Endpoint& endpoint, net::socket_t& out);

struct Conn {
  net::socket_t fd = net::kInvalidSocket;
  ConnState state = ConnState::Closed;
  size_t write_offset = 0;
  uint64_t responses = 0;
  uint64_t received_this_exchange = 0;
  std::chrono::steady_clock::time_point request_started{};
  std::chrono::steady_clock::time_point intended_departure{};
  bool claimed_late = false;
  ResponseParser parser;
};

}
