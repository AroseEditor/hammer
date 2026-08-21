#include "http_parser.h"

#include <algorithm>
#include <limits>

namespace hammer {
namespace {

constexpr std::string_view kOws = " \t";

char lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

std::string_view trim(std::string_view s) {
  const size_t first = s.find_first_not_of(kOws);
  if (first == std::string_view::npos) return {};
  const size_t last = s.find_last_not_of(kOws);
  return s.substr(first, last - first + 1);
}

bool is_token_char(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool is_token(std::string_view s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(), is_token_char);
}

bool parse_decimal(std::string_view s, uint64_t& out) {
  if (s.empty() || s.size() > 20) return false;
  uint64_t value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

bool parse_hex(std::string_view s, uint64_t& out) {
  if (s.empty() || s.size() > 16) return false;
  uint64_t value = 0;
  for (char c : s) {
    uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint64_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<uint64_t>(c - 'A' + 10);
    } else {
      return false;
    }
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 16) return false;
    value = value * 16 + digit;
  }
  out = value;
  return true;
}

bool status_forbids_body(int status) {
  return (status >= 100 && status < 200) || status == 204 || status == 304;
}

}

void ResponseParser::reset(bool head_request) {
  state_ = State::StatusLine;
  partial_line_.clear();
  error_.clear();
  status_code_ = 0;
  http_minor_ = 1;
  header_count_ = 0;
  remaining_ = 0;
  body_bytes_ = 0;
  head_request_ = head_request;
  started_ = false;
  chunked_ = false;
  has_content_length_ = false;
  content_length_ = 0;
  keep_alive_ = true;
}

Result ResponseParser::fail(std::string_view why, size_t consumed) {
  state_ = State::Error;
  error_.assign(why);
  return Result{consumed, false, true};
}

ResponseParser::Line ResponseParser::consume_line(std::span<const char> bytes, size_t& pos,
                                                  std::string_view& line) {
  const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(pos);
  const auto found = std::find(begin, bytes.end(), '\n');
  const size_t take = static_cast<size_t>(found - begin);

  if (partial_line_.size() + take > kMaxLineLength) return Line::TooLong;
  partial_line_.append(bytes.data() + pos, take);

  if (found == bytes.end()) {
    pos = bytes.size();
    return Line::NeedMore;
  }

  pos += take + 1;
  if (!partial_line_.empty() && partial_line_.back() == '\r') partial_line_.pop_back();
  line = partial_line_;
  return Line::Complete;
}

bool ResponseParser::on_status_line(std::string_view line) {
  if (line.rfind("HTTP/", 0) != 0) {
    error_ = "response does not start with an HTTP status line";
    return false;
  }
  std::string_view rest = line.substr(5);
  if (rest.size() < 3 || rest[1] != '.') {
    error_ = "malformed HTTP version in status line";
    return false;
  }
  if (rest[0] != '1' || (rest[2] != '0' && rest[2] != '1')) {
    error_ = "unsupported HTTP version in status line";
    return false;
  }
  http_minor_ = rest[2] - '0';
  rest = rest.substr(3);

  if (rest.empty() || rest.front() != ' ') {
    error_ = "status line has no status code";
    return false;
  }
  rest = rest.substr(1);
  if (rest.size() < 3) {
    error_ = "status code is not three digits";
    return false;
  }
  const std::string_view code = rest.substr(0, 3);
  uint64_t value = 0;
  if (!parse_decimal(code, value) || value < 100 || value > 599) {
    error_ = "status code is out of range";
    return false;
  }
  if (rest.size() > 3 && rest[3] != ' ') {
    error_ = "junk after status code";
    return false;
  }
  status_code_ = static_cast<int>(value);
  keep_alive_ = http_minor_ == 1;
  return true;
}

bool ResponseParser::on_header(std::string_view line) {
  if (line.front() == ' ' || line.front() == '\t') {
    error_ = "obsolete header line folding is not accepted";
    return false;
  }
  if (++header_count_ > kMaxHeaders) {
    error_ = "response has more than 100 headers";
    return false;
  }

  const size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    error_ = "header line has no colon";
    return false;
  }
  const std::string_view name = line.substr(0, colon);
  if (!is_token(name)) {
    error_ = "header name is not a valid token";
    return false;
  }
  const std::string_view value = trim(line.substr(colon + 1));

  if (iequals(name, "Content-Length")) {
    uint64_t length = 0;
    if (!parse_decimal(value, length)) {
      error_ = "Content-Length is not a non-negative integer";
      return false;
    }
    if (has_content_length_ && content_length_ != length) {
      error_ = "Content-Length repeated with a different value";
      return false;
    }
    has_content_length_ = true;
    content_length_ = length;
    return true;
  }

  if (iequals(name, "Transfer-Encoding")) {
    std::string_view remaining = value;
    std::string_view last;
    while (!remaining.empty()) {
      const size_t comma = remaining.find(',');
      const std::string_view coding = trim(remaining.substr(0, comma));
      if (!coding.empty()) last = coding;
      if (comma == std::string_view::npos) break;
      remaining = remaining.substr(comma + 1);
    }
    if (last.empty() || iequals(last, "identity")) return true;
    if (!iequals(last, "chunked")) {
      error_ = "unsupported transfer coding";
      return false;
    }
    chunked_ = true;
    return true;
  }

  if (iequals(name, "Connection")) {
    std::string_view remaining = value;
    while (!remaining.empty()) {
      const size_t comma = remaining.find(',');
      const std::string_view option = trim(remaining.substr(0, comma));
      if (iequals(option, "close")) keep_alive_ = false;
      if (iequals(option, "keep-alive")) keep_alive_ = true;
      if (comma == std::string_view::npos) break;
      remaining = remaining.substr(comma + 1);
    }
    return true;
  }

  return true;
}

bool ResponseParser::on_headers_complete() {
  if (chunked_ && has_content_length_) {
    error_ = "response carries both Transfer-Encoding: chunked and Content-Length";
    return false;
  }

  if (head_request_ || status_forbids_body(status_code_)) {
    state_ = State::Done;
    return true;
  }
  if (chunked_) {
    state_ = State::ChunkSize;
    return true;
  }
  if (has_content_length_) {
    remaining_ = content_length_;
    state_ = remaining_ == 0 ? State::Done : State::BodyLength;
    return true;
  }
  keep_alive_ = false;
  state_ = State::BodyUntilClose;
  return true;
}

Result ResponseParser::feed(std::span<const char> bytes) {
  if (state_ == State::Error) return Result{0, false, true};
  if (state_ == State::Done) return Result{0, true, false};
  if (!bytes.empty()) started_ = true;

  size_t pos = 0;
  while (pos < bytes.size()) {
    switch (state_) {
      case State::StatusLine:
      case State::Headers:
      case State::ChunkSize:
      case State::ChunkDataCrlf:
      case State::Trailers: {
        std::string_view line;
        const Line status = consume_line(bytes, pos, line);
        if (status == Line::TooLong) return fail("line exceeds 8192 bytes", pos);
        if (status == Line::NeedMore) return Result{pos, false, false};

        bool ok = true;
        if (state_ == State::StatusLine) {
          ok = on_status_line(line);
          if (ok) state_ = State::Headers;
        } else if (state_ == State::Headers) {
          if (line.empty()) {
            ok = on_headers_complete();
          } else {
            ok = on_header(line);
          }
        } else if (state_ == State::ChunkSize) {
          const size_t semi = line.find(';');
          const std::string_view digits = trim(semi == std::string_view::npos ? line
                                                                             : line.substr(0, semi));
          uint64_t size = 0;
          if (!parse_hex(digits, size)) {
            error_ = "chunk size is not a valid hex number";
            ok = false;
          } else if (size == 0) {
            state_ = State::Trailers;
          } else {
            remaining_ = size;
            state_ = State::ChunkData;
          }
        } else if (state_ == State::ChunkDataCrlf) {
          if (!line.empty()) {
            error_ = "chunk data is not followed by CRLF";
            ok = false;
          } else {
            state_ = State::ChunkSize;
          }
        } else {
          if (line.empty()) {
            state_ = State::Done;
          } else if (++header_count_ > kMaxHeaders) {
            error_ = "response has more than 100 trailer lines";
            ok = false;
          }
        }

        partial_line_.clear();
        if (!ok) return fail(error_, pos);
        if (state_ == State::Done) return Result{pos, true, false};
        break;
      }

      case State::BodyLength:
      case State::ChunkData: {
        const uint64_t available = bytes.size() - pos;
        const size_t take = static_cast<size_t>(std::min<uint64_t>(available, remaining_));
        pos += take;
        remaining_ -= take;
        body_bytes_ += take;
        if (remaining_ == 0) {
          if (state_ == State::BodyLength) {
            state_ = State::Done;
            return Result{pos, true, false};
          }
          state_ = State::ChunkDataCrlf;
        }
        break;
      }

      case State::BodyUntilClose: {
        const size_t take = bytes.size() - pos;
        pos += take;
        body_bytes_ += take;
        break;
      }

      case State::Done:
        return Result{pos, true, false};
      case State::Error:
        return Result{pos, false, true};
    }
  }

  return Result{pos, state_ == State::Done, false};
}

Result ResponseParser::eof() {
  if (state_ == State::Error) return Result{0, false, true};
  if (state_ == State::Done) return Result{0, true, false};
  if (state_ == State::BodyUntilClose) {
    state_ = State::Done;
    return Result{0, true, false};
  }
  if (!started_ && partial_line_.empty()) return Result{0, false, false};
  return fail("connection closed before the response was complete", 0);
}

}
