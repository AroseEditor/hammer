#include "loop.h"

#include "conn.h"
#include "http_parser.h"
#include "worker.h"

#include <chrono>
#include <vector>

namespace hammer {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t nanos_since(Clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

class BlockingConnection {
public:
  BlockingConnection(const Endpoint& endpoint, int timeout_ms)
      : endpoint_(endpoint), timeout_ms_(timeout_ms) {}

  ~BlockingConnection() { close(); }

  BlockingConnection(const BlockingConnection&) = delete;
  BlockingConnection& operator=(const BlockingConnection&) = delete;

  bool open() {
    close();
    handle_ = open_socket(endpoint_);
    if (!net::valid(handle_)) return false;
    net::set_recv_timeout(handle_, timeout_ms_);
    net::set_send_timeout(handle_, timeout_ms_);
    if (::connect(handle_, reinterpret_cast<const sockaddr*>(&endpoint_.storage),
                  endpoint_.length) != 0) {
      close();
      return false;
    }
    return true;
  }

  void close() {
    if (net::valid(handle_)) {
      net::close_socket(handle_);
      handle_ = net::kInvalidSocket;
    }
  }

  bool is_open() const { return net::valid(handle_); }
  net::socket_t handle() const { return handle_; }

private:
  Endpoint endpoint_;
  int timeout_ms_ = 2000;
  net::socket_t handle_ = net::kInvalidSocket;
};

enum class Exchange { Complete, Timeout, ReadError, WriteError, PeerClosed, IdleClose };

}

RunResult run_blocking(const Config& config) {
  RunResult result;

  Endpoint endpoint;
  if (!resolve_endpoint(config.url, endpoint, result.error)) return result;

  const std::string request = build_request(config);
  const bool head_request = config.method == "HEAD";

  std::vector<char> buffer(64 * 1024);
  ResponseParser parser;
  BlockingConnection connection{endpoint, config.timeout_ms};

  const Clock::time_point start = Clock::now();
  const auto deadline = start + std::chrono::seconds(config.duration_s);
  int consecutive_connect_failures = 0;
  uint64_t responses_on_connection = 0;

  while (Clock::now() < deadline) {
    if (!connection.is_open()) {
      if (!connection.open()) {
        ++result.stats.connect_errors;
        if (++consecutive_connect_failures >= 10) {
          result.error = "gave up after 10 consecutive connect failures";
          break;
        }
        continue;
      }
      consecutive_connect_failures = 0;
      responses_on_connection = 0;
    }

    const Clock::time_point sent_at = Clock::now();
    parser.reset(head_request);

    Exchange outcome = Exchange::Complete;
    uint64_t received_this_exchange = 0;
    size_t written = 0;
    while (written < request.size()) {
      const long n = net::send_bytes(connection.handle(), request.data() + written,
                                     request.size() - written);
      if (n > 0) {
        written += static_cast<size_t>(n);
        continue;
      }
      const int error = net::last_error();
      if (net::interrupted(error)) continue;
      if (net::connection_reset(error) && responses_on_connection > 0) {
        outcome = Exchange::IdleClose;
      } else {
        outcome = net::timed_out(error) ? Exchange::Timeout : Exchange::WriteError;
      }
      break;
    }

    while (outcome == Exchange::Complete) {
      const long n = net::recv_bytes(connection.handle(), buffer.data(), buffer.size());
      if (n > 0) {
        result.stats.bytes_read += static_cast<uint64_t>(n);
        received_this_exchange += static_cast<uint64_t>(n);
        const Result parsed = parser.feed({buffer.data(), static_cast<size_t>(n)});
        if (parsed.error) {
          outcome = Exchange::ReadError;
        } else if (parsed.message_complete) {
          break;
        }
        continue;
      }
      if (n == 0) {
        const Result parsed = parser.eof();
        if (parsed.message_complete) {
          outcome = Exchange::Complete;
        } else if (received_this_exchange == 0 && responses_on_connection > 0) {
          // a keep-alive connection closed between requests is the server's prerogative
          outcome = Exchange::IdleClose;
        } else {
          outcome = Exchange::PeerClosed;
        }
        break;
      }
      const int error = net::last_error();
      if (net::interrupted(error)) continue;
      if (net::timed_out(error)) {
        outcome = Exchange::Timeout;
      } else if (net::connection_reset(error) && received_this_exchange == 0 &&
                 responses_on_connection > 0) {
        // the peer reaped its keep-alive connection while our next request was in flight
        outcome = Exchange::IdleClose;
      } else {
        outcome = Exchange::ReadError;
      }
    }

    switch (outcome) {
      case Exchange::Complete:
        result.stats.latency.record(nanos_since(sent_at));
        ++result.stats.requests;
        ++responses_on_connection;
        if (parser.status_code() < 200 || parser.status_code() >= 300) ++result.stats.non_2xx;
        if (!parser.keep_alive()) connection.close();
        break;
      case Exchange::IdleClose:
        connection.close();
        break;
      case Exchange::Timeout:
        ++result.stats.timeouts;
        connection.close();
        break;
      case Exchange::WriteError:
        ++result.stats.write_errors;
        connection.close();
        break;
      case Exchange::ReadError:
      case Exchange::PeerClosed:
        ++result.stats.read_errors;
        connection.close();
        break;
    }
  }

  result.elapsed_seconds =
      std::chrono::duration<double>(Clock::now() - start).count();
  result.ok = result.error.empty();
  return result;
}


RunResult run_event_loop(const Config& config) {
  RunResult result;

  Endpoint endpoint;
  if (!resolve_endpoint(config.url, endpoint, result.error)) return result;

  const std::string request = build_request(config);

  Worker worker{config, endpoint, request, config.connections};
  const Clock::time_point start = Clock::now();
  worker.run(start + std::chrono::seconds(config.duration_s));

  result.elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  result.stats.merge(worker.stats());
  result.error = worker.error();
  result.ok = result.error.empty();
  return result;
}

}
