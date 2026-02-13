#include "engine/PerformanceStore.h"

#include <cmath>

namespace autolife {
namespace engine {
namespace {
int liquidityBucket(double liquidity_score) {
    if (liquidity_score < 40.0) {
        return 0;
    }
    if (liquidity_score < 60.0) {
        return 1;
    }
    if (liquidity_score < 80.0) {
        return 2;
    }
    return 3;
}

void accumulateStats(StrategyPerformanceStats& s, const risk::TradeHistory& trade) {
    s.trades++;
    s.net_profit += trade.profit_loss;
    if (trade.profit_loss > 0.0) {
        s.wins++;
        s.gross_profit += trade.profit_loss;
    } else if (trade.profit_loss < 0.0) {
        s.gross_loss_abs += std::abs(trade.profit_loss);
    }
}
}

void PerformanceStore::rebuild(const std::vector<risk::TradeHistory>& history) {
    by_strategy_.clear();
    by_bucket_.clear();

    for (const auto& trade : history) {
        const std::string strategy_name = trade.strategy_name.empty() ? "unknown" : trade.strategy_name;
        accumulateStats(by_strategy_[strategy_name], trade);

        PerformanceBucketKey key;
        key.strategy_name = strategy_name;
        key.regime = trade.market_regime;
        key.liquidity_bucket = liquidityBucket(trade.liquidity_score);
        accumulateStats(by_bucket_[key], trade);
    }
}

} // namespace engine
} // namespace autolife
