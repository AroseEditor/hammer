#pragma once

#include "conn.h"
#include "poller.h"
#include "stats.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace hammer {

class Worker {
public:
  using Clock = std::chrono::steady_clock;

  Worker(const Config& config, const Endpoint& endpoint, const std::string& request,
         int connections);

  void run(Clock::time_point deadline);

  const Stats& stats() const { return stats_; }
  const std::string& error() const { return error_; }

private:
  void open_connection(size_t index);
  void close_connection(size_t index);
  void begin_request(size_t index);
  void flush_write(size_t index);
  void on_writable(size_t index);
  void on_readable(size_t index);
  void finish_response(size_t index);
  void note_connect_failure();
  void expire_connects();

  const Config& config_;
  Endpoint endpoint_;
  const std::string& request_;
  bool head_request_ = false;

  Poller poller_;
  std::vector<Conn> conns_;
  std::unordered_map<net::socket_t, size_t> index_by_fd_;
  std::vector<size_t> pending_open_;
  std::vector<size_t> opening_;
  std::vector<char> buffer_;

  Stats stats_;
  std::string error_;
  int consecutive_connect_failures_ = 0;
};

}
