#include "timer_wheel.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <vector>

using hammer::TimerWheel;
using Clock = TimerWheel::Clock;

namespace {

std::vector<size_t> collect(TimerWheel& wheel, Clock::time_point now) {
  std::vector<size_t> fired;
  wheel.expire(now, [&fired](size_t index) { fired.push_back(index); });
  std::sort(fired.begin(), fired.end());
  return fired;
}

}

TEST_CASE("nothing fires before the timeout elapses") {
  const Clock::time_point start{};
  TimerWheel wheel;
  wheel.configure(4, std::chrono::milliseconds(100), start);

  wheel.arm(0, start);
  wheel.arm(1, start);
  REQUIRE(wheel.armed_count() == 2);

  REQUIRE(collect(wheel, start + std::chrono::milliseconds(50)).empty());
  REQUIRE(collect(wheel, start + std::chrono::milliseconds(99)).empty());
  REQUIRE(wheel.armed_count() == 2);
}

TEST_CASE("everything armed at the same moment fires together") {
  const Clock::time_point start{};
  TimerWheel wheel;
  wheel.configure(4, std::chrono::milliseconds(100), start);

  for (size_t i = 0; i < 4; ++i) wheel.arm(i, start);

  const std::vector<size_t> fired = collect(wheel, start + std::chrono::milliseconds(120));
  REQUIRE(fired == std::vector<size_t>{0, 1, 2, 3});
  REQUIRE(wheel.armed_count() == 0);
}

TEST_CASE("staggered timers fire in the order they were armed") {
  const Clock::time_point start{};
  TimerWheel wheel;
  wheel.configure(3, std::chrono::milliseconds(100), start);

  wheel.arm(0, start);
  wheel.arm(1, start + std::chrono::milliseconds(40));
  wheel.arm(2, start + std::chrono::milliseconds(80));

  REQUIRE(collect(wheel, start + std::chrono::milliseconds(110)) == std::vector<size_t>{0});
  REQUIRE(collect(wheel, start + std::chrono::milliseconds(150)) == std::vector<size_t>{1});
  REQUIRE(collect(wheel, start + std::chrono::milliseconds(190)) == std::vector<size_t>{2});
  REQUIRE(wheel.armed_count() == 0);
}

TEST_CASE("disarming stops a timer from firing") {
  const Clock::time_point start{};
  TimerWheel wheel;
  wheel.configure(3, std::chrono::milliseconds(100), start);

  wheel.arm(0, start);
  wheel.arm(1, start);
  wheel.arm(2, start);
  wheel.disarm(1);
  REQUIRE(wheel.armed_count() == 2);
  REQUIRE_FALSE(wheel.armed(1));

  REQUIRE(collect(wheel, start + std::chrono::milliseconds(150)) == std::vector<size_t>{0, 2});
}

TEST_CASE("re-arming pushes the deadline out") {
  const Clock::time_point start{};
  TimerWheel wheel;
  wheel.configure(2, std::chrono::milliseconds(100), start);

  wheel.arm(0, start);
  wheel.arm(0, start + std::chrono::milliseconds(80));
  REQUIRE(wheel.armed_count() == 1);

  REQUIRE(collect(wheel, start + std::chrono::milliseconds(120)).empty());
  REQUIRE(collect(wheel, start + std::chrono::milliseconds(200)) == std::vector<size_t>{0});
}

TEST_CASE("disarming an idle slot is harmless") {
  const Clock::time_point start{};
  TimerWheel wheel;
  wheel.configure(2, std::chrono::milliseconds(100), start);

  wheel.disarm(0);
  wheel.disarm(1);
  wheel.disarm(0);
  REQUIRE(wheel.armed_count() == 0);
  REQUIRE(collect(wheel, start + std::chrono::seconds(5)).empty());
}

TEST_CASE("a long stall still fires every overdue timer exactly once") {
  const Clock::time_point start{};
  TimerWheel wheel;
  wheel.configure(8, std::chrono::milliseconds(100), start);

  for (size_t i = 0; i < 8; ++i) {
    wheel.arm(i, start + std::chrono::milliseconds(static_cast<int>(i) * 10));
  }

  const std::vector<size_t> fired = collect(wheel, start + std::chrono::seconds(60));
  REQUIRE(fired == std::vector<size_t>{0, 1, 2, 3, 4, 5, 6, 7});
  REQUIRE(wheel.armed_count() == 0);
  REQUIRE(collect(wheel, start + std::chrono::seconds(120)).empty());
}

TEST_CASE("a large timeout keeps the wheel small") {
  const Clock::time_point start{};
  TimerWheel wheel;
  wheel.configure(2, std::chrono::milliseconds(3'600'000), start);

  wheel.arm(0, start);
  REQUIRE(collect(wheel, start + std::chrono::seconds(60)).empty());
  REQUIRE(collect(wheel, start + std::chrono::seconds(3700)) == std::vector<size_t>{0});
}

TEST_CASE("random arm and disarm traffic never loses or duplicates a timer") {
  std::mt19937 rng{0xD15EA5Eu};
  const Clock::time_point start{};
  const size_t capacity = 64;

  TimerWheel wheel;
  wheel.configure(capacity, std::chrono::milliseconds(200), start);

  std::vector<Clock::time_point> expected(capacity);
  std::vector<bool> live(capacity, false);
  size_t fired_total = 0;

  for (int step = 1; step <= 4000; ++step) {
    const Clock::time_point now = start + std::chrono::milliseconds(step);

    const size_t index = rng() % capacity;
    if (rng() % 3 == 0 && live[index]) {
      wheel.disarm(index);
      live[index] = false;
    } else if (!live[index]) {
      wheel.arm(index, now);
      expected[index] = now + std::chrono::milliseconds(200);
      live[index] = true;
    }

    wheel.expire(now, [&](size_t fired) {
      REQUIRE(live[fired]);
      REQUIRE(expected[fired] <= now);
      live[fired] = false;
      ++fired_total;
    });

    size_t live_count = 0;
    for (bool alive : live) live_count += alive ? 1 : 0;
    REQUIRE(wheel.armed_count() == live_count);
  }

  REQUIRE(fired_total > 200);

  wheel.expire(start + std::chrono::seconds(30), [&](size_t fired) {
    REQUIRE(live[fired]);
    live[fired] = false;
    ++fired_total;
  });
  REQUIRE(wheel.armed_count() == 0);
  REQUIRE(std::none_of(live.begin(), live.end(), [](bool alive) { return alive; }));
}
