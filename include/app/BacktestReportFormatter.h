#pragma once

#include "runtime/BacktestRuntime.h"

#include <map>
#include <ostream>
#include <string>

namespace autolife::app {

struct BacktestSummaryOptions {
    bool include_initial_capital = false;
    double initial_capital_krw = 0.0;
    bool include_profit_rate = false;
    bool include_extended_metrics = false;
    bool include_trailing_blank_line = false;
};

void printBacktestResultSummary(
    const backtest::BacktestEngine::Result& result,
    const BacktestSummaryOptions& options,
    std::ostream& out
);

void printTopEntryRejectionReasons(
    const std::map<std::string, int>& rejection_counts,
    std::ostream& out,
    size_t top_n = 5
);

}  // namespace autolife::app
