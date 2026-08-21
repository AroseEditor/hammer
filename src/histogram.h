#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hammer {

class Histogram {
public:
  Histogram();

  void record(uint64_t value_ns);
  void merge(const Histogram& other);
  void reset();

  uint64_t percentile(double p) const;
  uint64_t count() const { return count_; }
  uint64_t min() const { return count_ == 0 ? 0 : min_; }
  uint64_t max() const { return max_; }
  double mean() const;

  static constexpr uint64_t kLowestDiscernible = 1;
  static constexpr uint64_t kHighestTracked = 60ull * 1000 * 1000 * 1000;
  static constexpr int kSignificantDigits = 3;

  size_t slot_count() const { return counts_.size(); }
  uint64_t value_at_slot(size_t index) const { return highest_equivalent(index); }

private:
  size_t slot_for(uint64_t value) const;
  uint64_t value_at_index(size_t index) const;
  uint64_t equivalent_range_at_index(size_t index) const;
  uint64_t lowest_equivalent(size_t index) const;
  uint64_t highest_equivalent(size_t index) const;

  int sub_bucket_half_count_magnitude_ = 0;
  int sub_bucket_count_ = 0;
  int sub_bucket_half_count_ = 0;
  uint64_t sub_bucket_mask_ = 0;
  int unit_magnitude_ = 0;
  int bucket_count_ = 0;

  std::vector<uint64_t> counts_;
  uint64_t count_ = 0;
  uint64_t min_ = 0;
  uint64_t max_ = 0;
  double sum_ = 0;
};

}
