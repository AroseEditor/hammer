#include "cli.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("usage text names the binary and the url argument") {
  const auto usage = hammer::usage_text();
  REQUIRE(usage.find("hammer [options] <url>") != std::string_view::npos);
  REQUIRE(usage.find("--connections") != std::string_view::npos);
}

TEST_CASE("version string is baked in at configure time") {
  REQUIRE_FALSE(hammer::version_string().empty());
}
