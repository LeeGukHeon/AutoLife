#pragma once

#include "engine/EngineConfig.h"
#include "strategy/IStrategy.h"

#include <string>

namespace autolife::common::signal_policy {

std::string normalizeStrategyToken(std::string value);
bool isStrategyEnabledByConfig(const engine::EngineConfig& cfg, const std::string& strategy_name);

bool isBreakoutContinuationArchetype(const std::string& archetype);
bool isTrendReaccelerationArchetype(const std::string& archetype);
bool isConsolidationBreakArchetype(const std::string& archetype);
bool isRangePullbackArchetype(const std::string& archetype);
bool isDefensiveFoundationArchetype(const std::string& archetype);
bool isTrendContinuationStyleSignal(const strategy::Signal& signal);

double computeTargetRewardRisk(double strength, const engine::EngineConfig& cfg);
double computeImpliedLossToWinRatio(double win_rate, double profit_factor);
double computeHistoryRewardRiskAsymmetryPressure(
    const strategy::Signal& signal,
    const engine::EngineConfig& cfg
);
double computeEffectiveRoundTripCostPct(const strategy::Signal& signal, const engine::EngineConfig& cfg);
double computeCostAwareRewardRiskFloor(
    const strategy::Signal& signal,
    const engine::EngineConfig& cfg,
    double base_rr
);
bool rebalanceSignalRiskReward(strategy::Signal& signal, const engine::EngineConfig& cfg);
double computeStrategyHistoryWinProbPrior(const strategy::Signal& signal, const engine::EngineConfig& cfg);
double computeCalibratedExpectedEdgePct(const strategy::Signal& signal, const engine::EngineConfig& cfg);
bool isAlphaHeadFallbackCandidate(const strategy::Signal& signal, bool alpha_head_mode);
void normalizeSignalStopLossByRegime(strategy::Signal& signal, analytics::MarketRegime regime);

void applyArchetypeRiskAdjustments(
    const strategy::Signal& signal,
    double& required_signal_strength,
    double& regime_rr_add,
    double& regime_edge_add,
    bool& regime_pattern_block
);

bool requiresTypedArchetype(const std::string& strategy_name);

} // namespace autolife::common::signal_policy
