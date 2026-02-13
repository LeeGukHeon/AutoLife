#pragma once

#include "analytics/RegimeDetector.h"
#include "engine/PerformanceStore.h"
#include "strategy/IStrategy.h"
#include <unordered_map>
#include <vector>

namespace autolife {
namespace engine {

struct PolicyInput {
    std::vector<strategy::Signal> candidates;
    bool small_seed_mode = false;
    int max_new_orders_per_scan = 1;
    analytics::MarketRegime dominant_regime = analytics::MarketRegime::UNKNOWN;
    const std::unordered_map<std::string, StrategyPerformanceStats>* strategy_stats = nullptr;
    const std::unordered_map<PerformanceBucketKey, StrategyPerformanceStats, PerformanceBucketKeyHash>* bucket_stats = nullptr;
};

struct PolicyDecisionRecord {
    std::string market;
    std::string strategy_name;
    bool selected = false;
    std::string reason; // selected|dropped_low_strength|dropped_small_seed_quality|dropped_small_seed_liqvol|dropped_capacity
    double base_score = 0.0;
    double policy_score = 0.0;
    double strength = 0.0;
    double expected_value = 0.0;
    double liquidity_score = 0.0;
    double volatility = 0.0;
    int strategy_trades = 0;
    double strategy_win_rate = 0.0;
    double strategy_profit_factor = 0.0;
};

struct PolicyOutput {
    std::vector<strategy::Signal> selected_candidates;
    int dropped_by_policy = 0;
    std::vector<PolicyDecisionRecord> decisions;
};

// Phase 1 scaffold:
// - decouple candidate selection from TradingEngine
// - keep existing behavior mostly unchanged, with lightweight policy-level pruning
class AdaptivePolicyController {
public:
    PolicyOutput selectCandidates(const PolicyInput& input) const;
};

} // namespace engine
} // namespace autolife
