#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hammer {

struct Url {
  std::string host;
  std::string path;
  uint16_t port = 80;
};

struct Config {
  Url url;
  std::string raw_url;
  std::string method = "GET";
  std::string body;
  std::vector<std::string> headers;
  int connections = 50;
  int threads = 4;
  int duration_s = 10;
  uint64_t rate = 0;
  int timeout_ms = 2000;
  bool open_loop = false;
  bool print_latency = false;
  bool json = false;
};

enum class ParseOutcome { Ok, HelpRequested, VersionRequested, Error };

struct ParseResult {
  ParseOutcome outcome = ParseOutcome::Error;
  Config config;
  std::string error;
};

ParseResult parse_args(const std::vector<std::string>& args);
bool parse_url(std::string_view raw, Url& out, std::string& error);

std::string_view usage_text();
std::string_view version_string();

}
