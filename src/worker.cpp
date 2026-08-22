#include "worker.h"

#include <algorithm>

namespace hammer {
namespace {

constexpr int kMaxConsecutiveConnectFailures = 200;
constexpr int kPollSliceMs = 20;
// sleeping into a departure costs the host timer granularity, which is ~15ms on windows
constexpr long long kSpinBelowMs = 2;

uint64_t nanos_between(Worker::Clock::time_point from, Worker::Clock::time_point to) {
  const auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count();
  return delta > 0 ? static_cast<uint64_t>(delta) : 0;
}

}

Worker::Worker(const Config& config, const Endpoint& endpoint, const std::string& request,
               int connections, double rate_per_second)
    : config_(config),
      endpoint_(endpoint),
      request_(request),
      head_request_(config.method == "HEAD"),
      buffer_(64 * 1024) {
  interval_ns_ = rate_per_second > 0.0 ? 1e9 / rate_per_second : 0.0;
  correct_latency_ = config.open_loop;
  conns_.resize(static_cast<size_t>(connections));
  index_by_fd_.reserve(static_cast<size_t>(connections) * 2);
  pending_open_.reserve(static_cast<size_t>(connections));
  opening_.reserve(static_cast<size_t>(connections));
}

void Worker::note_connect_failure() {
  ++stats_.connect_errors;
  if (++consecutive_connect_failures_ >= kMaxConsecutiveConnectFailures) {
    error_ = "gave up after " + std::to_string(kMaxConsecutiveConnectFailures) +
             " consecutive connect failures";
  }
}

void Worker::open_connection(size_t index) {
  Conn& conn = conns_[index];
  conn.state = ConnState::Closed;
  conn.write_offset = 0;
  conn.responses = 0;
  conn.received_this_exchange = 0;

  net::socket_t fd = net::kInvalidSocket;
  const ConnectStart start = start_connect(endpoint_, fd);
  if (start == ConnectStart::Failed) {
    note_connect_failure();
    pending_open_.push_back(index);
    return;
  }

  conn.fd = fd;
  index_by_fd_[fd] = index;
  poller_.add(fd, false, true);

  if (start == ConnectStart::Established) {
    consecutive_connect_failures_ = 0;
    begin_request(index);
    return;
  }
  conn.state = ConnState::Connecting;
  conn.request_started = Clock::now();
}

void Worker::close_connection(size_t index) {
  Conn& conn = conns_[index];
  if (net::valid(conn.fd)) {
    poller_.del(conn.fd);
    index_by_fd_.erase(conn.fd);
    net::close_socket(conn.fd);
    conn.fd = net::kInvalidSocket;
  }
  conn.state = ConnState::Closed;
  pending_open_.push_back(index);
}

void Worker::begin_request(size_t index) {
  Conn& conn = conns_[index];
  conn.write_offset = 0;
  conn.received_this_exchange = 0;
  conn.parser.reset(head_request_);

  if (interval_ns_ <= 0.0) {
    conn.intended_departure = Clock::now();
    conn.request_started = conn.intended_departure;
    conn.state = ConnState::Writing;
    flush_write(index);
    return;
  }

  // the departure time is decided by the schedule, not by when a connection happened to free up
  const double offset = static_cast<double>(departures_++) * interval_ns_;
  conn.intended_departure = t0_ + std::chrono::nanoseconds(static_cast<int64_t>(offset));
  conn.state = ConnState::Scheduled;
  poller_.mod(conn.fd, false, false);

  const Clock::time_point now = Clock::now();
  conn.claimed_late = now >= conn.intended_departure;
  if (conn.claimed_late) dispatch(index);
}

void Worker::dispatch(size_t index) {
  Conn& conn = conns_[index];
  conn.request_started = Clock::now();

  const uint64_t lag = nanos_between(conn.intended_departure, conn.request_started);
  stats_.dispatch_lag.record(lag);
  if (conn.claimed_late) {
    ++stats_.behind_schedule;
  } else {
    // no connection was busy, so any lateness here is hammer's own scheduling
    stats_.scheduler_lag.record(lag);
  }

  conn.state = ConnState::Writing;
  flush_write(index);
}

Worker::Clock::time_point Worker::release_scheduled() {
  Clock::time_point earliest = Clock::time_point::max();
  const Clock::time_point now = Clock::now();

  for (size_t i = 0; i < conns_.size(); ++i) {
    if (conns_[i].state != ConnState::Scheduled) continue;
    if (conns_[i].intended_departure <= now) {
      dispatch(i);
      continue;
    }
    earliest = std::min(earliest, conns_[i].intended_departure);
  }
  return earliest;
}

void Worker::flush_write(size_t index) {
  Conn& conn = conns_[index];

  while (conn.write_offset < request_.size()) {
    const long written = net::send_bytes(conn.fd, request_.data() + conn.write_offset,
                                         request_.size() - conn.write_offset);
    if (written > 0) {
      conn.write_offset += static_cast<size_t>(written);
      continue;
    }

    const int error = net::last_error();
    if (net::interrupted(error)) continue;
    if (net::would_block(error)) {
      poller_.mod(conn.fd, false, true);
      return;
    }
    if (!net::connection_reset(error) || conn.responses == 0) ++stats_.write_errors;
    close_connection(index);
    return;
  }

  conn.state = ConnState::Reading;
  poller_.mod(conn.fd, true, false);
}

void Worker::on_writable(size_t index) {
  Conn& conn = conns_[index];

  if (conn.state == ConnState::Connecting) {
    const int error = net::socket_error(conn.fd);
    if (error != 0) {
      note_connect_failure();
      close_connection(index);
      return;
    }
    consecutive_connect_failures_ = 0;
    begin_request(index);
    return;
  }

  if (conn.state == ConnState::Writing) flush_write(index);
}

void Worker::on_readable(size_t index) {
  Conn& conn = conns_[index];
  if (conn.state != ConnState::Reading) return;

  const long received = net::recv_bytes(conn.fd, buffer_.data(), buffer_.size());

  if (received < 0) {
    const int error = net::last_error();
    if (net::would_block(error) || net::interrupted(error)) return;
    if (!net::connection_reset(error) || conn.received_this_exchange > 0 ||
        conn.responses == 0) {
      ++stats_.read_errors;
    }
    close_connection(index);
    return;
  }

  if (received == 0) {
    if (conn.parser.eof().message_complete) {
      finish_response(index);
      return;
    }
    if (conn.received_this_exchange > 0 || conn.responses == 0) ++stats_.read_errors;
    close_connection(index);
    return;
  }

  stats_.bytes_read += static_cast<uint64_t>(received);
  conn.received_this_exchange += static_cast<uint64_t>(received);

  size_t offset = 0;
  const size_t available = static_cast<size_t>(received);
  while (offset < available) {
    const Result parsed = conn.parser.feed({buffer_.data() + offset, available - offset});
    offset += parsed.consumed;

    if (parsed.error) {
      ++stats_.read_errors;
      close_connection(index);
      return;
    }
    if (!parsed.message_complete) return;

    finish_response(index);
    if (conn.state != ConnState::Reading) return;
  }
}

void Worker::finish_response(size_t index) {
  Conn& conn = conns_[index];

  const Clock::time_point origin =
      correct_latency_ ? conn.intended_departure : conn.request_started;
  stats_.latency.record(nanos_between(origin, Clock::now()));
  ++stats_.requests;
  ++conn.responses;
  if (conn.parser.status_code() < 200 || conn.parser.status_code() >= 300) ++stats_.non_2xx;

  if (!conn.parser.keep_alive()) {
    close_connection(index);
    return;
  }
  begin_request(index);
}

void Worker::expire_connects() {
  // WSAPoll never signals a failed non-blocking connect, so a deadline is the only way out
  const Clock::time_point now = Clock::now();
  const auto limit = std::chrono::milliseconds(config_.timeout_ms);

  for (size_t i = 0; i < conns_.size(); ++i) {
    Conn& conn = conns_[i];
    if (conn.state != ConnState::Connecting) continue;
    if (now - conn.request_started < limit) continue;
    note_connect_failure();
    close_connection(i);
  }
}

void Worker::run(Clock::time_point start, Clock::time_point deadline) {
  t0_ = start;
  for (size_t i = 0; i < conns_.size() && error_.empty(); ++i) open_connection(i);

  while (Clock::now() < deadline && error_.empty()) {
    const Clock::time_point next_departure = release_scheduled();

    const Clock::time_point now = Clock::now();
    long long slice = kPollSliceMs;
    if (next_departure != Clock::time_point::max()) {
      slice = std::chrono::duration_cast<std::chrono::milliseconds>(next_departure - now).count();
      if (slice <= kSpinBelowMs) slice = 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int timeout_ms =
        static_cast<int>(std::clamp<long long>(std::min(remaining, slice), 0, kPollSliceMs));

    for (const Event& event : poller_.wait(timeout_ms)) {
      const auto slot = index_by_fd_.find(event.fd);
      if (slot == index_by_fd_.end()) continue;
      const size_t index = slot->second;

      if (conns_[index].state == ConnState::Connecting) {
        if (event.writable || event.error) on_writable(index);
        continue;
      }
      // let recv/send report the real error so it lands in the right bucket
      if (event.writable) on_writable(index);
      if (net::valid(conns_[index].fd) && (event.readable || event.error)) on_readable(index);
      if (net::valid(conns_[index].fd) && event.error &&
          conns_[index].state == ConnState::Writing) {
        flush_write(index);
      }
    }

    expire_connects();

    opening_.swap(pending_open_);
    for (size_t index : opening_) {
      if (!error_.empty()) break;
      open_connection(index);
    }
    opening_.clear();
  }

  for (Conn& conn : conns_) {
    if (net::valid(conn.fd)) {
      poller_.del(conn.fd);
      index_by_fd_.erase(conn.fd);
      net::close_socket(conn.fd);
      conn.fd = net::kInvalidSocket;
    }
    conn.state = ConnState::Closed;
  }
  pending_open_.clear();
  opening_.clear();
}

}
