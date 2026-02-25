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
    double base_required_expected_value = 0.0;
    double uncertainty_term = 0.0;
    double cost_tail_term = 0.0;
    double probabilistic_confidence = 0.0;
    double ev_confidence = 1.0;
    double frontier_required_expected_value = 0.0;
    bool margin_pass = true;
    bool strength_pass = false;
    bool expected_value_pass = false;
    bool frontier_enabled = false;
    bool frontier_pass = false;
    bool ev_confidence_pass = true;
    bool cost_tail_pass = true;
    std::string reject_reason;
};

double rewardRiskRatio(const Signal& signal);
FilterDecision evaluateFilter(const FilterInput& input);

}  // namespace autolife::strategy::foundation

