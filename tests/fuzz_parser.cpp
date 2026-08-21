#include "http_parser.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;

  const size_t stride = static_cast<size_t>(data[0]) + 1;
  const char* bytes = reinterpret_cast<const char*>(data) + 1;
  const size_t length = size - 1;

  hammer::ResponseParser parser;
  parser.reset(stride % 2 == 0);

  size_t offset = 0;
  while (offset < length) {
    const size_t take = stride < length - offset ? stride : length - offset;
    const hammer::Result result = parser.feed({bytes + offset, take});
    offset += result.consumed;
    if (result.error || result.message_complete) return 0;
    if (result.consumed == 0) offset += take;
  }
  parser.eof();
  return 0;
}
