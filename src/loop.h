#pragma once

#include "cli.h"
#include "stats.h"

#include <string>

namespace hammer {

struct RunResult {
  Stats stats;
  double elapsed_seconds = 0.0;
  bool ok = false;
  std::string error;
};

RunResult run_blocking(const Config& config);

}
