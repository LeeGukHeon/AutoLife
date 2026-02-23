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
bool isHostileRegime(analytics::MarketRegime regime);
bool isRangePullbackLossTailRiskCell(
    const strategy::Signal& signal,
    analytics::MarketRegime regime
);
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
void normalizeSignalStopLossByRegime(strategy::Signal& signal, analytics::MarketRegime regime);

bool requiresTypedArchetype(const std::string& strategy_name);

} // namespace autolife::common::signal_policy
