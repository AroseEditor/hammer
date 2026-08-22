#include "poller.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace hammer {
namespace {

constexpr size_t kInitialCapacity = 1024;

SHORT mask_for(bool read, bool write) {
  SHORT mask = 0;
  if (read) mask |= POLLRDNORM;
  if (write) mask |= POLLWRNORM;
  return mask;
}

}

struct Poller::Impl {
  std::vector<WSAPOLLFD> fds;
  std::unordered_map<net::socket_t, size_t> slots;
};

Poller::Poller() : impl_(std::make_unique<Impl>()) {
  impl_->fds.reserve(kInitialCapacity);
  events_.reserve(kInitialCapacity);
}

Poller::~Poller() = default;

void Poller::add(net::socket_t fd, bool read, bool write) {
  if (impl_->slots.find(fd) != impl_->slots.end()) {
    mod(fd, read, write);
    return;
  }
  WSAPOLLFD entry{};
  entry.fd = fd;
  entry.events = mask_for(read, write);
  entry.revents = 0;
  impl_->slots.emplace(fd, impl_->fds.size());
  impl_->fds.push_back(entry);
}

void Poller::mod(net::socket_t fd, bool read, bool write) {
  const auto slot = impl_->slots.find(fd);
  if (slot == impl_->slots.end()) return;
  impl_->fds[slot->second].events = mask_for(read, write);
  impl_->fds[slot->second].revents = 0;
}

void Poller::del(net::socket_t fd) {
  const auto slot = impl_->slots.find(fd);
  if (slot == impl_->slots.end()) return;

  const size_t index = slot->second;
  const size_t last = impl_->fds.size() - 1;
  if (index != last) {
    impl_->fds[index] = impl_->fds[last];
    impl_->slots[impl_->fds[index].fd] = index;
  }
  impl_->fds.pop_back();
  impl_->slots.erase(slot);
}

std::span<const Event> Poller::wait(int timeout_ms) {
  events_.clear();

  if (impl_->fds.empty()) {
    if (timeout_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    return {};
  }

  const int count = ::WSAPoll(impl_->fds.data(), static_cast<ULONG>(impl_->fds.size()), timeout_ms);
  if (count <= 0) return {};

  for (const WSAPOLLFD& source : impl_->fds) {
    if (source.revents == 0) continue;
    Event event;
    event.fd = source.fd;
    event.readable = (source.revents & (POLLRDNORM | POLLHUP)) != 0;
    event.writable = (source.revents & POLLWRNORM) != 0;
    event.error = (source.revents & (POLLERR | POLLNVAL)) != 0;
    events_.push_back(event);
  }
  return events_;
}

}
