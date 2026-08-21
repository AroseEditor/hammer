#include "http_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using hammer::ResponseParser;
using hammer::Result;
using hammer::State;

namespace {

struct Outcome {
  bool complete = false;
  bool error = false;
  int status = 0;
  uint64_t body = 0;
  size_t consumed = 0;
  bool keep_alive = true;
  std::string error_message;

  bool operator==(const Outcome& other) const {
    return complete == other.complete && error == other.error && status == other.status &&
           body == other.body && consumed == other.consumed && keep_alive == other.keep_alive;
  }
};

Outcome drive(const std::string& response, const std::vector<size_t>& split_points,
              bool head_request = false, bool close_at_end = false) {
  ResponseParser parser;
  parser.reset(head_request);

  Outcome outcome;
  size_t offset = 0;
  size_t next_split = 0;

  while (offset < response.size()) {
    size_t end = response.size();
    while (next_split < split_points.size() && split_points[next_split] <= offset) ++next_split;
    if (next_split < split_points.size()) end = split_points[next_split];

    const Result result = parser.feed({response.data() + offset, end - offset});
    outcome.consumed += result.consumed;
    if (result.error) {
      outcome.error = true;
      break;
    }
    if (result.message_complete) {
      outcome.complete = true;
      break;
    }
    if (result.consumed == 0 && end == offset) break;
    offset = end;
  }

  if (close_at_end && !outcome.complete && !outcome.error) {
    const Result result = parser.eof();
    outcome.complete = result.message_complete;
    outcome.error = result.error;
  }

  outcome.status = parser.status_code();
  outcome.body = parser.body_bytes();
  outcome.keep_alive = parser.keep_alive();
  outcome.error_message = parser.error_message();
  return outcome;
}

Outcome one_shot(const std::string& response, bool head_request = false,
                 bool close_at_end = false) {
  return drive(response, {}, head_request, close_at_end);
}

Outcome byte_at_a_time(const std::string& response, bool head_request = false,
                       bool close_at_end = false) {
  std::vector<size_t> splits;
  splits.reserve(response.size());
  for (size_t i = 1; i < response.size(); ++i) splits.push_back(i);
  return drive(response, splits, head_request, close_at_end);
}

const std::string kSimple =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "\r\n"
    "hello, world!";

const std::string kChunked =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "5\r\nhello\r\n"
    "6\r\n world\r\n"
    "0\r\n"
    "\r\n";

const std::string kChunkedWithTrailers =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "Trailer: Expires\r\n"
    "\r\n"
    "4\r\nWiki\r\n"
    "5\r\npedia\r\n"
    "0\r\n"
    "Expires: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
    "\r\n";

const std::string kOneByteChunks =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "1\r\na\r\n1\r\nb\r\n1\r\nc\r\n1\r\nd\r\n0\r\n\r\n";

const std::string kChunkExtensions =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "5;name=value\r\nhello\r\n"
    "0;last\r\n"
    "\r\n";

const std::string kBareLf =
    "HTTP/1.1 200 OK\n"
    "Content-Length: 4\n"
    "\n"
    "body";

const std::string kUntilClose =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "streamed bytes with no length";

const std::string kNoContent =
    "HTTP/1.1 204 No Content\r\n"
    "\r\n";

const std::string kNotModified =
    "HTTP/1.1 304 Not Modified\r\n"
    "Content-Length: 4096\r\n"
    "\r\n";

const std::string kMixedCaseHeaders =
    "HTTP/1.1 200 OK\r\n"
    "cOnTeNt-LeNgTh: 3\r\n"
    "CONNECTION: Close\r\n"
    "\r\n"
    "abc";

std::vector<std::string> corpus() {
  return {kSimple,
          kChunked,
          kChunkedWithTrailers,
          kOneByteChunks,
          kChunkExtensions,
          kBareLf,
          kNoContent,
          kNotModified,
          kMixedCaseHeaders,
          "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n",
          "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot here!",
          "HTTP/1.1 500 \r\nContent-Length: 1\r\n\r\nx"};
}

}

TEST_CASE("content length body is parsed in one shot") {
  const Outcome out = one_shot(kSimple);
  REQUIRE(out.complete);
  REQUIRE_FALSE(out.error);
  REQUIRE(out.status == 200);
  REQUIRE(out.body == 13);
  REQUIRE(out.consumed == kSimple.size());
  REQUIRE(out.keep_alive);
}

TEST_CASE("chunked body is decoded and framing bytes are not counted") {
  const Outcome out = one_shot(kChunked);
  REQUIRE(out.complete);
  REQUIRE(out.body == 11);
  REQUIRE(out.consumed == kChunked.size());
}

TEST_CASE("chunked trailers are consumed") {
  const Outcome out = one_shot(kChunkedWithTrailers);
  REQUIRE(out.complete);
  REQUIRE(out.body == 9);
  REQUIRE(out.consumed == kChunkedWithTrailers.size());
}

TEST_CASE("chunk extensions are ignored") {
  const Outcome out = one_shot(kChunkExtensions);
  REQUIRE(out.complete);
  REQUIRE(out.body == 5);
}

TEST_CASE("bare LF terminated header lines are accepted") {
  const Outcome out = one_shot(kBareLf);
  REQUIRE(out.complete);
  REQUIRE(out.status == 200);
  REQUIRE(out.body == 4);
}

TEST_CASE("a body with no length runs until the connection closes") {
  const Outcome out = one_shot(kUntilClose, false, true);
  REQUIRE(out.complete);
  REQUIRE_FALSE(out.error);
  REQUIRE(out.body == 29);
  REQUIRE_FALSE(out.keep_alive);
}

TEST_CASE("204 and 304 have no body regardless of headers") {
  const Outcome no_content = one_shot(kNoContent);
  REQUIRE(no_content.complete);
  REQUIRE(no_content.status == 204);
  REQUIRE(no_content.body == 0);

  const Outcome not_modified = one_shot(kNotModified);
  REQUIRE(not_modified.complete);
  REQUIRE(not_modified.status == 304);
  REQUIRE(not_modified.body == 0);
  REQUIRE(not_modified.consumed == kNotModified.size());
}

TEST_CASE("a HEAD response has no body even with content length") {
  const std::string head_response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: 1024\r\n"
      "\r\n";
  const Outcome out = one_shot(head_response, true);
  REQUIRE(out.complete);
  REQUIRE(out.body == 0);
  REQUIRE(out.consumed == head_response.size());
}

TEST_CASE("header names are case insensitive") {
  const Outcome out = one_shot(kMixedCaseHeaders);
  REQUIRE(out.complete);
  REQUIRE(out.body == 3);
  REQUIRE_FALSE(out.keep_alive);
}

TEST_CASE("http 1.0 defaults to closing and keep-alive opts back in") {
  const Outcome plain = one_shot("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
  REQUIRE(plain.complete);
  REQUIRE_FALSE(plain.keep_alive);

  const Outcome kept = one_shot("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n");
  REQUIRE(kept.complete);
  REQUIRE(kept.keep_alive);
}

TEST_CASE("feeding one byte at a time matches feeding everything at once") {
  for (const std::string& response : corpus()) {
    INFO("response: " << response);
    REQUIRE(byte_at_a_time(response) == one_shot(response));
  }
  REQUIRE(byte_at_a_time(kUntilClose, false, true) == one_shot(kUntilClose, false, true));
}

TEST_CASE("random split points match feeding everything at once") {
  std::mt19937 rng{0xC0FFEEu};
  const std::vector<std::string> responses = corpus();

  for (int iteration = 0; iteration < 1000; ++iteration) {
    const std::string& response = responses[rng() % responses.size()];
    const Outcome expected = one_shot(response);

    std::vector<size_t> splits;
    const size_t count = rng() % 8 + 1;
    for (size_t i = 0; i < count; ++i) splits.push_back(rng() % response.size());
    std::sort(splits.begin(), splits.end());
    splits.erase(std::unique(splits.begin(), splits.end()), splits.end());

    INFO("iteration " << iteration << " response: " << response);
    REQUIRE(drive(response, splits) == expected);
  }
}

TEST_CASE("a split inside a header name is survivable") {
  const std::string response = kSimple;
  const size_t inside_content_length = response.find("Content-Length") + 4;
  const Outcome out = drive(response, {inside_content_length});
  REQUIRE(out == one_shot(response));
}

TEST_CASE("negative content length is rejected") {
  const Outcome out = one_shot("HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\n");
  REQUIRE(out.error);
  REQUIRE(out.error_message.find("Content-Length") != std::string::npos);
}

TEST_CASE("content length repeated with different values is rejected") {
  const Outcome out = one_shot("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 7\r\n\r\n");
  REQUIRE(out.error);

  const Outcome same = one_shot("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nab");
  REQUIRE(same.complete);
  REQUIRE(same.body == 2);
}

TEST_CASE("chunked plus content length is rejected") {
  const Outcome out = one_shot(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n");
  REQUIRE(out.error);
}

TEST_CASE("an overflowing chunk size is rejected") {
  const Outcome out = one_shot(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFF\r\n");
  REQUIRE(out.error);

  const Outcome not_hex = one_shot(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");
  REQUIRE(not_hex.error);
}

TEST_CASE("a header line past 8kb is rejected") {
  std::string response = "HTTP/1.1 200 OK\r\nX-Big: ";
  response.append(9000, 'a');
  response += "\r\nContent-Length: 0\r\n\r\n";
  const Outcome out = one_shot(response);
  REQUIRE(out.error);
  REQUIRE(out.error_message.find("8192") != std::string::npos);

  std::string just_under = "HTTP/1.1 200 OK\r\nX-Big: ";
  just_under.append(8000, 'a');
  just_under += "\r\nContent-Length: 0\r\n\r\n";
  REQUIRE(one_shot(just_under).complete);
}

TEST_CASE("more than 100 headers is rejected") {
  std::string response = "HTTP/1.1 200 OK\r\n";
  for (int i = 0; i < 120; ++i) {
    response += "X-Pad-" + std::to_string(i) + ": 1\r\n";
  }
  response += "Content-Length: 0\r\n\r\n";
  const Outcome out = one_shot(response);
  REQUIRE(out.error);
  REQUIRE(out.error_message.find("100") != std::string::npos);
}

TEST_CASE("garbage before the status line is rejected") {
  REQUIRE(one_shot("<html>oops</html>\r\n\r\n").error);
  REQUIRE(one_shot("HTTP/9.9 200 OK\r\n\r\n").error);
  REQUIRE(one_shot("HTTP/1.1 99 Nope\r\n\r\n").error);
  REQUIRE(one_shot("HTTP/1.1 20 OK\r\n\r\n").error);
  REQUIRE(one_shot("HTTP/1.1 200OK\r\n\r\n").error);
  REQUIRE(one_shot("HTTP/1.1\r\n\r\n").error);
}

TEST_CASE("malformed header lines are rejected") {
  REQUIRE(one_shot("HTTP/1.1 200 OK\r\nno-colon-here\r\n\r\n").error);
  REQUIRE(one_shot("HTTP/1.1 200 OK\r\nBad Name: 1\r\n\r\n").error);
  REQUIRE(one_shot("HTTP/1.1 200 OK\r\nX: 1\r\n\tfolded\r\n\r\n").error);
}

TEST_CASE("a connection closed mid response is an error") {
  const std::string truncated = kSimple.substr(0, kSimple.size() - 5);
  const Outcome out = one_shot(truncated, false, true);
  REQUIRE(out.error);
  REQUIRE_FALSE(out.complete);
}

TEST_CASE("a connection closed before any bytes arrive is not an error") {
  ResponseParser parser;
  parser.reset();
  const Result result = parser.eof();
  REQUIRE_FALSE(result.error);
  REQUIRE_FALSE(result.message_complete);
}

TEST_CASE("random bytes never crash or hang the parser") {
  std::mt19937 rng{0xBADF00Du};
  const std::string alphabet = "HTTPhttp/019: \r\n;abcdefxyz-\0";

  for (int iteration = 0; iteration < 2000; ++iteration) {
    std::string junk;
    const size_t length = rng() % 512;
    for (size_t i = 0; i < length; ++i) {
      junk.push_back(alphabet[rng() % alphabet.size()]);
    }

    ResponseParser parser;
    parser.reset(rng() % 2 == 0);
    size_t offset = 0;
    while (offset < junk.size()) {
      const size_t take = std::min<size_t>(rng() % 32 + 1, junk.size() - offset);
      const Result result = parser.feed({junk.data() + offset, take});
      offset += take;
      if (result.error || result.message_complete) break;
    }
    parser.eof();
  }
}

TEST_CASE("mutated valid responses never crash the parser") {
  std::mt19937 rng{0x5EEDu};
  const std::vector<std::string> responses = corpus();

  for (int iteration = 0; iteration < 4000; ++iteration) {
    std::string mutated = responses[rng() % responses.size()];
    const size_t edits = rng() % 4 + 1;
    for (size_t i = 0; i < edits && !mutated.empty(); ++i) {
      const size_t at = rng() % mutated.size();
      switch (rng() % 3) {
        case 0: mutated[at] = static_cast<char>(rng() % 256); break;
        case 1: mutated.erase(at, 1); break;
        default: mutated.insert(at, 1, static_cast<char>(rng() % 256)); break;
      }
    }

    ResponseParser parser;
    parser.reset();
    size_t offset = 0;
    while (offset < mutated.size()) {
      const size_t take = std::min<size_t>(rng() % 16 + 1, mutated.size() - offset);
      const Result result = parser.feed({mutated.data() + offset, take});
      offset += take;
      if (result.error || result.message_complete) break;
    }
    parser.eof();
  }
}
