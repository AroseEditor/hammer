#pragma once

#include "cli.h"
#include "net_compat.h"

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

}
