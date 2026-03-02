#pragma once

#include "analytics/MarketScanner.h"
#include "analytics/RegimeDetector.h"
#include "engine/EngineConfig.h"
#include "strategy/IStrategy.h"

#include <string>
#include <vector>

namespace autolife {
namespace common {
namespace execution_guard {

struct RealtimeEntryVetoThresholds {
    double max_drop_pct = 0.0035;
    double max_spread_pct = 0.0030;
    double min_orderbook_imbalance = -0.35;
};

struct DynamicSlippageThresholds {
    double max_slippage_pct = 0.0030;
    double guard_slippage_pct = 0.0045;
};

double computeHostilityTightenPressure(
    const engine::EngineConfig& cfg,
    double hostility_ewma
);

double computeMarketHostilityScore(
    const engine::EngineConfig& cfg,
    const analytics::CoinMetrics& metrics,
    analytics::MarketRegime regime
);

RealtimeEntryVetoThresholds computeRealtimeEntryVetoThresholds(
    const engine::EngineConfig& cfg,
    analytics::MarketRegime regime,
    double signal_strength,
    double liquidity_score,
    double hostility_ewma
);

RealtimeEntryVetoThresholds computeRealtimeEntryVetoThresholds(
    const engine::EngineConfig& cfg,
    const strategy::Signal& signal,
    double hostility_ewma
);

DynamicSlippageThresholds computeDynamicSlippageThresholds(
    const engine::EngineConfig& cfg,
    double hostility_ewma,
    bool is_buy,
    analytics::MarketRegime regime,
    double signal_strength,
    double liquidity_score,
    double expected_value,
    const std::string& exit_reason = std::string()
);

} // namespace execution_guard
} // namespace common
} // namespace autolife
