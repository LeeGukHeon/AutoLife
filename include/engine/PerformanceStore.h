#pragma once

#include "analytics/RegimeDetector.h"
#include "risk/RiskManager.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace autolife {
namespace engine {

struct StrategyPerformanceStats {
    int trades = 0;
    int wins = 0;
    double gross_profit = 0.0;
    double gross_loss_abs = 0.0;
    double net_profit = 0.0;

    double winRate() const {
        return (trades > 0) ? (static_cast<double>(wins) / static_cast<double>(trades)) : 0.0;
    }
    double expectancy() const {
        return (trades > 0) ? (net_profit / static_cast<double>(trades)) : 0.0;
    }
    double profitFactor() const {
        return (gross_loss_abs > 1e-12) ? (gross_profit / gross_loss_abs) : 0.0;
    }
};

struct PerformanceBucketKey {
    std::string strategy_name;
    analytics::MarketRegime regime = analytics::MarketRegime::UNKNOWN;
    int liquidity_bucket = 0; // 0: <40, 1: 40-59, 2: 60-79, 3: >=80

    bool operator==(const PerformanceBucketKey& other) const {
        return strategy_name == other.strategy_name &&
               regime == other.regime &&
               liquidity_bucket == other.liquidity_bucket;
    }
};

struct PerformanceBucketKeyHash {
    std::size_t operator()(const PerformanceBucketKey& key) const {
        std::size_t h1 = std::hash<std::string>{}(key.strategy_name);
        std::size_t h2 = std::hash<int>{}(static_cast<int>(key.regime));
        std::size_t h3 = std::hash<int>{}(key.liquidity_bucket);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

// Phase 1 scaffold:
// Aggregate realized trade outcomes into strategy-level stats.
class PerformanceStore {
public:
    void rebuild(const std::vector<risk::TradeHistory>& history);
    const std::unordered_map<std::string, StrategyPerformanceStats>& byStrategy() const {
        return by_strategy_;
    }
    const std::unordered_map<PerformanceBucketKey, StrategyPerformanceStats, PerformanceBucketKeyHash>& byBucket() const {
        return by_bucket_;
    }

private:
    std::unordered_map<std::string, StrategyPerformanceStats> by_strategy_;
    std::unordered_map<PerformanceBucketKey, StrategyPerformanceStats, PerformanceBucketKeyHash> by_bucket_;
};

} // namespace engine
} // namespace autolife
