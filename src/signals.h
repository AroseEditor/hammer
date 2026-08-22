#pragma once

namespace hammer {

void install_signal_handlers();

bool stop_requested();

void request_stop();

void clear_stop();

}
