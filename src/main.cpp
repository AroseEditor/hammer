#include "cli.h"
#include "loop.h"
#include "net_compat.h"
#include "signals.h"
#include "stats.h"

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

  const hammer::net::Startup network;
  hammer::net::ignore_sigpipe();
  hammer::install_signal_handlers();

  const hammer::Config& effective = parsed.config;

  hammer::print_banner(effective);
  std::fputs("  (open-loop scheduling is not wired up yet; running closed loop)\n", stderr);

  const hammer::RunResult result = hammer::run_event_loop(effective);
  if (!result.ok) {
    std::fprintf(stderr, "hammer: %s\n", result.error.c_str());
    return 1;
  }

  hammer::print_report(effective, result.stats, result.elapsed_seconds);
  return 0;
}
