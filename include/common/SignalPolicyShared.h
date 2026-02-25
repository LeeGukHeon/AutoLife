#pragma once

#include "engine/EngineConfig.h"
#include "strategy/IStrategy.h"

#include <string>

namespace autolife::common::signal_policy {

std::string normalizeStrategyToken(std::string value);
bool isStrategyEnabledByConfig(const engine::EngineConfig& cfg, const std::string& strategy_name);

double computeEffectiveRoundTripCostPct(const strategy::Signal& signal, const engine::EngineConfig& cfg);
bool rebalanceSignalRiskReward(strategy::Signal& signal, const engine::EngineConfig& cfg);
double computeCalibratedExpectedEdgePct(const strategy::Signal& signal, const engine::EngineConfig& cfg);
void normalizeSignalStopLossByRegime(strategy::Signal& signal, analytics::MarketRegime regime);

bool requiresTypedArchetype(const std::string& strategy_name);

} // namespace autolife::common::signal_policy
