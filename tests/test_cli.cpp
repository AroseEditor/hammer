#include "cli.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using hammer::Config;
using hammer::ParseOutcome;
using hammer::ParseResult;

namespace {

ParseResult parse(std::vector<std::string> args) {
  return hammer::parse_args(args);
}

Config ok(std::vector<std::string> args) {
  const ParseResult r = hammer::parse_args(args);
  REQUIRE(r.error.empty());
  REQUIRE(r.outcome == ParseOutcome::Ok);
  return r.config;
}

std::string rejection(std::vector<std::string> args) {
  const ParseResult r = hammer::parse_args(args);
  REQUIRE(r.outcome == ParseOutcome::Error);
  REQUIRE_FALSE(r.error.empty());
  return r.error;
}

bool mentions(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}

TEST_CASE("defaults match the documented ones") {
  const Config cfg = ok({"http://localhost:8080/"});
  REQUIRE(cfg.connections == 50);
  REQUIRE(cfg.threads == 4);
  REQUIRE(cfg.duration_s == 10);
  REQUIRE(cfg.timeout_ms == 2000);
  REQUIRE(cfg.method == "GET");
  REQUIRE(cfg.rate == 0);
  REQUIRE_FALSE(cfg.open_loop);
  REQUIRE_FALSE(cfg.json);
  REQUIRE_FALSE(cfg.print_latency);
  REQUIRE(cfg.headers.empty());
  REQUIRE(cfg.body.empty());
}

TEST_CASE("short and long forms set the same fields") {
  const Config a = ok({"-c", "20", "-t", "4", "-d", "30", "-m", "POST", "http://h/"});
  const Config b = ok({"--connections", "20", "--threads", "4", "--duration", "30",
                       "--method", "POST", "http://h/"});
  REQUIRE(a.connections == b.connections);
  REQUIRE(a.threads == b.threads);
  REQUIRE(a.duration_s == b.duration_s);
  REQUIRE(a.method == b.method);
  REQUIRE(a.connections == 20);
  REQUIRE(a.duration_s == 30);
  REQUIRE(a.method == "POST");
}

TEST_CASE("equals form is accepted for long flags") {
  const Config cfg = ok({"--connections=64", "--rate=1500", "http://h/"});
  REQUIRE(cfg.connections == 64);
  REQUIRE(cfg.rate == 1500);
}

TEST_CASE("rate turns on open loop and closed-loop overrides it") {
  const Config open = ok({"--rate", "20000", "http://h/"});
  REQUIRE(open.open_loop);
  REQUIRE(open.rate == 20000);

  const Config closed = ok({"--rate", "20000", "--closed-loop", "http://h/"});
  REQUIRE_FALSE(closed.open_loop);
  REQUIRE(closed.rate == 20000);

  const Config plain = ok({"--closed-loop", "http://h/"});
  REQUIRE_FALSE(plain.open_loop);
}

TEST_CASE("boolean flags") {
  const Config cfg = ok({"--latency", "--json", "http://h/"});
  REQUIRE(cfg.print_latency);
  REQUIRE(cfg.json);
}

TEST_CASE("repeated header flag keeps every header in order") {
  const Config cfg = ok({"-H", "A: 1", "-H", "B: 2", "-H", "C: 3", "-H", "D: 4", "http://h/"});
  REQUIRE(cfg.headers.size() == 4);
  REQUIRE(cfg.headers[0] == "A: 1");
  REQUIRE(cfg.headers[3] == "D: 4");
}

TEST_CASE("header without a colon is rejected") {
  REQUIRE(mentions(rejection({"-H", "nocolon", "http://h/"}), "-H"));
  REQUIRE(mentions(rejection({"-H", ": novalue", "http://h/"}), "-H"));
}

TEST_CASE("header carrying a line break is rejected") {
  REQUIRE(mentions(rejection({"-H", "X: a\r\nEvil: 1", "http://h/"}), "line break"));
}

TEST_CASE("help and version short-circuit before url validation") {
  REQUIRE(parse({"--help"}).outcome == ParseOutcome::HelpRequested);
  REQUIRE(parse({"-h"}).outcome == ParseOutcome::HelpRequested);
  REQUIRE(parse({"--version"}).outcome == ParseOutcome::VersionRequested);
  REQUIRE(parse({"--help", "not-a-url"}).outcome == ParseOutcome::HelpRequested);
}

TEST_CASE("missing url is an error") {
  REQUIRE(mentions(rejection({}), "missing <url>"));
  REQUIRE(mentions(rejection({"-c", "10"}), "missing <url>"));
}

TEST_CASE("a second positional argument is an error") {
  REQUIRE(mentions(rejection({"http://a/", "http://b/"}), "one url"));
}

TEST_CASE("unknown flags name themselves") {
  REQUIRE(mentions(rejection({"--turbo", "http://h/"}), "--turbo"));
  REQUIRE(mentions(rejection({"-z", "http://h/"}), "-z"));
}

TEST_CASE("missing value after a flag names the flag") {
  for (const std::string flag : {"-c", "--connections", "-t", "--threads", "-d", "--duration",
                                 "-r", "--rate", "--timeout", "-m", "--method", "-H",
                                 "--header", "--body"}) {
    const std::string error = rejection({flag});
    INFO("flag " << flag);
    REQUIRE(mentions(error, flag));
    REQUIRE(mentions(error, "expects a value"));
  }
}

TEST_CASE("rate zero is rejected") {
  REQUIRE(mentions(rejection({"--rate", "0", "http://h/"}), "--rate"));
}

TEST_CASE("non numeric and negative numbers are rejected") {
  REQUIRE(mentions(rejection({"-c", "abc", "http://h/"}), "-c"));
  REQUIRE(mentions(rejection({"-c", "-5", "http://h/"}), "-c"));
  REQUIRE(mentions(rejection({"-d", "10s", "http://h/"}), "-d"));
  REQUIRE(mentions(rejection({"--timeout", "1e3", "http://h/"}), "--timeout"));
}

TEST_CASE("zero connections threads duration and timeout are rejected") {
  REQUIRE(mentions(rejection({"-c", "0", "http://h/"}), "--connections"));
  REQUIRE(mentions(rejection({"-t", "0", "http://h/"}), "--threads"));
  REQUIRE(mentions(rejection({"-d", "0", "http://h/"}), "--duration"));
  REQUIRE(mentions(rejection({"--timeout", "0", "http://h/"}), "--timeout"));
}

TEST_CASE("connections below threads is an error not a clamp") {
  const std::string error = rejection({"-c", "2", "-t", "8", "http://h/"});
  REQUIRE(mentions(error, "--connections"));
  REQUIRE(mentions(error, "--threads"));
}

TEST_CASE("connections equal to threads is fine") {
  const Config cfg = ok({"-c", "8", "-t", "8", "http://h/"});
  REQUIRE(cfg.connections == 8);
  REQUIRE(cfg.threads == 8);
}

TEST_CASE("method must be a token") {
  REQUIRE(mentions(rejection({"-m", "GET POST", "http://h/"}), "-m"));
  REQUIRE(ok({"-m", "PATCH", "http://h/"}).method == "PATCH");
}

TEST_CASE("body is read from a file and a missing file is an error") {
  const std::string path = "hammer_test_body.txt";
  {
    std::ofstream out{path, std::ios::binary};
    out << "field=value&other=1";
  }
  const Config cfg = ok({"--body", path, "http://h/"});
  REQUIRE(cfg.body == "field=value&other=1");
  std::remove(path.c_str());

  REQUIRE(mentions(rejection({"--body", "no_such_file_here.bin", "http://h/"}), "--body"));
}

TEST_CASE("url splits into host port and path") {
  hammer::Url url;
  std::string error;

  REQUIRE(hammer::parse_url("http://localhost:8080/api/v1?x=1", url, error));
  REQUIRE(url.host == "localhost");
  REQUIRE(url.port == 8080);
  REQUIRE(url.path == "/api/v1?x=1");

  REQUIRE(hammer::parse_url("http://example.com", url, error));
  REQUIRE(url.host == "example.com");
  REQUIRE(url.port == 80);
  REQUIRE(url.path == "/");

  REQUIRE(hammer::parse_url("HTTP://Example.com/x", url, error));
  REQUIRE(url.host == "Example.com");
  REQUIRE(url.path == "/x");

  REQUIRE(hammer::parse_url("http://[::1]:9000/health", url, error));
  REQUIRE(url.host == "::1");
  REQUIRE(url.port == 9000);
  REQUIRE(url.path == "/health");

  REQUIRE(hammer::parse_url("http://h/x#frag", url, error));
  REQUIRE(url.path == "/x");
}

TEST_CASE("url rejections say what is wrong") {
  hammer::Url url;
  std::string error;

  REQUIRE_FALSE(hammer::parse_url("https://example.com/", url, error));
  REQUIRE(mentions(error, "https"));

  REQUIRE_FALSE(hammer::parse_url("ftp://example.com/", url, error));
  REQUIRE(mentions(error, "ftp"));

  REQUIRE_FALSE(hammer::parse_url("example.com/", url, error));
  REQUIRE(mentions(error, "scheme"));

  REQUIRE_FALSE(hammer::parse_url("http:///path", url, error));
  REQUIRE(mentions(error, "no host"));

  REQUIRE_FALSE(hammer::parse_url("http://user:pw@example.com/", url, error));
  REQUIRE(mentions(error, "userinfo"));

  REQUIRE_FALSE(hammer::parse_url("http://[::1/x", url, error));
  REQUIRE(mentions(error, "ipv6"));
}

TEST_CASE("port out of range is rejected") {
  hammer::Url url;
  std::string error;

  REQUIRE_FALSE(hammer::parse_url("http://h:0/", url, error));
  REQUIRE(mentions(error, "1-65535"));

  REQUIRE_FALSE(hammer::parse_url("http://h:65536/", url, error));
  REQUIRE(mentions(error, "1-65535"));

  REQUIRE_FALSE(hammer::parse_url("http://h:99999999999999999999/", url, error));
  REQUIRE_FALSE(hammer::parse_url("http://h:http/", url, error));

  REQUIRE(hammer::parse_url("http://h:65535/", url, error));
  REQUIRE(url.port == 65535);
}

TEST_CASE("bad urls flow out of parse_args as errors") {
  REQUIRE(mentions(rejection({"https://example.com/"}), "https"));
  REQUIRE(mentions(rejection({"-c", "10", "-t", "2", "nope"}), "scheme"));
}

TEST_CASE("usage text names the binary and the url argument") {
  const auto usage = hammer::usage_text();
  REQUIRE(usage.find("hammer [options] <url>") != std::string_view::npos);
  REQUIRE(usage.find("--connections") != std::string_view::npos);
}

TEST_CASE("version string is baked in at configure time") {
  REQUIRE_FALSE(hammer::version_string().empty());
}
