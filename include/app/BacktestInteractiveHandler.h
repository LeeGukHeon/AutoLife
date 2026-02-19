#pragma once

#include "common/Config.h"

namespace autolife::app {

// Run interactive backtest setup + execution path.
// Returns process exit code.
int runInteractiveBacktest(Config& config);

}  // namespace autolife::app
