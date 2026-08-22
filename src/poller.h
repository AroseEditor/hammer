#pragma once

#include "net_compat.h"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace hammer {

struct Event {
  net::socket_t fd = net::kInvalidSocket;
  bool readable = false;
  bool writable = false;
  bool error = false;
};

class Poller {
public:
  Poller();
  ~Poller();

  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  void add(net::socket_t fd, bool read, bool write);
  void mod(net::socket_t fd, bool read, bool write);
  void del(net::socket_t fd);

  std::span<const Event> wait(int timeout_ms);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<Event> events_;
};

}
