#include "histogram.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace hammer {
namespace {

int magnitude_ceiling(uint64_t value) {
  int magnitude = 0;
  while ((1ull << magnitude) < value) ++magnitude;
  return magnitude;
}

int magnitude_floor(uint64_t value) {
  int magnitude = 0;
  while ((1ull << (magnitude + 1)) <= value) ++magnitude;
  return magnitude;
}

}

Histogram::Histogram() {
  uint64_t single_unit_resolution = 2;
  for (int i = 0; i < kSignificantDigits; ++i) single_unit_resolution *= 10;

  const int sub_bucket_count_magnitude = magnitude_ceiling(single_unit_resolution);
  sub_bucket_half_count_magnitude_ = sub_bucket_count_magnitude - 1;
  unit_magnitude_ = magnitude_floor(kLowestDiscernible);
  sub_bucket_count_ = 1 << sub_bucket_count_magnitude;
  sub_bucket_half_count_ = sub_bucket_count_ / 2;
  sub_bucket_mask_ = static_cast<uint64_t>(sub_bucket_count_ - 1) << unit_magnitude_;

  uint64_t smallest_untrackable = static_cast<uint64_t>(sub_bucket_count_) << unit_magnitude_;
  bucket_count_ = 1;
  while (smallest_untrackable <= kHighestTracked) {
    smallest_untrackable <<= 1;
    ++bucket_count_;
  }

  counts_.assign(static_cast<size_t>(bucket_count_ + 1) *
                     static_cast<size_t>(sub_bucket_half_count_),
                 0);
}

size_t Histogram::slot_for(uint64_t value) const {
  const int pow2_ceiling = 64 - std::countl_zero(value | sub_bucket_mask_);
  const int bucket = pow2_ceiling - unit_magnitude_ - (sub_bucket_half_count_magnitude_ + 1);
  const int sub = static_cast<int>(value >> (bucket + unit_magnitude_));
  const int base = (bucket + 1) << sub_bucket_half_count_magnitude_;
  return static_cast<size_t>(base + sub - sub_bucket_half_count_);
}

uint64_t Histogram::value_at_index(size_t index) const {
  int bucket = static_cast<int>(index >> sub_bucket_half_count_magnitude_) - 1;
  int sub = static_cast<int>(index & static_cast<size_t>(sub_bucket_half_count_ - 1)) +
            sub_bucket_half_count_;
  if (bucket < 0) {
    sub -= sub_bucket_half_count_;
    bucket = 0;
  }
  return static_cast<uint64_t>(sub) << (bucket + unit_magnitude_);
}

uint64_t Histogram::equivalent_range_at_index(size_t index) const {
  int bucket = static_cast<int>(index >> sub_bucket_half_count_magnitude_) - 1;
  if (bucket < 0) bucket = 0;
  return 1ull << (bucket + unit_magnitude_);
}

uint64_t Histogram::lowest_equivalent(size_t index) const {
  return value_at_index(index);
}

uint64_t Histogram::highest_equivalent(size_t index) const {
  return value_at_index(index) + equivalent_range_at_index(index) - 1;
}

void Histogram::record(uint64_t value_ns) {
  const uint64_t clamped = std::min(value_ns, kHighestTracked);
  const size_t slot = std::min(slot_for(clamped), counts_.size() - 1);
  ++counts_[slot];

  if (count_ == 0 || value_ns < min_) min_ = value_ns;
  if (value_ns > max_) max_ = value_ns;
  ++count_;
  sum_ += static_cast<double>(value_ns);
}

void Histogram::merge(const Histogram& other) {
  if (other.count_ == 0) return;
  for (size_t i = 0; i < counts_.size(); ++i) counts_[i] += other.counts_[i];
  if (count_ == 0 || other.min_ < min_) min_ = other.min_;
  if (other.max_ > max_) max_ = other.max_;
  count_ += other.count_;
  sum_ += other.sum_;
}

void Histogram::reset() {
  std::fill(counts_.begin(), counts_.end(), 0ull);
  count_ = 0;
  min_ = 0;
  max_ = 0;
  sum_ = 0;
}

uint64_t Histogram::percentile(double p) const {
  if (count_ == 0) return 0;

  const double requested = std::clamp(p, 0.0, 100.0);
  uint64_t rank = static_cast<uint64_t>((requested / 100.0) * static_cast<double>(count_) + 0.5);
  rank = std::max<uint64_t>(rank, 1);

  uint64_t total = 0;
  for (size_t i = 0; i < counts_.size(); ++i) {
    total += counts_[i];
    if (total >= rank) {
      return requested == 0.0 ? lowest_equivalent(i) : highest_equivalent(i);
    }
  }
  return highest_equivalent(counts_.size() - 1);
}

double Histogram::mean() const {
  if (count_ == 0) return 0.0;
  return sum_ / static_cast<double>(count_);
}

}
