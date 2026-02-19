#pragma once

#include "common/Config.h"

namespace autolife::app {

// Returns true when CLI backtest path was handled.
// When handled, out_exit_code contains process exit code.
bool tryRunCliBacktest(int argc, char* argv[], Config& config, int& out_exit_code);

}  // namespace autolife::app
