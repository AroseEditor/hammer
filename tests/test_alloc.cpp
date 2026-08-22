#include "cli.h"
#include "loop.h"
#include "mock_server.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace {

std::atomic<uint64_t> g_allocations{0};

}

void* operator new(size_t size) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  void* memory = std::malloc(size == 0 ? 1 : size);
  if (memory == nullptr) throw std::bad_alloc{};
  return memory;
}

void* operator new[](size_t size) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  void* memory = std::malloc(size == 0 ? 1 : size);
  if (memory == nullptr) throw std::bad_alloc{};
  return memory;
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

TEST_CASE("the request loop allocates per connection rather than per request") {
  hammer::testing::MockServer server{hammer::testing::ServerOptions{}};

  const std::vector<std::string> args = {"-c", "8", "-t", "2", "-d", "1", server.url()};
  const hammer::ParseResult parsed = hammer::parse_args(args);
  REQUIRE(parsed.outcome == hammer::ParseOutcome::Ok);

  const uint64_t before = g_allocations.load();
  const hammer::RunResult result = hammer::run_event_loop(parsed.config);
  const uint64_t during = g_allocations.load() - before;

  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 1000);
  REQUIRE(result.stats.read_errors == 0);

  INFO("requests " << result.stats.requests << " allocations " << during);
  REQUIRE(during < 200);
}
