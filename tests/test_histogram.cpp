#include "histogram.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

using hammer::Histogram;

namespace {

uint64_t oracle_percentile(const std::vector<uint64_t>& sorted, double p) {
  const double requested = std::clamp(p, 0.0, 100.0);
  uint64_t rank = static_cast<uint64_t>((requested / 100.0) * static_cast<double>(sorted.size()) + 0.5);
  rank = std::max<uint64_t>(rank, 1);
  rank = std::min<uint64_t>(rank, sorted.size());
  return sorted[static_cast<size_t>(rank - 1)];
}

uint64_t tolerance_for(uint64_t value) {
  return value / 1024 + 1;
}

std::vector<double> percentiles_under_test() {
  std::vector<double> points;
  for (int i = 1; i <= 99; ++i) points.push_back(static_cast<double>(i));
  points.push_back(99.9);
  points.push_back(99.99);
  return points;
}

}

TEST_CASE("an empty histogram reports zeros rather than garbage") {
  const Histogram histogram;
  REQUIRE(histogram.count() == 0);
  REQUIRE(histogram.min() == 0);
  REQUIRE(histogram.max() == 0);
  REQUIRE(histogram.mean() == 0.0);
  REQUIRE(histogram.percentile(0) == 0);
  REQUIRE(histogram.percentile(50) == 0);
  REQUIRE(histogram.percentile(99.99) == 0);
  REQUIRE(histogram.percentile(100) == 0);
}

TEST_CASE("a single value is reported at every percentile") {
  Histogram histogram;
  histogram.record(1'234'567);

  REQUIRE(histogram.count() == 1);
  REQUIRE(histogram.min() == 1'234'567);
  REQUIRE(histogram.max() == 1'234'567);
  REQUIRE(histogram.mean() == 1'234'567.0);

  for (double p : {0.0, 1.0, 50.0, 99.0, 99.99, 100.0}) {
    const uint64_t reported = histogram.percentile(p);
    REQUIRE(reported >= 1'234'567 - tolerance_for(1'234'567));
    REQUIRE(reported <= 1'234'567 + tolerance_for(1'234'567));
  }
}

TEST_CASE("zero is a recordable value") {
  Histogram histogram;
  histogram.record(0);
  REQUIRE(histogram.count() == 1);
  REQUIRE(histogram.min() == 0);
  REQUIRE(histogram.max() == 0);
  REQUIRE(histogram.percentile(50) == 0);
}

TEST_CASE("values above the tracked maximum keep their true max") {
  Histogram histogram;
  histogram.record(1'000'000);
  histogram.record(500ull * 1000 * 1000 * 1000);

  REQUIRE(histogram.count() == 2);
  REQUIRE(histogram.max() == 500ull * 1000 * 1000 * 1000);
  REQUIRE(histogram.percentile(100) >= Histogram::kHighestTracked);
  REQUIRE(histogram.percentile(1) <= 1'000'000 + tolerance_for(1'000'000));
}

TEST_CASE("recorded values land within three significant digits") {
  Histogram histogram;
  const std::vector<uint64_t> values = {1,      999,      1'000,     1'001,      2'047,
                                        2'048,  65'536,   1'000'000, 41'300'000, 210'550'000,
                                        1'000'000'000, 59'999'999'999};
  for (uint64_t value : values) histogram.record(value);

  for (uint64_t value : values) {
    Histogram single;
    single.record(value);
    const uint64_t reported = single.percentile(50);
    INFO("value " << value << " reported as " << reported);
    REQUIRE(reported >= value);
    REQUIRE(reported <= value + tolerance_for(value));
  }
}

TEST_CASE("percentiles agree with a sorted vector oracle over 100k values") {
  std::mt19937_64 rng{0x1234'5678'9ABC'DEF0ull};
  std::uniform_real_distribution<double> exponent{std::log(1'000.0), std::log(6.0e10)};

  Histogram histogram;
  std::vector<uint64_t> oracle;
  oracle.reserve(100'000);

  for (int i = 0; i < 100'000; ++i) {
    const uint64_t value = static_cast<uint64_t>(std::exp(exponent(rng)));
    histogram.record(value);
    oracle.push_back(value);
  }
  std::sort(oracle.begin(), oracle.end());

  REQUIRE(histogram.count() == oracle.size());
  REQUIRE(histogram.min() == oracle.front());
  REQUIRE(histogram.max() == oracle.back());

  for (double p : percentiles_under_test()) {
    const uint64_t expected = oracle_percentile(oracle, p);
    const uint64_t reported = histogram.percentile(p);
    INFO("p" << p << " expected " << expected << " reported " << reported);
    REQUIRE(reported >= expected);
    REQUIRE(reported <= expected + tolerance_for(expected));
  }
}

TEST_CASE("percentiles agree with the oracle on a heavy tailed distribution") {
  std::mt19937_64 rng{0xFEED'BEEFull};
  std::uniform_int_distribution<uint64_t> body{50'000, 2'000'000};
  std::uniform_int_distribution<uint64_t> tail{100'000'000, 40'000'000'000};
  std::uniform_int_distribution<int> pick{0, 999};

  Histogram histogram;
  std::vector<uint64_t> oracle;
  oracle.reserve(100'000);

  for (int i = 0; i < 100'000; ++i) {
    const uint64_t value = pick(rng) < 995 ? body(rng) : tail(rng);
    histogram.record(value);
    oracle.push_back(value);
  }
  std::sort(oracle.begin(), oracle.end());

  for (double p : percentiles_under_test()) {
    const uint64_t expected = oracle_percentile(oracle, p);
    const uint64_t reported = histogram.percentile(p);
    INFO("p" << p << " expected " << expected << " reported " << reported);
    REQUIRE(reported >= expected);
    REQUIRE(reported <= expected + tolerance_for(expected));
  }

  const double oracle_mean =
      std::accumulate(oracle.begin(), oracle.end(), 0.0) / static_cast<double>(oracle.size());
  REQUIRE(std::abs(histogram.mean() - oracle_mean) < 1.0);
}

TEST_CASE("merging shards matches recording everything into one histogram") {
  std::mt19937_64 rng{0xA5A5'5A5Aull};
  std::uniform_int_distribution<uint64_t> values{1'000, 5'000'000'000};

  Histogram combined;
  Histogram shards[4];

  for (int i = 0; i < 40'000; ++i) {
    const uint64_t value = values(rng);
    combined.record(value);
    shards[i % 4].record(value);
  }

  Histogram merged;
  for (const Histogram& shard : shards) merged.merge(shard);

  REQUIRE(merged.count() == combined.count());
  REQUIRE(merged.min() == combined.min());
  REQUIRE(merged.max() == combined.max());
  REQUIRE(std::abs(merged.mean() - combined.mean()) < 1.0);

  for (double p : percentiles_under_test()) {
    INFO("p" << p);
    REQUIRE(merged.percentile(p) == combined.percentile(p));
  }
}

TEST_CASE("merging an empty histogram changes nothing") {
  Histogram histogram;
  histogram.record(5'000);
  histogram.record(50'000);

  const Histogram empty;
  histogram.merge(empty);

  REQUIRE(histogram.count() == 2);
  REQUIRE(histogram.min() == 5'000);
  REQUIRE(histogram.max() == 50'000);
}

TEST_CASE("reset clears every statistic") {
  Histogram histogram;
  for (uint64_t i = 1; i <= 1000; ++i) histogram.record(i * 1000);
  histogram.reset();

  REQUIRE(histogram.count() == 0);
  REQUIRE(histogram.min() == 0);
  REQUIRE(histogram.max() == 0);
  REQUIRE(histogram.percentile(99) == 0);
}

TEST_CASE("percentiles are monotonically non decreasing") {
  std::mt19937_64 rng{0x0DDBA11ull};
  std::uniform_int_distribution<uint64_t> values{1, 60'000'000'000};

  Histogram histogram;
  for (int i = 0; i < 20'000; ++i) histogram.record(values(rng));

  uint64_t previous = 0;
  for (double p = 0.0; p <= 100.0; p += 0.25) {
    const uint64_t reported = histogram.percentile(p);
    INFO("p" << p);
    REQUIRE(reported >= previous);
    previous = reported;
  }
}
