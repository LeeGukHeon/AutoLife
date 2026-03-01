#include "common/SignalPolicyShared.h"

#include "common/Config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace autolife::common::signal_policy {

std::string normalizeStrategyToken(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
    );

    if (value == "foundation" ||
        value == "foundation_adaptive_strategy" ||
        value == "foundation adaptive strategy") {
        return "foundation_adaptive";
    }
    return value;
}

bool isStrategyEnabledByConfig(const engine::EngineConfig& cfg, const std::string& strategy_name) {
    if (cfg.enabled_strategies.empty()) {
        return true;
    }
    const std::string target = normalizeStrategyToken(strategy_name);
    for (const auto& item : cfg.enabled_strategies) {
        if (normalizeStrategyToken(item) == target) {
            return true;
        }
    }
    return false;
}

bool isBreakoutContinuationArchetype(const std::string& archetype) {
    return archetype == "BREAKOUT_CONTINUATION" ||
           archetype == "FOUNDATION_UPTREND_CONTINUATION";
}

bool isTrendReaccelerationArchetype(const std::string& archetype) {
    return archetype == "TREND_REACCELERATION";
}

bool isConsolidationBreakArchetype(const std::string& archetype) {
    return archetype == "CONSOLIDATION_BREAK";
}

bool isHostileRegime(analytics::MarketRegime regime) {
    return regime == analytics::MarketRegime::HIGH_VOLATILITY ||
           regime == analytics::MarketRegime::TRENDING_DOWN;
}

bool isTrendContinuationStyleSignal(const strategy::Signal& signal) {
    if (isBreakoutContinuationArchetype(signal.entry_archetype) ||
        isTrendReaccelerationArchetype(signal.entry_archetype) ||
        isConsolidationBreakArchetype(signal.entry_archetype)) {
        return true;
    }
    return false;
}

double computeTargetRewardRisk(double strength, const engine::EngineConfig& cfg) {
    const double weak_rr = std::max(0.5, cfg.min_rr_weak_signal);
    const double strong_rr = std::max(0.5, std::min(cfg.min_rr_strong_signal, weak_rr));
    const double t = std::clamp((strength - 0.40) / 0.60, 0.0, 1.0);
    return weak_rr - (weak_rr - strong_rr) * t;
}

double computeEffectiveRoundTripCostPct(const strategy::Signal& signal, const engine::EngineConfig& cfg);

double computeCostAwareRewardRiskFloor(
    const strategy::Signal& signal,
    const engine::EngineConfig& cfg,
    double base_rr
) {
    if (signal.entry_price <= 0.0 || signal.stop_loss <= 0.0 || signal.stop_loss >= signal.entry_price) {
        return base_rr;
    }
    const double risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
    if (risk_pct <= 1e-8) {
        return base_rr;
    }

    const double round_trip_cost_pct = computeEffectiveRoundTripCostPct(signal, cfg);
    double rr_floor = std::max(base_rr, (round_trip_cost_pct + 0.00035) / risk_pct + 0.30);

    if (signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
        rr_floor += 0.08;
    }
    if (signal.liquidity_score > 0.0 && signal.liquidity_score < 50.0) {
        rr_floor += 0.05;
    }
    if (signal.strength >= 0.78 && signal.liquidity_score >= 65.0) {
        rr_floor -= 0.06;
    }

    return std::clamp(rr_floor, base_rr, base_rr + 0.65);
}

double computeImpliedLossToWinRatio(double win_rate, double profit_factor) {
    if (profit_factor <= 1e-9 || win_rate <= 1e-6 || win_rate >= (1.0 - 1e-6)) {
        return 0.0;
    }
    const double loss_rate = 1.0 - win_rate;
    if (loss_rate <= 1e-9) {
        return 0.0;
    }
    return win_rate / (loss_rate * profit_factor);
}

double computeHistoryRewardRiskAsymmetryPressure(
    const strategy::Signal& signal,
    const engine::EngineConfig& cfg
) {
    const int min_sample = std::max(10, cfg.min_strategy_trades_for_ev / 2);
    if (signal.strategy_trade_count < min_sample || signal.strategy_profit_factor <= 1e-9) {
        return 0.0;
    }
    if (signal.strategy_win_rate < 0.50 || signal.strategy_profit_factor >= 1.02) {
        return 0.0;
    }
    const double loss_to_win_ratio = computeImpliedLossToWinRatio(
        signal.strategy_win_rate,
        signal.strategy_profit_factor
    );
    if (loss_to_win_ratio < 1.08) {
        return 0.0;
    }
    const double asymmetry_raw = std::clamp((loss_to_win_ratio - 1.10) / 1.10, 0.0, 1.0);
    const double sample_conf = std::clamp(
        (static_cast<double>(signal.strategy_trade_count) - static_cast<double>(min_sample)) / 24.0,
        0.35,
        1.0
    );
    return asymmetry_raw * sample_conf;
}

bool rebalanceSignalRiskReward(strategy::Signal& signal, const engine::EngineConfig& cfg) {
    if (signal.entry_price <= 0.0 || signal.stop_loss <= 0.0 || signal.stop_loss >= signal.entry_price) {
        return false;
    }

    double risk_price = signal.entry_price - signal.stop_loss;
    if (risk_price <= 0.0) {
        return false;
    }

    const double asymmetry_pressure = computeHistoryRewardRiskAsymmetryPressure(signal, cfg);
    if (asymmetry_pressure > 1e-9) {
        const bool hostile_regime = isHostileRegime(signal.market_regime);
        const double tighten_scale = hostile_regime ? 0.10 : 0.18;
        const double compressed_risk_price = risk_price * std::clamp(
            1.0 - (tighten_scale * asymmetry_pressure),
            0.80,
            1.0
        );
        if (compressed_risk_price > 1e-9) {
            risk_price = compressed_risk_price;
            signal.stop_loss = signal.entry_price - risk_price;
        }
    }

    if (signal.take_profit_1 <= signal.entry_price) {
        signal.take_profit_1 = signal.entry_price + risk_price * 1.05;
    }
    if (signal.take_profit_2 <= signal.entry_price) {
        signal.take_profit_2 = signal.take_profit_1;
    }

    const double base_target_rr = computeTargetRewardRisk(signal.strength, cfg);
    double target_rr = computeCostAwareRewardRiskFloor(signal, cfg, base_target_rr);
    const bool trend_cont_strategy = isTrendContinuationStyleSignal(signal);
    const bool off_trend_regime =
        trend_cont_strategy &&
        signal.market_regime != analytics::MarketRegime::TRENDING_UP;
    if (off_trend_regime) {
        double rr_role_add = 0.14;
        if (signal.market_regime == analytics::MarketRegime::RANGING &&
            signal.liquidity_score > 0.0 &&
            signal.liquidity_score < 60.0) {
            rr_role_add += 0.06;
        }
        if (signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
            rr_role_add += 0.10;
        }
        if (signal.strength < 0.60) {
            rr_role_add += 0.05;
        }
        target_rr += rr_role_add;
    }
    if (asymmetry_pressure > 1e-9) {
        const bool favorable_regime =
            signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
            signal.market_regime == analytics::MarketRegime::RANGING;
        const double rr_boost = (favorable_regime ? 0.12 : 0.08) + (0.22 * asymmetry_pressure);
        target_rr = std::min(target_rr + rr_boost, base_target_rr + 1.10);
    }
    target_rr = std::min(target_rr, base_target_rr + 1.10);
    const double current_rr = (signal.take_profit_2 - signal.entry_price) / risk_price;
    if (current_rr + 1e-9 < target_rr) {
        signal.take_profit_2 = signal.entry_price + risk_price * target_rr;
    }

    const double risk_pct = risk_price / std::max(1e-9, signal.entry_price);
    const double round_trip_cost_pct = computeEffectiveRoundTripCostPct(signal, cfg);
    const double tp1_cost_cover_rr = ((round_trip_cost_pct * 0.65) + 0.00015) / std::max(1e-9, risk_pct);
    double tp1_asymmetry_ratio = 0.60 + (0.08 * asymmetry_pressure);
    if (off_trend_regime) {
        tp1_asymmetry_ratio = std::max(tp1_asymmetry_ratio, 0.66);
    }
    const double min_tp1_rr = std::max({1.0, target_rr * tp1_asymmetry_ratio, tp1_cost_cover_rr});
    const double min_tp1 = signal.entry_price + risk_price * min_tp1_rr;
    if (signal.take_profit_1 < min_tp1) {
        signal.take_profit_1 = min_tp1;
    }
    if (asymmetry_pressure > 1e-9) {
        const double min_tp2_gap_rr = 0.18 + (0.22 * asymmetry_pressure);
        const double min_tp2_rr = min_tp1_rr + min_tp2_gap_rr;
        const double tp2_rr = (signal.take_profit_2 - signal.entry_price) / std::max(1e-9, risk_price);
        if (tp2_rr < min_tp2_rr) {
            signal.take_profit_2 = signal.entry_price + risk_price * min_tp2_rr;
        }
    }
    if (signal.take_profit_2 < signal.take_profit_1) {
        signal.take_profit_2 = signal.take_profit_1;
    }
    return true;
}

double computeEffectiveRoundTripCostPct(const strategy::Signal& signal, const engine::EngineConfig& cfg) {
    const double fee_rate_per_side = Config::getInstance().getFeeRate();
    double slippage_per_side = std::clamp(cfg.max_slippage_pct * 0.35, 0.00045, 0.00120);
    if (signal.liquidity_score >= 70.0 && signal.volatility <= 3.5) {
        slippage_per_side *= 0.85;
    } else if (signal.liquidity_score > 0.0 && signal.liquidity_score < 50.0) {
        slippage_per_side *= 1.10;
    }
    if (signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
        slippage_per_side *= 1.08;
    }
    slippage_per_side = std::clamp(slippage_per_side, 0.00035, 0.00140);

    double spread_buffer = 0.00015;
    if (signal.liquidity_score >= 75.0) {
        spread_buffer = 0.00008;
    } else if (signal.liquidity_score > 0.0 && signal.liquidity_score < 50.0) {
        spread_buffer = 0.00022;
    }
    return (fee_rate_per_side * 2.0) + (slippage_per_side * 2.0) + spread_buffer;
}

double computeCalibratedExpectedEdgePct(const strategy::Signal& signal, const engine::EngineConfig& cfg) {
    const double entry_price = signal.entry_price;
    const double take_profit_price = (signal.take_profit_2 > 0.0) ? signal.take_profit_2 : signal.take_profit_1;
    const double stop_loss_price = signal.stop_loss;
    if (entry_price <= 0.0 || take_profit_price <= 0.0 || stop_loss_price <= 0.0 ||
        take_profit_price <= entry_price || stop_loss_price >= entry_price) {
        return std::numeric_limits<double>::lowest();
    }

    const double gross_reward_pct = (take_profit_price - entry_price) / entry_price;
    const double gross_risk_pct = (entry_price - stop_loss_price) / entry_price;
    if (gross_reward_pct <= 0.0 || gross_risk_pct <= 0.0) {
        return std::numeric_limits<double>::lowest();
    }

    double win_prob = 0.50;
    if (signal.probabilistic_runtime_applied &&
        std::isfinite(signal.probabilistic_h5_calibrated) &&
        signal.probabilistic_h5_calibrated > 0.0) {
        const double prob = std::clamp(signal.probabilistic_h5_calibrated, 0.0, 1.0);
        const double margin = std::clamp(signal.probabilistic_h5_margin, -1.0, 1.0);
        const std::string backend = normalizeStrategyToken(signal.phase3.prob_model_backend);
        if (backend == "lgbm") {
            win_prob = std::clamp(prob, 0.12, 0.88);
        } else {
            win_prob = std::clamp(prob + (margin * 0.35), 0.12, 0.88);
        }
    } else {
        if (std::isfinite(signal.expected_value) && std::abs(signal.expected_value) > 1e-9) {
            const double implied_win_from_edge =
                (signal.expected_value + gross_risk_pct) / (gross_reward_pct + gross_risk_pct);
            win_prob = std::clamp(implied_win_from_edge, 0.12, 0.88);
        } else {
            double implied = std::clamp(
                std::clamp(cfg.foundation_implied_win_base, 0.0, 1.0) +
                    (signal.strength *
                     std::clamp(cfg.foundation_implied_win_strength_scale, -10.0, 10.0)),
                std::clamp(cfg.foundation_implied_win_preclamp_min, 0.0, 1.0),
                std::clamp(cfg.foundation_implied_win_preclamp_max, 0.0, 1.0)
            );
            if (signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
                implied = std::clamp(
                    implied -
                        std::clamp(cfg.foundation_implied_win_hostile_uptrend_penalty, -1.0, 1.0),
                    std::clamp(cfg.foundation_implied_win_hostile_min, 0.0, 1.0),
                    std::clamp(cfg.foundation_implied_win_hostile_max, 0.0, 1.0)
                );
            }
            win_prob = implied;
        }
    }
    win_prob = std::clamp(win_prob, 0.12, 0.88);

    const double round_trip_cost_pct = computeEffectiveRoundTripCostPct(signal, cfg);
    const double expected_gross_pct = (win_prob * gross_reward_pct) - ((1.0 - win_prob) * gross_risk_pct);
    return expected_gross_pct - round_trip_cost_pct;
}

bool requiresTypedArchetype(const std::string& strategy_name) {
    const std::string normalized = normalizeStrategyToken(strategy_name);
    if (normalized.empty() || normalized == "recovered") {
        return false;
    }
    return normalized == "foundation_adaptive";
}

void normalizeSignalStopLossByRegime(strategy::Signal& signal, analytics::MarketRegime regime) {
    if (signal.entry_price <= 0.0) {
        return;
    }

    const auto& cfg = Config::getInstance().getEngineConfig();
    double min_risk_pct = std::clamp(cfg.foundation_risk_floor_unknown, 0.0, 1.0);
    double max_risk_pct = std::clamp(cfg.foundation_risk_cap_unknown, 0.0, 1.0);
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            min_risk_pct = std::clamp(cfg.foundation_risk_floor_uptrend, 0.0, 1.0);
            max_risk_pct = std::clamp(cfg.foundation_risk_cap_uptrend, 0.0, 1.0);
            break;
        case analytics::MarketRegime::RANGING:
            min_risk_pct = std::clamp(cfg.foundation_risk_floor_ranging, 0.0, 1.0);
            max_risk_pct = std::clamp(cfg.foundation_risk_cap_ranging, 0.0, 1.0);
            break;
        case analytics::MarketRegime::HIGH_VOLATILITY:
        case analytics::MarketRegime::TRENDING_DOWN:
            min_risk_pct = std::clamp(cfg.foundation_risk_floor_hostile, 0.0, 1.0);
            max_risk_pct = std::clamp(cfg.foundation_risk_cap_hostile, 0.0, 1.0);
            break;
        case analytics::MarketRegime::UNKNOWN:
        default:
            break;
    }

    const double strength_t = std::clamp((signal.strength - 0.40) / 0.60, 0.0, 1.0);
    const double tighten = 1.0 - (0.15 * strength_t);
    min_risk_pct *= tighten;
    max_risk_pct *= tighten;
    if (max_risk_pct < min_risk_pct) {
        max_risk_pct = min_risk_pct;
    }

    double risk_pct = 0.0;
    if (signal.stop_loss > 0.0 && signal.stop_loss < signal.entry_price) {
        risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
    }
    if (risk_pct <= 0.0) {
        risk_pct = (min_risk_pct + max_risk_pct) * 0.5;
    }

    risk_pct = std::clamp(risk_pct, min_risk_pct, max_risk_pct);
    signal.stop_loss = signal.entry_price * (1.0 - risk_pct);
}

} // namespace autolife::common::signal_policy
