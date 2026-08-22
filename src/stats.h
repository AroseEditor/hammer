#pragma once

#include "cli.h"
#include "histogram.h"

#include <cstdint>
#include <string>

namespace hammer {

struct Stats {
  Histogram latency;
  Histogram dispatch_lag;
  Histogram scheduler_lag;
  uint64_t requests = 0;
  uint64_t bytes_read = 0;
  uint64_t non_2xx = 0;
  uint64_t connect_errors = 0;
  uint64_t read_errors = 0;
  uint64_t write_errors = 0;
  uint64_t timeouts = 0;
  uint64_t behind_schedule = 0;

  void merge(const Stats& other);
};

std::string format_duration(double nanoseconds);
std::string format_bytes(double bytes);

void print_banner(const Config& config);
void print_report(const Config& config, const Stats& stats, double elapsed_seconds);

}
