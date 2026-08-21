#include "cli.h"

#include <cstdio>

int main() {
  std::fwrite(hammer::usage_text().data(), 1, hammer::usage_text().size(), stdout);
  return 0;
}
