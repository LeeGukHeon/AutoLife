#pragma once

#include "analytics/MarketScanner.h"
#include "strategy/IStrategy.h"

#include <string>

namespace autolife::strategy::foundation {

struct FilterInput {
    const Signal* signal = nullptr;
    analytics::MarketRegime regime = analytics::MarketRegime::UNKNOWN;
    double min_strength = 0.0;
    double min_expected_value = 0.0;
    bool core_signal_ownership = false;
    bool policy_block = false;
    bool policy_hold = false;
    bool off_trend_regime = false;
    bool hostile_regime = false;
    bool no_trade_bias_active = false;
    int min_history_sample = 0;
    double min_history_win_rate = 0.0;
    double min_history_profit_factor = 0.0;
};

struct FilterDecision {
    bool pass = false;
    double reward_risk_ratio = 0.0;
    double required_strength = 0.0;
    double required_expected_value = 0.0;
    std::string reject_reason;
};

double rewardRiskRatio(const Signal& signal);
FilterDecision evaluateFilter(const FilterInput& input);

}  // namespace autolife::strategy::foundation

