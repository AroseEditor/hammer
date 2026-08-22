#include "cli.h"
#include "conn.h"
#include "loop.h"
#include "mock_server.h"
#include "net_compat.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using hammer::Config;
using hammer::ParseOutcome;
using hammer::RunResult;
using hammer::testing::MockServer;
using hammer::testing::ServerOptions;

namespace {

const hammer::net::Startup g_network;

Config config_for(const std::string& url, std::vector<std::string> extra = {}) {
  std::vector<std::string> args = {"-c", "1", "-t", "1", "-d", "1"};
  args.insert(args.end(), extra.begin(), extra.end());
  args.push_back(url);

  const hammer::ParseResult parsed = hammer::parse_args(args);
  REQUIRE(parsed.error.empty());
  REQUIRE(parsed.outcome == ParseOutcome::Ok);
  return parsed.config;
}

}

TEST_CASE("the blocking client drives a keep-alive server end to end") {
  MockServer server{ServerOptions{}};
  const RunResult result = hammer::run_blocking(config_for(server.url()));

  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 0);
  REQUIRE(result.stats.latency.count() == result.stats.requests);
  REQUIRE(result.stats.non_2xx == 0);
  REQUIRE(result.stats.connect_errors == 0);
  REQUIRE(result.stats.read_errors == 0);
  REQUIRE(result.stats.write_errors == 0);
  REQUIRE(result.stats.timeouts == 0);
  REQUIRE(result.stats.bytes_read >= result.stats.requests * 50);
  REQUIRE(result.elapsed_seconds >= 1.0);
  REQUIRE(result.stats.latency.max() > 0);
  REQUIRE(server.requests_served() == result.stats.requests);
}

TEST_CASE("a chunked server is driven end to end") {
  ServerOptions options;
  options.response =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  MockServer server{options};

  const RunResult result = hammer::run_blocking(config_for(server.url()));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 0);
  REQUIRE(result.stats.read_errors == 0);
  REQUIRE(result.stats.non_2xx == 0);
}

TEST_CASE("connection close responses reconnect without counting errors") {
  ServerOptions options;
  options.response =
      "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nhi";
  options.close_after_response = true;
  MockServer server{options};

  const RunResult result = hammer::run_blocking(config_for(server.url()));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 1);
  REQUIRE(result.stats.read_errors == 0);
  REQUIRE(result.stats.connect_errors == 0);
}

TEST_CASE("a server that closes a keep-alive connection early is not an error") {
  ServerOptions options;
  options.responses_before_close = 3;
  MockServer server{options};

  const RunResult result = hammer::run_blocking(config_for(server.url()));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 3);
  REQUIRE(result.stats.read_errors == 0);
}

TEST_CASE("non 2xx responses are counted separately") {
  ServerOptions options;
  options.response = "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nnah";
  MockServer server{options};

  const RunResult result = hammer::run_blocking(config_for(server.url()));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 0);
  REQUIRE(result.stats.non_2xx == result.stats.requests);
}

TEST_CASE("a server that closes before responding is a read error, not a hang") {
  ServerOptions options;
  options.close_before_response = true;
  MockServer server{options};

  const RunResult result = hammer::run_blocking(config_for(server.url()));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests == 0);
  REQUIRE(result.stats.read_errors > 0);
  REQUIRE(result.stats.latency.count() == 0);
}

TEST_CASE("a server that never responds produces timeouts, not a hang") {
  ServerOptions options;
  options.never_respond = true;
  MockServer server{options};

  const RunResult result =
      hammer::run_blocking(config_for(server.url(), {"--timeout", "200"}));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests == 0);
  REQUIRE(result.stats.timeouts > 0);
  REQUIRE(result.stats.latency.count() == 0);
  REQUIRE(result.elapsed_seconds < 3.0);
}

TEST_CASE("garbage before the status line is counted as a read error") {
  ServerOptions options;
  options.response = "not http at all\r\n\r\n";
  MockServer server{options};

  const RunResult result = hammer::run_blocking(config_for(server.url()));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests == 0);
  REQUIRE(result.stats.read_errors > 0);
}

TEST_CASE("a refused connection gives up instead of spinning for the full duration") {
  const std::string url =
      "http://127.0.0.1:" + std::to_string(hammer::testing::reserve_closed_port()) + "/";
  const RunResult result = hammer::run_blocking(config_for(url));

  REQUIRE(result.stats.requests == 0);
  REQUIRE(result.stats.connect_errors > 0);
  REQUIRE(result.elapsed_seconds < 5.0);
  if (result.stats.connect_errors >= 10) {
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("connect") != std::string::npos);
  }
}

TEST_CASE("an unresolvable host fails before any socket work") {
  const RunResult result =
      hammer::run_blocking(config_for("http://no-such-host.invalid.example/"));
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error.find("resolve") != std::string::npos);
  REQUIRE(result.stats.requests == 0);
}

TEST_CASE("the request line and default headers are built correctly") {
  const Config config = config_for("http://example.com/api?x=1");
  const std::string request = hammer::build_request(config);

  REQUIRE(request.rfind("GET /api?x=1 HTTP/1.1\r\n", 0) == 0);
  REQUIRE(request.find("Host: example.com\r\n") != std::string::npos);
  REQUIRE(request.find("User-Agent: hammer/") != std::string::npos);
  REQUIRE(request.find("Accept: */*\r\n") != std::string::npos);
  REQUIRE(request.substr(request.size() - 4) == "\r\n\r\n");
}

TEST_CASE("a non default port appears in the host header") {
  const Config config = config_for("http://example.com:8080/");
  REQUIRE(hammer::build_request(config).find("Host: example.com:8080\r\n") != std::string::npos);
}

TEST_CASE("user supplied headers replace the defaults they duplicate") {
  const Config config = config_for(
      "http://example.com/", {"-H", "Host: override.example", "-H", "X-Trace: abc"});
  const std::string request = hammer::build_request(config);

  REQUIRE(request.find("Host: override.example\r\n") != std::string::npos);
  REQUIRE(request.find("Host: example.com\r\n") == std::string::npos);
  REQUIRE(request.find("X-Trace: abc\r\n") != std::string::npos);
}

TEST_CASE("a body gets a content length and lands after the headers") {
  Config config = config_for("http://example.com/");
  config.method = "POST";
  config.body = "a=1&b=2";

  const std::string request = hammer::build_request(config);
  REQUIRE(request.rfind("POST / HTTP/1.1\r\n", 0) == 0);
  REQUIRE(request.find("Content-Length: 7\r\n") != std::string::npos);
  REQUIRE(request.substr(request.size() - 11) == "\r\n\r\na=1&b=2");
}

TEST_CASE("the event loop drives a keep-alive server end to end") {
  MockServer server{ServerOptions{}};
  const RunResult result = hammer::run_event_loop(config_for(server.url()));

  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 0);
  REQUIRE(result.stats.latency.count() == result.stats.requests);
  REQUIRE(result.stats.non_2xx == 0);
  REQUIRE(result.stats.connect_errors == 0);
  REQUIRE(result.stats.read_errors == 0);
  REQUIRE(result.stats.write_errors == 0);
  REQUIRE(result.stats.bytes_read >= result.stats.requests * 50);
  REQUIRE(server.requests_served() >= result.stats.requests);
  REQUIRE(server.requests_served() <= result.stats.requests + 1);
}

TEST_CASE("the event loop beats the blocking client on the same server") {
  MockServer server{ServerOptions{}};

  const RunResult blocking = hammer::run_blocking(config_for(server.url()));
  const RunResult evented = hammer::run_event_loop(config_for(server.url(), {"-c", "16"}));

  REQUIRE(blocking.ok);
  REQUIRE(evented.ok);
  REQUIRE(evented.stats.requests > blocking.stats.requests);
  REQUIRE(evented.stats.read_errors == 0);
}

TEST_CASE("many connections all make progress") {
  MockServer server{ServerOptions{}};
  const RunResult result = hammer::run_event_loop(config_for(server.url(), {"-c", "32"}));

  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 32);
  REQUIRE(result.stats.read_errors == 0);
  REQUIRE(result.stats.connect_errors == 0);
}

TEST_CASE("the event loop handles chunked responses") {
  ServerOptions options;
  options.response =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  MockServer server{options};

  const RunResult result = hammer::run_event_loop(config_for(server.url(), {"-c", "8"}));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 0);
  REQUIRE(result.stats.read_errors == 0);
  REQUIRE(result.stats.non_2xx == 0);
}

TEST_CASE("the event loop reconnects after connection close responses") {
  ServerOptions options;
  options.response = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nhi";
  options.close_after_response = true;
  MockServer server{options};

  const RunResult result = hammer::run_event_loop(config_for(server.url(), {"-c", "4"}));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 4);
  REQUIRE(result.stats.read_errors == 0);
}

TEST_CASE("the event loop treats an early keep-alive close as a reconnect") {
  ServerOptions options;
  options.responses_before_close = 3;
  MockServer server{options};

  const RunResult result = hammer::run_event_loop(config_for(server.url(), {"-c", "4"}));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 12);
  REQUIRE(result.stats.read_errors == 0);
}

TEST_CASE("the event loop counts garbage as a read error without hanging") {
  ServerOptions options;
  options.response = "not http at all\r\n\r\n";
  MockServer server{options};

  const RunResult result = hammer::run_event_loop(config_for(server.url(), {"-c", "4"}));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests == 0);
  REQUIRE(result.stats.read_errors > 0);
  REQUIRE(result.elapsed_seconds < 4.0);
}

TEST_CASE("the event loop survives a server that never responds") {
  ServerOptions options;
  options.never_respond = true;
  MockServer server{options};

  const RunResult result = hammer::run_event_loop(config_for(server.url(), {"-c", "4"}));
  REQUIRE(result.ok);
  REQUIRE(result.stats.requests == 0);
  REQUIRE(result.elapsed_seconds < 4.0);
}

TEST_CASE("the event loop gives up on a refused port instead of spinning") {
  const std::string url =
      "http://127.0.0.1:" + std::to_string(hammer::testing::reserve_closed_port()) + "/";
  const RunResult result =
      hammer::run_event_loop(config_for(url, {"-c", "4", "--timeout", "200"}));

  REQUIRE(result.stats.requests == 0);
  REQUIRE(result.stats.connect_errors > 0);
  REQUIRE(result.elapsed_seconds < 5.0);
}

TEST_CASE("connections are sharded across worker threads") {
  MockServer server{ServerOptions{}};
  const RunResult result =
      hammer::run_event_loop(config_for(server.url(), {"-c", "16", "-t", "4"}));

  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 16);
  REQUIRE(result.stats.latency.count() == result.stats.requests);
  REQUIRE(result.stats.read_errors == 0);
  REQUIRE(result.stats.connect_errors == 0);
}

TEST_CASE("more threads than connections does not starve a thread") {
  MockServer server{ServerOptions{}};
  const RunResult result =
      hammer::run_event_loop(config_for(server.url(), {"-c", "3", "-t", "3"}));

  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 3);
  REQUIRE(result.stats.read_errors == 0);
}

TEST_CASE("an uneven shard split still uses every connection") {
  MockServer server{ServerOptions{}};
  const RunResult result =
      hammer::run_event_loop(config_for(server.url(), {"-c", "7", "-t", "3"}));

  REQUIRE(result.ok);
  REQUIRE(result.stats.requests > 7);
  REQUIRE(result.stats.read_errors == 0);
}
