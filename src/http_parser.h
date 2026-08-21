#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hammer {

enum class State {
  StatusLine,
  Headers,
  BodyLength,
  ChunkSize,
  ChunkData,
  ChunkDataCrlf,
  Trailers,
  BodyUntilClose,
  Done,
  Error,
};

struct Result {
  size_t consumed = 0;
  bool message_complete = false;
  bool error = false;
};

class ResponseParser {
public:
  void reset(bool head_request = false);

  Result feed(std::span<const char> bytes);
  Result eof();

  State state() const { return state_; }
  int status_code() const { return status_code_; }
  bool keep_alive() const { return keep_alive_; }
  uint64_t body_bytes() const { return body_bytes_; }
  const std::string& error_message() const { return error_; }

private:
  enum class Line { Complete, NeedMore, TooLong };

  Result fail(std::string_view why, size_t consumed);
  Line consume_line(std::span<const char> bytes, size_t& pos, std::string_view& line);
  bool on_status_line(std::string_view line);
  bool on_header(std::string_view line);
  bool on_headers_complete();

  static constexpr size_t kMaxLineLength = 8192;
  static constexpr size_t kMaxHeaders = 100;

  State state_ = State::StatusLine;
  std::string partial_line_;
  std::string error_;
  int status_code_ = 0;
  int http_minor_ = 1;
  size_t header_count_ = 0;
  uint64_t remaining_ = 0;
  uint64_t body_bytes_ = 0;
  bool head_request_ = false;
  bool started_ = false;
  bool chunked_ = false;
  bool has_content_length_ = false;
  uint64_t content_length_ = 0;
  bool keep_alive_ = true;
};

}
