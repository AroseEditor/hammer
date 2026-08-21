#include "cli.h"

namespace hammer {

std::string_view usage_text() {
  return
    "hammer [options] <url>\n"
    "\n"
    "  -c, --connections N    total connections           (default 50)\n"
    "  -t, --threads N        worker threads              (default 4)\n"
    "  -d, --duration S       test duration in seconds    (default 10)\n"
    "  -r, --rate N           target req/s; turns on open-loop mode\n"
    "      --closed-loop      closed loop even with --rate set\n"
    "  -m, --method M         HTTP method                 (default GET)\n"
    "  -H, --header \"K: V\"    extra header, repeatable\n"
    "      --body FILE        request body from file\n"
    "      --timeout MS       per-request timeout         (default 2000)\n"
    "      --latency          print the full percentile spectrum\n"
    "      --json             machine-readable output\n"
    "      --version          print version and exit\n"
    "  -h, --help             this message\n";
}

std::string_view version_string() {
  return HAMMER_VERSION_STRING;
}

}
