#include "cli.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace hammer {
namespace {

bool is_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool parse_u64(std::string_view s, uint64_t& out) {
  if (!is_digits(s)) return false;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(first, last, out);
  return ec == std::errc{} && ptr == last;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
    const char cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
    if (ca != cb) return false;
  }
  return true;
}

bool is_token(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '!' || c == '*';
    if (!ok) return false;
  }
  return true;
}

std::string quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  out.append(s);
  out.push_back('"');
  return out;
}

}

bool parse_url(std::string_view raw, Url& out, std::string& error) {
  if (raw.empty()) {
    error = "url is empty";
    return false;
  }

  const size_t scheme_end = raw.find("://");
  if (scheme_end == std::string_view::npos) {
    error = "url " + quote(raw) + " has no scheme; hammer needs an http:// url";
    return false;
  }

  const std::string_view scheme = raw.substr(0, scheme_end);
  if (!iequals(scheme, "http")) {
    if (iequals(scheme, "https")) {
      error = "url " + quote(raw) + " uses https; hammer speaks plain http only";
    } else {
      error = "url " + quote(raw) + " uses scheme " + quote(scheme) + "; only http:// is supported";
    }
    return false;
  }

  std::string_view rest = raw.substr(scheme_end + 3);
  const size_t fragment = rest.find('#');
  if (fragment != std::string_view::npos) rest = rest.substr(0, fragment);

  const size_t path_start = rest.find('/');
  const std::string_view authority = rest.substr(0, path_start);
  out.path = path_start == std::string_view::npos ? std::string("/") : std::string(rest.substr(path_start));
  if (out.path.empty()) out.path = "/";

  if (authority.find('@') != std::string_view::npos) {
    error = "url " + quote(raw) + " carries userinfo; hammer does not do http auth";
    return false;
  }

  std::string_view port_text;
  if (!authority.empty() && authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == std::string_view::npos) {
      error = "url " + quote(raw) + " has an unterminated ipv6 literal";
      return false;
    }
    out.host = std::string(authority.substr(1, close - 1));
    const std::string_view tail = authority.substr(close + 1);
    if (!tail.empty()) {
      if (tail.front() != ':') {
        error = "url " + quote(raw) + " has trailing junk after the ipv6 literal";
        return false;
      }
      port_text = tail.substr(1);
    }
  } else {
    const size_t colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
      out.host = std::string(authority);
    } else {
      out.host = std::string(authority.substr(0, colon));
      port_text = authority.substr(colon + 1);
    }
  }

  if (out.host.empty()) {
    error = "url " + quote(raw) + " has no host";
    return false;
  }

  if (port_text.empty()) {
    out.port = 80;
    return true;
  }

  uint64_t port = 0;
  if (!parse_u64(port_text, port) || port == 0 || port > 65535) {
    error = "url " + quote(raw) + " has port " + quote(port_text) + "; expected 1-65535";
    return false;
  }
  out.port = static_cast<uint16_t>(port);
  return true;
}

ParseResult parse_args(const std::vector<std::string>& args) {
  ParseResult result;
  Config& cfg = result.config;
  bool rate_set = false;
  bool force_closed_loop = false;
  bool url_seen = false;

  auto fail = [&](std::string message) -> ParseResult& {
    result.outcome = ParseOutcome::Error;
    result.error = std::move(message);
    return result;
  };

  for (size_t i = 0; i < args.size(); ++i) {
    std::string_view arg = args[i];
    std::string_view inline_value;
    bool has_inline = false;

    if (arg.size() > 2 && arg.rfind("--", 0) == 0) {
      const size_t eq = arg.find('=');
      if (eq != std::string_view::npos) {
        inline_value = arg.substr(eq + 1);
        arg = arg.substr(0, eq);
        has_inline = true;
      }
    }

    auto take_value = [&](std::string_view& out) -> bool {
      if (has_inline) {
        out = inline_value;
        return true;
      }
      if (i + 1 >= args.size()) return false;
      out = args[++i];
      return true;
    };

    if (arg == "-h" || arg == "--help") {
      result.outcome = ParseOutcome::HelpRequested;
      return result;
    }
    if (arg == "--version") {
      result.outcome = ParseOutcome::VersionRequested;
      return result;
    }

    if (arg == "-c" || arg == "--connections" || arg == "-t" || arg == "--threads" ||
        arg == "-d" || arg == "--duration" || arg == "-r" || arg == "--rate" ||
        arg == "--timeout") {
      std::string_view value;
      if (!take_value(value)) return fail(std::string(arg) + " expects a value");
      uint64_t n = 0;
      if (!parse_u64(value, n)) {
        return fail(std::string(arg) + " wants a non-negative integer, got " + quote(value));
      }
      if (arg == "-c" || arg == "--connections") {
        if (n < 1 || n > 1000000) return fail("--connections must be 1-1000000, got " + quote(value));
        cfg.connections = static_cast<int>(n);
      } else if (arg == "-t" || arg == "--threads") {
        if (n < 1 || n > 4096) return fail("--threads must be 1-4096, got " + quote(value));
        cfg.threads = static_cast<int>(n);
      } else if (arg == "-d" || arg == "--duration") {
        if (n < 1 || n > 86400) return fail("--duration must be 1-86400 seconds, got " + quote(value));
        cfg.duration_s = static_cast<int>(n);
      } else if (arg == "-r" || arg == "--rate") {
        if (n < 1) return fail("--rate must be at least 1 req/s, got " + quote(value));
        cfg.rate = n;
        rate_set = true;
      } else {
        if (n < 1 || n > 3600000) return fail("--timeout must be 1-3600000 ms, got " + quote(value));
        cfg.timeout_ms = static_cast<int>(n);
      }
      continue;
    }

    if (arg == "-m" || arg == "--method") {
      std::string_view value;
      if (!take_value(value)) return fail(std::string(arg) + " expects a value");
      if (!is_token(value)) {
        return fail(std::string(arg) + " got " + quote(value) + ", which is not a valid http method");
      }
      cfg.method.assign(value);
      continue;
    }

    if (arg == "-H" || arg == "--header") {
      std::string_view value;
      if (!take_value(value)) return fail(std::string(arg) + " expects a value");
      const size_t colon = value.find(':');
      if (colon == std::string_view::npos || colon == 0) {
        return fail(std::string(arg) + " got " + quote(value) + ", expected Key: Value");
      }
      if (value.find('\r') != std::string_view::npos || value.find('\n') != std::string_view::npos) {
        return fail(std::string(arg) + " got a header containing a line break");
      }
      cfg.headers.emplace_back(value);
      continue;
    }

    if (arg == "--body") {
      std::string_view value;
      if (!take_value(value)) return fail(std::string(arg) + " expects a value");
      std::ifstream file{std::string(value), std::ios::binary};
      if (!file) return fail("--body cannot read file " + quote(value));
      std::ostringstream buffer;
      buffer << file.rdbuf();
      cfg.body = buffer.str();
      continue;
    }

    if (arg == "--closed-loop") {
      force_closed_loop = true;
      continue;
    }
    if (arg == "--latency") {
      cfg.print_latency = true;
      continue;
    }
    if (arg == "--json") {
      cfg.json = true;
      continue;
    }

    if (!arg.empty() && arg.front() == '-' && arg != "-") {
      return fail("unknown flag " + quote(arg));
    }

    if (url_seen) {
      return fail("unexpected extra argument " + quote(arg) + "; hammer takes one url");
    }
    cfg.raw_url.assign(arg);
    url_seen = true;
  }

  if (!url_seen) return fail("missing <url>");

  std::string url_error;
  if (!parse_url(cfg.raw_url, cfg.url, url_error)) return fail(std::move(url_error));

  if (cfg.connections < cfg.threads) {
    return fail("--connections " + std::to_string(cfg.connections) +
                " is below --threads " + std::to_string(cfg.threads) +
                "; every thread needs at least one connection");
  }

  cfg.paced = rate_set;
  cfg.open_loop = rate_set && !force_closed_loop;
  result.outcome = ParseOutcome::Ok;
  return result;
}

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
