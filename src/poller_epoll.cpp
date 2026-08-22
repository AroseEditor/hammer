#include "poller.h"

#include <sys/epoll.h>
#include <unistd.h>

namespace hammer {
namespace {

constexpr size_t kInitialCapacity = 1024;

uint32_t mask_for(bool read, bool write) {
  uint32_t mask = 0;
  if (read) mask |= EPOLLIN;
  if (write) mask |= EPOLLOUT;
  return mask;
}

}

struct Poller::Impl {
  int epfd = -1;
  std::vector<epoll_event> ready;
};

Poller::Poller() : impl_(std::make_unique<Impl>()) {
  impl_->epfd = ::epoll_create1(EPOLL_CLOEXEC);
  impl_->ready.resize(kInitialCapacity);
  events_.reserve(kInitialCapacity);
}

Poller::~Poller() {
  if (impl_->epfd >= 0) ::close(impl_->epfd);
}

void Poller::add(net::socket_t fd, bool read, bool write) {
  // level-triggered: one read per wakeup is enough, no drain loop needed
  epoll_event event{};
  event.events = mask_for(read, write);
  event.data.fd = fd;
  ::epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, fd, &event);
}

void Poller::mod(net::socket_t fd, bool read, bool write) {
  epoll_event event{};
  event.events = mask_for(read, write);
  event.data.fd = fd;
  ::epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &event);
}

void Poller::del(net::socket_t fd) {
  ::epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, fd, nullptr);
}

std::span<const Event> Poller::wait(int timeout_ms) {
  events_.clear();

  const int count = ::epoll_wait(impl_->epfd, impl_->ready.data(),
                                 static_cast<int>(impl_->ready.size()), timeout_ms);
  if (count <= 0) return {};

  for (int i = 0; i < count; ++i) {
    const epoll_event& source = impl_->ready[static_cast<size_t>(i)];
    Event event;
    event.fd = source.data.fd;
    event.readable = (source.events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) != 0;
    event.writable = (source.events & EPOLLOUT) != 0;
    event.error = (source.events & EPOLLERR) != 0;
    events_.push_back(event);
  }

  if (static_cast<size_t>(count) == impl_->ready.size()) {
    impl_->ready.resize(impl_->ready.size() * 2);
    events_.reserve(impl_->ready.size());
  }
  return events_;
}

}
