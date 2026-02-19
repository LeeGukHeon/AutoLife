#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "analytics/RegimeDetector.h"
#include "risk/RiskManager.h"

namespace autolife::common::strategy_edge {

struct StrategyEdgeStats {
    int trades = 0;
    int wins = 0;
    double gross_profit = 0.0;
    double gross_loss_abs = 0.0;
    double net_profit = 0.0;

    double expectancy() const {
        return (trades > 0) ? (net_profit / static_cast<double>(trades)) : 0.0;
    }
    double winRate() const {
        return (trades > 0) ? (static_cast<double>(wins) / static_cast<double>(trades)) : 0.0;
    }
    double profitFactor() const {
        if (gross_loss_abs > 1e-12) {
            return gross_profit / gross_loss_abs;
        }
        return (gross_profit > 1e-12) ? 99.9 : 0.0;
    }
    double avgWinKrw() const {
        return (wins > 0) ? (gross_profit / static_cast<double>(wins)) : 0.0;
    }
    double avgLossAbsKrw() const {
        const int losses = std::max(0, trades - wins);
        return (losses > 0) ? (gross_loss_abs / static_cast<double>(losses)) : 0.0;
    }
};

std::string makeStrategyRegimeKey(const std::string& strategy_name, analytics::MarketRegime regime);
std::string makeMarketStrategyRegimeKey(
    const std::string& market,
    const std::string& strategy_name,
    analytics::MarketRegime regime
);

std::map<std::string, StrategyEdgeStats> buildStrategyEdgeStats(
    const std::vector<risk::TradeHistory>& history,
    int max_recent_trades_per_strategy = 0
);

std::map<std::string, StrategyEdgeStats> buildStrategyRegimeEdgeStats(
    const std::vector<risk::TradeHistory>& history,
    int max_recent_trades_per_key = 0
);

std::map<std::string, StrategyEdgeStats> buildMarketStrategyRegimeEdgeStats(
    const std::vector<risk::TradeHistory>& history,
    int max_recent_trades_per_key = 0
);

} // namespace autolife::common::strategy_edge
