#include "cli.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  const hammer::ParseResult parsed = hammer::parse_args(args);

  switch (parsed.outcome) {
    case hammer::ParseOutcome::HelpRequested:
      std::fputs(std::string(hammer::usage_text()).c_str(), stdout);
      return 0;
    case hammer::ParseOutcome::VersionRequested:
      std::printf("hammer %s\n", std::string(hammer::version_string()).c_str());
      return 0;
    case hammer::ParseOutcome::Error:
      std::fprintf(stderr, "hammer: %s\n", parsed.error.c_str());
      std::fputs(std::string(hammer::usage_text()).c_str(), stderr);
      return 1;
    case hammer::ParseOutcome::Ok:
      break;
  }

  const hammer::Config& cfg = parsed.config;
  std::printf("Running %ds test @ %s\n", cfg.duration_s, cfg.raw_url.c_str());
  if (cfg.open_loop) {
    std::printf("  %d threads, %d connections, open loop @ %llu req/s\n", cfg.threads,
                cfg.connections, static_cast<unsigned long long>(cfg.rate));
  } else {
    std::printf("  %d threads, %d connections, closed loop\n", cfg.threads, cfg.connections);
  }
  std::fputs("hammer: load generation is not wired up yet\n", stderr);
  return 0;
}
