#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace hammer {

class TimerWheel {
public:
  using Clock = std::chrono::steady_clock;

  void configure(size_t capacity, std::chrono::nanoseconds timeout, Clock::time_point now) {
    timeout_ = timeout > std::chrono::nanoseconds{1} ? timeout : std::chrono::nanoseconds{1};

    const int64_t span = timeout_.count();
    const int64_t resolution = std::max<int64_t>(1'000'000, span / kSlotTarget + 1);
    resolution_ = std::chrono::nanoseconds{resolution};
    slots_ = static_cast<size_t>(span / resolution) + 3;

    nodes_.assign(capacity, Node{});
    heads_.assign(slots_, -1);
    epoch_ = now;
    cursor_ = 0;
    armed_ = 0;
  }

  void arm(size_t index, Clock::time_point now) {
    disarm(index);

    Node& node = nodes_[index];
    node.deadline = now + timeout_;
    link(index, slot_for(node.deadline));
    ++armed_;
  }

  void disarm(size_t index) {
    Node& node = nodes_[index];
    if (node.slot < 0) return;
    unlink(index);
    --armed_;
  }

  bool armed(size_t index) const { return nodes_[index].slot >= 0; }
  size_t armed_count() const { return armed_; }
  std::chrono::nanoseconds resolution() const { return resolution_; }

  template <class Fn>
  void expire(Clock::time_point now, Fn&& on_expired) {
    if (armed_ == 0) {
      cursor_ = tick_for(now);
      return;
    }

    const uint64_t target = tick_for(now);
    if (target <= cursor_) return;

    uint64_t tick = cursor_ + 1;
    if (target - cursor_ > slots_) tick = target - slots_ + 1;

    for (; tick <= target; ++tick) {
      const size_t slot = static_cast<size_t>(tick % slots_);
      int32_t index = heads_[slot];
      heads_[slot] = -1;

      while (index >= 0) {
        const int32_t next = nodes_[static_cast<size_t>(index)].next;
        Node& node = nodes_[static_cast<size_t>(index)];
        node.slot = -1;
        node.next = -1;
        node.prev = -1;

        if (node.deadline <= now) {
          --armed_;
          on_expired(static_cast<size_t>(index));
        } else {
          link(static_cast<size_t>(index), slot_for(node.deadline));
        }
        index = next;
      }
    }
    cursor_ = target;
  }

private:
  static constexpr int64_t kSlotTarget = 512;

  struct Node {
    int32_t next = -1;
    int32_t prev = -1;
    int32_t slot = -1;
    Clock::time_point deadline{};
  };

  uint64_t tick_for(Clock::time_point point) const {
    const int64_t delta = std::chrono::duration_cast<std::chrono::nanoseconds>(point - epoch_).count();
    return delta <= 0 ? 0 : static_cast<uint64_t>(delta / resolution_.count());
  }

  size_t slot_for(Clock::time_point deadline) const { return tick_for(deadline) % slots_; }

  void link(size_t index, size_t slot) {
    Node& node = nodes_[index];
    node.slot = static_cast<int32_t>(slot);
    node.prev = -1;
    node.next = heads_[slot];
    if (node.next >= 0) nodes_[static_cast<size_t>(node.next)].prev = static_cast<int32_t>(index);
    heads_[slot] = static_cast<int32_t>(index);
  }

  void unlink(size_t index) {
    Node& node = nodes_[index];
    const size_t slot = static_cast<size_t>(node.slot);
    if (node.prev >= 0) {
      nodes_[static_cast<size_t>(node.prev)].next = node.next;
    } else {
      heads_[slot] = node.next;
    }
    if (node.next >= 0) nodes_[static_cast<size_t>(node.next)].prev = node.prev;
    node.slot = -1;
    node.next = -1;
    node.prev = -1;
  }

  std::vector<Node> nodes_;
  std::vector<int32_t> heads_;
  std::chrono::nanoseconds timeout_{std::chrono::seconds{2}};
  std::chrono::nanoseconds resolution_{std::chrono::milliseconds{1}};
  size_t slots_ = 1;
  uint64_t cursor_ = 0;
  size_t armed_ = 0;
  Clock::time_point epoch_{};
};

}
