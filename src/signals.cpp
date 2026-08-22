#include "signals.h"

#include <csignal>

namespace hammer {
namespace {

volatile std::sig_atomic_t g_stop = 0;

extern "C" void on_interrupt(int) { g_stop = 1; }

}

void install_signal_handlers() {
  std::signal(SIGINT, on_interrupt);
  std::signal(SIGTERM, on_interrupt);
}

bool stop_requested() { return g_stop != 0; }

void request_stop() { g_stop = 1; }

void clear_stop() { g_stop = 0; }

}
