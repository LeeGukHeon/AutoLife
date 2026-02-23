#pragma once

#include "common/Types.h"

#include <vector>

namespace autolife {
namespace engine {
struct EngineConfig;
} // namespace engine

namespace common {
namespace probabilistic_regime {

enum class State {
    NORMAL,
    VOLATILE,
    HOSTILE,
};

struct Snapshot {
    bool enabled = false;
    bool sufficient_data = false;
    State state = State::NORMAL;
    double volatility_zscore = 0.0;
    double drawdown_speed_bps = 0.0;
    double btc_correlation = 0.0;
    double correlation_shock = 0.0;
    bool correlation_shock_triggered = false;
};

Snapshot analyze(
    const std::vector<Candle>& market_candles,
    const engine::EngineConfig& cfg,
    const std::vector<Candle>* btc_reference_candles = nullptr
);

double thresholdAdd(const engine::EngineConfig& cfg, State state);
double sizeMultiplier(const engine::EngineConfig& cfg, State state);
bool blockNewEntries(const engine::EngineConfig& cfg, State state);
const char* stateLabel(State state);

} // namespace probabilistic_regime
} // namespace common
} // namespace autolife
