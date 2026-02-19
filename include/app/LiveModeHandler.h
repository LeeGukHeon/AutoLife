#pragma once

#include "common/Config.h"
#include "runtime/LiveTradingRuntime.h"

#include <memory>

namespace autolife::app {

// Run interactive live mode setup + engine bootstrap.
// Returns process exit code.
int runInteractiveLiveMode(
    Config& config,
    std::unique_ptr<engine::TradingEngine>& engine_instance,
    void (*signal_handler)(int)
);

}  // namespace autolife::app
