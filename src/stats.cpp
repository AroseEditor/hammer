#include "stats.h"

#include <cstdio>
#include <vector>

namespace hammer {
namespace {

std::string format_number(double value, const char* unit) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.2f%s", value, unit);
  return buffer;
}

std::string pad_left(const std::string& text, size_t width) {
  if (text.size() >= width) return text;
  return std::string(width - text.size(), ' ') + text;
}

struct PercentileRow {
  const char* label;
  double p;
};

const std::vector<PercentileRow>& short_spectrum() {
  static const std::vector<PercentileRow> rows = {
      {"50%", 50.0}, {"75%", 75.0}, {"90%", 90.0}, {"99%", 99.0}, {"99.9%", 99.9}};
  return rows;
}

const std::vector<PercentileRow>& full_spectrum() {
  static const std::vector<PercentileRow> rows = {
      {"50%", 50.0},     {"75%", 75.0},     {"90%", 90.0},       {"95%", 95.0},
      {"97.5%", 97.5},   {"99%", 99.0},     {"99.9%", 99.9},     {"99.99%", 99.99},
      {"99.999%", 99.999}};
  return rows;
}

}

void Stats::merge(const Stats& other) {
  latency.merge(other.latency);
  dispatch_lag.merge(other.dispatch_lag);
  requests += other.requests;
  bytes_read += other.bytes_read;
  non_2xx += other.non_2xx;
  connect_errors += other.connect_errors;
  read_errors += other.read_errors;
  write_errors += other.write_errors;
  timeouts += other.timeouts;
  behind_schedule += other.behind_schedule;
}

std::string format_duration(double nanoseconds) {
  if (nanoseconds < 1'000.0) return format_number(nanoseconds, "ns");
  if (nanoseconds < 1'000'000.0) return format_number(nanoseconds / 1'000.0, "us");
  if (nanoseconds < 1'000'000'000.0) return format_number(nanoseconds / 1'000'000.0, "ms");
  return format_number(nanoseconds / 1'000'000'000.0, "s");
}

std::string format_bytes(double bytes) {
  if (bytes < 1024.0) return format_number(bytes, "B");
  if (bytes < 1024.0 * 1024.0) return format_number(bytes / 1024.0, "KB");
  if (bytes < 1024.0 * 1024.0 * 1024.0) return format_number(bytes / (1024.0 * 1024.0), "MB");
  return format_number(bytes / (1024.0 * 1024.0 * 1024.0), "GB");
}

void print_banner(const Config& config) {
  std::printf("Running %ds test @ %s\n", config.duration_s, config.raw_url.c_str());
  if (config.open_loop) {
    std::printf("  %d threads, %d connections, open loop @ %llu req/s\n", config.threads,
                config.connections, static_cast<unsigned long long>(config.rate));
  } else {
    std::printf("  %d threads, %d connections, closed loop\n", config.threads,
                config.connections);
  }
  std::fflush(stdout);
}

void print_report(const Config& config, const Stats& stats, double elapsed_seconds) {
  const std::vector<PercentileRow>& rows =
      config.print_latency ? full_spectrum() : short_spectrum();

  std::printf("\n%s\n", config.open_loop ? "Latency (corrected)" : "Latency (closed loop)");
  for (const PercentileRow& row : rows) {
    const double value = static_cast<double>(stats.latency.percentile(row.p));
    std::printf("%s %s\n", pad_left(row.label, 8).c_str(),
                pad_left(format_duration(value), 10).c_str());
  }
  std::printf("%s %s\n", pad_left("max", 8).c_str(),
              pad_left(format_duration(static_cast<double>(stats.latency.max())), 10).c_str());

  const double seconds = elapsed_seconds > 0.0 ? elapsed_seconds : 1e-9;
  std::printf("\n  %llu requests in %.2fs, %s read\n",
              static_cast<unsigned long long>(stats.requests), elapsed_seconds,
              format_bytes(static_cast<double>(stats.bytes_read)).c_str());
  std::printf("  Requests/sec %14.2f\n", static_cast<double>(stats.requests) / seconds);
  std::printf("  Transfer/sec %14s\n",
              format_bytes(static_cast<double>(stats.bytes_read) / seconds).c_str());
  std::printf("  Socket errors  connect %llu, read %llu, write %llu, timeout %llu\n",
              static_cast<unsigned long long>(stats.connect_errors),
              static_cast<unsigned long long>(stats.read_errors),
              static_cast<unsigned long long>(stats.write_errors),
              static_cast<unsigned long long>(stats.timeouts));
  if (stats.non_2xx > 0) {
    std::printf("  Non-2xx responses %llu\n", static_cast<unsigned long long>(stats.non_2xx));
  }

  if (config.open_loop) {
    const double mean_lag = stats.dispatch_lag.mean();
    std::printf("  Dispatch lag   mean %s, max %s, %llu requests behind schedule\n",
                format_duration(mean_lag).c_str(),
                format_duration(static_cast<double>(stats.dispatch_lag.max())).c_str(),
                static_cast<unsigned long long>(stats.behind_schedule));
    if (mean_lag > 10'000'000.0) {
      std::printf(
          "\n  warning: mean dispatch lag is %s, so hammer could not keep up with the\n"
          "  requested rate. These numbers describe hammer's own bottleneck, not the\n"
          "  server's. Lower --rate or add threads.\n",
          format_duration(mean_lag).c_str());
    }
  }
  std::fflush(stdout);
}

}
