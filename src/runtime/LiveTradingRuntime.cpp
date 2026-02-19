#include "runtime/LiveTradingRuntime.h"
#include "common/Logger.h"
#include "common/Config.h"
#include "strategy/FoundationAdaptiveStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include "analytics/OrderbookAnalyzer.h"
#include "core/adapters/ExecutionPlaneAdapter.h"
#include "core/adapters/PolicyLearningPlaneAdapter.h"
#include "core/adapters/UpbitComplianceAdapter.h"
#include "core/state/EventJournalJsonl.h"
#include "core/state/LearningStateStoreJson.h"
#include "risk/RiskManager.h"
#include "common/PathUtils.h"
#include "common/MarketDataWindowPolicy.h"
#include "common/TickSizeHelper.h"  // [Phase 3] ???????????? ????????®Î∫§ÍΩ????ÄÎ∂æÍµù????????????
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>

#undef max
#undef min
#include <sstream>
#include <iomanip>
#include <map>
#include <cmath>
#include <set>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace {
long long getCurrentTimestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

const char* regimeToString(autolife::analytics::MarketRegime regime) {
    switch (regime) {
        case autolife::analytics::MarketRegime::TRENDING_UP: return "TRENDING_UP";
        case autolife::analytics::MarketRegime::TRENDING_DOWN: return "TRENDING_DOWN";
        case autolife::analytics::MarketRegime::RANGING: return "RANGING";
        case autolife::analytics::MarketRegime::HIGH_VOLATILITY: return "HIGH_VOLATILITY";
        default: return "UNKNOWN";
    }
}

double computeTargetRewardRisk(double strength, const autolife::engine::EngineConfig& cfg) {
    const double weak_rr = std::max(0.5, cfg.min_rr_weak_signal);
    const double strong_rr = std::max(0.5, std::min(cfg.min_rr_strong_signal, weak_rr));
    const double t = std::clamp((strength - 0.40) / 0.60, 0.0, 1.0);
    return weak_rr - (weak_rr - strong_rr) * t;
}

double computeEffectiveRoundTripCostPct(
    const autolife::strategy::Signal& signal,
    const autolife::engine::EngineConfig& cfg
);

double computeCostAwareRewardRiskFloor(
    const autolife::strategy::Signal& signal,
    const autolife::engine::EngineConfig& cfg,
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

    if (signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN) {
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
    const autolife::strategy::Signal& signal,
    const autolife::engine::EngineConfig& cfg
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

bool rebalanceSignalRiskReward(autolife::strategy::Signal& signal, const autolife::engine::EngineConfig& cfg) {
    if (signal.entry_price <= 0.0 || signal.stop_loss <= 0.0 || signal.stop_loss >= signal.entry_price) {
        return false;
    }

    double risk_price = signal.entry_price - signal.stop_loss;
    if (risk_price <= 0.0) {
        return false;
    }

    const double asymmetry_pressure = computeHistoryRewardRiskAsymmetryPressure(signal, cfg);
    if (asymmetry_pressure > 1e-9) {
        const bool hostile_regime =
            signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
            signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
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
    const bool trend_cont_strategy =
        signal.strategy_name == "Advanced Momentum" ||
        signal.strategy_name == "Breakout Strategy";
    const bool off_trend_regime =
        trend_cont_strategy &&
        signal.market_regime != autolife::analytics::MarketRegime::TRENDING_UP;
    if (off_trend_regime) {
        double rr_role_add = 0.14;
        if (signal.market_regime == autolife::analytics::MarketRegime::RANGING &&
            signal.liquidity_score > 0.0 &&
            signal.liquidity_score < 60.0) {
            rr_role_add += 0.06;
        }
        if (signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
            signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN) {
            rr_role_add += 0.10;
        }
        if (signal.strength < 0.60) {
            rr_role_add += 0.05;
        }
        if (signal.reason == "alpha_head_fallback_candidate") {
            rr_role_add += 0.05;
        }
        target_rr += rr_role_add;
    }
    if (asymmetry_pressure > 1e-9) {
        const bool favorable_regime =
            signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP ||
            signal.market_regime == autolife::analytics::MarketRegime::RANGING;
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

double computeEffectiveRoundTripCostPct(
    const autolife::strategy::Signal& signal,
    const autolife::engine::EngineConfig& cfg
) {
    const double fee_rate_per_side = autolife::Config::getInstance().getFeeRate();
    double slippage_per_side = std::clamp(cfg.max_slippage_pct * 0.35, 0.00045, 0.00120);
    if (signal.liquidity_score >= 70.0 && signal.volatility <= 3.5) {
        slippage_per_side *= 0.85;
    } else if (signal.liquidity_score > 0.0 && signal.liquidity_score < 50.0) {
        slippage_per_side *= 1.10;
    }
    if (signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN) {
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

double computeStrategyHistoryWinProbPrior(
    const autolife::strategy::Signal& signal,
    const autolife::engine::EngineConfig& cfg
) {
    const int min_sample = std::max(8, cfg.min_strategy_trades_for_ev / 2);
    if (signal.strategy_trade_count < min_sample || !std::isfinite(signal.strategy_win_rate)) {
        return 0.0;
    }

    const double history_win = std::clamp(signal.strategy_win_rate, 0.20, 0.82);
    const double history_pf = (signal.strategy_profit_factor > 0.0)
        ? std::clamp(signal.strategy_profit_factor, 0.50, 2.20)
        : 1.0;

    double prior = ((history_win - 0.50) * 0.22) + ((history_pf - 1.0) * 0.05);
    if (signal.strategy_win_rate < 0.42) {
        prior -= 0.02;
    }
    if (signal.strategy_profit_factor > 0.0 && signal.strategy_profit_factor < cfg.min_strategy_profit_factor) {
        prior -= 0.02;
    }
    if (signal.strategy_win_rate > 0.56 && signal.strategy_profit_factor >= 1.20) {
        prior += 0.01;
    }

    return std::clamp(prior, -0.10, 0.07);
}

double computeCalibratedExpectedEdgePct(
    const autolife::strategy::Signal& signal,
    const autolife::engine::EngineConfig& cfg
) {
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

    const double rr = gross_reward_pct / gross_risk_pct;
    double win_prob = std::clamp(0.24 + (signal.strength * 0.50), 0.20, 0.78);
    if (signal.liquidity_score > 0.0) {
        win_prob += std::clamp((signal.liquidity_score - 60.0) / 220.0, -0.04, 0.05);
    }
    if (signal.volatility > 0.0) {
        win_prob -= std::clamp((signal.volatility - 2.0) / 30.0, 0.0, 0.08);
    }
    if (rr > 1.60) {
        win_prob += std::min(0.05, (rr - 1.60) * 0.04);
    } else if (rr < 1.20) {
        win_prob -= std::min(0.06, (1.20 - rr) * 0.10);
    }

    switch (signal.market_regime) {
        case autolife::analytics::MarketRegime::TRENDING_UP:
            win_prob += 0.02;
            break;
        case autolife::analytics::MarketRegime::RANGING:
            win_prob -= 0.02;
            break;
        case autolife::analytics::MarketRegime::HIGH_VOLATILITY:
            win_prob -= 0.04;
            break;
        case autolife::analytics::MarketRegime::TRENDING_DOWN:
            win_prob -= 0.05;
            break;
        default:
            break;
    }
    if (signal.reason == "alpha_head_fallback_candidate") {
        if (signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP) {
            win_prob += 0.08;
        } else if (signal.market_regime == autolife::analytics::MarketRegime::RANGING ||
                   signal.market_regime == autolife::analytics::MarketRegime::UNKNOWN) {
            win_prob += 0.05;
        } else {
            win_prob += 0.01;
        }
        if (signal.liquidity_score >= 60.0) {
            win_prob += 0.03;
        } else if (signal.liquidity_score > 0.0 && signal.liquidity_score < 50.0) {
            win_prob -= 0.02;
        }
        if (signal.volatility > 0.0 && signal.volatility <= 3.0) {
            win_prob += 0.02;
        }
    }

    // Archetype-level priors from loss-cluster diagnostics.
    // Keep it market-agnostic: apply only by strategy/archetype/regime.
    const bool is_breakout_cont = (signal.entry_archetype == "BREAKOUT_CONTINUATION");
    const bool is_trend_reacc = (signal.entry_archetype == "TREND_REACCELERATION");
    const bool is_scalping = (signal.strategy_name == "Advanced Scalping");
    const bool is_momentum = (signal.strategy_name == "Advanced Momentum");
    const bool is_breakout_strategy = (signal.strategy_name == "Breakout Strategy");
    const bool is_consolidation_break = (signal.entry_archetype == "CONSOLIDATION_BREAK");

    if (is_scalping && is_breakout_cont) {
        if (signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP) {
            win_prob -= 0.14;
        } else if (signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN) {
            win_prob -= 0.18;
        }
    }
    if (is_momentum && is_breakout_cont) {
        if (signal.market_regime == autolife::analytics::MarketRegime::RANGING) {
            win_prob -= 0.14;
        } else if (signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP) {
            win_prob -= 0.08;
        }
    }
    if (is_momentum && is_trend_reacc &&
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP) {
        win_prob -= 0.09;
    }
    if (is_breakout_strategy && is_consolidation_break &&
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP) {
        win_prob -= 0.08;
    }

    win_prob += computeStrategyHistoryWinProbPrior(signal, cfg);
    win_prob = std::clamp(win_prob, 0.12, 0.88);

    double round_trip_cost_pct = computeEffectiveRoundTripCostPct(signal, cfg);
    if (signal.reason == "alpha_head_fallback_candidate" && signal.liquidity_score >= 58.0) {
        round_trip_cost_pct *= 0.88;
    }
    const double expected_gross_pct = (win_prob * gross_reward_pct) - ((1.0 - win_prob) * gross_risk_pct);
    return expected_gross_pct - round_trip_cost_pct;
}

enum class SecondStageConfirmationFailureKind {
    None,
    RRMarginShortfall,
    EdgeMarginShortfall,
    HostileSafetyAdders,
};

enum class SecondStageConfirmationSafetySource {
    Unknown,
    Regime,
    Liquidity,
    StrategyHistory,
    DynamicTighten,
};

enum class SecondStageHistorySafetySeverity {
    Mild,
    Moderate,
    Severe,
};

struct EntryQualityHeadSnapshot {
    bool pass = false;
    double reward_risk_ratio = 0.0;
    double calibrated_expected_edge_pct = 0.0;
    double reward_risk_gate = 0.0;
    double expected_edge_gate = 0.0;
};

struct SecondStageConfirmationSnapshot {
    bool pass = false;
    bool baseline_gate_pass = false;
    SecondStageConfirmationFailureKind failure = SecondStageConfirmationFailureKind::None;
    SecondStageConfirmationSafetySource safety_source = SecondStageConfirmationSafetySource::Unknown;
    double rr_margin = 0.0;
    double edge_margin = 0.0;
    double min_rr_margin = 0.0;
    double min_edge_margin = 0.0;
    double baseline_rr_score = 0.0;
    double baseline_edge_score = 0.0;
    double rr_margin_score = 0.0;
    double edge_margin_score = 0.0;
    double rr_margin_gap = 0.0;
    double edge_margin_gap = 0.0;
    bool rr_margin_near_miss = false;
    bool rr_margin_soft_score_applied = false;
    double rr_margin_soft_score_floor_value = 0.0;
    double head_score = 0.0;
};

struct TwoHeadEntryAggregationSnapshot {
    bool enabled = false;
    bool entry_head_pass = false;
    bool second_stage_head_pass = false;
    bool aggregate_pass = false;
    bool override_applied = false;
    bool override_allowed = false;
    bool rr_margin_near_miss_relief_applied = false;
    bool rr_margin_near_miss_head_score_floor_applied = false;
    bool rr_margin_near_miss_floor_relax_applied = false;
    bool rr_margin_near_miss_adaptive_floor_relax_applied = false;
    bool rr_margin_near_miss_relief_blocked = false;
    bool rr_margin_near_miss_relief_blocked_override_disallowed = false;
    bool rr_margin_near_miss_relief_blocked_entry_floor = false;
    bool rr_margin_near_miss_relief_blocked_second_stage_floor = false;
    bool rr_margin_near_miss_relief_blocked_aggregate_score = false;
    bool rr_margin_near_miss_surplus_compensation_applied = false;
    bool rr_margin_near_miss_surplus_second_stage_compensated = false;
    bool rr_margin_near_miss_surplus_aggregate_compensated = false;
    double entry_head_score = 0.0;
    double second_stage_head_score = 0.0;
    double aggregate_score = 0.0;
    double aggregate_score_before_surplus_compensation = 0.0;
    double rr_margin_near_miss_surplus_bonus = 0.0;
    double rr_margin_near_miss_head_score_floor_value = 0.0;
    double rr_margin_near_miss_adaptive_floor_relax_strength = 0.0;
    double entry_weight = 0.5;
    double second_stage_weight = 0.5;
    double min_entry_score = 1.0;
    double min_second_stage_score = 1.0;
    double min_aggregate_score = 1.0;
    double effective_min_second_stage_score = 1.0;
    double effective_min_aggregate_score = 1.0;
    double entry_surplus = 0.0;
    double entry_deficit = 0.0;
    double second_stage_deficit = 0.0;
    double aggregate_deficit = 0.0;
};

double computeRatioScore(double numerator, double denominator) {
    if (!std::isfinite(numerator) || !std::isfinite(denominator) || denominator <= 1e-12) {
        return 0.0;
    }
    return std::clamp(numerator / denominator, 0.0, 2.5);
}

double computeSecondStageRRMarginSoftScoreFloor(
    const autolife::engine::EngineConfig& cfg,
    double baseline_rr_score,
    double rr_margin,
    double rr_margin_gap
) {
    if (!cfg.enable_second_stage_rr_margin_soft_score) {
        return 0.0;
    }
    if (!std::isfinite(rr_margin) || !std::isfinite(rr_margin_gap) || rr_margin >= 0.0) {
        return 0.0;
    }
    const double max_gap = std::clamp(cfg.second_stage_rr_margin_soft_score_max_gap, 1e-6, 0.10);
    if (rr_margin_gap <= 0.0 || rr_margin_gap > max_gap) {
        return 0.0;
    }
    const double base_floor = std::clamp(cfg.second_stage_rr_margin_soft_score_floor, 0.0, 1.20);
    const double gap_weight = std::clamp(
        cfg.second_stage_rr_margin_soft_score_gap_tightness_weight,
        0.0,
        0.80
    );
    const double gap_tightness = 1.0 - std::clamp(rr_margin_gap / max_gap, 0.0, 1.0);
    const double baseline_anchor = std::clamp(baseline_rr_score, 0.0, 1.50);
    const double floor = base_floor + (gap_weight * gap_tightness);
    return std::clamp(std::min(baseline_anchor, floor), 0.0, 1.50);
}

double computeEntryQualityHeadScore(const EntryQualityHeadSnapshot& snapshot) {
    const double rr_score = computeRatioScore(
        snapshot.reward_risk_ratio,
        snapshot.reward_risk_gate
    );
    const double edge_score = computeRatioScore(
        snapshot.calibrated_expected_edge_pct,
        snapshot.expected_edge_gate
    );
    return std::clamp(std::min(rr_score, edge_score), 0.0, 2.5);
}

SecondStageHistorySafetySeverity classifySecondStageHistorySafetySeverity(
    const autolife::strategy::Signal& signal
) {
    const double win_rate = signal.strategy_win_rate;
    const double pf = signal.strategy_profit_factor;
    const bool severe =
        win_rate < 0.42 ||
        (pf > 0.0 && pf < 0.90);
    if (severe) {
        return SecondStageHistorySafetySeverity::Severe;
    }
    const bool moderate =
        win_rate < 0.45 ||
        (pf > 0.0 && pf < 0.95);
    if (moderate) {
        return SecondStageHistorySafetySeverity::Moderate;
    }
    return SecondStageHistorySafetySeverity::Mild;
}

const char* secondStageFailureReasonCode(SecondStageConfirmationFailureKind failure) {
    switch (failure) {
        case SecondStageConfirmationFailureKind::RRMarginShortfall:
            return "rr_margin";
        case SecondStageConfirmationFailureKind::EdgeMarginShortfall:
            return "edge_margin";
        case SecondStageConfirmationFailureKind::HostileSafetyAdders:
            return "hostile_safety_adders";
        case SecondStageConfirmationFailureKind::None:
        default:
            return "none";
    }
}

const char* secondStageSafetySourceReasonCode(SecondStageConfirmationSafetySource source) {
    switch (source) {
        case SecondStageConfirmationSafetySource::Regime:
            return "regime";
        case SecondStageConfirmationSafetySource::Liquidity:
            return "liquidity";
        case SecondStageConfirmationSafetySource::StrategyHistory:
            return "history";
        case SecondStageConfirmationSafetySource::DynamicTighten:
            return "dynamic_tighten";
        case SecondStageConfirmationSafetySource::Unknown:
        default:
            return "unknown";
    }
}

SecondStageConfirmationSnapshot evaluateSecondStageEntryConfirmation(
    const autolife::strategy::Signal& signal,
    const autolife::engine::EngineConfig& cfg,
    double reward_risk_ratio,
    double rr_gate,
    double calibrated_expected_edge_pct,
    double edge_gate
) {
    SecondStageConfirmationSnapshot snapshot;
    snapshot.baseline_rr_score = computeRatioScore(reward_risk_ratio, rr_gate);
    snapshot.baseline_edge_score = computeRatioScore(calibrated_expected_edge_pct, edge_gate);
    snapshot.baseline_gate_pass =
        reward_risk_ratio >= rr_gate &&
        calibrated_expected_edge_pct >= edge_gate;
    if (!snapshot.baseline_gate_pass) {
        const bool rr_failed = reward_risk_ratio < rr_gate;
        const bool edge_failed = calibrated_expected_edge_pct < edge_gate;
        if (rr_failed && !edge_failed) {
            snapshot.failure = SecondStageConfirmationFailureKind::RRMarginShortfall;
        } else if (edge_failed && !rr_failed) {
            snapshot.failure = SecondStageConfirmationFailureKind::EdgeMarginShortfall;
        } else {
            const double rr_gap = std::max(0.0, rr_gate - reward_risk_ratio);
            const double edge_gap = std::max(0.0, edge_gate - calibrated_expected_edge_pct);
            const double rr_norm = rr_gap / std::max(rr_gate, 1e-6);
            const double edge_norm = edge_gap / std::max(edge_gate, 1e-9);
            snapshot.failure = (rr_norm >= edge_norm)
                ? SecondStageConfirmationFailureKind::RRMarginShortfall
                : SecondStageConfirmationFailureKind::EdgeMarginShortfall;
        }
        snapshot.head_score = std::clamp(
            std::min(snapshot.baseline_rr_score, snapshot.baseline_edge_score),
            0.0,
            2.5
        );
        return snapshot;
    }

    const double round_trip_cost_pct = computeEffectiveRoundTripCostPct(signal, cfg);
    double min_rr_margin = 0.045;
    double min_edge_margin = std::max(0.00007, round_trip_cost_pct * 0.11);
    double safety_rr_add = 0.0;
    double safety_edge_add = 0.0;
    double safety_regime_rr_add = 0.0;
    double safety_regime_edge_add = 0.0;
    double safety_liquidity_rr_add = 0.0;
    double safety_liquidity_edge_add = 0.0;
    double safety_history_rr_add = 0.0;
    double safety_history_edge_add = 0.0;
    double safety_dynamic_rr_add = 0.0;
    double safety_dynamic_edge_add = 0.0;

    if (signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN) {
        min_rr_margin += 0.07;
        min_edge_margin += 0.00012;
        safety_rr_add += 0.07;
        safety_edge_add += 0.00012;
        safety_regime_rr_add += 0.07;
        safety_regime_edge_add += 0.00012;
    }
    if (signal.liquidity_score > 0.0 && signal.liquidity_score < 55.0) {
        min_rr_margin += 0.04;
        min_edge_margin += 0.00008;
        safety_rr_add += 0.04;
        safety_edge_add += 0.00008;
        safety_liquidity_rr_add += 0.04;
        safety_liquidity_edge_add += 0.00008;
    }
    if (signal.strategy_trade_count >= std::max(8, cfg.min_strategy_trades_for_ev / 2) &&
        (signal.strategy_win_rate < 0.45 ||
         (signal.strategy_profit_factor > 0.0 && signal.strategy_profit_factor < 0.95))) {
        const SecondStageHistorySafetySeverity history_severity =
            classifySecondStageHistorySafetySeverity(signal);
        double history_rr_add = 0.03;
        double history_edge_add = 0.00006;
        if (history_severity == SecondStageHistorySafetySeverity::Moderate) {
            history_rr_add *= 1.05;
            history_edge_add *= 1.05;
        } else if (history_severity == SecondStageHistorySafetySeverity::Severe) {
            const double severe_scale = std::clamp(
                cfg.second_stage_history_safety_severe_scale,
                1.0,
                2.0
            );
            history_rr_add *= severe_scale;
            history_edge_add *= severe_scale;

            if (cfg.enable_second_stage_history_safety_severe_relief) {
                const bool history_hostile_regime =
                    signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
                    signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
                const bool regime_relief_allowed =
                    !cfg.second_stage_history_safety_relief_block_hostile_regime ||
                    !history_hostile_regime;
                const bool quality_relief_ready =
                    regime_relief_allowed &&
                    signal.strategy_trade_count >= std::max(
                        cfg.second_stage_history_safety_relief_min_strategy_trades,
                        std::max(8, cfg.min_strategy_trades_for_ev / 2)
                    ) &&
                    signal.strength >= cfg.second_stage_history_safety_relief_min_signal_strength &&
                    signal.expected_value >= cfg.second_stage_history_safety_relief_min_expected_value &&
                    signal.liquidity_score >= cfg.second_stage_history_safety_relief_min_liquidity_score;
                if (quality_relief_ready) {
                    const double strength_score = std::clamp(
                        (signal.strength - cfg.second_stage_history_safety_relief_min_signal_strength) / 0.18,
                        0.0,
                        1.0
                    );
                    const double edge_score = std::clamp(
                        (signal.expected_value - cfg.second_stage_history_safety_relief_min_expected_value) / 0.0010,
                        0.0,
                        1.0
                    );
                    const double liquidity_score = std::clamp(
                        (signal.liquidity_score - cfg.second_stage_history_safety_relief_min_liquidity_score) / 16.0,
                        0.0,
                        1.0
                    );
                    const double relief_quality = (strength_score + edge_score + liquidity_score) / 3.0;
                    const double relief_scale = std::clamp(
                        cfg.second_stage_history_safety_relief_max_scale * relief_quality,
                        0.0,
                        0.80
                    );
                    history_rr_add *= (1.0 - relief_scale);
                    history_edge_add *= (1.0 - relief_scale);
                }
            }
        }
        min_rr_margin += history_rr_add;
        min_edge_margin += history_edge_add;
        safety_rr_add += history_rr_add;
        safety_edge_add += history_edge_add;
        safety_history_rr_add += history_rr_add;
        safety_history_edge_add += history_edge_add;
    }
    if (signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP &&
        signal.strength >= 0.74 &&
        signal.liquidity_score >= 62.0) {
        min_rr_margin -= 0.02;
        min_edge_margin -= 0.00003;
    }
    if (signal.reason == "alpha_head_fallback_candidate" &&
        (signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP ||
         signal.market_regime == autolife::analytics::MarketRegime::RANGING) &&
        signal.liquidity_score >= 58.0) {
        min_rr_margin -= 0.015;
        min_edge_margin -= 0.00002;
    }

    const bool hostile_regime =
        signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    const bool favorable_regime =
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP ||
        signal.market_regime == autolife::analytics::MarketRegime::RANGING;

    double hostility_pressure = 0.0;
    if (hostile_regime) {
        hostility_pressure += 0.45;
    }
    if (signal.liquidity_score > 0.0 && signal.liquidity_score < 58.0) {
        hostility_pressure += std::clamp((58.0 - signal.liquidity_score) / 38.0, 0.0, 0.30);
    }
    if (signal.strategy_trade_count >= std::max(8, cfg.min_strategy_trades_for_ev / 2)) {
        if (signal.strategy_win_rate < 0.44) {
            hostility_pressure += 0.12;
        }
        if (signal.strategy_profit_factor > 0.0 && signal.strategy_profit_factor < 0.95) {
            hostility_pressure += 0.12;
        }
    }

    double quality_conf = 0.0;
    if (favorable_regime) {
        quality_conf += 0.18;
    }
    quality_conf += std::clamp((signal.strength - 0.64) / 0.22, 0.0, 1.0) * 0.30;
    quality_conf += std::clamp((signal.liquidity_score - 55.0) / 20.0, 0.0, 1.0) * 0.22;
    quality_conf += std::clamp((signal.expected_value - 0.0004) / 0.0011, 0.0, 1.0) * 0.20;
    if (signal.strategy_trade_count >= std::max(10, cfg.min_strategy_trades_for_ev / 2) &&
        signal.strategy_win_rate >= 0.53 &&
        signal.strategy_profit_factor >= 1.03) {
        quality_conf += 0.15;
    }
    if (signal.strategy_trade_count >= std::max(14, cfg.min_strategy_trades_for_ev / 2) &&
        signal.strategy_win_rate >= 0.58 &&
        signal.strategy_profit_factor >= 1.10) {
        quality_conf += 0.10;
    }

    hostility_pressure = std::clamp(hostility_pressure, 0.0, 1.0);
    quality_conf = std::clamp(quality_conf, 0.0, 1.0);
    double relief_scale = std::clamp(quality_conf - (0.70 * hostility_pressure), 0.0, 1.0);
    const double tighten_scale = std::clamp(hostility_pressure - (0.55 * quality_conf), 0.0, 1.0);
    if (signal.expected_value < 0.00075) {
        relief_scale *= 0.55;
    }
    if (signal.liquidity_score > 0.0 && signal.liquidity_score < 58.0) {
        relief_scale *= 0.70;
    }
    if (hostile_regime) {
        relief_scale *= 0.35;
    }
    if (signal.strategy_trade_count >= std::max(10, cfg.min_strategy_trades_for_ev / 2) &&
        (signal.strategy_win_rate < 0.50 ||
         (signal.strategy_profit_factor > 0.0 && signal.strategy_profit_factor < 1.00))) {
        relief_scale *= 0.65;
    }
    if (signal.reason == "alpha_head_fallback_candidate" && signal.expected_value < 0.0009) {
        relief_scale *= 0.75;
    }
    relief_scale = std::clamp(relief_scale, 0.0, 1.0);

    const double dynamic_safety_rr_add = 0.050 * tighten_scale;
    const double dynamic_safety_edge_add = 0.00010 * tighten_scale;
    min_rr_margin += dynamic_safety_rr_add;
    min_edge_margin += dynamic_safety_edge_add;
    safety_rr_add += dynamic_safety_rr_add;
    safety_edge_add += dynamic_safety_edge_add;
    safety_dynamic_rr_add += dynamic_safety_rr_add;
    safety_dynamic_edge_add += dynamic_safety_edge_add;
    min_rr_margin -= (0.028 * relief_scale);
    min_edge_margin -= (0.000045 * relief_scale);

    const double rr_gate_pressure = std::clamp(
        (rr_gate - cfg.min_reward_risk) / 0.40,
        0.0,
        1.0
    );
    const double edge_gate_pressure = std::clamp(
        (edge_gate - cfg.min_expected_edge_pct) / 0.0010,
        0.0,
        1.0
    );
    min_rr_margin -= (0.010 * relief_scale * rr_gate_pressure);
    min_edge_margin -= (0.000020 * relief_scale * edge_gate_pressure);

    min_rr_margin = std::clamp(min_rr_margin, 0.015, 0.24);
    min_edge_margin = std::clamp(min_edge_margin, 0.00003, 0.00050);

    const double rr_margin = reward_risk_ratio - rr_gate;
    const double edge_margin = calibrated_expected_edge_pct - edge_gate;
    snapshot.rr_margin = rr_margin;
    snapshot.edge_margin = edge_margin;
    snapshot.min_rr_margin = min_rr_margin;
    snapshot.min_edge_margin = min_edge_margin;
    snapshot.rr_margin_score = computeRatioScore(rr_margin, min_rr_margin);
    snapshot.edge_margin_score = computeRatioScore(edge_margin, min_edge_margin);
    snapshot.rr_margin_gap = std::max(0.0, min_rr_margin - rr_margin);
    snapshot.edge_margin_gap = std::max(0.0, min_edge_margin - edge_margin);
    const double rr_soft_score_floor = computeSecondStageRRMarginSoftScoreFloor(
        cfg,
        snapshot.baseline_rr_score,
        rr_margin,
        snapshot.rr_margin_gap
    );
    snapshot.rr_margin_soft_score_floor_value = rr_soft_score_floor;
    if (rr_soft_score_floor > snapshot.rr_margin_score) {
        snapshot.rr_margin_score = rr_soft_score_floor;
        snapshot.rr_margin_soft_score_applied = true;
    }
    snapshot.head_score = std::clamp(
        std::min(snapshot.rr_margin_score, snapshot.edge_margin_score),
        0.0,
        2.5
    );

    const bool rr_failed = rr_margin < min_rr_margin;
    const bool edge_failed = edge_margin < min_edge_margin;
    const bool rr_only_failed = rr_failed && !edge_failed;
    if (cfg.enable_second_stage_rr_margin_near_miss_relief &&
        rr_only_failed &&
        snapshot.rr_margin_gap > 0.0 &&
        snapshot.rr_margin_gap <= std::max(0.0, cfg.second_stage_rr_margin_near_miss_max_gap)) {
        snapshot.rr_margin_near_miss = true;
    }
    if (!rr_failed && !edge_failed) {
        snapshot.pass = true;
        return snapshot;
    }

    SecondStageConfirmationFailureKind failure = SecondStageConfirmationFailureKind::None;
    const double rr_relaxed_threshold = std::max(0.0, min_rr_margin - safety_rr_add);
    const double edge_relaxed_threshold = std::max(0.0, min_edge_margin - safety_edge_add);
    const bool rr_safety_only =
        rr_failed &&
        safety_rr_add > 1e-9 &&
        rr_margin >= rr_relaxed_threshold;
    const bool edge_safety_only =
        edge_failed &&
        safety_edge_add > 1e-12 &&
        edge_margin >= edge_relaxed_threshold;
    if (rr_safety_only || edge_safety_only) {
        failure = SecondStageConfirmationFailureKind::HostileSafetyAdders;
        SecondStageConfirmationSafetySource safety_source = SecondStageConfirmationSafetySource::Unknown;
        double regime_pressure = 0.0;
        double liquidity_pressure = 0.0;
        double history_pressure = 0.0;
        double dynamic_pressure = 0.0;
        if (rr_safety_only) {
            const double rr_norm_den = std::max(min_rr_margin, 1e-6);
            regime_pressure += safety_regime_rr_add / rr_norm_den;
            liquidity_pressure += safety_liquidity_rr_add / rr_norm_den;
            history_pressure += safety_history_rr_add / rr_norm_den;
            dynamic_pressure += safety_dynamic_rr_add / rr_norm_den;
        }
        if (edge_safety_only) {
            const double edge_norm_den = std::max(min_edge_margin, 1e-9);
            regime_pressure += safety_regime_edge_add / edge_norm_den;
            liquidity_pressure += safety_liquidity_edge_add / edge_norm_den;
            history_pressure += safety_history_edge_add / edge_norm_den;
            dynamic_pressure += safety_dynamic_edge_add / edge_norm_den;
        }
        const double max_pressure = std::max(
            std::max(regime_pressure, liquidity_pressure),
            std::max(history_pressure, dynamic_pressure)
        );
        if (max_pressure > 1e-12) {
            if (regime_pressure >= liquidity_pressure &&
                regime_pressure >= history_pressure &&
                regime_pressure >= dynamic_pressure) {
                safety_source = SecondStageConfirmationSafetySource::Regime;
            } else if (liquidity_pressure >= regime_pressure &&
                       liquidity_pressure >= history_pressure &&
                       liquidity_pressure >= dynamic_pressure) {
                safety_source = SecondStageConfirmationSafetySource::Liquidity;
            } else if (history_pressure >= regime_pressure &&
                       history_pressure >= liquidity_pressure &&
                       history_pressure >= dynamic_pressure) {
                safety_source = SecondStageConfirmationSafetySource::StrategyHistory;
            } else {
                safety_source = SecondStageConfirmationSafetySource::DynamicTighten;
            }
        }
        snapshot.safety_source = safety_source;
    } else if (rr_failed && !edge_failed) {
        failure = SecondStageConfirmationFailureKind::RRMarginShortfall;
    } else if (edge_failed && !rr_failed) {
        failure = SecondStageConfirmationFailureKind::EdgeMarginShortfall;
    } else {
        const double rr_gap = std::max(0.0, min_rr_margin - rr_margin);
        const double edge_gap = std::max(0.0, min_edge_margin - edge_margin);
        const double rr_norm = rr_gap / std::max(min_rr_margin, 1e-6);
        const double edge_norm = edge_gap / std::max(min_edge_margin, 1e-9);
        failure = (rr_norm >= edge_norm)
            ? SecondStageConfirmationFailureKind::RRMarginShortfall
            : SecondStageConfirmationFailureKind::EdgeMarginShortfall;
    }

    snapshot.failure = failure;
    return snapshot;
}

bool passesSecondStageEntryConfirmation(
    const autolife::strategy::Signal& signal,
    const autolife::engine::EngineConfig& cfg,
    double reward_risk_ratio,
    double rr_gate,
    double calibrated_expected_edge_pct,
    double edge_gate,
    SecondStageConfirmationFailureKind* out_failure = nullptr,
    SecondStageConfirmationSafetySource* out_safety_source = nullptr
) {
    const SecondStageConfirmationSnapshot snapshot =
        evaluateSecondStageEntryConfirmation(
            signal,
            cfg,
            reward_risk_ratio,
            rr_gate,
            calibrated_expected_edge_pct,
            edge_gate
        );
    if (out_failure != nullptr) {
        *out_failure = snapshot.failure;
    }
    if (out_safety_source != nullptr) {
        *out_safety_source = snapshot.safety_source;
    }
    return snapshot.pass;
}

TwoHeadEntryAggregationSnapshot evaluateTwoHeadEntryAggregation(
    const autolife::strategy::Signal& signal,
    const autolife::engine::EngineConfig& cfg,
    const EntryQualityHeadSnapshot& entry_quality_head,
    const SecondStageConfirmationSnapshot& second_stage_eval
) {
    TwoHeadEntryAggregationSnapshot snapshot;
    snapshot.enabled = cfg.enable_two_head_entry_second_stage_aggregation;
    snapshot.entry_head_pass = entry_quality_head.pass;
    snapshot.second_stage_head_pass = second_stage_eval.pass;
    snapshot.entry_head_score = computeEntryQualityHeadScore(entry_quality_head);
    snapshot.second_stage_head_score = std::clamp(second_stage_eval.head_score, 0.0, 2.5);
    if (!std::isfinite(snapshot.second_stage_head_score) || snapshot.second_stage_head_score <= 0.0) {
        snapshot.second_stage_head_score = std::clamp(
            std::min(second_stage_eval.baseline_rr_score, second_stage_eval.baseline_edge_score),
            0.0,
            2.5
        );
    }

    const double raw_entry_weight = std::max(0.0, cfg.two_head_entry_quality_weight);
    const double raw_second_weight = std::max(0.0, cfg.two_head_second_stage_weight);
    const double weight_sum = raw_entry_weight + raw_second_weight;
    if (weight_sum > 1e-12) {
        snapshot.entry_weight = raw_entry_weight / weight_sum;
        snapshot.second_stage_weight = raw_second_weight / weight_sum;
    }
    snapshot.min_entry_score = std::clamp(cfg.two_head_min_entry_quality_score, 0.50, 1.20);
    snapshot.min_second_stage_score = std::clamp(cfg.two_head_min_second_stage_score, 0.50, 1.20);
    snapshot.min_aggregate_score = std::clamp(cfg.two_head_min_aggregate_score, 0.80, 1.20);
    snapshot.effective_min_second_stage_score = snapshot.min_second_stage_score;
    snapshot.effective_min_aggregate_score = snapshot.min_aggregate_score;
    snapshot.aggregate_score =
        (snapshot.entry_weight * snapshot.entry_head_score) +
        (snapshot.second_stage_weight * snapshot.second_stage_head_score);
    snapshot.entry_surplus = std::max(0.0, snapshot.entry_head_score - snapshot.min_entry_score);
    snapshot.entry_deficit = std::max(0.0, snapshot.min_entry_score - snapshot.entry_head_score);
    snapshot.second_stage_deficit = std::max(
        0.0,
        snapshot.min_second_stage_score - snapshot.second_stage_head_score
    );
    snapshot.aggregate_deficit = std::max(
        0.0,
        snapshot.min_aggregate_score - snapshot.aggregate_score
    );

    if (snapshot.entry_head_pass && snapshot.second_stage_head_pass) {
        snapshot.aggregate_pass = true;
        snapshot.override_applied = false;
        snapshot.override_allowed = false;
        return snapshot;
    }
    if (!snapshot.enabled) {
        snapshot.aggregate_pass = false;
        snapshot.override_applied = false;
        snapshot.override_allowed = false;
        return snapshot;
    }

    const bool high_stress_regime =
        signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    const bool high_stress_blocked =
        cfg.two_head_aggregation_block_high_stress_regime && high_stress_regime;
    const bool has_min_history =
        signal.strategy_trade_count >= std::max(0, cfg.two_head_aggregation_min_strategy_trades);
    const double near_miss_max_gap = std::max(0.0, cfg.second_stage_rr_margin_near_miss_max_gap);
    const bool near_miss_gap_ok =
        second_stage_eval.rr_margin_near_miss &&
        second_stage_eval.rr_margin_gap > 0.0 &&
        second_stage_eval.rr_margin_gap <= near_miss_max_gap;
    const bool near_miss_regime_allowed =
        !cfg.second_stage_rr_margin_near_miss_block_high_stress_regime || !high_stress_regime;
    const bool near_miss_history_ok =
        signal.strategy_trade_count >= std::max(0, cfg.second_stage_rr_margin_near_miss_min_strategy_trades);
    const bool near_miss_quality_ok =
        signal.strength >= cfg.second_stage_rr_margin_near_miss_min_signal_strength &&
        signal.expected_value >= cfg.second_stage_rr_margin_near_miss_min_expected_value &&
        signal.liquidity_score >= cfg.second_stage_rr_margin_near_miss_min_liquidity_score;
    double near_miss_quality_score = 0.0;
    double near_miss_gap_tightness = 0.0;
    if (cfg.enable_second_stage_rr_margin_near_miss_relief &&
        near_miss_gap_ok &&
        near_miss_regime_allowed &&
        near_miss_history_ok &&
        near_miss_quality_ok) {
        const double strength_score = std::clamp(
            (signal.strength - cfg.second_stage_rr_margin_near_miss_min_signal_strength) / 0.18,
            0.0,
            1.0
        );
        const double edge_score = std::clamp(
            (signal.expected_value - cfg.second_stage_rr_margin_near_miss_min_expected_value) / 0.0012,
            0.0,
            1.0
        );
        const double liquidity_score = std::clamp(
            (signal.liquidity_score - cfg.second_stage_rr_margin_near_miss_min_liquidity_score) / 16.0,
            0.0,
            1.0
        );
        near_miss_quality_score = (strength_score + edge_score + liquidity_score) / 3.0;
        near_miss_gap_tightness = 1.0 - std::clamp(
            second_stage_eval.rr_margin_gap / std::max(near_miss_max_gap, 1e-9),
            0.0,
            1.0
        );
        const double base_boost = std::clamp(
            cfg.second_stage_rr_margin_near_miss_score_boost,
            0.0,
            0.25
        );
        const double boost_scale = std::clamp(
            (0.45 * near_miss_quality_score) + (0.55 * near_miss_gap_tightness),
            0.0,
            1.0
        );
        const double applied_boost = base_boost * boost_scale;
        if (applied_boost > 1e-12) {
            snapshot.second_stage_head_score = std::clamp(
                snapshot.second_stage_head_score + applied_boost,
                0.0,
                2.5
            );
            snapshot.rr_margin_near_miss_relief_applied = true;
        }
    }
    if (snapshot.rr_margin_near_miss_relief_applied &&
        cfg.enable_second_stage_rr_margin_near_miss_head_score_floor) {
        const double floor_base = std::clamp(
            cfg.second_stage_rr_margin_near_miss_head_score_floor_base,
            0.0,
            1.30
        );
        const double floor_q_weight = std::clamp(
            cfg.second_stage_rr_margin_near_miss_head_score_floor_quality_weight,
            0.0,
            1.00
        );
        const double floor_gap_weight = std::clamp(
            cfg.second_stage_rr_margin_near_miss_head_score_floor_gap_weight,
            0.0,
            1.00
        );
        const double floor_cap = std::clamp(
            cfg.second_stage_rr_margin_near_miss_head_score_floor_max,
            0.50,
            1.40
        );
        const double floor_value = std::min(
            floor_cap,
            floor_base +
                (floor_q_weight * near_miss_quality_score) +
                (floor_gap_weight * near_miss_gap_tightness)
        );
        snapshot.rr_margin_near_miss_head_score_floor_value = floor_value;
        if (floor_value > snapshot.second_stage_head_score) {
            snapshot.second_stage_head_score = floor_value;
            snapshot.rr_margin_near_miss_head_score_floor_applied = true;
        }
    }
    double second_floor_relax = 0.0;
    double aggregate_floor_relax = 0.0;
    if (snapshot.rr_margin_near_miss_relief_applied &&
        cfg.enable_two_head_rr_margin_near_miss_floor_relax) {
        second_floor_relax = std::clamp(
            cfg.two_head_rr_margin_near_miss_second_stage_floor_relax,
            0.0,
            0.20
        );
        aggregate_floor_relax = std::clamp(
            cfg.two_head_rr_margin_near_miss_aggregate_floor_relax,
            0.0,
            0.15
        );
    }
    if (snapshot.rr_margin_near_miss_relief_applied &&
        cfg.enable_two_head_rr_margin_near_miss_adaptive_floor_relax) {
        const double quality_weight = std::clamp(
            cfg.two_head_rr_margin_near_miss_adaptive_floor_relax_quality_weight,
            0.0,
            1.00
        );
        const double gap_weight = std::clamp(
            cfg.two_head_rr_margin_near_miss_adaptive_floor_relax_gap_weight,
            0.0,
            1.00
        );
        const double adaptive_weight_sum = quality_weight + gap_weight;
        if (adaptive_weight_sum > 1e-12) {
            const double adaptive_strength = std::clamp(
                ((quality_weight * near_miss_quality_score) + (gap_weight * near_miss_gap_tightness)) /
                    adaptive_weight_sum,
                0.0,
                1.0
            );
            const double min_activation = std::clamp(
                cfg.two_head_rr_margin_near_miss_adaptive_floor_relax_min_activation,
                0.0,
                1.0
            );
            if (adaptive_strength >= min_activation) {
                const double max_second_stage_relax = std::clamp(
                    cfg.two_head_rr_margin_near_miss_adaptive_floor_relax_max_second_stage,
                    0.0,
                    0.20
                );
                const double max_aggregate_relax = std::clamp(
                    cfg.two_head_rr_margin_near_miss_adaptive_floor_relax_max_aggregate,
                    0.0,
                    0.15
                );
                second_floor_relax = std::max(second_floor_relax, max_second_stage_relax * adaptive_strength);
                aggregate_floor_relax = std::max(aggregate_floor_relax, max_aggregate_relax * adaptive_strength);
                snapshot.rr_margin_near_miss_adaptive_floor_relax_applied = true;
                snapshot.rr_margin_near_miss_adaptive_floor_relax_strength = adaptive_strength;
            }
        }
    }
    if (second_floor_relax > 1e-12 || aggregate_floor_relax > 1e-12) {
        snapshot.effective_min_second_stage_score = std::max(
            0.50,
            snapshot.min_second_stage_score - second_floor_relax
        );
        snapshot.effective_min_aggregate_score = std::max(
            0.80,
            snapshot.min_aggregate_score - aggregate_floor_relax
        );
        snapshot.rr_margin_near_miss_floor_relax_applied = true;
    }
    snapshot.second_stage_head_pass =
        second_stage_eval.pass ||
        snapshot.second_stage_head_score >= snapshot.effective_min_second_stage_score;
    snapshot.aggregate_score =
        (snapshot.entry_weight * snapshot.entry_head_score) +
        (snapshot.second_stage_weight * snapshot.second_stage_head_score);
    snapshot.entry_surplus = std::max(0.0, snapshot.entry_head_score - snapshot.min_entry_score);
    snapshot.entry_deficit = std::max(0.0, snapshot.min_entry_score - snapshot.entry_head_score);
    snapshot.second_stage_deficit = std::max(
        0.0,
        snapshot.effective_min_second_stage_score - snapshot.second_stage_head_score
    );
    snapshot.aggregate_deficit = std::max(
        0.0,
        snapshot.effective_min_aggregate_score - snapshot.aggregate_score
    );
    snapshot.override_allowed = !high_stress_blocked && has_min_history;
    const bool entry_floor_ok = snapshot.entry_head_score >= snapshot.min_entry_score;
    bool second_stage_floor_ok =
        snapshot.second_stage_head_score >= snapshot.effective_min_second_stage_score;
    bool aggregate_score_ok =
        snapshot.aggregate_score >= snapshot.effective_min_aggregate_score;

    if (snapshot.rr_margin_near_miss_relief_applied &&
        cfg.enable_two_head_rr_margin_near_miss_surplus_compensation) {
        const double min_entry_surplus = std::clamp(
            cfg.two_head_rr_margin_near_miss_surplus_min_entry_surplus,
            0.0,
            0.35
        );
        const double min_edge_score = std::clamp(
            cfg.two_head_rr_margin_near_miss_surplus_min_edge_score,
            0.80,
            1.60
        );
        const double max_second_stage_deficit = std::clamp(
            cfg.two_head_rr_margin_near_miss_surplus_max_second_stage_deficit,
            0.0,
            0.20
        );
        const double max_aggregate_deficit = std::clamp(
            cfg.two_head_rr_margin_near_miss_surplus_max_aggregate_deficit,
            0.0,
            0.20
        );
        const double bonus_weight = std::clamp(
            cfg.two_head_rr_margin_near_miss_surplus_entry_weight,
            0.0,
            1.50
        );
        const double max_bonus = std::clamp(
            cfg.two_head_rr_margin_near_miss_surplus_max_aggregate_bonus,
            0.0,
            0.20
        );
        const double entry_surplus = std::max(0.0, snapshot.entry_head_score - snapshot.min_entry_score);
        const double second_stage_deficit = std::max(
            0.0,
            snapshot.effective_min_second_stage_score - snapshot.second_stage_head_score
        );
        const double aggregate_deficit = std::max(
            0.0,
            snapshot.effective_min_aggregate_score - snapshot.aggregate_score
        );
        const bool entry_surplus_ok = entry_surplus >= min_entry_surplus;
        const bool edge_score_ok = second_stage_eval.baseline_edge_score >= min_edge_score;
        const bool second_stage_compensation_ok =
            entry_floor_ok &&
            entry_surplus_ok &&
            edge_score_ok &&
            second_stage_deficit > 0.0 &&
            second_stage_deficit <= max_second_stage_deficit;
        if (second_stage_compensation_ok) {
            second_stage_floor_ok = true;
            snapshot.rr_margin_near_miss_surplus_second_stage_compensated = true;
        }

        snapshot.aggregate_score_before_surplus_compensation = snapshot.aggregate_score;
        const double bonus = std::min(max_bonus, entry_surplus * bonus_weight);
        snapshot.rr_margin_near_miss_surplus_bonus = bonus;
        const bool aggregate_compensation_ok =
            entry_floor_ok &&
            entry_surplus_ok &&
            edge_score_ok &&
            aggregate_deficit > 0.0 &&
            aggregate_deficit <= max_aggregate_deficit &&
            bonus > 1e-12 &&
            (snapshot.aggregate_score + bonus) >= snapshot.effective_min_aggregate_score;
        if (aggregate_compensation_ok) {
            snapshot.aggregate_score = snapshot.aggregate_score + bonus;
            aggregate_score_ok = true;
            snapshot.rr_margin_near_miss_surplus_aggregate_compensated = true;
        }
        snapshot.aggregate_deficit = std::max(
            0.0,
            snapshot.effective_min_aggregate_score - snapshot.aggregate_score
        );
        snapshot.rr_margin_near_miss_surplus_compensation_applied =
            snapshot.rr_margin_near_miss_surplus_second_stage_compensated ||
            snapshot.rr_margin_near_miss_surplus_aggregate_compensated;
    }
    const bool floors_ok = entry_floor_ok && second_stage_floor_ok;
    snapshot.aggregate_pass =
        snapshot.override_allowed &&
        floors_ok &&
        aggregate_score_ok;
    if (snapshot.rr_margin_near_miss_relief_applied && !snapshot.aggregate_pass) {
        snapshot.rr_margin_near_miss_relief_blocked = true;
        snapshot.rr_margin_near_miss_relief_blocked_override_disallowed =
            !snapshot.override_allowed;
        snapshot.rr_margin_near_miss_relief_blocked_entry_floor = !entry_floor_ok;
        snapshot.rr_margin_near_miss_relief_blocked_second_stage_floor =
            !second_stage_floor_ok;
        snapshot.rr_margin_near_miss_relief_blocked_aggregate_score =
            !aggregate_score_ok;
    }
    snapshot.override_applied = snapshot.aggregate_pass;
    return snapshot;
}

void applyArchetypeRiskAdjustments(
    const autolife::strategy::Signal& signal,
    double& required_signal_strength,
    double& regime_rr_add,
    double& regime_edge_add,
    bool& regime_pattern_block
) {
    if (regime_pattern_block) {
        return;
    }

    const bool is_scalping = (signal.strategy_name == "Advanced Scalping");
    const bool is_momentum = (signal.strategy_name == "Advanced Momentum");
    const bool is_breakout = (signal.strategy_name == "Breakout Strategy");
    const bool is_breakout_cont = (signal.entry_archetype == "BREAKOUT_CONTINUATION");
    const bool is_trend_reacc = (signal.entry_archetype == "TREND_REACCELERATION");
    const bool is_consolidation_break = (signal.entry_archetype == "CONSOLIDATION_BREAK");

    if (is_scalping && is_breakout_cont) {
        if (signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP ||
            signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN) {
            regime_pattern_block = true;
            return;
        }
    }

    if (is_momentum && is_breakout_cont &&
        signal.market_regime == autolife::analytics::MarketRegime::RANGING) {
        required_signal_strength = std::max(required_signal_strength, 0.72);
        regime_rr_add += 0.14;
        regime_edge_add += 0.00025;
        const bool high_quality =
            signal.strength >= 0.75 &&
            signal.liquidity_score >= 62.0 &&
            signal.expected_value >= 0.0006;
        if (!high_quality) {
            regime_pattern_block = true;
            return;
        }
    }
    if (is_momentum && is_breakout_cont &&
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP) {
        // Current hostile dataset consistently shows poor payoff for this pattern.
        // Revisit when broader market sample confirms a robust edge.
        regime_pattern_block = true;
        return;
    }
    if (is_momentum && is_trend_reacc &&
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP) {
        required_signal_strength = std::max(required_signal_strength, 0.72);
        regime_rr_add += 0.12;
        regime_edge_add += 0.00018;
        const bool high_quality =
            signal.strength >= 0.74 &&
            signal.liquidity_score >= 60.0 &&
            signal.expected_value >= 0.0007;
        if (!high_quality) {
            regime_pattern_block = true;
            return;
        }
    }
    if (is_breakout && is_consolidation_break &&
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP) {
        regime_pattern_block = true;
        return;
    }
}

struct StrategyEdgeStats {
    int trades = 0;
    int wins = 0;
    double gross_profit = 0.0;
    double gross_loss_abs = 0.0;
    double net_profit = 0.0;

    double expectancy() const {
        return (trades > 0) ? (net_profit / static_cast<double>(trades)) : 0.0;
    }
    double winRate() const {
        return (trades > 0) ? (static_cast<double>(wins) / static_cast<double>(trades)) : 0.0;
    }
    double profitFactor() const {
        if (gross_loss_abs > 1e-12) {
            return gross_profit / gross_loss_abs;
        }
        return (gross_profit > 1e-12) ? 99.9 : 0.0;
    }
    double avgWinKrw() const {
        return (wins > 0) ? (gross_profit / static_cast<double>(wins)) : 0.0;
    }
    double avgLossAbsKrw() const {
        const int losses = std::max(0, trades - wins);
        return (losses > 0) ? (gross_loss_abs / static_cast<double>(losses)) : 0.0;
    }
};

std::map<std::string, StrategyEdgeStats> buildStrategyEdgeStats(
    const std::vector<autolife::risk::TradeHistory>& history,
    int max_recent_trades_per_strategy = 0
) {
    std::map<std::string, StrategyEdgeStats> out;
    if (max_recent_trades_per_strategy > 0) {
        std::map<std::string, int> strategy_counts;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            const auto& trade = *it;
            if (trade.strategy_name.empty()) {
                continue;
            }
            auto& used = strategy_counts[trade.strategy_name];
            if (used >= max_recent_trades_per_strategy) {
                continue;
            }
            used++;
            auto& s = out[trade.strategy_name];
            s.trades++;
            s.net_profit += trade.profit_loss;
            if (trade.profit_loss > 0.0) {
                s.wins++;
                s.gross_profit += trade.profit_loss;
            } else if (trade.profit_loss < 0.0) {
                s.gross_loss_abs += std::abs(trade.profit_loss);
            }
        }
        return out;
    }

    for (const auto& trade : history) {
        if (trade.strategy_name.empty()) {
            continue;
        }
        auto& s = out[trade.strategy_name];
        s.trades++;
        s.net_profit += trade.profit_loss;
        if (trade.profit_loss > 0.0) {
            s.wins++;
            s.gross_profit += trade.profit_loss;
        } else if (trade.profit_loss < 0.0) {
            s.gross_loss_abs += std::abs(trade.profit_loss);
        }
    }
    return out;
}

std::string makeStrategyRegimeKey(const std::string& strategy_name, autolife::analytics::MarketRegime regime) {
    return strategy_name + "|" + std::to_string(static_cast<int>(regime));
}

std::string makeMarketStrategyRegimeKey(
    const std::string& market,
    const std::string& strategy_name,
    autolife::analytics::MarketRegime regime
) {
    return market + "|" + strategy_name + "|" + std::to_string(static_cast<int>(regime));
}

std::map<std::string, StrategyEdgeStats> buildStrategyRegimeEdgeStats(
    const std::vector<autolife::risk::TradeHistory>& history,
    int max_recent_trades_per_key = 0
) {
    std::map<std::string, StrategyEdgeStats> out;
    if (max_recent_trades_per_key > 0) {
        std::map<std::string, int> key_counts;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            const auto& trade = *it;
            if (trade.strategy_name.empty()) {
                continue;
            }
            const std::string key = makeStrategyRegimeKey(trade.strategy_name, trade.market_regime);
            auto& used = key_counts[key];
            if (used >= max_recent_trades_per_key) {
                continue;
            }
            used++;
            auto& s = out[key];
            s.trades++;
            s.net_profit += trade.profit_loss;
            if (trade.profit_loss > 0.0) {
                s.wins++;
                s.gross_profit += trade.profit_loss;
            } else if (trade.profit_loss < 0.0) {
                s.gross_loss_abs += std::abs(trade.profit_loss);
            }
        }
        return out;
    }

    for (const auto& trade : history) {
        if (trade.strategy_name.empty()) {
            continue;
        }
        const std::string key = makeStrategyRegimeKey(trade.strategy_name, trade.market_regime);
        auto& s = out[key];
        s.trades++;
        s.net_profit += trade.profit_loss;
        if (trade.profit_loss > 0.0) {
            s.wins++;
            s.gross_profit += trade.profit_loss;
        } else if (trade.profit_loss < 0.0) {
            s.gross_loss_abs += std::abs(trade.profit_loss);
        }
    }
    return out;
}

std::map<std::string, StrategyEdgeStats> buildMarketStrategyRegimeEdgeStats(
    const std::vector<autolife::risk::TradeHistory>& history,
    int max_recent_trades_per_key = 0
) {
    std::map<std::string, StrategyEdgeStats> out;
    if (max_recent_trades_per_key > 0) {
        std::map<std::string, int> key_counts;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            const auto& trade = *it;
            if (trade.strategy_name.empty() || trade.market.empty()) {
                continue;
            }
            const std::string key = makeMarketStrategyRegimeKey(
                trade.market, trade.strategy_name, trade.market_regime
            );
            auto& used = key_counts[key];
            if (used >= max_recent_trades_per_key) {
                continue;
            }
            used++;
            auto& s = out[key];
            s.trades++;
            s.net_profit += trade.profit_loss;
            if (trade.profit_loss > 0.0) {
                s.wins++;
                s.gross_profit += trade.profit_loss;
            } else if (trade.profit_loss < 0.0) {
                s.gross_loss_abs += std::abs(trade.profit_loss);
            }
        }
        return out;
    }

    for (const auto& trade : history) {
        if (trade.strategy_name.empty() || trade.market.empty()) {
            continue;
        }
        const std::string key = makeMarketStrategyRegimeKey(
            trade.market, trade.strategy_name, trade.market_regime
        );
        auto& s = out[key];
        s.trades++;
        s.net_profit += trade.profit_loss;
        if (trade.profit_loss > 0.0) {
            s.wins++;
            s.gross_profit += trade.profit_loss;
        } else if (trade.profit_loss < 0.0) {
            s.gross_loss_abs += std::abs(trade.profit_loss);
        }
    }
    return out;
}

void normalizeSignalStopLossByRegime(autolife::strategy::Signal& signal, autolife::analytics::MarketRegime regime) {
    if (signal.entry_price <= 0.0) {
        return;
    }

    double min_risk_pct = 0.0035;
    double max_risk_pct = 0.0100;
    if (regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY) {
        min_risk_pct = 0.0060;
        max_risk_pct = 0.0150;
    } else if (regime == autolife::analytics::MarketRegime::TRENDING_DOWN) {
        min_risk_pct = 0.0050;
        max_risk_pct = 0.0120;
    }

    const double strength_t = std::clamp((signal.strength - 0.40) / 0.60, 0.0, 1.0);
    const double tighten = 1.0 - (0.15 * strength_t); // stronger signal -> slightly tighter stop
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

std::string toLowerCopy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<autolife::Candle> aggregateCandlesByStep(
    const std::vector<autolife::Candle>& source,
    size_t step,
    size_t max_bars = 0
) {
    std::vector<autolife::Candle> out;
    if (step == 0 || source.size() < step) {
        return out;
    }

    out.reserve(source.size() / step + 1);
    for (size_t i = 0; i + step <= source.size(); i += step) {
        autolife::Candle aggregated;
        aggregated.open = source[i].open;
        aggregated.close = source[i + step - 1].close;
        aggregated.high = source[i].high;
        aggregated.low = source[i].low;
        aggregated.volume = 0.0;
        aggregated.timestamp = source[i].timestamp;

        for (size_t j = i; j < i + step; ++j) {
            aggregated.high = std::max(aggregated.high, source[j].high);
            aggregated.low = std::min(aggregated.low, source[j].low);
            aggregated.volume += source[j].volume;
        }
        out.push_back(std::move(aggregated));
    }

    if (max_bars > 0 && out.size() > max_bars) {
        out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(max_bars));
    }
    return out;
}

void ensureParityCompanionTimeframes(autolife::analytics::CoinMetrics& metrics) {
    const size_t kTf15mMaxBars = autolife::common::targetBarsForTimeframe("15m", 120);
    if (metrics.candles_by_tf.find("15m") != metrics.candles_by_tf.end()) {
        return;
    }

    auto tf_1m_it = metrics.candles_by_tf.find("1m");
    if (tf_1m_it != metrics.candles_by_tf.end() && tf_1m_it->second.size() >= 15) {
        auto candles_15m = aggregateCandlesByStep(tf_1m_it->second, 15, kTf15mMaxBars);
        if (!candles_15m.empty()) {
            metrics.candles_by_tf["15m"] = std::move(candles_15m);
            return;
        }
    }

    auto tf_5m_it = metrics.candles_by_tf.find("5m");
    if (tf_5m_it != metrics.candles_by_tf.end() && tf_5m_it->second.size() >= 3) {
        auto candles_15m = aggregateCandlesByStep(tf_5m_it->second, 3, kTf15mMaxBars);
        if (!candles_15m.empty()) {
            metrics.candles_by_tf["15m"] = std::move(candles_15m);
        }
    }
}

bool requiresTypedArchetype(const std::string& strategy_name) {
    return strategy_name == "Advanced Scalping" ||
           strategy_name == "Advanced Momentum";
}

bool isAlphaHeadFallbackCandidate(const autolife::strategy::Signal& signal, bool alpha_head_mode) {
    return alpha_head_mode && signal.reason == "alpha_head_fallback_candidate";
}

const char* classifySignalRejectionGroup(const std::string& reason) {
    if (reason == "no_signal_generated") {
        return "signal_generation";
    }
    if (reason == "skipped_due_to_open_position" || reason == "skipped_due_to_active_order") {
        return "position_state";
    }
    if (reason == "no_candle_data") {
        return "data_availability";
    }
    if (reason == "data_parity_window_insufficient") {
        return "data_availability";
    }
    if (reason == "filtered_out_by_manager" ||
        reason.rfind("filtered_out_by_manager_", 0) == 0) {
        return "manager_prefilter";
    }
    if (reason == "no_best_signal" ||
        reason.rfind("no_best_signal_", 0) == 0) {
        return "best_signal_selection";
    }
    if (reason.rfind("blocked_", 0) == 0) {
        return "risk_or_execution_gate";
    }
    return "other";
}

std::string normalizeCaptureTimeframeKey(const std::string& raw_tf) {
    const std::string tf = toLowerCopy(raw_tf);
    if (tf == "1m" || tf == "1min") {
        return "1m";
    }
    if (tf == "5m" || tf == "5min") {
        return "5m";
    }
    if (tf == "15m" || tf == "15min") {
        return "15m";
    }
    if (tf == "1h" || tf == "60m" || tf == "60min") {
        return "1h";
    }
    if (tf == "4h" || tf == "240m" || tf == "240min") {
        return "4h";
    }
    if (tf == "1d" || tf == "d" || tf == "day" || tf == "1440m") {
        return "1d";
    }
    return {};
}

std::string timeframeKeyToDatasetToken(const std::string& tf_key) {
    if (tf_key == "1h") {
        return "60m";
    }
    if (tf_key == "4h") {
        return "240m";
    }
    return tf_key;
}

std::string toDatasetMarketToken(const std::string& market) {
    std::string token = market;
    std::replace(token.begin(), token.end(), '-', '_');
    std::replace(token.begin(), token.end(), '/', '_');
    for (char& ch : token) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
            ch = '_';
        }
    }
    return token;
}

long long readLastCsvTimestamp(const std::filesystem::path& path) {
    std::ifstream in(path.string(), std::ios::binary);
    if (!in.is_open()) {
        return 0;
    }

    std::string line;
    std::string last_data_line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto line_lower = toLowerCopy(line);
        if (line_lower.rfind("timestamp", 0) == 0) {
            continue;
        }
        last_data_line = line;
    }

    if (last_data_line.empty()) {
        return 0;
    }
    const size_t comma_pos = last_data_line.find(',');
    if (comma_pos == std::string::npos || comma_pos == 0) {
        return 0;
    }
    try {
        return std::stoll(last_data_line.substr(0, comma_pos));
    } catch (...) {
        return 0;
    }
}

struct CandleAppendResult {
    int appended_rows = 0;
    long long last_timestamp = 0;
    bool wrote_header = false;
    bool ok = false;
};

CandleAppendResult appendCandlesCsv(
    const std::filesystem::path& path,
    const std::vector<autolife::Candle>& candles,
    long long known_last_timestamp
) {
    CandleAppendResult result;
    if (candles.empty()) {
        result.ok = true;
        result.last_timestamp = known_last_timestamp;
        return result;
    }

    std::filesystem::create_directories(path.parent_path());
    const bool file_exists = std::filesystem::exists(path);
    const bool needs_header = !file_exists || std::filesystem::file_size(path) == 0;
    long long last_timestamp = known_last_timestamp;
    if (last_timestamp <= 0 && file_exists) {
        last_timestamp = readLastCsvTimestamp(path);
    }

    std::ofstream out(path.string(), std::ios::out | std::ios::app);
    if (!out.is_open()) {
        return result;
    }
    if (needs_header) {
        out << "timestamp,open,high,low,close,volume\n";
        result.wrote_header = true;
    }

    out << std::setprecision(12);
    for (const auto& candle : candles) {
        if (!std::isfinite(candle.open) || !std::isfinite(candle.high) ||
            !std::isfinite(candle.low) || !std::isfinite(candle.close) ||
            !std::isfinite(candle.volume)) {
            continue;
        }
        if (candle.timestamp <= last_timestamp) {
            continue;
        }
        out << candle.timestamp
            << "," << candle.open
            << "," << candle.high
            << "," << candle.low
            << "," << candle.close
            << "," << candle.volume
            << "\n";
        last_timestamp = candle.timestamp;
        result.appended_rows++;
    }

    out.flush();
    if (!out) {
        return result;
    }

    result.ok = true;
    result.last_timestamp = last_timestamp;
    return result;
}

void appendPolicyDecisionAudit(
    const std::vector<autolife::engine::PolicyDecisionRecord>& decisions,
    autolife::analytics::MarketRegime dominant_regime,
    bool small_seed_mode,
    int max_new_orders_per_scan
) {
    if (decisions.empty()) {
        return;
    }

    nlohmann::json line;
    line["ts"] = getCurrentTimestampMs();
    line["dominant_regime"] = regimeToString(dominant_regime);
    line["small_seed_mode"] = small_seed_mode;
    line["max_new_orders_per_scan"] = max_new_orders_per_scan;
    line["decisions"] = nlohmann::json::array();

    for (const auto& d : decisions) {
        nlohmann::json item;
        item["market"] = d.market;
        item["strategy"] = d.strategy_name;
        item["selected"] = d.selected;
        item["reason"] = d.reason;
        item["base_score"] = d.base_score;
        item["policy_score"] = d.policy_score;
        item["strength"] = d.strength;
        item["expected_value"] = d.expected_value;
        item["liquidity"] = d.liquidity_score;
        item["volatility"] = d.volatility;
        item["trades"] = d.strategy_trades;
        item["win_rate"] = d.strategy_win_rate;
        item["profit_factor"] = d.strategy_profit_factor;
        item["used_preloaded_tf_5m"] = d.used_preloaded_tf_5m;
        item["used_preloaded_tf_1h"] = d.used_preloaded_tf_1h;
        item["used_resampled_tf_fallback"] = d.used_resampled_tf_fallback;
        line["decisions"].push_back(std::move(item));
    }

    auto path = autolife::utils::PathUtils::resolveRelativePath("logs/policy_decisions.jsonl");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::app);
    out << line.dump() << "\n";
}

}
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

// Using declarations for risk namespace types
using autolife::risk::TradeHistory;
using autolife::risk::Position;

namespace autolife {
namespace engine {

TradingEngine::TradingEngine(
    const EngineConfig& config,
    std::shared_ptr<network::UpbitHttpClient> http_client
)
    : config_(config)
    , http_client_(http_client)
    , running_(false)
    , start_time_(0)
    , total_scans_(0)
    , total_signals_(0)
{
    LOG_INFO("TradingEngine initialized");
    LOG_INFO("Trading mode: {}", config.mode == TradingMode::LIVE ? "LIVE" : "PAPER");
    LOG_INFO("Initial capital: {:.0f} KRW", config.initial_capital);

    if (config.mode == TradingMode::LIVE) {
        LOG_INFO("Live risk and order constraints enabled");
        LOG_INFO("  Max daily loss (KRW): {:.0f}", config.max_daily_loss_krw);
        LOG_INFO("  Max order amount (KRW): {:.0f}", config.max_order_krw);
        LOG_INFO("  Min order amount (KRW): {:.0f}", config.min_order_krw);
        LOG_INFO("  Dry Run: {}", config.dry_run ? "ON" : "OFF");
        LOG_INFO("  Live Order Submission: {}", config.allow_live_orders ? "ENABLED" : "DISABLED");
        LOG_INFO(
            "  Live MTF dataset capture: {} (interval={}s, output={}, tf={})",
            config.enable_live_mtf_dataset_capture ? "ON" : "OFF",
            config.live_mtf_dataset_capture_interval_seconds,
            config.live_mtf_dataset_capture_output_dir,
            config.live_mtf_dataset_capture_timeframes.empty()
                ? std::string("none")
                : [&]() {
                      std::ostringstream oss;
                      for (size_t i = 0; i < config.live_mtf_dataset_capture_timeframes.size(); ++i) {
                          if (i > 0) {
                              oss << ",";
                          }
                          oss << config.live_mtf_dataset_capture_timeframes[i];
                      }
                      return oss.str();
                  }()
        );
    }

    scanner_ = std::make_unique<analytics::MarketScanner>(http_client);
    strategy_manager_ = std::make_unique<strategy::StrategyManager>(http_client);
    policy_controller_ = std::make_unique<AdaptivePolicyController>();
    performance_store_ = std::make_unique<PerformanceStore>();
    risk_manager_ = std::make_unique<risk::RiskManager>(config.initial_capital);
    order_manager_ = std::make_unique<execution::OrderManager>(
        http_client,
        config.mode == TradingMode::LIVE
    );
    regime_detector_ = std::make_unique<analytics::RegimeDetector>();
    learning_state_store_ = std::make_unique<core::LearningStateStoreJson>(
        utils::PathUtils::resolveRelativePath("state/learning_state.json")
    );
    event_journal_ = std::make_unique<core::EventJournalJsonl>(
        utils::PathUtils::resolveRelativePath("state/event_journal.jsonl")
    );

    if (config_.enable_core_plane_bridge) {
        if (config_.enable_core_policy_plane && policy_controller_) {
            core_policy_plane_ = std::make_shared<core::PolicyLearningPlaneAdapter>(
                *policy_controller_,
                performance_store_.get()
            );
        }
        if (config_.enable_core_risk_plane && risk_manager_) {
            core_risk_plane_ = std::make_shared<core::UpbitComplianceAdapter>(
                http_client_,
                *risk_manager_,
                config_
            );
        }
        if (config_.enable_core_execution_plane && order_manager_) {
            core_execution_plane_ = std::make_shared<core::ExecutionPlaneAdapter>(
                *order_manager_
            );
        }
        core_cycle_ = std::make_unique<core::TradingCycleCoordinator>(
            core_policy_plane_,
            core_risk_plane_,
            core_execution_plane_
        );

        LOG_INFO(
            "Core plane bridge enabled (policy={}, risk={}, execution={})",
            config_.enable_core_policy_plane ? "on" : "off",
            config_.enable_core_risk_plane ? "on" : "off",
            config_.enable_core_execution_plane ? "on" : "off"
        );
    }

    risk_manager_->setMaxPositions(config.max_positions);
    risk_manager_->setMaxDailyTrades(config.max_daily_trades);
    risk_manager_->setMaxDrawdown(config.max_drawdown);
    risk_manager_->setMaxExposurePct(config.max_exposure_pct);
    risk_manager_->setDailyLossLimitPct(config.max_daily_loss_pct);
    risk_manager_->setDailyLossLimitKrw(config.max_daily_loss_krw);
    risk_manager_->setMinOrderKrw(config.min_order_krw);

    auto foundation = std::make_shared<strategy::FoundationAdaptiveStrategy>(http_client);
    strategy_manager_->registerStrategy(foundation);
    LOG_INFO("Registered strategy: foundation_adaptive (legacy strategy pack disconnected)");
}

TradingEngine::~TradingEngine() {
    stop();
}

bool TradingEngine::start() {
    if (running_) {
        LOG_WARN("Trading engine is already running");
        return false;
    }

    LOG_INFO("========================================");
    LOG_INFO("AutoLife trading engine start requested");
    LOG_INFO("========================================");
    pre_cat_recovery_hysteresis_hold_by_key_.clear();
    pre_cat_recovery_bridge_activation_by_key_.clear();
    pre_cat_no_soft_quality_relief_activation_by_key_.clear();
    pre_cat_candidate_rr_failsafe_activation_by_key_.clear();
    pre_cat_pressure_rebound_relief_activation_by_key_.clear();
    pre_cat_negative_history_quarantine_hold_by_key_.clear();

    loadState();
    loadLearningState();
    if (config_.mode == TradingMode::LIVE) {
        syncAccountState();
    }

    running_ = true;
    start_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    worker_thread_ = std::make_unique<std::thread>(&TradingEngine::run, this);

    prometheus_http_thread_ = std::make_unique<std::thread>(&TradingEngine::runPrometheusHttpServer, this, 8080);
    LOG_INFO("Prometheus HTTP server started (port 8080)");

    state_persist_running_ = true;
    state_persist_thread_ = std::make_unique<std::thread>(&TradingEngine::runStatePersistence, this);
    LOG_INFO("State persistence thread started (every 30 seconds)");

    return true;
}

void TradingEngine::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("========================================");
    LOG_INFO("Trading engine stop requested");
    LOG_INFO("========================================");

    running_ = false;

    prometheus_server_running_ = false;
    if (prometheus_http_thread_ && prometheus_http_thread_->joinable()) {
        prometheus_http_thread_->join();
    }

    state_persist_running_ = false;
    if (state_persist_thread_ && state_persist_thread_->joinable()) {
        state_persist_thread_->join();
    }

    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->join();
    }

    logPerformance();
    saveState();
}

void TradingEngine::run() {
    LOG_INFO("Trading engine main loop started");

    auto scan_interval = std::chrono::seconds(config_.scan_interval_seconds);
    auto last_scan_time = std::chrono::steady_clock::now() - scan_interval;
    auto monitor_interval = std::chrono::milliseconds(500);
    auto last_account_sync_time = std::chrono::steady_clock::now();
    auto account_sync_interval = std::chrono::seconds(300);

    while (running_) {
        auto tick_start = std::chrono::steady_clock::now();

        try {
            monitorPositions();

            if (order_manager_) {
                order_manager_->monitorOrders();

                auto filled_orders = order_manager_->getFilledOrders();
                for (const auto& order : filled_orders) {
                    LOG_INFO("Order fill event: {} {} {:.8f} @ {:.0f}",
                             order.market, (order.side == OrderSide::BUY ? "BUY" : "SELL"),
                             order.filled_volume, order.price);

                    if (order.side == OrderSide::BUY) {
                        double filled_amount = order.price * order.filled_volume;
                        risk_manager_->releasePendingCapital(filled_amount);

                        risk_manager_->enterPosition(
                            order.market,
                            order.price,
                            order.filled_volume,
                            order.stop_loss,
                            order.take_profit_1,
                            order.take_profit_2 > 0.0 ? order.take_profit_2 : order.take_profit_1,
                            order.strategy_name,
                            order.breakeven_trigger,
                            order.trailing_start
                        );

                        nlohmann::json fill_payload;
                        fill_payload["side"] = "BUY";
                        fill_payload["filled_volume"] = order.filled_volume;
                        fill_payload["avg_price"] = order.price;
                        fill_payload["strategy_name"] = order.strategy_name;
                        fill_payload["stop_loss"] = order.stop_loss;
                        fill_payload["take_profit_1"] = order.take_profit_1;
                        fill_payload["take_profit_2"] = order.take_profit_2;
                        appendJournalEvent(
                            core::JournalEventType::FILL_APPLIED,
                            order.market,
                            order.order_id,
                            fill_payload
                        );

                        nlohmann::json position_payload;
                        position_payload["entry_price"] = order.price;
                        position_payload["quantity"] = order.filled_volume;
                        position_payload["strategy_name"] = order.strategy_name;
                        appendJournalEvent(
                            core::JournalEventType::POSITION_OPENED,
                            order.market,
                            order.order_id,
                            position_payload
                        );
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (config_.mode == TradingMode::LIVE) {
                if (now - last_account_sync_time >= account_sync_interval) {
                    LOG_INFO("Periodic account sync started");
                    syncAccountState();
                    last_account_sync_time = now;
                }
            }

            auto elapsed_since_scan = std::chrono::duration_cast<std::chrono::seconds>(now - last_scan_time);
            if (elapsed_since_scan >= scan_interval) {
                LOG_INFO("Starting market scan (elapsed {}s)", elapsed_since_scan.count());

                scanMarkets();
                captureLiveMtfDatasetSnapshotIfDue();
                generateSignals();
                learnOptimalFilterValue();
                executeSignals();

                last_scan_time = std::chrono::steady_clock::now();
                updateMetrics();
            }

            auto tick_end = std::chrono::steady_clock::now();
            auto tick_duration = tick_end - tick_start;
            auto sleep_duration = monitor_interval - tick_duration;

            if (sleep_duration.count() > 0) {
                std::this_thread::sleep_for(sleep_duration);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Trading engine loop error: {}", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    LOG_INFO("Trading engine loop ended");
}

void TradingEngine::scanMarkets() {
    LOG_INFO("===== Market Scan Start =====");

    total_scans_++;

    scanned_markets_ = scanner_->scanAllMarkets();
    if (scanned_markets_.empty()) {
        LOG_WARN("No markets scanned");
        return;
    }

    std::sort(scanned_markets_.begin(), scanned_markets_.end(),
        [](const analytics::CoinMetrics& a, const analytics::CoinMetrics& b) {
            return a.composite_score > b.composite_score;
        });

    std::vector<analytics::CoinMetrics> filtered;
    const double MAX_SPREAD_PCT = 0.35;
    for (const auto& coin : scanned_markets_) {
        if (coin.volume_24h < config_.min_volume_krw) {
            continue;
        }
        if (!coin.orderbook_snapshot.valid) {
            continue;
        }
        if (coin.orderbook_snapshot.spread_pct > MAX_SPREAD_PCT) {
            continue;
        }
        if (coin.orderbook_snapshot.best_bid <= 0.0 || coin.orderbook_snapshot.best_ask <= 0.0) {
            continue;
        }
        if (coin.orderbook_snapshot.ask_notional < config_.min_order_krw * 5.0) {
            continue;
        }

        filtered.push_back(coin);
    }

    if (filtered.size() > 20) {
        filtered.resize(20);
    }

    scanned_markets_ = filtered;

    LOG_INFO("Scanned markets: {}", scanned_markets_.size());

    int count = 0;
    for (const auto& coin : scanned_markets_) {
        if (count++ >= 5) {
            break;
        }

        LOG_INFO("  #{} {} - score {:.1f}, 24h vol {:.0f}?µÏõê, volatility {:.2f}%",
                 count, coin.market, coin.composite_score,
                 coin.volume_24h / 100000000.0,
                 coin.volatility);
    }
}

void TradingEngine::captureLiveMtfDatasetSnapshotIfDue() {
    if (config_.mode != TradingMode::LIVE || !config_.enable_live_mtf_dataset_capture) {
        return;
    }
    if (scanned_markets_.empty()) {
        return;
    }

    const int interval_seconds = std::max(30, config_.live_mtf_dataset_capture_interval_seconds);
    const auto now = std::chrono::steady_clock::now();
    if (last_live_mtf_capture_time_.time_since_epoch().count() != 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_live_mtf_capture_time_);
        if (elapsed.count() < interval_seconds) {
            return;
        }
    }

    std::set<std::string> capture_tfs;
    std::vector<std::string> ignored_tfs;
    for (const auto& raw_tf : config_.live_mtf_dataset_capture_timeframes) {
        const auto tf_key = normalizeCaptureTimeframeKey(raw_tf);
        if (tf_key.empty()) {
            ignored_tfs.push_back(raw_tf);
            continue;
        }
        capture_tfs.insert(tf_key);
    }
    if (capture_tfs.empty()) {
        if (!ignored_tfs.empty()) {
            LOG_WARN("Live MTF dataset capture skipped: no valid tf keys (ignored={})",
                     static_cast<int>(ignored_tfs.size()));
        }
        last_live_mtf_capture_time_ = now;
        return;
    }

    try {
        const auto output_root =
            utils::PathUtils::resolveRelativePath(config_.live_mtf_dataset_capture_output_dir);
        std::filesystem::create_directories(output_root);

        int files_ok = 0;
        int files_failed = 0;
        int total_appended_rows = 0;
        int missing_tf_pairs = 0;
        int markets_with_new_rows = 0;
        nlohmann::json market_rows = nlohmann::json::array();

        for (const auto& coin : scanned_markets_) {
            analytics::CoinMetrics metrics = coin;
            if (metrics.candles_by_tf.find("1m") == metrics.candles_by_tf.end() &&
                !metrics.candles.empty()) {
                metrics.candles_by_tf["1m"] = metrics.candles;
            }
            ensureParityCompanionTimeframes(metrics);

            const std::string market_token = toDatasetMarketToken(metrics.market);
            int market_appended_rows = 0;
            nlohmann::json missing_tfs = nlohmann::json::array();

            for (const auto& tf_key : capture_tfs) {
                const auto tf_it = metrics.candles_by_tf.find(tf_key);
                if (tf_it == metrics.candles_by_tf.end() || tf_it->second.empty()) {
                    missing_tf_pairs++;
                    missing_tfs.push_back(tf_key);
                    continue;
                }

                const std::string tf_token = timeframeKeyToDatasetToken(tf_key);
                const auto file_path = output_root /
                    ("upbit_" + market_token + "_" + tf_token + "_live.csv");
                const std::string file_key = file_path.string();
                long long known_last_timestamp = 0;
                auto known_it = live_mtf_capture_last_timestamp_by_file_.find(file_key);
                if (known_it != live_mtf_capture_last_timestamp_by_file_.end()) {
                    known_last_timestamp = known_it->second;
                }

                const auto append_result = appendCandlesCsv(file_path, tf_it->second, known_last_timestamp);
                if (!append_result.ok) {
                    files_failed++;
                    continue;
                }

                files_ok++;
                live_mtf_capture_last_timestamp_by_file_[file_key] = append_result.last_timestamp;
                total_appended_rows += append_result.appended_rows;
                market_appended_rows += append_result.appended_rows;
            }

            if (market_appended_rows > 0) {
                markets_with_new_rows++;
            }
            nlohmann::json market_item;
            market_item["market"] = metrics.market;
            market_item["appended_rows"] = market_appended_rows;
            market_item["missing_tfs"] = std::move(missing_tfs);
            market_rows.push_back(std::move(market_item));
        }

        nlohmann::json report;
        report["updated_at_ms"] = getCurrentTimestampMs();
        report["output_dir"] = output_root.string();
        report["capture_interval_seconds"] = interval_seconds;
        report["capture_timeframes"] = nlohmann::json::array();
        for (const auto& tf_key : capture_tfs) {
            report["capture_timeframes"].push_back(tf_key);
        }
        report["ignored_timeframes"] = ignored_tfs;
        report["markets_scanned"] = static_cast<int>(scanned_markets_.size());
        report["markets_with_new_rows"] = markets_with_new_rows;
        report["files_ok"] = files_ok;
        report["files_failed"] = files_failed;
        report["total_appended_rows"] = total_appended_rows;
        report["missing_tf_pairs"] = missing_tf_pairs;
        report["market_rows"] = std::move(market_rows);

        const auto report_path =
            utils::PathUtils::resolveRelativePath("logs/live_mtf_dataset_capture_report.json");
        std::filesystem::create_directories(report_path.parent_path());
        std::ofstream report_out(report_path.string(), std::ios::out | std::ios::trunc);
        if (report_out.is_open()) {
            report_out << report.dump(2);
        }

        LOG_INFO(
            "Live MTF dataset capture: markets={}, tf={}, files_ok={}, files_failed={}, appended_rows={}, missing_tf_pairs={}",
            scanned_markets_.size(),
            capture_tfs.size(),
            files_ok,
            files_failed,
            total_appended_rows,
            missing_tf_pairs
        );
    } catch (const std::exception& e) {
        LOG_ERROR("Live MTF dataset capture failed: {}", e.what());
    }

    last_live_mtf_capture_time_ = now;
}

void TradingEngine::generateSignals() {
    if (scanned_markets_.empty()) {
        return;
    }

    LOG_INFO("===== Generate Signals =====");

    pending_signals_.clear();
    live_signal_funnel_.scan_count++;
    bool live_no_trade_bias_active = false;
    double live_signal_generation_share = 0.0;
    double live_manager_prefilter_share = 0.0;
    double live_position_state_share = 0.0;
    std::string live_top_group = "other";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        long long total_rejections = 0;
        std::map<std::string, long long> group_counts;
        for (const auto& kv : live_signal_funnel_.rejection_reason_counts) {
            const auto* group = classifySignalRejectionGroup(kv.first);
            group_counts[group] += kv.second;
            total_rejections += kv.second;
        }
        if (total_rejections > 0) {
            live_signal_generation_share =
                static_cast<double>(group_counts["signal_generation"]) / static_cast<double>(total_rejections);
            live_manager_prefilter_share =
                static_cast<double>(group_counts["manager_prefilter"]) / static_cast<double>(total_rejections);
            live_position_state_share =
                static_cast<double>(group_counts["position_state"]) / static_cast<double>(total_rejections);
            if (live_signal_generation_share >= live_manager_prefilter_share &&
                live_signal_generation_share >= live_position_state_share) {
                live_top_group = "signal_generation";
            } else if (live_manager_prefilter_share >= live_signal_generation_share &&
                       live_manager_prefilter_share >= live_position_state_share) {
                live_top_group = "manager_prefilter";
            } else {
                live_top_group = "position_state";
            }
            live_no_trade_bias_active =
                live_signal_funnel_.scan_count >= 3 &&
                total_rejections >= 20 &&
                live_position_state_share < 0.40 &&
                (live_signal_generation_share >= 0.55 ||
                 (live_signal_generation_share + live_manager_prefilter_share) >= 0.75);
        }
    }
    strategy::StrategyManager::LiveSignalBottleneckHint manager_hint;
    manager_hint.enabled = true;
    manager_hint.top_group = live_top_group;
    manager_hint.no_trade_bias_active = live_no_trade_bias_active;
    manager_hint.signal_generation_share = live_signal_generation_share;
    manager_hint.manager_prefilter_share = live_manager_prefilter_share;
    manager_hint.position_state_share = live_position_state_share;
    strategy_manager_->setLiveSignalBottleneckHint(manager_hint);
    std::map<std::string, int> last_scan_rejection_counts;
    std::map<std::string, int> last_scan_hint_adjustment_counts;
    auto markScanReject = [&](const std::string& reason, int count = 1) {
        if (reason.empty() || count <= 0) {
            return;
        }
        last_scan_rejection_counts[reason] += count;
        recordLiveSignalReject(reason, static_cast<long long>(count));
    };

    for (const auto& coin : scanned_markets_) {
        live_signal_funnel_.markets_considered++;
        const bool has_open_position = (risk_manager_->getPosition(coin.market) != nullptr);
        if (has_open_position) {
            bool& skip_latched = open_position_skip_latch_[coin.market];
            if (!skip_latched) {
                live_signal_funnel_.skipped_due_to_open_position++;
                markScanReject("skipped_due_to_open_position");
                skip_latched = true;
            }
            continue;
        }
        open_position_skip_latch_.erase(coin.market);

        if (order_manager_->hasActiveOrder(coin.market)) {
            live_signal_funnel_.skipped_due_to_active_order++;
            markScanReject("skipped_due_to_active_order");
            continue;
        }

        try {
            analytics::CoinMetrics signal_metrics = coin;
            if (signal_metrics.candles_by_tf.find("1m") == signal_metrics.candles_by_tf.end() &&
                !signal_metrics.candles.empty()) {
                signal_metrics.candles_by_tf["1m"] = signal_metrics.candles;
            }
            ensureParityCompanionTimeframes(signal_metrics);
            common::trimCandlesByPolicy(signal_metrics.candles_by_tf);

            auto tf_1m_it = signal_metrics.candles_by_tf.find("1m");
            if (tf_1m_it != signal_metrics.candles_by_tf.end()) {
                signal_metrics.candles = tf_1m_it->second;
            }

            const auto data_window_check = common::checkLiveEquivalentWindow(signal_metrics.candles_by_tf);
            if (!data_window_check.pass) {
                LOG_INFO("{} - data parity window insufficient: {}",
                         coin.market,
                         common::buildWindowCheckSummary(data_window_check));
                live_signal_funnel_.no_candle_data++;
                markScanReject("data_parity_window_insufficient");
                continue;
            }

            const auto& candles = signal_metrics.candles;
            if (candles.empty()) {
                LOG_INFO("{} - no candle data, skipping", coin.market);
                live_signal_funnel_.no_candle_data++;
                markScanReject("no_candle_data");
                continue;
            }

            double current_price = candles.back().close;
            auto regime = regime_detector_->analyzeRegime(candles);

            auto signals = strategy_manager_->collectSignals(
                signal_metrics.market,
                signal_metrics,
                candles,
                current_price,
                risk_manager_->getRiskMetrics().available_capital,
                regime
            );

            if (signals.empty()) {
                live_signal_funnel_.no_signal_generated++;
                markScanReject("no_signal_generated");
                continue;
            }
            live_signal_funnel_.generated_signal_candidates += static_cast<long long>(signals.size());

            strategy::Signal best_signal;
            int candidate_count = static_cast<int>(signals.size());
            if (config_.use_strategy_alpha_head_mode) {
                // Alpha-head mode keeps multi-candidate flow, but applies a permissive
                // manager prefilter so extremely weak/negative-edge signals are dropped early.
                double alpha_min_strength = 0.34;
                double alpha_min_expected_value = -0.00015;
                if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
                    alpha_min_strength = 0.42;
                    alpha_min_expected_value = 0.00005;
                } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
                    alpha_min_strength = 0.45;
                    alpha_min_expected_value = 0.00008;
                } else if (regime.regime == analytics::MarketRegime::RANGING) {
                    alpha_min_strength = 0.36;
                    alpha_min_expected_value = -0.00008;
                }
                if (live_no_trade_bias_active) {
                    alpha_min_strength = std::max(alpha_min_strength, 0.40);
                    alpha_min_expected_value = std::max(alpha_min_expected_value, 0.00005);
                }
                strategy::StrategyManager::FilterDiagnostics manager_filter_diag;
                auto alpha_filtered = strategy_manager_->filterSignalsWithDiagnostics(
                    signals,
                    alpha_min_strength,
                    alpha_min_expected_value,
                    regime.regime,
                    &manager_filter_diag
                );
                if (alpha_filtered.empty()) {
                    live_signal_funnel_.filtered_out_by_manager++;
                    markScanReject("filtered_out_by_manager");
                    for (const auto& kv : manager_filter_diag.rejection_reason_counts) {
                        markScanReject(kv.first, kv.second);
                    }
                    continue;
                }

                int added = 0;
                for (auto& signal : alpha_filtered) {
                    if (signal.type == strategy::SignalType::NONE) {
                        continue;
                    }
                    signal.market_regime = regime.regime;
                    pending_signals_.push_back(std::move(signal));
                    added++;
                }
                total_signals_ += added;
                live_signal_funnel_.selected_signal_candidates += added;
                if (added > 0) {
                    live_signal_funnel_.markets_with_selected_candidate++;
                } else {
                    markScanReject("alpha_head_no_effective_candidates");
                }
                LOG_INFO(
                    "Alpha-head candidates collected: market={}, added={}, from_signals={}, after_prefilter={}",
                    coin.market,
                    added,
                    candidate_count,
                    static_cast<int>(alpha_filtered.size())
                );
                continue;
            } else {
                double min_strength = 0.40;
                double min_expected_value = 0.0;
                if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
                    min_strength = 0.48;
                } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
                    min_strength = 0.52;
                } else if (regime.regime == analytics::MarketRegime::RANGING) {
                    min_strength = 0.43;
                }
                const bool allow_entry_frequency_relaxation =
                    scans_without_new_entry_ >= 6 &&
                    regime.regime != analytics::MarketRegime::TRENDING_DOWN &&
                    regime.regime != analytics::MarketRegime::HIGH_VOLATILITY;
                if (allow_entry_frequency_relaxation) {
                    min_strength = std::max(0.35, min_strength - 0.04);
                    min_expected_value = std::max(-0.0001, min_expected_value - 0.0001);
                }
                if (live_no_trade_bias_active &&
                    regime.regime != analytics::MarketRegime::TRENDING_DOWN &&
                    regime.regime != analytics::MarketRegime::HIGH_VOLATILITY) {
                    // Runtime hint parity with tuning loop:
                    // if signal-generation/prefilter bottleneck dominates, prefer modest
                    // additional relaxation before manager prefiltering.
                    min_strength = std::max(0.33, min_strength - 0.03);
                    min_expected_value = std::max(-0.0002, min_expected_value - 0.0001);
                }

                strategy::StrategyManager::FilterDiagnostics manager_filter_diag;
                auto filtered = strategy_manager_->filterSignalsWithDiagnostics(
                    signals,
                    min_strength,
                    min_expected_value,
                    regime.regime,
                    &manager_filter_diag
                );
                if (filtered.empty()) {
                    live_signal_funnel_.filtered_out_by_manager++;
                    markScanReject("filtered_out_by_manager");
                    for (const auto& kv : manager_filter_diag.rejection_reason_counts) {
                        markScanReject(kv.first, kv.second);
                    }
                    continue;
                }
                candidate_count = static_cast<int>(filtered.size());
                strategy::StrategyManager::SelectionDiagnostics manager_select_diag;
                best_signal = strategy_manager_->selectRobustSignalWithDiagnostics(
                    filtered,
                    regime.regime,
                    &manager_select_diag
                );
                live_signal_funnel_.selection_call_count++;
                live_signal_funnel_.selection_scored_candidate_count +=
                    static_cast<long long>(manager_select_diag.scored_candidate_count);
                live_signal_funnel_.selection_hint_adjusted_candidate_count +=
                    static_cast<long long>(manager_select_diag.live_hint_adjusted_candidate_count);
                for (const auto& kv : manager_select_diag.live_hint_adjustment_counts) {
                    if (kv.second <= 0) {
                        continue;
                    }
                    live_signal_funnel_.selection_hint_adjustment_counts[kv.first] +=
                        static_cast<long long>(kv.second);
                    last_scan_hint_adjustment_counts[kv.first] += kv.second;
                }
                if (best_signal.type == strategy::SignalType::NONE) {
                    live_signal_funnel_.no_best_signal++;
                    markScanReject("no_best_signal");
                    for (const auto& kv : manager_select_diag.rejection_reason_counts) {
                        markScanReject(kv.first, kv.second);
                    }
                    continue;
                }
            }
            if (best_signal.type != strategy::SignalType::NONE) {
                best_signal.market_regime = regime.regime;
                pending_signals_.push_back(best_signal);
                total_signals_++;
                live_signal_funnel_.selected_signal_candidates++;
                live_signal_funnel_.markets_with_selected_candidate++;

                LOG_INFO("Signal selected: {} - {} (strength: {:.2f}, candidates={}, alpha_head={}, tf5m_preloaded={}, tf1h_preloaded={}, fallback={})",
                         coin.market,
                         best_signal.type == strategy::SignalType::STRONG_BUY ? "STRONG_BUY" : "BUY",
                         best_signal.strength,
                         candidate_count,
                         config_.use_strategy_alpha_head_mode ? "Y" : "N",
                         best_signal.used_preloaded_tf_5m ? "Y" : "N",
                         best_signal.used_preloaded_tf_1h ? "Y" : "N",
                         best_signal.used_resampled_tf_fallback ? "Y" : "N");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Signal generation failed: {} - {}", coin.market, e.what());
        }
    }

    if (live_no_trade_bias_active) {
        LOG_INFO(
            "Live no-trade bias active: signal_gen_share={:.2f}, manager_prefilter_share={:.2f}",
            live_signal_generation_share,
            live_manager_prefilter_share
        );
    }
    LOG_INFO("Signal generation complete: {} candidates", pending_signals_.size());
    flushLiveSignalFunnelTaxonomyReport(last_scan_rejection_counts, last_scan_hint_adjustment_counts);
}

void TradingEngine::recordLiveSignalReject(const std::string& reason, long long count) {
    if (reason.empty() || count <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    live_signal_funnel_.rejection_reason_counts[reason] += count;
}

void TradingEngine::flushLiveSignalFunnelTaxonomyReport(
    const std::map<std::string, int>& last_scan_rejection_counts,
    const std::map<std::string, int>& last_scan_hint_adjustment_counts
) const {
    LiveSignalFunnelTelemetry snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = live_signal_funnel_;
    }

    auto buildSortedCounts = [](const auto& counts) {
        std::vector<std::pair<std::string, long long>> sorted;
        sorted.reserve(counts.size());
        for (const auto& kv : counts) {
            sorted.emplace_back(kv.first, static_cast<long long>(kv.second));
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) {
                          return a.second > b.second;
                      }
                      return a.first < b.first;
                  });
        return sorted;
    };

    auto buildTopList = [](const std::vector<std::pair<std::string, long long>>& sorted, size_t limit) {
        nlohmann::json arr = nlohmann::json::array();
        const size_t end = std::min(limit, sorted.size());
        for (size_t i = 0; i < end; ++i) {
            nlohmann::json item;
            item["reason"] = sorted[i].first;
            item["count"] = sorted[i].second;
            item["group"] = classifySignalRejectionGroup(sorted[i].first);
            arr.push_back(std::move(item));
        }
        return arr;
    };

    auto buildTopGroups = [](const std::vector<std::pair<std::string, long long>>& sorted, size_t limit) {
        nlohmann::json arr = nlohmann::json::array();
        const size_t end = std::min(limit, sorted.size());
        for (size_t i = 0; i < end; ++i) {
            nlohmann::json item;
            item["group"] = sorted[i].first;
            item["count"] = sorted[i].second;
            arr.push_back(std::move(item));
        }
        return arr;
    };

    auto buildTopNamedCounts = [](
        const std::vector<std::pair<std::string, long long>>& sorted,
        size_t limit,
        const char* name_key
    ) {
        nlohmann::json arr = nlohmann::json::array();
        const size_t end = std::min(limit, sorted.size());
        for (size_t i = 0; i < end; ++i) {
            nlohmann::json item;
            item[name_key] = sorted[i].first;
            item["count"] = sorted[i].second;
            arr.push_back(std::move(item));
        }
        return arr;
    };

    std::map<std::string, long long> rejection_group_counts;
    for (const auto& kv : snapshot.rejection_reason_counts) {
        rejection_group_counts[classifySignalRejectionGroup(kv.first)] += kv.second;
    }

    long long total_rejections = 0;
    for (const auto& kv : rejection_group_counts) {
        total_rejections += kv.second;
    }
    const double signal_generation_share =
        total_rejections > 0 ? static_cast<double>(rejection_group_counts["signal_generation"]) / static_cast<double>(total_rejections) : 0.0;
    const double manager_prefilter_share =
        total_rejections > 0 ? static_cast<double>(rejection_group_counts["manager_prefilter"]) / static_cast<double>(total_rejections) : 0.0;
    const double position_state_share =
        total_rejections > 0 ? static_cast<double>(rejection_group_counts["position_state"]) / static_cast<double>(total_rejections) : 0.0;
    const bool no_trade_bias_active =
        snapshot.scan_count >= 3 &&
        total_rejections >= 20 &&
        position_state_share < 0.40 &&
        (signal_generation_share >= 0.55 || (signal_generation_share + manager_prefilter_share) >= 0.75);
    const double recommended_trade_floor_scale = no_trade_bias_active ? 0.75 : 1.0;

    std::map<std::string, long long> last_scan_group_counts;
    for (const auto& kv : last_scan_rejection_counts) {
        if (kv.second <= 0) {
            continue;
        }
        last_scan_group_counts[classifySignalRejectionGroup(kv.first)] += kv.second;
    }

    const auto sorted_cumulative_reasons = buildSortedCounts(snapshot.rejection_reason_counts);
    const auto sorted_last_scan_reasons = buildSortedCounts(last_scan_rejection_counts);
    const auto sorted_cumulative_groups = buildSortedCounts(rejection_group_counts);
    const auto sorted_last_scan_groups = buildSortedCounts(last_scan_group_counts);
    const auto sorted_cumulative_hint_adjustments = buildSortedCounts(snapshot.selection_hint_adjustment_counts);
    const auto sorted_last_scan_hint_adjustments = buildSortedCounts(last_scan_hint_adjustment_counts);
    long long total_hint_adjustments = 0;
    for (const auto& kv : snapshot.selection_hint_adjustment_counts) {
        total_hint_adjustments += kv.second;
    }
    const double selection_hint_adjusted_ratio =
        snapshot.selection_scored_candidate_count > 0
            ? static_cast<double>(snapshot.selection_hint_adjusted_candidate_count) /
                static_cast<double>(snapshot.selection_scored_candidate_count)
            : 0.0;

    nlohmann::json report;
    report["updated_at_ms"] = getCurrentTimestampMs();
    report["scan_count"] = snapshot.scan_count;
    report["markets_considered"] = snapshot.markets_considered;
    report["skipped_due_to_open_position"] = snapshot.skipped_due_to_open_position;
    report["skipped_due_to_active_order"] = snapshot.skipped_due_to_active_order;
    report["no_candle_data"] = snapshot.no_candle_data;
    report["no_signal_generated"] = snapshot.no_signal_generated;
    report["filtered_out_by_manager"] = snapshot.filtered_out_by_manager;
    report["no_best_signal"] = snapshot.no_best_signal;
    report["markets_with_selected_candidate"] = snapshot.markets_with_selected_candidate;
    report["generated_signal_candidates"] = snapshot.generated_signal_candidates;
    report["selected_signal_candidates"] = snapshot.selected_signal_candidates;
    report["selection_call_count"] = snapshot.selection_call_count;
    report["selection_scored_candidate_count"] = snapshot.selection_scored_candidate_count;
    report["selection_hint_adjusted_candidate_count"] = snapshot.selection_hint_adjusted_candidate_count;
    report["selection_hint_adjusted_ratio"] = selection_hint_adjusted_ratio;
    report["selection_hint_adjustment_counts"] = snapshot.selection_hint_adjustment_counts;
    report["selection_hint_total_adjustments"] = total_hint_adjustments;
    report["top_selection_hint_adjustments"] =
        buildTopNamedCounts(sorted_cumulative_hint_adjustments, 8, "adjustment");
    report["rejection_reason_counts"] = snapshot.rejection_reason_counts;
    report["rejection_group_counts"] = rejection_group_counts;
    report["total_rejections"] = total_rejections;
    report["signal_generation_share"] = signal_generation_share;
    report["manager_prefilter_share"] = manager_prefilter_share;
    report["position_state_share"] = position_state_share;
    report["no_trade_bias_active"] = no_trade_bias_active;
    report["recommended_trade_floor_scale"] = recommended_trade_floor_scale;
    report["top_rejections"] = buildTopList(sorted_cumulative_reasons, 12);
    report["top_rejection_groups"] = buildTopGroups(sorted_cumulative_groups, 8);

    nlohmann::json last_scan;
    last_scan["rejection_reason_counts"] = last_scan_rejection_counts;
    last_scan["rejection_group_counts"] = last_scan_group_counts;
    last_scan["top_rejections"] = buildTopList(sorted_last_scan_reasons, 12);
    last_scan["top_rejection_groups"] = buildTopGroups(sorted_last_scan_groups, 8);
    last_scan["selection_hint_adjustment_counts"] = last_scan_hint_adjustment_counts;
    last_scan["top_selection_hint_adjustments"] =
        buildTopNamedCounts(sorted_last_scan_hint_adjustments, 8, "adjustment");
    report["last_scan"] = std::move(last_scan);

    try {
        auto report_path =
            autolife::utils::PathUtils::resolveRelativePath("logs/live_signal_funnel_taxonomy_report.json");
        std::filesystem::create_directories(report_path.parent_path());

        std::ofstream out(report_path.string(), std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            LOG_ERROR("Failed to open live signal funnel taxonomy report: {}", report_path.string());
            return;
        }
        out << report.dump(2);
        out.close();

        if (!out) {
            LOG_ERROR("Failed to write live signal funnel taxonomy report: {}", report_path.string());
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to flush live signal funnel taxonomy report: {}", e.what());
    }
}

void TradingEngine::executeSignals() {
    if (pending_signals_.empty()) {
        return;
    }

    LOG_INFO("===== Execute Signals =====");

    if (risk_manager_->isDailyLossLimitExceeded()) {
        const double loss_pct = risk_manager_->getDailyLossPct();
        LOG_ERROR("Daily loss limit exceeded: {:.2f}% >= {:.2f}% (skip new entries)",
                  loss_pct * 100.0, config_.max_daily_loss_pct * 100.0);
        {
            nlohmann::json payload;
            payload["reason"] = "daily_loss_limit_exceeded";
            payload["daily_loss_pct"] = loss_pct;
            payload["max_daily_loss_pct"] = config_.max_daily_loss_pct;
            payload["pending_candidates"] = pending_signals_.size();
            appendJournalEvent(
                core::JournalEventType::NO_TRADE,
                "MULTI",
                "risk_guard",
                payload
            );
        }
        pending_signals_.clear();
        return;
    }
    if (risk_manager_->isDrawdownExceeded()) {
        LOG_ERROR("Max drawdown exceeded (limit {:.2f}%), skip new entries",
                  config_.max_drawdown * 100.0);
        {
            nlohmann::json payload;
            payload["reason"] = "max_drawdown_exceeded";
            payload["max_drawdown_limit"] = config_.max_drawdown;
            payload["pending_candidates"] = pending_signals_.size();
            appendJournalEvent(
                core::JournalEventType::NO_TRADE,
                "MULTI",
                "risk_guard",
                payload
            );
        }
        pending_signals_.clear();
        return;
    }

    const double current_filter = calculateDynamicFilterValue();

    const double current_scale = calculatePositionScaleMultiplier();
    LOG_INFO("Position scale multiplier: {:.2f}", current_scale);

    const auto metrics_snapshot = risk_manager_->getRiskMetrics();
    const bool small_seed_mode =
        metrics_snapshot.total_capital > 0.0 &&
        metrics_snapshot.total_capital <= config_.small_account_tier2_capital_krw;
    const double starvation_relax_base = std::min(0.06, scans_without_new_entry_ * 0.005);
    double starvation_relax = starvation_relax_base;
    double adaptive_filter_floor = std::clamp(current_filter + (small_seed_mode ? 0.08 : 0.0), 0.35, 0.90);
    int per_scan_buy_limit = small_seed_mode
        ? 1
        : std::max(1, config_.max_new_orders_per_scan);
    const double small_seed_rr_add = small_seed_mode ? 0.16 : 0.0;
    const double small_seed_edge_mul = small_seed_mode ? 1.30 : 1.0;
    double min_reward_risk_gate = config_.min_reward_risk + small_seed_rr_add;
    double min_expected_edge_gate = config_.min_expected_edge_pct * small_seed_edge_mul;
    if (small_seed_mode) {
        LOG_INFO("Small-seed mode active: capital {:.0f}, filter {:.3f}, rr>= {:.2f}, edge>= {:.3f}%",
                 metrics_snapshot.total_capital,
                 adaptive_filter_floor,
                 min_reward_risk_gate,
                 min_expected_edge_gate * 100.0);
    }

    std::map<std::string, StrategyEdgeStats> strategy_edge;
    std::map<std::string, StrategyEdgeStats> strategy_edge_recent;
    std::map<std::string, StrategyEdgeStats> strategy_regime_edge;
    std::map<std::string, StrategyEdgeStats> strategy_regime_edge_recent;
    std::map<std::string, StrategyEdgeStats> market_strategy_regime_edge;
    std::map<std::string, StrategyEdgeStats> market_strategy_regime_edge_recent;
    double recent_expectancy_krw = 0.0;
    double recent_win_rate = 0.5;
    int recent_sample = 0;
    {
        const auto history = risk_manager_->getTradeHistory();
        if (performance_store_) {
            performance_store_->rebuild(history);
        }
        const int recent_strategy_window = std::max(16, config_.min_strategy_trades_for_ev);
        const int recent_regime_window = std::max(8, config_.min_strategy_trades_for_ev / 2);
        const int recent_market_regime_window = std::max(6, config_.min_strategy_trades_for_ev / 2);
        strategy_edge = buildStrategyEdgeStats(history);
        strategy_edge_recent = buildStrategyEdgeStats(history, recent_strategy_window);
        strategy_regime_edge = buildStrategyRegimeEdgeStats(history);
        strategy_regime_edge_recent = buildStrategyRegimeEdgeStats(history, recent_regime_window);
        market_strategy_regime_edge = buildMarketStrategyRegimeEdgeStats(history);
        market_strategy_regime_edge_recent = buildMarketStrategyRegimeEdgeStats(
            history,
            recent_market_regime_window
        );
        double recent_profit_sum = 0.0;
        int recent_wins = 0;
        for (auto it = history.rbegin(); it != history.rend() && recent_sample < 30; ++it) {
            recent_profit_sum += it->profit_loss;
            if (it->profit_loss > 0.0) {
                recent_wins++;
            }
            recent_sample++;
        }
        if (recent_sample > 0) {
            recent_expectancy_krw = recent_profit_sum / static_cast<double>(recent_sample);
            recent_win_rate = static_cast<double>(recent_wins) / static_cast<double>(recent_sample);
        }
    }

    analytics::MarketRegime dominant_regime = analytics::MarketRegime::UNKNOWN;
    std::map<analytics::MarketRegime, int> regime_counts;
    int analyzed_regimes = 0;
    {
        for (const auto& coin : scanned_markets_) {
            if (coin.candles.empty()) {
                continue;
            }
            auto r = regime_detector_->analyzeRegime(coin.candles);
            regime_counts[r.regime]++;
            analyzed_regimes++;
        }
        int best_count = -1;
        for (const auto& [r, count] : regime_counts) {
            if (count > best_count) {
                best_count = count;
                dominant_regime = r;
            }
        }
    }

    const auto regime_ratio = [&](analytics::MarketRegime r) -> double {
        if (analyzed_regimes <= 0) {
            return 0.0;
        }
        auto it = regime_counts.find(r);
        if (it == regime_counts.end()) {
            return 0.0;
        }
        return static_cast<double>(it->second) / static_cast<double>(analyzed_regimes);
    };

    const double high_vol_ratio = regime_ratio(analytics::MarketRegime::HIGH_VOLATILITY);
    const double down_ratio = regime_ratio(analytics::MarketRegime::TRENDING_DOWN);
    const double ranging_ratio = regime_ratio(analytics::MarketRegime::RANGING);
    const double up_ratio = regime_ratio(analytics::MarketRegime::TRENDING_UP);
    const double market_hostility_score = std::clamp(
        (high_vol_ratio * 1.00) +
        (down_ratio * 0.85) +
        (ranging_ratio * 0.30) +
        ((1.0 - up_ratio) * 0.10),
        0.0,
        1.0
    );
    const double hostility_alpha = std::clamp(config_.hostility_ewma_alpha, 0.01, 0.99);
    const double hostile_threshold = std::clamp(config_.hostility_hostile_threshold, 0.0, 1.0);
    const double severe_threshold = std::clamp(
        std::max(config_.hostility_severe_threshold, hostile_threshold),
        0.0,
        1.0
    );
    const double extreme_threshold = std::clamp(
        std::max(config_.hostility_extreme_threshold, severe_threshold),
        0.0,
        1.0
    );
    const int pause_scans_base = std::clamp(config_.hostility_pause_scans, 1, 64);
    const int pause_scans_extreme = std::clamp(
        std::max(config_.hostility_pause_scans_extreme, pause_scans_base),
        pause_scans_base,
        96
    );
    const int pause_sample_min = std::clamp(config_.hostility_pause_recent_sample_min, 1, 100);
    const double pause_expectancy_threshold = config_.hostility_pause_recent_expectancy_krw;
    const double pause_win_rate_threshold = std::clamp(config_.hostility_pause_recent_win_rate, 0.0, 1.0);

    if (market_hostility_ewma_ <= 1e-9) {
        market_hostility_ewma_ = market_hostility_score;
    } else {
        market_hostility_ewma_ = std::clamp(
            market_hostility_ewma_ * (1.0 - hostility_alpha) + market_hostility_score * hostility_alpha,
            0.0,
            1.0
        );
    }
    const double effective_hostility = std::max(market_hostility_score, market_hostility_ewma_);

    const bool hostile_market = effective_hostility >= hostile_threshold;
    const bool severe_hostile_market = effective_hostility >= severe_threshold;
    const bool bad_recent_quality =
        recent_sample >= pause_sample_min &&
        recent_expectancy_krw < pause_expectancy_threshold &&
        recent_win_rate < pause_win_rate_threshold;
    const bool pause_new_entries_this_scan =
        severe_hostile_market &&
        bad_recent_quality;
    if (pause_new_entries_this_scan) {
        const int base_pause = (effective_hostility >= extreme_threshold) ? pause_scans_extreme : pause_scans_base;
        hostile_pause_scans_remaining_ = std::max(hostile_pause_scans_remaining_, base_pause);
    }

    starvation_relax = hostile_market
        ? std::min(0.02, starvation_relax_base * 0.33)
        : starvation_relax_base;
    const double effective_filter = std::max(0.35, current_filter - starvation_relax);
    double hostility_filter_add = 0.0;
    if (effective_hostility > (hostile_threshold - 0.04)) {
        hostility_filter_add = std::clamp((effective_hostility - (hostile_threshold - 0.04)) * 0.22, 0.0, 0.08);
    }
    adaptive_filter_floor = std::clamp(
        effective_filter + (small_seed_mode ? 0.08 : 0.0) + hostility_filter_add,
        0.35,
        0.93
    );

    if (!small_seed_mode && effective_hostility >= (hostile_threshold + 0.13)) {
        per_scan_buy_limit = 1;
    }

    if (effective_hostility >= hostile_threshold) {
        min_reward_risk_gate += std::clamp((effective_hostility - hostile_threshold) * 0.45, 0.0, 0.18);
        min_expected_edge_gate += std::clamp((effective_hostility - hostile_threshold) * 0.0011, 0.0, 0.00045);
    } else if (effective_hostility <= std::max(0.0, hostile_threshold - 0.25) && scans_without_new_entry_ >= 6) {
        min_reward_risk_gate = std::max(1.05, min_reward_risk_gate - 0.05);
        min_expected_edge_gate = std::max(config_.min_expected_edge_pct * 0.85, min_expected_edge_gate - 0.00010);
    }

    min_reward_risk_gate = std::clamp(
        min_reward_risk_gate,
        config_.min_reward_risk,
        config_.min_reward_risk + 0.35
    );
    min_expected_edge_gate = std::clamp(
        min_expected_edge_gate,
        config_.min_expected_edge_pct,
        config_.min_expected_edge_pct + 0.0009
    );

    LOG_INFO(
        "Adaptive scan profile: hostility_now={:.3f}, hostility_ewma={:.3f} (up {:.2f}/range {:.2f}/down {:.2f}/hv {:.2f}), filter {:.3f} -> {:.3f}, per_scan_limit={}, rr>= {:.2f}, edge>= {:.3f}%, pause_remaining={}",
        market_hostility_score,
        market_hostility_ewma_,
        up_ratio,
        ranging_ratio,
        down_ratio,
        high_vol_ratio,
        current_filter,
        adaptive_filter_floor,
        per_scan_buy_limit,
        min_reward_risk_gate,
        min_expected_edge_gate * 100.0,
        hostile_pause_scans_remaining_
    );
    if (starvation_relax > 0.0) {
        LOG_INFO("Adaptive filter relaxation: streak={}, relax={:.3f}, effective={:.3f}",
                 scans_without_new_entry_, starvation_relax, effective_filter);
    }
    if (pause_new_entries_this_scan) {
        LOG_WARN(
            "Hostile market entry pause armed: hostility_now {:.3f}, hostility_ewma {:.3f}, recent_expectancy {:.2f} KRW, recent_win_rate {:.2f}, sample={}, pause_scans={}",
            market_hostility_score,
            market_hostility_ewma_,
            recent_expectancy_krw,
            recent_win_rate,
            recent_sample,
            hostile_pause_scans_remaining_
        );
    }
    if (hostile_pause_scans_remaining_ > 0) {
        hostile_pause_scans_remaining_--;
        LOG_WARN(
            "Hostile market entry pause active: remaining_scans={}, hostility_now {:.3f}, hostility_ewma {:.3f}",
            hostile_pause_scans_remaining_,
            market_hostility_score,
            market_hostility_ewma_
        );
        {
            nlohmann::json payload;
            payload["reason"] = "hostility_pause_active";
            payload["remaining_scans"] = hostile_pause_scans_remaining_;
            payload["hostility_now"] = market_hostility_score;
            payload["hostility_ewma"] = market_hostility_ewma_;
            payload["dominant_regime"] = regimeToString(dominant_regime);
            payload["recent_expectancy_krw"] = recent_expectancy_krw;
            payload["recent_win_rate"] = recent_win_rate;
            payload["pending_candidates"] = pending_signals_.size();
            appendJournalEvent(
                core::JournalEventType::NO_TRADE,
                "MULTI",
                "hostility_guard",
                payload
            );
        }
        scans_without_new_entry_ = std::min(scans_without_new_entry_ + 1, 100);
        pending_signals_.clear();
        return;
    }

    if (strategy_manager_) {
        if (config_.use_strategy_alpha_head_mode) {
            // Alpha-head mode keeps strategy modules as feature/signal generators.
            // Do not disable strategies here by historical EV/performance gate.
            auto strategies = strategy_manager_->getStrategies();
            for (auto& strategy_ptr : strategies) {
                if (strategy_ptr && !strategy_ptr->isEnabled()) {
                    strategy_ptr->setEnabled(true);
                    LOG_INFO("Alpha-head mode: strategy re-enabled: {}",
                             strategy_ptr->getInfo().name);
                }
            }
        } else {
            strategy_manager_->refreshStrategyStatesFromHistory(
                risk_manager_->getTradeHistory(),
                dominant_regime,
                config_.avoid_high_volatility,
                config_.avoid_trending_down,
                config_.min_strategy_trades_for_ev,
                config_.min_strategy_expectancy_krw,
                config_.min_strategy_profit_factor
            );
        }
    }

    std::vector<strategy::Signal> policy_input_signals = pending_signals_;
    std::vector<strategy::Signal> execution_candidates = pending_signals_;
    std::vector<engine::PolicyDecisionRecord> core_policy_decisions;
    int core_dropped_by_policy = 0;
    bool policy_probe_available = false;
    if (config_.enable_core_plane_bridge &&
        config_.enable_core_policy_plane &&
        core_cycle_) {
        core::PolicyContext context;
        context.small_seed_mode = small_seed_mode;
        context.max_new_orders_per_scan = per_scan_buy_limit;
        context.dominant_regime = dominant_regime;

        auto decision_batch = core_cycle_->selectPolicyCandidates(pending_signals_, context);
        execution_candidates = std::move(decision_batch.selected_candidates);
        core_policy_decisions = decision_batch.decisions;
        core_dropped_by_policy = decision_batch.dropped_by_policy;
        policy_probe_available = true;
        appendPolicyDecisionAudit(
            decision_batch.decisions,
            dominant_regime,
            small_seed_mode,
            per_scan_buy_limit
        );
        if (!decision_batch.decisions.empty()) {
            nlohmann::json payload;
            payload["selected"] = decision_batch.selected_candidates.size();
            payload["decisions"] = decision_batch.decisions.size();
            payload["small_seed_mode"] = small_seed_mode;
            payload["max_new_orders_per_scan"] = per_scan_buy_limit;
            appendJournalEvent(
                core::JournalEventType::POLICY_CHANGED,
                "MULTI",
                "policy_cycle",
                payload
            );
        }
        if (decision_batch.dropped_by_policy > 0) {
            LOG_INFO("Policy plane dropped {} candidate(s)",
                     decision_batch.dropped_by_policy);
        }
    } else if (policy_controller_) {
        PolicyInput policy_input;
        policy_input.candidates = pending_signals_;
        policy_input.small_seed_mode = small_seed_mode;
        policy_input.max_new_orders_per_scan = per_scan_buy_limit;
        policy_input.dominant_regime = dominant_regime;
        if (performance_store_) {
            policy_input.strategy_stats = &performance_store_->byStrategy();
            policy_input.bucket_stats = &performance_store_->byBucket();
        }
        auto policy_output = policy_controller_->selectCandidates(policy_input);
        execution_candidates = std::move(policy_output.selected_candidates);
        core_policy_decisions = policy_output.decisions;
        core_dropped_by_policy = policy_output.dropped_by_policy;
        policy_probe_available = true;
        appendPolicyDecisionAudit(
            policy_output.decisions,
            dominant_regime,
            small_seed_mode,
            per_scan_buy_limit
        );
        if (!policy_output.decisions.empty()) {
            nlohmann::json payload;
            payload["selected"] = policy_output.selected_candidates.size();
            payload["decisions"] = policy_output.decisions.size();
            payload["small_seed_mode"] = small_seed_mode;
            payload["max_new_orders_per_scan"] = per_scan_buy_limit;
            appendJournalEvent(
                core::JournalEventType::POLICY_CHANGED,
                "MULTI",
                "policy_cycle",
                payload
            );
        }
        if (policy_output.dropped_by_policy > 0) {
            LOG_INFO("AdaptivePolicyController dropped {} candidate(s)",
                     policy_output.dropped_by_policy);
        }
    }

    if (execution_candidates.empty()) {
        nlohmann::json payload;
        payload["reason"] = "policy_selected_zero_candidates";
        payload["input_candidates"] = pending_signals_.size();
        payload["dominant_regime"] = regimeToString(dominant_regime);
        payload["small_seed_mode"] = small_seed_mode;
        payload["max_new_orders_per_scan"] = per_scan_buy_limit;
        appendJournalEvent(
            core::JournalEventType::NO_TRADE,
            "MULTI",
            "policy_guard",
            payload
        );
        scans_without_new_entry_ = std::min(scans_without_new_entry_ + 1, 100);
        pending_signals_.clear();
        return;
    }

    int executed = 0;
    int filtered_out = 0;
    int executed_buys_this_scan = 0;

    auto getTotalPotentialPositions = [&]() -> size_t {
        const size_t current_positions = static_cast<size_t>(risk_manager_->getRiskMetrics().active_positions);
        const size_t pending_buys = order_manager_ ? order_manager_->getActiveBuyOrderCount() : 0;
        return current_positions + pending_buys;
    };

    size_t total_potential = getTotalPotentialPositions();
    if (total_potential >= config_.max_positions) {
        LOG_WARN("Position limit reached: {} / max {}", total_potential, config_.max_positions);
    }

    for (auto& signal : execution_candidates) {
        if (signal.type != strategy::SignalType::BUY &&
            signal.type != strategy::SignalType::STRONG_BUY) {
            continue;
        }

        if (executed_buys_this_scan >= per_scan_buy_limit) {
            LOG_WARN("Per-scan buy limit reached: {} / {}",
                     executed_buys_this_scan, per_scan_buy_limit);
            filtered_out++;
            continue;
        }

        total_potential = getTotalPotentialPositions();
        if (total_potential >= config_.max_positions) {
            filtered_out++;
            continue;
        }

        const bool alpha_head_fallback_candidate =
            isAlphaHeadFallbackCandidate(signal, config_.use_strategy_alpha_head_mode);
        const double history_penalty_scale = alpha_head_fallback_candidate ? 0.45 : 1.0;
        bool alpha_head_relief_eligible = true;
        bool strategy_ev_hard_block = false;
        const bool early_core_risk_gate_enabled =
            config_.enable_core_plane_bridge && config_.enable_core_risk_plane;
        double required_signal_strength = adaptive_filter_floor;
        if (alpha_head_fallback_candidate) {
            required_signal_strength = std::min(required_signal_strength, 0.58);
        }
        double regime_rr_add = 0.0;
        double regime_edge_add = 0.0;
        bool regime_pattern_block = false;
        auto markPatternBlockOrSoften = [&](int trades, double exp_krw, double wr, double pf) {
            if (!alpha_head_fallback_candidate) {
                regime_pattern_block = true;
                return;
            }
            if (trades >= 18 && exp_krw <= -28.0 && wr <= 0.18 && pf <= 0.70) {
                regime_pattern_block = true;
                return;
            }
            required_signal_strength = std::max(required_signal_strength, 0.60);
            regime_rr_add += 0.10;
            regime_edge_add += 0.00015;
        };
        auto regime_edge_it = strategy_regime_edge.find(
            makeStrategyRegimeKey(signal.strategy_name, signal.market_regime)
        );
        auto regime_edge_recent_it = strategy_regime_edge_recent.find(
            makeStrategyRegimeKey(signal.strategy_name, signal.market_regime)
        );
        auto market_regime_it = market_strategy_regime_edge.find(
            makeMarketStrategyRegimeKey(
                signal.market,
                signal.strategy_name,
                signal.market_regime
            )
        );
        auto market_regime_recent_it = market_strategy_regime_edge_recent.find(
            makeMarketStrategyRegimeKey(
                signal.market,
                signal.strategy_name,
                signal.market_regime
            )
        );
        auto computeContextSeverity = [&](const StrategyEdgeStats* full_stat,
                                          const StrategyEdgeStats* recent_stat,
                                          bool market_scope) {
            double severity = 0.0;
            auto accumulate = [&](const StrategyEdgeStats* stat, bool recent_weighted) {
                if (!stat) {
                    return;
                }
                const int min_trades = market_scope ? 4 : 6;
                if (stat->trades < min_trades) {
                    return;
                }
                const double wr = stat->winRate();
                const double pf = stat->profitFactor();
                const double exp_krw = stat->expectancy();
                const double avg_win = stat->avgWinKrw();
                const double avg_loss_abs = stat->avgLossAbsKrw();
                const double loss_to_win_ratio = (avg_win > 1e-9)
                    ? (avg_loss_abs / avg_win)
                    : ((stat->trades > stat->wins) ? 2.0 : 0.0);

                const double weight = recent_weighted ? 1.0 : 0.65;
                const double exp_cut = market_scope ? -14.0 : -12.0;
                const double pf_cut = market_scope ? 0.90 : 0.92;
                const double wr_cut = market_scope ? 0.42 : 0.44;
                const double asym_cut = market_scope ? 1.40 : 1.32;
                const double severe_exp_cut = market_scope ? -22.0 : -18.0;
                const double severe_pf_cut = market_scope ? 0.82 : 0.85;
                const double severe_asym_cut = market_scope ? 1.65 : 1.58;

                if (exp_krw < exp_cut) {
                    severity += 0.24 * weight;
                }
                if (pf < pf_cut) {
                    severity += 0.20 * weight;
                }
                if (wr < wr_cut) {
                    severity += 0.12 * weight;
                }
                if (loss_to_win_ratio > asym_cut) {
                    severity += 0.18 * weight;
                }
                if (exp_krw < severe_exp_cut &&
                    pf < severe_pf_cut &&
                    loss_to_win_ratio > severe_asym_cut) {
                    severity += 0.26 * weight;
                }
            };
            accumulate(full_stat, false);
            accumulate(recent_stat, true);
            return std::clamp(severity, 0.0, 1.0);
        };
        const StrategyEdgeStats* regime_full_stat =
            (regime_edge_it != strategy_regime_edge.end()) ? &regime_edge_it->second : nullptr;
        const StrategyEdgeStats* regime_recent_stat =
            (regime_edge_recent_it != strategy_regime_edge_recent.end())
            ? &regime_edge_recent_it->second
            : nullptr;
        const StrategyEdgeStats* market_full_stat =
            (market_regime_it != market_strategy_regime_edge.end()) ? &market_regime_it->second : nullptr;
        const StrategyEdgeStats* market_recent_stat =
            (market_regime_recent_it != market_strategy_regime_edge_recent.end())
            ? &market_regime_recent_it->second
            : nullptr;
        const double regime_context_severity =
            computeContextSeverity(regime_full_stat, regime_recent_stat, false);
        const double market_context_severity =
            computeContextSeverity(market_full_stat, market_recent_stat, true);
        bool enable_loser_context_quarantine = false;
        const double blended_context_severity = std::clamp(
            (0.62 * regime_context_severity) + (0.38 * market_context_severity),
            0.0,
            1.0
        );
        const bool has_dual_recent_context =
            regime_recent_stat != nullptr && market_recent_stat != nullptr;
        const bool severe_context_overlap =
            regime_context_severity >= 0.58 && market_context_severity >= 0.52;
        double loser_context_severity = blended_context_severity;
        if (severe_context_overlap) {
            loser_context_severity = std::min(1.0, loser_context_severity + 0.08);
        }
        if (!has_dual_recent_context) {
            loser_context_severity *= 0.60;
        }
        auto strategy_context_it = strategy_edge.find(signal.strategy_name);
        const bool strategy_history_deeply_negative =
            strategy_context_it != strategy_edge.end() &&
            strategy_context_it->second.trades >= std::max(12, config_.min_strategy_trades_for_ev / 2) &&
            strategy_context_it->second.expectancy() <= (config_.min_strategy_expectancy_krw - 12.0) &&
            strategy_context_it->second.profitFactor() <=
                std::max(0.86, config_.min_strategy_profit_factor - 0.12);
        if (enable_loser_context_quarantine &&
            early_core_risk_gate_enabled &&
            loser_context_severity >= 0.80 &&
            severe_context_overlap &&
            strategy_history_deeply_negative) {
            const StrategyEdgeStats* block_ref = market_recent_stat
                ? market_recent_stat
                : (regime_recent_stat ? regime_recent_stat : (market_full_stat ? market_full_stat : regime_full_stat));
            if (block_ref) {
                markPatternBlockOrSoften(
                    block_ref->trades,
                    block_ref->expectancy(),
                    block_ref->winRate(),
                    block_ref->profitFactor()
                );
            }
            alpha_head_relief_eligible = false;
        } else if (enable_loser_context_quarantine &&
                   early_core_risk_gate_enabled &&
                   loser_context_severity >= 0.48 &&
                   severe_context_overlap) {
            required_signal_strength = std::max(
                required_signal_strength,
                0.59 + (0.04 * loser_context_severity)
            );
            regime_rr_add += (0.05 + (0.08 * loser_context_severity)) * history_penalty_scale;
            regime_edge_add += (0.00008 + (0.00012 * loser_context_severity)) * history_penalty_scale;
            if (loser_context_severity >= 0.62 && strategy_history_deeply_negative) {
                alpha_head_relief_eligible = false;
            }
        }
        if (regime_edge_it != strategy_regime_edge.end()) {
            const auto& stat = regime_edge_it->second;
            if (stat.trades >= 6) {
                const double wr = stat.winRate();
                const double pf = stat.profitFactor();
                const double exp_krw = stat.expectancy();

                if (stat.trades >= 10 &&
                    (exp_krw < -20.0 || (wr < 0.30 && pf < 0.78))) {
                    markPatternBlockOrSoften(stat.trades, exp_krw, wr, pf);
                }
                if (stat.trades >= 12 &&
                    exp_krw < -15.0 &&
                    (wr < 0.40 || pf < 0.90)) {
                    required_signal_strength = std::max(required_signal_strength, 0.62);
                    regime_rr_add += (0.20 * history_penalty_scale);
                    regime_edge_add += (0.0003 * history_penalty_scale);
                }
                if (stat.trades >= 18 &&
                    exp_krw < -22.0 &&
                    wr < 0.20) {
                    markPatternBlockOrSoften(stat.trades, exp_krw, wr, pf);
                }
                if (exp_krw < 0.0) {
                    required_signal_strength += std::clamp((-exp_krw) / 2500.0, 0.0, 0.08) * history_penalty_scale;
                    regime_rr_add += std::clamp((-exp_krw) / 1800.0, 0.0, 0.20) * history_penalty_scale;
                    regime_edge_add += std::clamp((-exp_krw) / 200000.0, 0.0, 0.0004) * history_penalty_scale;
                } else if (exp_krw > 8.0 && wr >= 0.58 && pf >= 1.15) {
                    required_signal_strength -= 0.02;
                    regime_rr_add -= 0.08;
                    regime_edge_add -= 0.00015;
                }
                if (wr < 0.42) {
                    required_signal_strength += (0.03 * history_penalty_scale);
                    regime_rr_add += (0.10 * history_penalty_scale);
                    regime_edge_add += (0.0002 * history_penalty_scale);
                }
                if (pf < 0.95) {
                    required_signal_strength += (0.02 * history_penalty_scale);
                    regime_rr_add += (0.08 * history_penalty_scale);
                    regime_edge_add += (0.0002 * history_penalty_scale);
                }
            }
        }
        if (market_regime_it != market_strategy_regime_edge.end()) {
            const auto& stat = market_regime_it->second;
            if (stat.trades >= 4) {
                const double wr = stat.winRate();
                const double pf = stat.profitFactor();
                const double exp_krw = stat.expectancy();

                if (stat.trades >= 6 &&
                    exp_krw < -40.0 && wr < 0.25 && pf < 0.75) {
                    markPatternBlockOrSoften(stat.trades, exp_krw, wr, pf);
                }
                if (exp_krw < -20.0 && wr < 0.35 && pf < 0.85) {
                    required_signal_strength += (0.05 * history_penalty_scale);
                    regime_rr_add += (0.15 * history_penalty_scale);
                    regime_edge_add += (0.0003 * history_penalty_scale);
                } else if (exp_krw < -10.0) {
                    required_signal_strength += (0.02 * history_penalty_scale);
                    regime_rr_add += (0.08 * history_penalty_scale);
                    regime_edge_add += (0.00015 * history_penalty_scale);
                } else if (exp_krw > 10.0 && wr >= 0.60 && pf >= 1.20) {
                    required_signal_strength -= 0.02;
                    regime_rr_add -= 0.06;
                    regime_edge_add -= 0.0001;
                }
            }
        }
        if (auto strategy_it = strategy_edge.find(signal.strategy_name);
            strategy_it != strategy_edge.end()) {
            const auto& stat = strategy_it->second;
            if (stat.trades >= 10) {
                const double wr = stat.winRate();
                const double pf = stat.profitFactor();
                const double exp_krw = stat.expectancy();
                const double avg_win_krw = stat.avgWinKrw();
                const double avg_loss_abs_krw = stat.avgLossAbsKrw();
                const double loss_to_win_ratio = (avg_win_krw > 1e-9)
                    ? (avg_loss_abs_krw / avg_win_krw)
                    : 0.0;
                if (exp_krw < 0.0 &&
                    pf < 1.00 &&
                    wr >= 0.50 &&
                    loss_to_win_ratio >= 1.30) {
                    if (exp_krw < -2.0 || pf < 0.95 || loss_to_win_ratio >= 1.35) {
                        alpha_head_relief_eligible = false;
                    }
                    const double asymmetry_pressure = std::clamp(
                        (loss_to_win_ratio - 1.20) / 1.20,
                        0.0,
                        1.0
                    );
                    required_signal_strength = std::max(
                        required_signal_strength,
                        alpha_head_fallback_candidate ? 0.58 : 0.62
                    );
                    regime_rr_add += (0.08 + (0.10 * asymmetry_pressure)) * history_penalty_scale;
                    regime_edge_add += (0.00014 + (0.00018 * asymmetry_pressure)) * history_penalty_scale;
                    if (stat.trades >= 14 &&
                        exp_krw < -6.0 &&
                        pf < 0.85 &&
                        loss_to_win_ratio >= 1.65) {
                        if (alpha_head_fallback_candidate) {
                            required_signal_strength = std::max(required_signal_strength, 0.70);
                            regime_rr_add += 0.16;
                            regime_edge_add += 0.00024;
                        } else {
                            regime_pattern_block = true;
                        }
                    }
                }
            }
        }
        applyArchetypeRiskAdjustments(
            signal,
            required_signal_strength,
            regime_rr_add,
            regime_edge_add,
            regime_pattern_block
        );
        if (alpha_head_fallback_candidate) {
            required_signal_strength = std::max(0.46, required_signal_strength - 0.05);
        }
        required_signal_strength = std::clamp(
            required_signal_strength,
            alpha_head_fallback_candidate ? 0.34 : 0.35,
            alpha_head_fallback_candidate ? 0.78 : 0.92
        );
        signal.signal_filter = required_signal_strength;
        const bool core_risk_gate_enabled =
            config_.enable_core_plane_bridge && config_.enable_core_risk_plane;
        if (core_risk_gate_enabled && requiresTypedArchetype(signal.strategy_name)) {
            const bool archetype_fallback_allowed =
                signal.strength >= 0.74 &&
                signal.expected_value >= 0.0005;
            const bool archetype_ready =
                !signal.entry_archetype.empty() &&
                signal.entry_archetype != "UNSPECIFIED";
            if (!archetype_ready) {
                if (!archetype_fallback_allowed) {
                    LOG_INFO("{} skipped by archetype gate [{}]: typed entry_archetype required",
                             signal.market, signal.strategy_name);
                    filtered_out++;
                    continue;
                }
                LOG_INFO("{} archetype fallback accepted [{}]: strength {:.3f}, ev {:.5f}",
                         signal.market, signal.strategy_name, signal.strength, signal.expected_value);
            }
        }
        const bool alpha_head_quality_override =
            alpha_head_fallback_candidate &&
            !regime_pattern_block &&
            signal.expected_value >= 0.0007 &&
            signal.liquidity_score >= 58.0 &&
            signal.strength >= (required_signal_strength - 0.03);
        if (regime_pattern_block) {
            LOG_INFO("{} skipped by regime-pattern gate [{}]: severe negative pattern history",
                     signal.market, signal.strategy_name);
            filtered_out++;
            continue;
        }
        if (signal.strength < required_signal_strength && !alpha_head_quality_override) {
            LOG_INFO("{} filtered out by dynamic filter (strength {:.3f} < filter {:.3f})",
                     signal.market, signal.strength, required_signal_strength);
            filtered_out++;
            continue;
        }

        auto stat_it = strategy_edge.find(signal.strategy_name);
        if (stat_it != strategy_edge.end()) {
            const auto& stat = stat_it->second;
            const auto recent_stat_it = strategy_edge_recent.find(signal.strategy_name);
            const bool has_recent_recovery =
                recent_stat_it != strategy_edge_recent.end() &&
                recent_stat_it->second.trades >= std::max(10, config_.min_strategy_trades_for_ev / 2) &&
                recent_stat_it->second.expectancy() >= (config_.min_strategy_expectancy_krw + 1.0) &&
                recent_stat_it->second.profitFactor() >=
                    std::max(1.00, config_.min_strategy_profit_factor) &&
                recent_stat_it->second.winRate() >= 0.52;
            const bool has_regime_recovery =
                regime_edge_it != strategy_regime_edge.end() &&
                regime_edge_it->second.trades >= 8 &&
                regime_edge_it->second.expectancy() >= config_.min_strategy_expectancy_krw &&
                regime_edge_it->second.profitFactor() >=
                    std::max(0.98, config_.min_strategy_profit_factor - 0.02) &&
                regime_edge_it->second.winRate() >= 0.50;
            const bool has_recovery_evidence_any = has_recent_recovery || has_regime_recovery;
            const int relaxed_recovery_min_trades = std::max(
                6,
                config_.strategy_ev_pre_cat_relaxed_recovery_min_trades
            );
            const double relaxed_recovery_expectancy_floor =
                config_.min_strategy_expectancy_krw -
                std::max(0.0, config_.strategy_ev_pre_cat_relaxed_recovery_expectancy_gap_krw);
            const double relaxed_recovery_profit_factor_floor = std::max(
                0.90,
                config_.min_strategy_profit_factor -
                std::max(0.0, config_.strategy_ev_pre_cat_relaxed_recovery_profit_factor_gap)
            );
            const bool has_recent_recovery_relaxed =
                recent_stat_it != strategy_edge_recent.end() &&
                recent_stat_it->second.trades >= relaxed_recovery_min_trades &&
                recent_stat_it->second.expectancy() >= relaxed_recovery_expectancy_floor &&
                recent_stat_it->second.profitFactor() >= relaxed_recovery_profit_factor_floor &&
                recent_stat_it->second.winRate() >= config_.strategy_ev_pre_cat_relaxed_recovery_min_win_rate;
            const bool has_regime_recovery_relaxed =
                regime_edge_it != strategy_regime_edge.end() &&
                regime_edge_it->second.trades >= relaxed_recovery_min_trades &&
                regime_edge_it->second.expectancy() >= relaxed_recovery_expectancy_floor &&
                regime_edge_it->second.profitFactor() >= relaxed_recovery_profit_factor_floor &&
                regime_edge_it->second.winRate() >= config_.strategy_ev_pre_cat_relaxed_recovery_min_win_rate;
            const bool has_recovery_evidence_relaxed_recent_regime =
                has_recent_recovery_relaxed || has_regime_recovery_relaxed;
            const bool has_recovery_evidence_relaxed_full_history =
                stat.trades >= relaxed_recovery_min_trades &&
                stat.expectancy() >= relaxed_recovery_expectancy_floor &&
                stat.profitFactor() >= relaxed_recovery_profit_factor_floor &&
                stat.winRate() >= config_.strategy_ev_pre_cat_relaxed_recovery_min_win_rate;
            const bool has_recovery_evidence_relaxed_any =
                has_recovery_evidence_relaxed_recent_regime ||
                (config_.enable_strategy_ev_pre_cat_relaxed_recovery_full_history_anchor &&
                 has_recovery_evidence_relaxed_full_history);
            const bool has_recovery_evidence_for_soften_raw =
                has_recovery_evidence_any ||
                (config_.enable_strategy_ev_pre_cat_relaxed_recovery_evidence &&
                 has_recovery_evidence_relaxed_any);
            bool has_recovery_evidence_hysteresis_override = false;
            if (config_.enable_strategy_ev_pre_cat_recovery_evidence_hysteresis) {
                const int hysteresis_hold_steps = std::clamp(
                    config_.strategy_ev_pre_cat_recovery_evidence_hysteresis_hold_steps,
                    1,
                    120
                );
                const int hysteresis_min_trades = std::max(
                    0,
                    config_.strategy_ev_pre_cat_recovery_evidence_hysteresis_min_trades
                );
                const std::string hysteresis_key = makeStrategyRegimeKey(
                    signal.strategy_name,
                    signal.market_regime
                );
                int& hold_steps_left = pre_cat_recovery_hysteresis_hold_by_key_[hysteresis_key];
                if (has_recovery_evidence_for_soften_raw && stat.trades >= hysteresis_min_trades) {
                    hold_steps_left = hysteresis_hold_steps;
                } else if (!has_recovery_evidence_for_soften_raw &&
                           hold_steps_left > 0 &&
                           stat.trades >= hysteresis_min_trades) {
                    --hold_steps_left;
                    has_recovery_evidence_hysteresis_override = true;
                } else if (hold_steps_left > 0) {
                    hold_steps_left = std::max(0, hold_steps_left - 1);
                }
            }
            const bool has_recovery_evidence_for_soften =
                has_recovery_evidence_for_soften_raw || has_recovery_evidence_hysteresis_override;
            const bool recovery_quality_context =
                (signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
                 signal.market_regime == analytics::MarketRegime::RANGING) &&
                signal.strength >= 0.70 &&
                signal.expected_value >= 0.00075 &&
                signal.liquidity_score >= 58.0;
            const bool recovery_quality_context_hysteresis_relaxed =
                (signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
                 signal.market_regime == analytics::MarketRegime::RANGING) &&
                signal.strength >= config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_strength &&
                signal.expected_value >= config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_expected_value &&
                signal.liquidity_score >= config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_liquidity;
            const bool recovery_quality_hysteresis_override =
                config_.enable_strategy_ev_pre_cat_recovery_quality_hysteresis_relief &&
                has_recovery_evidence_hysteresis_override &&
                !recovery_quality_context &&
                recovery_quality_context_hysteresis_relaxed;
            const bool recovery_quality_context_for_soften =
                recovery_quality_context || recovery_quality_hysteresis_override;
            const StrategyEdgeStats* recent_stat =
                recent_stat_it != strategy_edge_recent.end() ? &recent_stat_it->second : nullptr;
            const StrategyEdgeStats* regime_stat =
                regime_edge_it != strategy_regime_edge.end() ? &regime_edge_it->second : nullptr;
            const int pre_cat_local_recent_trades = recent_stat != nullptr ? recent_stat->trades : -1;
            const int pre_cat_local_regime_trades = regime_stat != nullptr ? regime_stat->trades : -1;
            const int pre_cat_local_trade_scope = std::max(
                pre_cat_local_recent_trades,
                pre_cat_local_regime_trades
            );
            // Keep relief activation bounded on local regime/recent scope.
            // Full-history-only trade count permanently blocks relief after warm-up.
            const int pre_cat_relief_trade_scope =
                pre_cat_local_trade_scope > 0 ? pre_cat_local_trade_scope : stat.trades;
            auto computeHistoryNegativePressure = [&](const StrategyEdgeStats& edge, bool relaxed_scope) {
                double pressure = 0.0;
                const double exp_krw = edge.expectancy();
                const double pf = edge.profitFactor();
                const double wr = edge.winRate();
                const double avg_win_krw = edge.avgWinKrw();
                const double avg_loss_abs_krw = edge.avgLossAbsKrw();
                const double loss_to_win_ratio = (avg_win_krw > 1e-9)
                    ? (avg_loss_abs_krw / avg_win_krw)
                    : ((edge.trades > edge.wins) ? 2.0 : 0.0);
                const double exp_floor = config_.min_strategy_expectancy_krw - (relaxed_scope ? 7.0 : 5.0);
                const double pf_floor = std::max(
                    0.80,
                    config_.min_strategy_profit_factor - (relaxed_scope ? 0.24 : 0.18)
                );
                if (exp_krw < exp_floor) {
                    pressure += 0.32;
                }
                if (pf < pf_floor) {
                    pressure += 0.30;
                }
                if (wr < 0.46) {
                    pressure += 0.20;
                }
                if (loss_to_win_ratio >= 1.45) {
                    pressure += 0.24;
                }
                if (exp_krw < (config_.min_strategy_expectancy_krw - 18.0) &&
                    pf < std::max(0.72, config_.min_strategy_profit_factor - 0.32) &&
                    loss_to_win_ratio >= 1.70) {
                    pressure += 0.28;
                }
                return std::clamp(pressure, 0.0, 1.0);
            };
            const double full_history_pressure = computeHistoryNegativePressure(stat, false);
            const double recent_history_pressure =
                (recent_stat != nullptr &&
                 recent_stat->trades >= std::max(8, config_.min_strategy_trades_for_ev / 5))
                ? computeHistoryNegativePressure(*recent_stat, true)
                : 0.0;
            const double regime_history_pressure =
                (regime_stat != nullptr && regime_stat->trades >= 8)
                ? computeHistoryNegativePressure(*regime_stat, true)
                : 0.0;
            const bool severe_full_negative =
                stat.trades >= std::max(18, config_.min_strategy_trades_for_ev / 3) &&
                full_history_pressure >= 0.70;
            const int severe_full_min_trades_pre_cat = config_.enable_strategy_ev_pre_cat_relaxed_severe_gate
                ? std::max(10, config_.strategy_ev_pre_cat_relaxed_severe_min_trades)
                : std::max(18, config_.min_strategy_trades_for_ev / 3);
            const double severe_full_pressure_threshold_pre_cat =
                config_.enable_strategy_ev_pre_cat_relaxed_severe_gate
                ? std::clamp(config_.strategy_ev_pre_cat_relaxed_severe_pressure_threshold, 0.70, 0.95)
                : 0.70;
            const bool severe_full_negative_pre_cat_threshold =
                stat.trades >= severe_full_min_trades_pre_cat &&
                full_history_pressure >= severe_full_pressure_threshold_pre_cat;
            const double stat_exp_krw = stat.expectancy();
            const double stat_pf = stat.profitFactor();
            const double stat_wr = stat.winRate();
            const double stat_avg_win_krw = stat.avgWinKrw();
            const double stat_avg_loss_abs_krw = stat.avgLossAbsKrw();
            const double stat_loss_to_win_ratio = (stat_avg_win_krw > 1e-9)
                ? (stat_avg_loss_abs_krw / stat_avg_win_krw)
                : ((stat.trades > stat.wins) ? 2.0 : 0.0);
            const bool severe_axis_exp =
                stat_exp_krw < (config_.min_strategy_expectancy_krw - 10.0);
            const bool severe_axis_pf =
                stat_pf < std::max(0.78, config_.min_strategy_profit_factor - 0.24);
            const bool severe_axis_wr = stat_wr < 0.40;
            const bool severe_axis_asym = stat_loss_to_win_ratio >= 1.55;
            const int severe_axis_count =
                static_cast<int>(severe_axis_exp) +
                static_cast<int>(severe_axis_pf) +
                static_cast<int>(severe_axis_wr) +
                static_cast<int>(severe_axis_asym);
            const bool severe_combo_catastrophic =
                stat_exp_krw < (config_.min_strategy_expectancy_krw - 16.0) &&
                stat_pf < std::max(0.75, config_.min_strategy_profit_factor - 0.28) &&
                (stat_wr < 0.36 || stat_loss_to_win_ratio >= 1.65);
            const double severe_composite_pressure_threshold = std::clamp(
                config_.strategy_ev_pre_cat_composite_pressure_threshold,
                0.70,
                0.98
            );
            const int severe_composite_min_critical_signals = std::clamp(
                config_.strategy_ev_pre_cat_composite_min_critical_signals,
                1,
                4
            );
            const bool severe_composite_catastrophic_path =
                stat.trades >= severe_full_min_trades_pre_cat &&
                severe_combo_catastrophic;
            const bool severe_composite_pressure_axis =
                stat.trades >= severe_full_min_trades_pre_cat &&
                full_history_pressure >= severe_composite_pressure_threshold &&
                severe_axis_count >= severe_composite_min_critical_signals;
            const bool severe_full_negative_pre_cat_composite =
                severe_composite_catastrophic_path || severe_composite_pressure_axis;
            const bool severe_full_negative_pre_cat =
                config_.enable_strategy_ev_pre_cat_composite_severe_model
                ? severe_full_negative_pre_cat_composite
                : severe_full_negative_pre_cat_threshold;
            const bool severe_recent_negative =
                recent_stat != nullptr &&
                recent_stat->trades >= std::max(8, config_.min_strategy_trades_for_ev / 5) &&
                recent_history_pressure >= 0.62;
            const bool severe_regime_negative =
                regime_stat != nullptr &&
                regime_stat->trades >= 8 &&
                regime_history_pressure >= 0.64;
            const bool enable_strategy_ev_sync_guard =
                config_.enable_strategy_ev_pre_cat_sync_guard;
            const bool severe_history_sync =
                severe_full_negative &&
                (enable_strategy_ev_sync_guard
                    ? (severe_recent_negative || severe_regime_negative)
                    : true);
            const bool contextual_recovery_ready =
                recovery_quality_context &&
                (enable_strategy_ev_sync_guard
                    ? (has_recent_recovery || has_regime_recovery)
                    : (has_recent_recovery && has_regime_recovery));
            const bool non_hostile_regime =
                signal.market_regime != analytics::MarketRegime::HIGH_VOLATILITY &&
                signal.market_regime != analytics::MarketRegime::TRENDING_DOWN;
            const double contextual_severe_max_pressure = std::clamp(
                config_.strategy_ev_pre_cat_contextual_severe_max_pressure,
                0.75,
                0.98
            );
            const int contextual_severe_max_axis_count = std::clamp(
                config_.strategy_ev_pre_cat_contextual_severe_max_axis_count,
                1,
                4
            );
            const bool contextual_severe_downgrade_ready =
                config_.enable_strategy_ev_pre_cat_contextual_severe_downgrade &&
                severe_full_negative_pre_cat &&
                recovery_quality_context_for_soften &&
                has_recovery_evidence_for_soften &&
                non_hostile_regime &&
                full_history_pressure <= contextual_severe_max_pressure &&
                severe_axis_count <= contextual_severe_max_axis_count;
            const bool severe_full_negative_pre_cat_effective =
                severe_full_negative_pre_cat && !contextual_severe_downgrade_ready;
            const bool severe_history_sync_pre_cat =
                severe_full_negative_pre_cat_effective &&
                (enable_strategy_ev_sync_guard
                    ? (severe_recent_negative || severe_regime_negative)
                    : true);
            const double raw_reward_risk_ratio =
                (signal.expected_risk_pct > 1e-9)
                ? (signal.expected_return_pct / signal.expected_risk_pct)
                : 0.0;
            const double pre_cat_unsynced_override_min_strength = std::clamp(
                config_.strategy_ev_pre_cat_unsynced_override_min_strength,
                0.60,
                0.92
            );
            const double pre_cat_unsynced_override_min_expected_value = std::clamp(
                config_.strategy_ev_pre_cat_unsynced_override_min_expected_value,
                0.00040,
                0.00300
            );
            const double pre_cat_unsynced_override_min_liquidity = std::clamp(
                config_.strategy_ev_pre_cat_unsynced_override_min_liquidity,
                45.0,
                95.0
            );
            const double pre_cat_unsynced_override_min_reward_risk = std::clamp(
                config_.strategy_ev_pre_cat_unsynced_override_min_reward_risk,
                1.05,
                2.60
            );
            const double pre_cat_unsynced_override_max_full_history_pressure = std::clamp(
                config_.strategy_ev_pre_cat_unsynced_override_max_full_history_pressure,
                0.70,
                1.00
            );
            const int pre_cat_unsynced_override_max_severe_axis_count = std::clamp(
                config_.strategy_ev_pre_cat_unsynced_override_max_severe_axis_count,
                1,
                4
            );
            const bool unsynced_soft_override_ready =
                enable_strategy_ev_sync_guard &&
                non_hostile_regime &&
                signal.strength >= pre_cat_unsynced_override_min_strength &&
                signal.expected_value >= pre_cat_unsynced_override_min_expected_value &&
                signal.liquidity_score >= pre_cat_unsynced_override_min_liquidity &&
                (raw_reward_risk_ratio <= 0.0 ||
                 raw_reward_risk_ratio >= pre_cat_unsynced_override_min_reward_risk) &&
                full_history_pressure <= pre_cat_unsynced_override_max_full_history_pressure &&
                severe_axis_count <= pre_cat_unsynced_override_max_severe_axis_count;
            const double pre_cat_soften_strength_floor =
                recovery_quality_hysteresis_override
                ? std::min(0.70, config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_strength)
                : 0.70;
            const double pre_cat_soften_expected_value_floor =
                recovery_quality_hysteresis_override
                ? std::min(0.00070, config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_expected_value)
                : 0.00070;
            const double pre_cat_soften_liquidity_floor =
                recovery_quality_hysteresis_override
                ? std::min(58.0, config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_liquidity)
                : 58.0;
            const int pre_cat_recovery_bridge_max_trades = std::max(
                8,
                config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_strategy_trades
            );
            const double pre_cat_recovery_bridge_max_full_history_pressure = std::clamp(
                config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_full_history_pressure,
                0.70,
                1.00
            );
            const int pre_cat_recovery_bridge_max_severe_axis_count = std::clamp(
                config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_severe_axis_count,
                1,
                4
            );
            const int pre_cat_recovery_bridge_max_activations_per_key = std::max(
                1,
                config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_activations_per_key
            );
            const std::string pre_cat_recovery_bridge_key = makeStrategyRegimeKey(
                signal.strategy_name,
                signal.market_regime
            );
            const int pre_cat_negative_history_quarantine_hold_steps = std::clamp(
                config_.strategy_ev_pre_cat_negative_history_quarantine_hold_steps,
                1,
                60
            );
            const double pre_cat_negative_history_quarantine_min_full_history_pressure = std::clamp(
                config_.strategy_ev_pre_cat_negative_history_quarantine_min_full_history_pressure,
                0.70,
                1.00
            );
            const double pre_cat_negative_history_quarantine_max_history_pf = std::clamp(
                config_.strategy_ev_pre_cat_negative_history_quarantine_max_history_pf,
                0.20,
                1.20
            );
            const double pre_cat_negative_history_quarantine_max_history_expectancy_krw =
                config_.strategy_ev_pre_cat_negative_history_quarantine_max_history_expectancy_krw;
            int& pre_cat_negative_history_quarantine_hold_left =
                pre_cat_negative_history_quarantine_hold_by_key_[pre_cat_recovery_bridge_key];
            bool pre_cat_negative_history_quarantine_active = false;
            if (config_.enable_strategy_ev_pre_cat_negative_history_quarantine &&
                pre_cat_negative_history_quarantine_hold_left > 0) {
                pre_cat_negative_history_quarantine_active = true;
                pre_cat_negative_history_quarantine_hold_left =
                    std::max(0, pre_cat_negative_history_quarantine_hold_left - 1);
            }
            const int pre_cat_recovery_bridge_activations_used =
                pre_cat_recovery_bridge_activation_by_key_.count(pre_cat_recovery_bridge_key) > 0
                ? pre_cat_recovery_bridge_activation_by_key_.at(pre_cat_recovery_bridge_key)
                : 0;
            const bool pre_cat_recovery_bridge_surrogate_ready =
                config_.enable_strategy_ev_pre_cat_recovery_evidence_bridge_surrogate &&
                !has_recovery_evidence_relaxed_any &&
                pre_cat_relief_trade_scope <= std::max(12, pre_cat_recovery_bridge_max_trades / 2) &&
                signal.strength >= std::max(0.72, pre_cat_soften_strength_floor + 0.02) &&
                signal.expected_value >= std::max(0.00100, pre_cat_soften_expected_value_floor + 0.00020) &&
                signal.liquidity_score >= std::max(60.0, pre_cat_soften_liquidity_floor + 2.0) &&
                full_history_pressure <= std::min(pre_cat_recovery_bridge_max_full_history_pressure, 0.92) &&
                recent_history_pressure <= 0.78 &&
                regime_history_pressure <= 0.80 &&
                severe_axis_count <= std::min(pre_cat_recovery_bridge_max_severe_axis_count, 2);
            const bool pre_cat_recovery_bridge_ready =
                config_.enable_strategy_ev_pre_cat_recovery_evidence_bridge &&
                !has_recovery_evidence_for_soften &&
                pre_cat_recovery_bridge_activations_used <
                    pre_cat_recovery_bridge_max_activations_per_key &&
                pre_cat_relief_trade_scope <= pre_cat_recovery_bridge_max_trades &&
                (has_recovery_evidence_relaxed_any || pre_cat_recovery_bridge_surrogate_ready) &&
                recovery_quality_context_for_soften &&
                non_hostile_regime &&
                !severe_full_negative_pre_cat_effective &&
                (raw_reward_risk_ratio <= 0.0 || raw_reward_risk_ratio >= 1.18) &&
                full_history_pressure <= pre_cat_recovery_bridge_max_full_history_pressure &&
                severe_axis_count <= pre_cat_recovery_bridge_max_severe_axis_count;
            const bool has_recovery_evidence_for_soften_effective =
                has_recovery_evidence_for_soften || pre_cat_recovery_bridge_ready;
            const bool pre_cat_soften_candidate_quality_and_evidence =
                recovery_quality_context_for_soften && has_recovery_evidence_for_soften_effective;
            const bool pre_cat_soften_candidate_non_severe =
                pre_cat_soften_candidate_quality_and_evidence && !severe_full_negative_pre_cat_effective;
            const bool pre_cat_soften_candidate_non_hostile =
                pre_cat_soften_candidate_non_severe && non_hostile_regime;
            const bool pre_cat_soften_candidate_rr_ok =
                pre_cat_soften_candidate_non_hostile &&
                (raw_reward_risk_ratio <= 0.0 || raw_reward_risk_ratio >= 1.18);
            const bool pre_cat_soften_non_severe_ready =
                config_.enable_strategy_ev_pre_cat_soften_non_severe &&
                pre_cat_soften_candidate_rr_ok &&
                signal.strength >= pre_cat_soften_strength_floor &&
                signal.expected_value >= pre_cat_soften_expected_value_floor &&
                signal.liquidity_score >= pre_cat_soften_liquidity_floor;
            const int pre_cat_no_soft_quality_relief_max_trades = std::max(
                8,
                config_.strategy_ev_pre_cat_no_soft_quality_relief_max_strategy_trades
            );
            const double pre_cat_no_soft_quality_relief_min_strength = std::clamp(
                config_.strategy_ev_pre_cat_no_soft_quality_relief_min_strength,
                0.60,
                0.90
            );
            const double pre_cat_no_soft_quality_relief_min_expected_value = std::clamp(
                config_.strategy_ev_pre_cat_no_soft_quality_relief_min_expected_value,
                0.00040,
                0.00250
            );
            const double pre_cat_no_soft_quality_relief_min_liquidity = std::clamp(
                config_.strategy_ev_pre_cat_no_soft_quality_relief_min_liquidity,
                48.0,
                90.0
            );
            const double pre_cat_no_soft_quality_relief_min_reward_risk = std::clamp(
                config_.strategy_ev_pre_cat_no_soft_quality_relief_min_reward_risk,
                1.10,
                2.40
            );
            const double pre_cat_no_soft_quality_relief_max_full_history_pressure = std::clamp(
                config_.strategy_ev_pre_cat_no_soft_quality_relief_max_full_history_pressure,
                0.70,
                1.00
            );
            const int pre_cat_no_soft_quality_relief_max_severe_axis_count = std::clamp(
                config_.strategy_ev_pre_cat_no_soft_quality_relief_max_severe_axis_count,
                1,
                4
            );
            const int pre_cat_no_soft_quality_relief_max_activations_per_key = std::max(
                1,
                config_.strategy_ev_pre_cat_no_soft_quality_relief_max_activations_per_key
            );
            const std::string pre_cat_no_soft_quality_relief_key = makeStrategyRegimeKey(
                signal.strategy_name,
                signal.market_regime
            );
            const int pre_cat_no_soft_quality_relief_activations_used =
                pre_cat_no_soft_quality_relief_activation_by_key_.count(pre_cat_no_soft_quality_relief_key) > 0
                ? pre_cat_no_soft_quality_relief_activation_by_key_.at(pre_cat_no_soft_quality_relief_key)
                : 0;
            const bool pre_cat_no_soft_quality_relief_ready =
                config_.enable_strategy_ev_pre_cat_no_soft_quality_relief &&
                !has_recovery_evidence_for_soften_effective &&
                pre_cat_no_soft_quality_relief_activations_used <
                    pre_cat_no_soft_quality_relief_max_activations_per_key &&
                pre_cat_relief_trade_scope <= pre_cat_no_soft_quality_relief_max_trades &&
                recovery_quality_context_for_soften &&
                !severe_full_negative_pre_cat_effective &&
                signal.strength >= pre_cat_no_soft_quality_relief_min_strength &&
                signal.expected_value >= pre_cat_no_soft_quality_relief_min_expected_value &&
                signal.liquidity_score >= pre_cat_no_soft_quality_relief_min_liquidity &&
                (raw_reward_risk_ratio <= 0.0 ||
                 raw_reward_risk_ratio >= pre_cat_no_soft_quality_relief_min_reward_risk) &&
                full_history_pressure <= pre_cat_no_soft_quality_relief_max_full_history_pressure &&
                severe_axis_count <= pre_cat_no_soft_quality_relief_max_severe_axis_count;
            const int pre_cat_pressure_rebound_relief_max_trades = std::max(
                8,
                config_.strategy_ev_pre_cat_pressure_rebound_relief_max_strategy_trades
            );
            const double pre_cat_pressure_rebound_relief_min_strength = std::clamp(
                config_.strategy_ev_pre_cat_pressure_rebound_relief_min_strength,
                0.60,
                0.92
            );
            const double pre_cat_pressure_rebound_relief_min_expected_value = std::clamp(
                config_.strategy_ev_pre_cat_pressure_rebound_relief_min_expected_value,
                0.00040,
                0.00300
            );
            const double pre_cat_pressure_rebound_relief_min_liquidity = std::clamp(
                config_.strategy_ev_pre_cat_pressure_rebound_relief_min_liquidity,
                45.0,
                95.0
            );
            const double pre_cat_pressure_rebound_relief_min_reward_risk = std::clamp(
                config_.strategy_ev_pre_cat_pressure_rebound_relief_min_reward_risk,
                1.05,
                2.60
            );
            const double pre_cat_pressure_rebound_relief_min_full_history_pressure = std::clamp(
                config_.strategy_ev_pre_cat_pressure_rebound_relief_min_full_history_pressure,
                0.70,
                1.00
            );
            const double pre_cat_pressure_rebound_relief_max_recent_history_pressure = std::clamp(
                config_.strategy_ev_pre_cat_pressure_rebound_relief_max_recent_history_pressure,
                0.50,
                0.98
            );
            const double pre_cat_pressure_rebound_relief_max_regime_history_pressure = std::clamp(
                config_.strategy_ev_pre_cat_pressure_rebound_relief_max_regime_history_pressure,
                0.50,
                0.98
            );
            const int pre_cat_pressure_rebound_relief_max_severe_axis_count = std::clamp(
                config_.strategy_ev_pre_cat_pressure_rebound_relief_max_severe_axis_count,
                1,
                4
            );
            const int pre_cat_pressure_rebound_relief_max_activations_per_key = std::max(
                1,
                config_.strategy_ev_pre_cat_pressure_rebound_relief_max_activations_per_key
            );
            const int pre_cat_pressure_rebound_relief_activations_used =
                pre_cat_pressure_rebound_relief_activation_by_key_.count(pre_cat_no_soft_quality_relief_key) > 0
                ? pre_cat_pressure_rebound_relief_activation_by_key_.at(pre_cat_no_soft_quality_relief_key)
                : 0;
            const bool pre_cat_pressure_rebound_relief_ready =
                config_.enable_strategy_ev_pre_cat_pressure_rebound_relief &&
                !has_recovery_evidence_for_soften_effective &&
                pre_cat_pressure_rebound_relief_activations_used <
                    pre_cat_pressure_rebound_relief_max_activations_per_key &&
                pre_cat_relief_trade_scope <= pre_cat_pressure_rebound_relief_max_trades &&
                recovery_quality_context_for_soften &&
                non_hostile_regime &&
                !severe_history_sync_pre_cat &&
                signal.strength >= pre_cat_pressure_rebound_relief_min_strength &&
                signal.expected_value >= pre_cat_pressure_rebound_relief_min_expected_value &&
                signal.liquidity_score >= pre_cat_pressure_rebound_relief_min_liquidity &&
                (raw_reward_risk_ratio <= 0.0 ||
                 raw_reward_risk_ratio >= pre_cat_pressure_rebound_relief_min_reward_risk) &&
                full_history_pressure >= pre_cat_pressure_rebound_relief_min_full_history_pressure &&
                recent_history_pressure <= pre_cat_pressure_rebound_relief_max_recent_history_pressure &&
                regime_history_pressure <= pre_cat_pressure_rebound_relief_max_regime_history_pressure &&
                severe_axis_count <= pre_cat_pressure_rebound_relief_max_severe_axis_count;
            const bool pre_cat_candidate_rr_failsafe_enabled =
                config_.enable_strategy_ev_pre_cat_candidate_rr_failsafe;
            const int pre_cat_candidate_rr_failsafe_max_trades = std::max(
                8,
                config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_strategy_trades
            );
            const double pre_cat_candidate_rr_failsafe_min_strength = std::clamp(
                config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_strength,
                0.60,
                0.92
            );
            const double pre_cat_candidate_rr_failsafe_min_expected_value = std::clamp(
                config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_expected_value,
                0.00040,
                0.00300
            );
            const double pre_cat_candidate_rr_failsafe_min_liquidity = std::clamp(
                config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_liquidity,
                45.0,
                95.0
            );
            const double pre_cat_candidate_rr_failsafe_min_reward_risk = std::clamp(
                config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_reward_risk,
                1.05,
                2.60
            );
            const double pre_cat_candidate_rr_failsafe_max_full_history_pressure = std::clamp(
                config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_full_history_pressure,
                0.70,
                1.00
            );
            const int pre_cat_candidate_rr_failsafe_max_severe_axis_count = std::clamp(
                config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_severe_axis_count,
                1,
                4
            );
            const int pre_cat_candidate_rr_failsafe_max_activations_per_key = std::max(
                1,
                config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_activations_per_key
            );
            const int pre_cat_candidate_rr_failsafe_activations_used =
                pre_cat_candidate_rr_failsafe_activation_by_key_.count(pre_cat_no_soft_quality_relief_key) > 0
                ? pre_cat_candidate_rr_failsafe_activation_by_key_.at(pre_cat_no_soft_quality_relief_key)
                : 0;
            const bool pre_cat_candidate_rr_failsafe_ready =
                pre_cat_candidate_rr_failsafe_enabled &&
                !config_.enable_strategy_ev_pre_cat_soften_non_severe &&
                pre_cat_soften_candidate_rr_ok &&
                pre_cat_candidate_rr_failsafe_activations_used <
                    pre_cat_candidate_rr_failsafe_max_activations_per_key &&
                pre_cat_relief_trade_scope <= pre_cat_candidate_rr_failsafe_max_trades &&
                signal.strength >= pre_cat_candidate_rr_failsafe_min_strength &&
                signal.expected_value >= pre_cat_candidate_rr_failsafe_min_expected_value &&
                signal.liquidity_score >= pre_cat_candidate_rr_failsafe_min_liquidity &&
                (raw_reward_risk_ratio <= 0.0 ||
                 raw_reward_risk_ratio >= pre_cat_candidate_rr_failsafe_min_reward_risk) &&
                full_history_pressure <= pre_cat_candidate_rr_failsafe_max_full_history_pressure &&
                severe_axis_count <= pre_cat_candidate_rr_failsafe_max_severe_axis_count;
            const bool pre_cat_soften_ready =
                pre_cat_soften_non_severe_ready ||
                pre_cat_candidate_rr_failsafe_ready ||
                pre_cat_pressure_rebound_relief_ready;
            const double pre_cat_sync_override_max_full_history_pressure = std::min(
                contextual_severe_max_pressure,
                pre_cat_unsynced_override_max_full_history_pressure
            );
            const int pre_cat_sync_override_max_severe_axis_count = std::max(
                1,
                std::min(
                    contextual_severe_max_axis_count,
                    pre_cat_unsynced_override_max_severe_axis_count
                )
            );
            const bool pre_cat_sync_override_quality_ready =
                recovery_quality_context_for_soften &&
                has_recovery_evidence_for_soften &&
                non_hostile_regime;
            const bool pre_cat_sync_override_ready =
                severe_history_sync_pre_cat &&
                pre_cat_sync_override_quality_ready &&
                signal.strength >= pre_cat_soften_strength_floor &&
                signal.expected_value >= pre_cat_soften_expected_value_floor &&
                signal.liquidity_score >= pre_cat_soften_liquidity_floor &&
                (raw_reward_risk_ratio <= 0.0 ||
                 raw_reward_risk_ratio >= 1.18) &&
                full_history_pressure <= pre_cat_sync_override_max_full_history_pressure &&
                severe_axis_count <= pre_cat_sync_override_max_severe_axis_count;
            if (stat.trades >= std::max(12, config_.min_strategy_trades_for_ev / 4)) {
                const double wr = stat.winRate();
                const double pf = stat.profitFactor();
                const double exp_krw = stat.expectancy();
                const bool pre_catastrophic =
                    exp_krw < (config_.min_strategy_expectancy_krw - 5.0) &&
                    pf < std::max(0.88, config_.min_strategy_profit_factor - 0.18) &&
                    wr < 0.60;
                if (pre_catastrophic) {
                    if (pre_cat_negative_history_quarantine_active) {
                        strategy_ev_hard_block = true;
                    } else if (contextual_recovery_ready) {
                        required_signal_strength = std::max(
                            required_signal_strength,
                            alpha_head_fallback_candidate ? 0.58 : 0.62
                        );
                        regime_rr_add += (0.06 * history_penalty_scale);
                        regime_edge_add += (0.00010 * history_penalty_scale);
                    } else if (pre_cat_sync_override_ready) {
                        required_signal_strength = std::max(
                            required_signal_strength,
                            alpha_head_fallback_candidate ? 0.66 : 0.70
                        );
                        regime_rr_add += (0.11 * history_penalty_scale);
                        regime_edge_add += (0.00016 * history_penalty_scale);
                    } else if (severe_history_sync_pre_cat) {
                        strategy_ev_hard_block = true;
                    } else if (unsynced_soft_override_ready ||
                               pre_cat_soften_ready ||
                               pre_cat_no_soft_quality_relief_ready) {
                        const bool used_no_soft_quality_relief =
                            pre_cat_no_soft_quality_relief_ready &&
                            !unsynced_soft_override_ready &&
                            !pre_cat_soften_ready;
                        const bool used_candidate_rr_failsafe =
                            pre_cat_candidate_rr_failsafe_ready &&
                            !unsynced_soft_override_ready &&
                            !pre_cat_soften_non_severe_ready &&
                            !pre_cat_no_soft_quality_relief_ready &&
                            !pre_cat_pressure_rebound_relief_ready;
                        const bool used_pressure_rebound_relief =
                            pre_cat_pressure_rebound_relief_ready &&
                            !unsynced_soft_override_ready &&
                            !pre_cat_soften_non_severe_ready &&
                            !pre_cat_candidate_rr_failsafe_ready &&
                            !pre_cat_no_soft_quality_relief_ready;
                        const bool used_recovery_evidence_bridge =
                            pre_cat_recovery_bridge_ready &&
                            !unsynced_soft_override_ready &&
                            !used_no_soft_quality_relief &&
                            !used_candidate_rr_failsafe &&
                            !used_pressure_rebound_relief;
                        if (used_no_soft_quality_relief) {
                            pre_cat_no_soft_quality_relief_activation_by_key_[pre_cat_no_soft_quality_relief_key]++;
                            required_signal_strength = std::max(
                                required_signal_strength,
                                alpha_head_fallback_candidate ? 0.63 : 0.67
                            );
                            regime_rr_add += (0.09 * history_penalty_scale);
                            regime_edge_add += (0.00014 * history_penalty_scale);
                        } else if (used_candidate_rr_failsafe) {
                            pre_cat_candidate_rr_failsafe_activation_by_key_[pre_cat_no_soft_quality_relief_key]++;
                            required_signal_strength = std::max(
                                required_signal_strength,
                                alpha_head_fallback_candidate ? 0.65 : 0.69
                            );
                            regime_rr_add += (0.10 * history_penalty_scale);
                            regime_edge_add += (0.00015 * history_penalty_scale);
                        } else if (used_pressure_rebound_relief) {
                            pre_cat_pressure_rebound_relief_activation_by_key_[pre_cat_no_soft_quality_relief_key]++;
                            required_signal_strength = std::max(
                                required_signal_strength,
                                alpha_head_fallback_candidate ? 0.66 : 0.70
                            );
                            regime_rr_add += (0.12 * history_penalty_scale);
                            regime_edge_add += (0.00018 * history_penalty_scale);
                        } else {
                            required_signal_strength = std::max(
                                required_signal_strength,
                                alpha_head_fallback_candidate ? 0.60 : 0.64
                            );
                            regime_rr_add += (0.07 * history_penalty_scale);
                            regime_edge_add += (0.00012 * history_penalty_scale);
                        }
                        if (used_recovery_evidence_bridge) {
                            pre_cat_recovery_bridge_activation_by_key_[pre_cat_recovery_bridge_key]++;
                        }
                    } else {
                        const bool pre_cat_negative_history_quarantine_set_ready =
                            config_.enable_strategy_ev_pre_cat_negative_history_quarantine &&
                            full_history_pressure >=
                                pre_cat_negative_history_quarantine_min_full_history_pressure &&
                            stat_pf <= pre_cat_negative_history_quarantine_max_history_pf &&
                            stat_exp_krw <=
                                pre_cat_negative_history_quarantine_max_history_expectancy_krw;
                        if (pre_cat_negative_history_quarantine_set_ready) {
                            pre_cat_negative_history_quarantine_hold_by_key_[pre_cat_recovery_bridge_key] =
                                pre_cat_negative_history_quarantine_hold_steps;
                        }
                        strategy_ev_hard_block = true;
                    }
                }
            }
            if (stat.trades >= config_.min_strategy_trades_for_ev) {
                const double exp_krw = stat.expectancy();
                const double pf = stat.profitFactor();
                if (exp_krw < config_.min_strategy_expectancy_krw ||
                    pf < config_.min_strategy_profit_factor) {
                    required_signal_strength += (0.04 * history_penalty_scale);
                    regime_rr_add += (0.12 * history_penalty_scale);
                    regime_edge_add += (0.00025 * history_penalty_scale);
                    if (exp_krw < (config_.min_strategy_expectancy_krw - 15.0) &&
                        pf < std::max(0.75, config_.min_strategy_profit_factor - 0.20)) {
                        if (alpha_head_fallback_candidate) {
                            const bool catastrophic_history =
                                stat.trades >= std::max(18, config_.min_strategy_trades_for_ev / 2) &&
                                (exp_krw < (config_.min_strategy_expectancy_krw - 28.0) ||
                                 pf < std::max(0.65, config_.min_strategy_profit_factor - 0.45));
                            if (catastrophic_history) {
                                strategy_ev_hard_block = true;
                            } else {
                                required_signal_strength = std::max(required_signal_strength, 0.62);
                                regime_rr_add += 0.08;
                                regime_edge_add += 0.00012;
                            }
                        } else {
                            if (severe_history_sync) {
                                LOG_INFO("{} skipped by severe strategy EV gate [{}]: exp {:.2f}, PF {:.2f}",
                                         signal.market, signal.strategy_name, exp_krw, pf);
                                strategy_ev_hard_block = true;
                            } else if (unsynced_soft_override_ready) {
                                required_signal_strength = std::max(required_signal_strength, 0.63);
                                regime_rr_add += (0.10 * history_penalty_scale);
                                regime_edge_add += (0.00016 * history_penalty_scale);
                            } else {
                                LOG_INFO("{} skipped by severe strategy EV gate [{}]: exp {:.2f}, PF {:.2f}",
                                         signal.market, signal.strategy_name, exp_krw, pf);
                                strategy_ev_hard_block = true;
                            }
                        }
                    }
                }
            }
        }
        if (strategy_ev_hard_block) {
            LOG_INFO(
                "{} skipped by catastrophic strategy EV gate [{}]: trades {}, win_rate {:.2f}, PF {:.2f}",
                signal.market,
                signal.strategy_name,
                signal.strategy_trade_count,
                signal.strategy_win_rate,
                signal.strategy_profit_factor
            );
            filtered_out++;
            continue;
        }
        if (small_seed_mode &&
            signal.strategy_trade_count >= 10 &&
            signal.strategy_win_rate < 0.55) {
            LOG_INFO("{} skipped by small-seed quality gate: win_rate {:.2f} < 0.55 ({} trades)",
                     signal.market, signal.strategy_win_rate, signal.strategy_trade_count);
            filtered_out++;
            continue;
        }

        normalizeSignalStopLossByRegime(signal, signal.market_regime);

        if (!rebalanceSignalRiskReward(signal, config_)) {
            LOG_WARN("{} skipped: invalid price levels for RR normalization", signal.market);
            filtered_out++;
            continue;
        }

        if (config_.avoid_high_volatility &&
            signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            LOG_INFO("{} skipped by regime gate: {}", signal.market, regimeToString(signal.market_regime));
            filtered_out++;
            continue;
        }
        if (config_.avoid_trending_down &&
            signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
            LOG_INFO("{} skipped by regime gate: {}", signal.market, regimeToString(signal.market_regime));
            filtered_out++;
            continue;
        }

        const double entry_price = signal.entry_price;
        const double take_profit_price = (signal.take_profit_2 > 0.0) ? signal.take_profit_2 : signal.take_profit_1;
        const double stop_loss_price = signal.stop_loss;

        if (entry_price <= 0.0 || take_profit_price <= 0.0 || stop_loss_price <= 0.0 ||
            take_profit_price <= entry_price || stop_loss_price >= entry_price) {
            LOG_WARN("{} skipped: invalid TP/SL for entry quality gate (entry {:.0f}, tp {:.0f}, sl {:.0f})",
                     signal.market, entry_price, take_profit_price, stop_loss_price);
            filtered_out++;
            continue;
        }

        const double gross_reward_pct = (take_profit_price - entry_price) / entry_price;
        const double gross_risk_pct = (entry_price - stop_loss_price) / entry_price;
        const double reward_risk_ratio = (gross_risk_pct > 1e-8) ? (gross_reward_pct / gross_risk_pct) : 0.0;

        const double round_trip_cost_pct = computeEffectiveRoundTripCostPct(signal, config_);
        const double calibrated_expected_edge_pct = computeCalibratedExpectedEdgePct(signal, config_);

        double adaptive_rr_gate = min_reward_risk_gate;
        double adaptive_edge_gate = min_expected_edge_gate;
        if (small_seed_mode) {
            const bool favorable_small_seed_signal =
                signal.market_regime == analytics::MarketRegime::TRENDING_UP &&
                signal.strength >= 0.72 &&
                signal.liquidity_score >= 60.0;
            if (signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
                adaptive_rr_gate = std::max(adaptive_rr_gate, config_.min_reward_risk + 0.22);
                adaptive_edge_gate = std::max(adaptive_edge_gate, config_.min_expected_edge_pct * 1.45);
            } else if (favorable_small_seed_signal) {
                adaptive_rr_gate = std::max(config_.min_reward_risk + 0.08, adaptive_rr_gate - 0.08);
                adaptive_edge_gate = std::max(config_.min_expected_edge_pct * 1.10, adaptive_edge_gate * 0.88);
            }
        }
        if (auto quality_it = strategy_edge.find(signal.strategy_name); quality_it != strategy_edge.end()) {
            const auto& stat = quality_it->second;
            if (stat.trades >= 8) {
                const double wr = stat.winRate();
                const double pf = stat.profitFactor();
                const double exp_krw = stat.expectancy();

                if (wr < 0.42) {
                    adaptive_rr_gate += 0.35;
                    adaptive_edge_gate += 0.0006;
                } else if (wr < 0.48) {
                    adaptive_rr_gate += 0.20;
                    adaptive_edge_gate += 0.0003;
                }

                if (pf < 0.90) {
                    adaptive_rr_gate += 0.25;
                    adaptive_edge_gate += 0.0005;
                } else if (pf < 1.00) {
                    adaptive_rr_gate += 0.12;
                    adaptive_edge_gate += 0.0002;
                }

                if (exp_krw < 0.0) {
                    adaptive_rr_gate += std::clamp((-exp_krw) / 1000.0, 0.0, 0.25);
                    adaptive_edge_gate += std::clamp((-exp_krw) / 80000.0, 0.0, 0.0004);
                }
            }
        }
        adaptive_rr_gate += regime_rr_add;
        adaptive_edge_gate += regime_edge_add;
        if (alpha_head_fallback_candidate) {
            const bool hostile_or_thin =
                signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                signal.market_regime == analytics::MarketRegime::TRENDING_DOWN ||
                (signal.liquidity_score > 0.0 && signal.liquidity_score < 50.0);
            double rr_relax = hostile_or_thin ? 0.06 : 0.16;
            double edge_relax = hostile_or_thin ? 0.00035 : 0.00100;
            if (!alpha_head_relief_eligible) {
                rr_relax *= 0.35;
                edge_relax *= 0.30;
            }
            adaptive_rr_gate = std::max(1.03, adaptive_rr_gate - rr_relax);
            adaptive_edge_gate = std::max(
                std::max(0.00035, config_.min_expected_edge_pct * 0.35),
                adaptive_edge_gate - edge_relax
            );
        }
        adaptive_rr_gate = std::clamp(adaptive_rr_gate, config_.min_reward_risk, config_.min_reward_risk + 0.45);
        adaptive_edge_gate = std::clamp(adaptive_edge_gate, config_.min_expected_edge_pct, config_.min_expected_edge_pct + 0.0012);
        if (scans_without_new_entry_ >= 6 &&
            (signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
             signal.market_regime == analytics::MarketRegime::RANGING) &&
            signal.strength >= 0.72 &&
            signal.liquidity_score >= 60.0) {
            // Entry-frequency recovery path for favorable regimes.
            adaptive_rr_gate = std::max(1.05, adaptive_rr_gate - 0.10);
            adaptive_edge_gate = std::max(
                config_.min_expected_edge_pct * 0.80,
                adaptive_edge_gate - 0.00018
            );
        }

        bool adaptive_relief_applied = false;
        if (config_.enable_entry_quality_adaptive_relief) {
            const bool adaptive_only_tightened =
                (adaptive_rr_gate > (min_reward_risk_gate + 1e-12)) ||
                (adaptive_edge_gate > (min_expected_edge_gate + 1e-12));
            const bool high_stress_regime =
                signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
            const bool relief_regime_ok =
                !config_.entry_quality_adaptive_relief_block_high_stress_regime || !high_stress_regime;
            const bool has_min_history =
                signal.strategy_trade_count >= std::max(0, config_.entry_quality_adaptive_relief_min_strategy_trades);
            const bool history_win_ok =
                signal.strategy_win_rate >= config_.entry_quality_adaptive_relief_min_strategy_win_rate;
            const bool history_pf_ok =
                signal.strategy_profit_factor <= 0.0 ||
                signal.strategy_profit_factor >= config_.entry_quality_adaptive_relief_min_strategy_profit_factor;
            const bool quality_ok =
                signal.strength >= config_.entry_quality_adaptive_relief_min_signal_strength &&
                signal.expected_value >= config_.entry_quality_adaptive_relief_min_expected_value &&
                signal.liquidity_score >= config_.entry_quality_adaptive_relief_min_liquidity_score &&
                has_min_history &&
                history_win_ok &&
                history_pf_ok &&
                relief_regime_ok;
            if (adaptive_only_tightened && quality_ok) {
                const double rr_relax = std::clamp(
                    config_.entry_quality_adaptive_relief_rr_max_gap,
                    0.0,
                    0.25
                );
                const double edge_relax = std::clamp(
                    config_.entry_quality_adaptive_relief_edge_max_gap,
                    0.0,
                    0.0010
                );
                const double relieved_rr_gate = std::max(min_reward_risk_gate, adaptive_rr_gate - rr_relax);
                const double relieved_edge_gate = std::max(min_expected_edge_gate, adaptive_edge_gate - edge_relax);
                if (reward_risk_ratio >= relieved_rr_gate &&
                    calibrated_expected_edge_pct >= relieved_edge_gate) {
                    adaptive_rr_gate = relieved_rr_gate;
                    adaptive_edge_gate = relieved_edge_gate;
                    adaptive_relief_applied = true;
                }
            }
        }

        EntryQualityHeadSnapshot entry_quality_head;
        entry_quality_head.reward_risk_ratio = reward_risk_ratio;
        entry_quality_head.calibrated_expected_edge_pct = calibrated_expected_edge_pct;
        entry_quality_head.reward_risk_gate = adaptive_rr_gate;
        entry_quality_head.expected_edge_gate = adaptive_edge_gate;
        entry_quality_head.pass =
            reward_risk_ratio >= adaptive_rr_gate &&
            calibrated_expected_edge_pct >= adaptive_edge_gate;

        const SecondStageConfirmationSnapshot second_stage_eval =
            evaluateSecondStageEntryConfirmation(
                signal,
                config_,
                reward_risk_ratio,
                adaptive_rr_gate,
                calibrated_expected_edge_pct,
                adaptive_edge_gate
            );
        const bool second_stage_ok = second_stage_eval.pass;
        const TwoHeadEntryAggregationSnapshot two_head_eval =
            evaluateTwoHeadEntryAggregation(
                signal,
                config_,
                entry_quality_head,
                second_stage_eval
            );
        const bool entry_pipeline_pass =
            config_.enable_two_head_entry_second_stage_aggregation
            ? two_head_eval.aggregate_pass
            : (entry_quality_head.pass && second_stage_ok);

        if (!entry_pipeline_pass) {
            bool attribute_second_stage = false;
            if (!entry_quality_head.pass && second_stage_ok) {
                attribute_second_stage = false;
            } else if (entry_quality_head.pass && !second_stage_ok) {
                attribute_second_stage = true;
            } else if (!entry_quality_head.pass && !second_stage_ok) {
                attribute_second_stage = two_head_eval.second_stage_deficit >= two_head_eval.entry_deficit;
            }

            if (attribute_second_stage && !second_stage_ok) {
                if (second_stage_eval.failure == SecondStageConfirmationFailureKind::HostileSafetyAdders) {
                    LOG_INFO("{} skipped by second-stage confirmation ({}:{}): rr {:.2f}, edge {:.3f}%",
                             signal.market,
                             secondStageFailureReasonCode(second_stage_eval.failure),
                             secondStageSafetySourceReasonCode(second_stage_eval.safety_source),
                             reward_risk_ratio,
                             calibrated_expected_edge_pct * 100.0);
                } else {
                    LOG_INFO("{} skipped by second-stage confirmation ({}): rr {:.2f}, edge {:.3f}%",
                             signal.market,
                             secondStageFailureReasonCode(second_stage_eval.failure),
                             reward_risk_ratio,
                             calibrated_expected_edge_pct * 100.0);
                }
            } else if (!entry_quality_head.pass) {
                const bool rr_failed = reward_risk_ratio < adaptive_rr_gate;
                const bool edge_failed = calibrated_expected_edge_pct < adaptive_edge_gate;
                if (rr_failed && edge_failed) {
                    LOG_INFO("{} skipped by entry-quality gate: rr {:.2f}<{:.2f}, edge {:.3f}%<{:.3f}% (gross {:.3f}%, cost {:.3f}%)",
                             signal.market,
                             reward_risk_ratio,
                             adaptive_rr_gate,
                             calibrated_expected_edge_pct * 100.0,
                             adaptive_edge_gate * 100.0,
                             gross_reward_pct * 100.0,
                             round_trip_cost_pct * 100.0);
                } else if (rr_failed) {
                    LOG_INFO("{} skipped by RR gate: {:.2f} < {:.2f}",
                             signal.market, reward_risk_ratio, adaptive_rr_gate);
                } else {
                    LOG_INFO("{} skipped by edge gate: {:.3f}% < {:.3f}% (gross {:.3f}%, cost {:.3f}%)",
                             signal.market,
                             calibrated_expected_edge_pct * 100.0,
                             adaptive_edge_gate * 100.0,
                             gross_reward_pct * 100.0,
                             round_trip_cost_pct * 100.0);
                }
            } else {
                LOG_INFO("{} skipped by two-head aggregation floor: entry_score {:.3f}, second_score {:.3f}, agg {:.3f}",
                         signal.market,
                         two_head_eval.entry_head_score,
                         two_head_eval.second_stage_head_score,
                         two_head_eval.aggregate_score);
                if (two_head_eval.rr_margin_near_miss_relief_blocked) {
                    LOG_INFO("{} near-miss relief blocked [{}]: override_disallowed={}, entry_floor={}, second_floor={}, aggregate_score={}, head_floor_applied={}, head_floor_value={:.4f}, floor_relax={}, adaptive_floor_relax={}, adaptive_relax_strength={:.4f}, surplus_comp={}, surplus_second={}, surplus_agg={}, entry_surplus={:.4f}, second_deficit={:.4f}, aggregate_deficit={:.4f}, edge_score={:.4f}, surplus_bonus={:.4f}",
                             signal.market,
                             signal.strategy_name,
                             two_head_eval.rr_margin_near_miss_relief_blocked_override_disallowed,
                             two_head_eval.rr_margin_near_miss_relief_blocked_entry_floor,
                             two_head_eval.rr_margin_near_miss_relief_blocked_second_stage_floor,
                             two_head_eval.rr_margin_near_miss_relief_blocked_aggregate_score,
                             two_head_eval.rr_margin_near_miss_head_score_floor_applied,
                             two_head_eval.rr_margin_near_miss_head_score_floor_value,
                             two_head_eval.rr_margin_near_miss_floor_relax_applied,
                             two_head_eval.rr_margin_near_miss_adaptive_floor_relax_applied,
                             two_head_eval.rr_margin_near_miss_adaptive_floor_relax_strength,
                             two_head_eval.rr_margin_near_miss_surplus_compensation_applied,
                             two_head_eval.rr_margin_near_miss_surplus_second_stage_compensated,
                             two_head_eval.rr_margin_near_miss_surplus_aggregate_compensated,
                             two_head_eval.entry_surplus,
                             two_head_eval.second_stage_deficit,
                             two_head_eval.aggregate_deficit,
                             second_stage_eval.baseline_edge_score,
                             two_head_eval.rr_margin_near_miss_surplus_bonus);
                }
            }
            filtered_out++;
            continue;
        }
        if (config_.enable_two_head_entry_second_stage_aggregation && two_head_eval.override_applied) {
            LOG_INFO("{} two-head aggregation override accepted [{}]: entry_score {:.3f}, second_score {:.3f}, agg {:.3f}",
                     signal.market,
                     signal.strategy_name,
                     two_head_eval.entry_head_score,
                     two_head_eval.second_stage_head_score,
                     two_head_eval.aggregate_score);
            if (two_head_eval.rr_margin_near_miss_relief_applied) {
                LOG_INFO("{} two-head near-miss relief [{}]: rr_gap {:.4f}, min_rr_margin {:.4f}, head_floor_applied={}, head_floor_value={:.4f}, floor_relax={}, adaptive_floor_relax={}, adaptive_relax_strength={:.4f}, surplus_comp={}, surplus_second={}, surplus_agg={}, surplus_bonus {:.4f}, eff_second_floor {:.3f}, eff_agg_floor {:.3f}",
                         signal.market,
                         signal.strategy_name,
                         second_stage_eval.rr_margin_gap,
                         second_stage_eval.min_rr_margin,
                         two_head_eval.rr_margin_near_miss_head_score_floor_applied,
                         two_head_eval.rr_margin_near_miss_head_score_floor_value,
                         two_head_eval.rr_margin_near_miss_floor_relax_applied,
                         two_head_eval.rr_margin_near_miss_adaptive_floor_relax_applied,
                         two_head_eval.rr_margin_near_miss_adaptive_floor_relax_strength,
                         two_head_eval.rr_margin_near_miss_surplus_compensation_applied,
                         two_head_eval.rr_margin_near_miss_surplus_second_stage_compensated,
                         two_head_eval.rr_margin_near_miss_surplus_aggregate_compensated,
                         two_head_eval.rr_margin_near_miss_surplus_bonus,
                         two_head_eval.effective_min_second_stage_score,
                         two_head_eval.effective_min_aggregate_score);
            }
        }

        signal.position_size *= current_scale;
        if (adaptive_relief_applied) {
            const double relief_scale = std::clamp(
                config_.entry_quality_adaptive_relief_position_scale,
                0.20,
                1.0
            );
            LOG_INFO("{} entry-quality adaptive relief applied [{}]: rr {:.2f} gate {:.2f}, edge {:.3f}% gate {:.3f}%, size_scale {:.2f}x",
                     signal.market,
                     signal.strategy_name,
                     reward_risk_ratio,
                     adaptive_rr_gate,
                     calibrated_expected_edge_pct * 100.0,
                     adaptive_edge_gate * 100.0,
                     relief_scale);
            signal.position_size *= relief_scale;
        }
        const double strength_multiplier = std::clamp(0.5 + signal.strength, 0.75, 1.5);
        signal.position_size *= strength_multiplier;

        LOG_INFO("Signal candidate - {} [{}] (strength {:.3f}, scale {:.2f}x, strength_mul {:.2f}x => pos {:.4f})",
                 signal.market, signal.strategy_name, signal.strength, current_scale,
                 strength_multiplier, signal.position_size);

        auto risk_metrics = risk_manager_->getRiskMetrics();
        const double risk_budget_krw = risk_metrics.total_capital * config_.risk_per_trade_pct;
        const double min_required_krw = config_.min_order_krw;

        if (risk_metrics.available_capital < min_required_krw) {
            LOG_WARN("{} skipped - insufficient available capital (have {:.0f}, need {:.0f})",
                     signal.market, risk_metrics.available_capital, min_required_krw);
            continue;
        }

        double min_position_size = min_required_krw / risk_metrics.available_capital;
        if (signal.position_size < min_position_size) {
            LOG_INFO("{} raised position_size {:.4f} -> {:.4f} (min order {:.0f})",
                     signal.market, signal.position_size, min_position_size, min_required_krw);
            signal.position_size = min_position_size;
        }

        if (signal.position_size > 1.0) {
            LOG_WARN("{} position_size clamped: {:.4f} -> 1.0", signal.market, signal.position_size);
            signal.position_size = 1.0;
        }

        double risk_pct = 0.0;
        if (signal.entry_price > 0.0 && signal.stop_loss > 0.0) {
            risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
        }

        if (risk_pct > 0.0 && risk_metrics.available_capital > 0.0 && risk_budget_krw > 0.0) {
            const double max_invest_amount = risk_budget_krw / risk_pct;
            const double max_position_size = max_invest_amount / risk_metrics.available_capital;

            if (signal.position_size > max_position_size) {
                LOG_INFO("{} risk-based size clamp: {:.4f} -> {:.4f} (budget {:.0f})",
                         signal.market, signal.position_size, max_position_size, risk_budget_krw);
                signal.position_size = max_position_size;
            }
        }

        auto strategy_ptr = strategy_manager_ ? strategy_manager_->getStrategy(signal.strategy_name) : nullptr;
        const bool is_grid_strategy = (signal.strategy_name == "Grid Trading Strategy");

        if (is_grid_strategy && strategy_ptr) {
            auto grid_metrics = risk_manager_->getRiskMetrics();
            const double allocated_capital = grid_metrics.available_capital * signal.position_size;

            if (!risk_manager_->reserveGridCapital(signal.market, allocated_capital, signal.strategy_name)) {
                LOG_WARN("{} grid order skipped (capital reservation failed)", signal.market);
                continue;
            }

            if (!strategy_ptr->onSignalAccepted(signal, allocated_capital)) {
                risk_manager_->releaseGridCapital(signal.market);
                LOG_WARN("{} grid order skipped (strategy acceptance failed)", signal.market);
                continue;
            }

            LOG_INFO("{} grid order accepted (allocated capital {:.0f})", signal.market, allocated_capital);
            executed++;
            continue;
        }

        if (!risk_manager_->canEnterPosition(
            signal.market,
            signal.entry_price,
            signal.position_size,
            signal.strategy_name
        )) {
            LOG_WARN("{} buy skipped (risk check rejected)", signal.market);
            continue;
        }

        signal.entry_amount = risk_metrics.available_capital * signal.position_size;
        if (signal.entry_amount < min_required_krw) {
            signal.entry_amount = min_required_krw;
        }

        if (executeBuyOrder(signal.market, signal)) {
            risk_manager_->setPositionSignalInfo(
                signal.market,
                signal.signal_filter,
                signal.strength,
                signal.market_regime,
                signal.liquidity_score,
                signal.volatility,
                (std::isfinite(calibrated_expected_edge_pct)
                    ? calibrated_expected_edge_pct
                    : (signal.expected_value != 0.0 ? signal.expected_value : 0.0)),
                reward_risk_ratio,
                signal.entry_archetype
            );
            if (!is_grid_strategy && strategy_ptr) {
                const double allocated_capital =
                    signal.entry_amount > 0.0
                        ? signal.entry_amount
                        : (risk_metrics.available_capital * signal.position_size);
                if (!strategy_ptr->onSignalAccepted(signal, allocated_capital)) {
                    LOG_WARN("{} strategy accepted order but state registration was skipped: {}",
                             signal.market, signal.strategy_name);
                }
            }
            executed++;
            executed_buys_this_scan++;

            const size_t previous_total = total_potential;
            total_potential = getTotalPotentialPositions();
            LOG_INFO("Position counter updated: {} -> {} (max {})",
                     previous_total, total_potential, config_.max_positions);
        }
    }

    LOG_INFO("Signal execution done: executed {}, filtered {}", executed, filtered_out);
    if (executed_buys_this_scan > 0) {
        scans_without_new_entry_ = 0;
    } else {
        nlohmann::json payload;
        payload["reason"] = "all_candidates_rejected_by_execution_gates";
        payload["execution_candidates"] = execution_candidates.size();
        payload["filtered_out"] = filtered_out;
        payload["dominant_regime"] = regimeToString(dominant_regime);
        payload["small_seed_mode"] = small_seed_mode;
        payload["hostility_ewma"] = market_hostility_ewma_;
        payload["hostility_now"] = market_hostility_score;
        appendJournalEvent(
            core::JournalEventType::NO_TRADE,
            "MULTI",
            "execution_guard",
            payload
        );
        scans_without_new_entry_ = std::min(scans_without_new_entry_ + 1, 100);
    }
    pending_signals_.clear();
}
bool TradingEngine::executeBuyOrder(
    const std::string& market,
    const strategy::Signal& signal
) {
    LOG_INFO("?È•îÎÇÖ??????????ÍøîÍ∫Ç????? {} (???????øÍºç???????´Îäâ??Á≠??ÁØÄÍæ™Î†≠?? {:.2f})", market, signal.strength);
    
    try {
        // 1. [????? ?????Orderbook) ???????????????È•îÎÇÖ????????????∫Ïñò???????????????´Î???????Ô¶âÏï∏???Á≠????
        //    ???????????Íæ©Î£ÜÔß?ù∞Ï≠?????????´ÏëÑ?????????õ¬Ä(Ticker)???????´Î??? ??????Íæ©Î£ÜÔß?ù∞Ï≠???? ???????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ?????????????????'????∫Ïñò??????????Î∂∫Î™≠????1???'?????????¥Ïò®?????
        auto orderbook = http_client_->getOrderBook(market);
        if (orderbook.empty()) {
            LOG_ERROR("??? ????®ÏÄ´Î????????????§Ïä£?? {}", market);
            return false;
        }
        
        // [??????¨Í≥£Î≤ÄÂ´?????Íæ§Îô¥??? API ??????Ï¢äÌã£?????????????????????Ü‚ïã?????????éÏ≤é?Â´???????∫Ïñò???????????Ô¶´ÎöÆ??∑Î≥†ÍΩ??
        nlohmann::json units;
        if (orderbook.is_array() && !orderbook.empty()) {
            units = orderbook[0]["orderbook_units"];
        } else if (orderbook.contains("orderbook_units")) {
            units = orderbook["orderbook_units"];
        } else {
            LOG_ERROR("{} - ??? ?????????orderbook_units)???È•îÎÇÖ????????????????ÍπÖÏ¶Ω?????????ÅÏ°Ñ: {}", market, orderbook.dump());
            return false;
        }

        double best_ask_price = calculateOptimalBuyPrice(market, signal.entry_price, orderbook); // ????∫Ïñò??????????Î∂∫Î™≠????1???
        
        // 2. ??????????¥ÏèÇÍµ????????????????????????????
        auto metrics = risk_manager_->getRiskMetrics();
        
        // Keep execution minimum aligned with config and strategy sizing.
        const double MIN_ORDER_BUFFER = config_.min_order_krw;

        double invest_amount = 0.0;
        double safe_position_size = 0.0;
        
        // [NEW] executeSignals?????????????????????ÄÎ∂æÍµù??????????????¥ÏèÇÍµ????????????
        if (signal.entry_amount > 0.0) {
            invest_amount = signal.entry_amount;
            // ???? position_size ??????Ë¢Å‚ë£????(??????Ï¢äÌã£?????Ê∫êÎÇÜÏ°???????∫Ïñò????????? ??Ô¶âÏï∏???Á≠??????∫Ïñò????È∂?Ö∫?????•¬Ä?£Í±ñ???Ô¶??
            if (metrics.available_capital > 0) {
                safe_position_size = invest_amount / metrics.available_capital;
            }
        } else {
            // Fallback (???????????????‰∫?ªãÍº?∂≠???
            safe_position_size = signal.position_size;
            if (safe_position_size > 1.0) safe_position_size = 1.0;
            invest_amount = metrics.available_capital * safe_position_size;
        }

        // [????∫Ïñò????????????∞Í∂Ω?±Íµ©??????∫Ïñò???????????1] ???????´Î???????????????????∫Ïñò?????????????Â´???????ÄÎ∂æÍµù??????????(???? ????????????????????È•îÎÇÖ???????
        // ??????Ë´õÎ™Ñ???? signal.entry_amount??????????????????????´Î?????????????????????????????????éÏ≤é?Â´?Êø°„Çç???∫Î∏ç????????∞ÏîÆ?????
        // ??????Íæ©Î£ÜÔß?ù∞Ï≠?????????? ???????´ÎµÅ????????????????????????∫Ïñò?????? ???ÄÎ∂æÍµù????????????????Íæ©Î£ÜÔß?ù∞Ï≠??
        if (metrics.available_capital < invest_amount) {
             // ?????????????±ÏÇ©?????? ??? ??????Íæ©Î£ÜÔß?ù∞Ï≠???µæ?????©ÎÅè?????? ??????Íæ©Î£ÜÔß?ù∞Ï≠???????∑Î™ÑÍµ?????????Ë´õÎ™ÉÎß???
             // ????∫Ïñò????????????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠????????∞ÎÆõ?????????????È•îÎÇÖ????Ô¶´ÎöÆ???Ïæ?????????????Ë´õÎ™ÉÎß????????ÁππÎ®ÆÍµûÂΩõ????
             if (metrics.available_capital < MIN_ORDER_BUFFER) {
                 LOG_WARN("{} - ?????´Îäâ??????????????Îºø¬Ä???{:.0f} < {:.0f} (?È•îÎÇÖ????????????•¬Ä?´ÎüØ??)", 
                          market, metrics.available_capital, MIN_ORDER_BUFFER);
                 return false;
             }
             // ??? ???????éÏ≤é?Â´???®Î™ÑÍ≤?¶´?????∫Ïñò????????????????∞ÎÆõ?????????????Ä???®ÎöÆÎº??∞≈??? ??? ?????Œº?úÂ™õ?Í±???????????????∫Ïñò????????(Partial Entry)
             LOG_INFO("{} - ?????´Îäâ??????????????Îºø¬Ä?????®ÏÄ´Î???????????????Ôß??????®ÏÄ´Î????? {:.0f} -> {:.0f}", 
                      market, invest_amount, metrics.available_capital);
             invest_amount = metrics.available_capital;
        }
        
        LOG_INFO("{} - [??È∂?Ö∫??????? ????????•Ïã≤Í∞?Åî?????‰∫?ÇÉÏΩ???????? {:.0f} KRW (?????´Îäâ????{:.0f})", 
                     market, invest_amount, metrics.available_capital);
        
        if (invest_amount < MIN_ORDER_BUFFER) {
            LOG_WARN("{} - ?È•îÎÇÖ??????????????∞Î≠ê???????Îºø¬Ä??????‰∫?ÇÉÏΩ???????? {:.0f}, ?????Ë´õÎ™ÉÎß?? {:.0f} KRW) [??????????????]", 
                     market, invest_amount, MIN_ORDER_BUFFER);
            return false;
        }
        
        // [?????????¨Í≥£Î≤ÄÂ´?????Íæ§Îô¥??? ??????®Î∫§Î•?????position_size??RiskManager???????
        if (!risk_manager_->canEnterPosition(
            market,
            signal.entry_price,
            safe_position_size,
            signal.strategy_name
        )) {
            LOG_WARN("{} - ?????æÏªØ?????±Î†±Ô¶????Ê£∫Â†â?Î§???????????§Ïä£??(?????æÏªØ?????±Î†±Ô¶??????????", market);
            return false;
        }

        if (invest_amount > config_.max_order_krw) invest_amount = config_.max_order_krw;
        
        // ????∫Ïñò????????? ??????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠????????????????????(??????8????????≤„É´???????)
        double quantity = invest_amount / best_ask_price;

        // [NEW] ??????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠???????????????????????? ???????
        double slippage_pct = estimateOrderbookSlippagePct(
            orderbook,
            quantity,
            true,
            best_ask_price
        );

        if (slippage_pct > config_.max_slippage_pct) {
            LOG_WARN("{} ?????? ????? {:.3f}% > {:.3f}% (?È•îÎÇÖ????????È•îÎÇÖ????∞Í∑•????ÔΩãÍ∂ô????",
                     market, slippage_pct * 100.0, config_.max_slippage_pct * 100.0);
            return false;
        }

        if (config_.enable_core_plane_bridge &&
            config_.enable_core_risk_plane &&
            core_cycle_) {
            core::ExecutionRequest request;
            request.market = market;
            request.side = OrderSide::BUY;
            request.price = best_ask_price;
            request.volume = quantity;
            request.strategy_name = signal.strategy_name;
            request.stop_loss = signal.stop_loss;
            request.take_profit_1 = signal.take_profit_1;
            request.take_profit_2 = signal.take_profit_2;
            request.breakeven_trigger = signal.breakeven_trigger;
            request.trailing_start = signal.trailing_start;

            auto compliance = core_cycle_->validateEntry(request, signal);
            if (!compliance.allowed) {
                LOG_WARN("{} buy skipped by compliance gate: {}", market, compliance.reason);
                return false;
            }
        }
        
        // ???????????????®Î∫§Î•????(??????????ÄÎ∂æÍµù???????????????????∫Ïñò???????????Ô¶´ÎöÆ??∑Î≥†ÍΩ?????????∞Ïó®????????ÅÎ¢ø??????????????????Ë´õÎ™Ñ????
        // [?????? ?????????????????(sprintf ?????stringstream ????
        char buffer[64];
        // ??????? ??????8????????≤„É´???????, ??????????¨Ïëì?????????0 ???????∞Î??????????????‰∫?ªãÍº?∂≠?????????Íæ©Î£ÜÔß?ù∞Ï≠?????????????Ë¢Å‚ë£???
        std::snprintf(buffer, sizeof(buffer), "%.8f", quantity); 
        std::string vol_str(buffer);
        
        LOG_INFO("  ??????πÎ¶Ñ??≤„É´????¥Ïö∞???????Íæ®„Åç?Á≠åÏöå?Ä????é≥??? ??? {:.0f}, ??????{}, ???‰∫?ÇÉÏΩ????????{:.0f}", 
                 best_ask_price, vol_str, invest_amount);

            // 3. [???????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ??? ???????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ???????∫Ïñò??????????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠??(OrderManager ??????Íæ©Î£ÜÔß?ù∞Ï≠??Â£§Íµø????Ï≤?
        if (config_.mode == TradingMode::LIVE && !config_.dry_run && !config_.allow_live_orders) {
            LOG_WARN("{} live buy blocked: trading.allow_live_orders=false, fallback to paper simulation", market);
        }

        if (isLiveRealOrderEnabled()) {
            
            // [NEW] OrderManager??????????????áÌÖ£???????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠?????È•îÎÇÖ???????
            // Strategy Metadata ??????Íæ©Î£ÜÔß?ù∞Ï≠???
            bool submitted = order_manager_->submitOrder(
                market,
                OrderSide::BUY,
                best_ask_price,
                quantity,
                signal.strategy_name,
                signal.stop_loss,
                signal.take_profit_1,
                signal.take_profit_2,
                signal.breakeven_trigger,
                signal.trailing_start
            );

            if (submitted) {
                LOG_INFO("OrderManager ??????πÎ¶Ñ??≤„É´????¥Ïö∞?????ÍøîÍ∫Ç??????????Ë´õÎ™ÉÎß?? {}", market);
                risk_manager_->reservePendingCapital(invest_amount);  // ???È•îÎÇÖ???????????????????????
                nlohmann::json order_payload;
                order_payload["side"] = "BUY";
                order_payload["price"] = best_ask_price;
                order_payload["volume"] = quantity;
                order_payload["strategy_name"] = signal.strategy_name;
                order_payload["entry_amount"] = invest_amount;
                appendJournalEvent(
                    core::JournalEventType::ORDER_SUBMITTED,
                    market,
                    "live_buy",
                    order_payload
                );
                return true; // Async Success
            } else {
                LOG_ERROR("OrderManager submit failed");
                return false;
            }
        } 
        else {
            // Paper Trading (????∫Ïñò???????????∫Ïñú?????? - ???????????????‰∫?ªãÍº?∂≠?????? (Simulation)
            // ... (Paper Mode Logic stays roughly same or we use OrderManager for Paper too?)
            // OrderManager currently hits API. So for Paper Mode we should NOT use OrderManager 
            // unless we Mock API.
            // Current Paper Mode simulates "Fill" immediately.
            
            // [?????Œº?úÂ™õ?Í±?Á≠åÎöÆÏ±∂Â§∑??????????????????
            double dynamic_stop_loss = best_ask_price * 0.975; // ?????????? -2.5%
            try {
                auto candles_json = http_client_->getCandles(market, "60", 200);
                if (!candles_json.empty() && candles_json.is_array()) {
                    auto candles = analytics::TechnicalIndicators::jsonToCandles(candles_json);
                    if (!candles.empty()) {
                        dynamic_stop_loss = risk_manager_->calculateDynamicStopLoss(best_ask_price, candles);
                        LOG_INFO("[PAPER] ????ÊøöÎ∞∏≈¶?Î§∏Î§É???????????????õ¬Ä ??È∂?Ö∫??????? {:.0f} ({:.2f}%)", 
                                 dynamic_stop_loss, (dynamic_stop_loss - best_ask_price) / best_ask_price * 100.0);
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN("[PAPER] ????ÊøöÎ∞∏≈¶?Î§∏Î§É???????????????õ¬Ä ??È∂?Ö∫??????????????§Ïä£?? ??????????-2.5%) ???? {}", e.what());
            }

            double applied_stop_loss = dynamic_stop_loss;
            if (signal.stop_loss > 0.0 && signal.strategy_name == "Advanced Scalping") {
                if (signal.stop_loss < best_ask_price) {
                    applied_stop_loss = signal.stop_loss;
                }
            }
            
             double tp1 = signal.take_profit_1 > 0 ? signal.take_profit_1 : best_ask_price * 1.020;
             double tp2 = signal.take_profit_2 > 0 ? signal.take_profit_2 : best_ask_price * 1.030;

             risk_manager_->enterPosition(
                market,
                best_ask_price,
                quantity,
                applied_stop_loss,
                tp1,
                tp2,
                signal.strategy_name,
                signal.breakeven_trigger,
                signal.trailing_start
            );

            nlohmann::json paper_fill_payload;
            paper_fill_payload["side"] = "BUY";
            paper_fill_payload["filled_volume"] = quantity;
            paper_fill_payload["avg_price"] = best_ask_price;
            paper_fill_payload["strategy_name"] = signal.strategy_name;
            appendJournalEvent(
                core::JournalEventType::FILL_APPLIED,
                market,
                "paper_buy",
                paper_fill_payload
            );

            nlohmann::json paper_pos_payload;
            paper_pos_payload["entry_price"] = best_ask_price;
            paper_pos_payload["quantity"] = quantity;
            paper_pos_payload["strategy_name"] = signal.strategy_name;
            appendJournalEvent(
                core::JournalEventType::POSITION_OPENED,
                market,
                "paper_buy",
                paper_pos_payload
            );
            
            return true;
        }

        // Unreachable
        return false;

    } catch (const std::exception& e) {
        LOG_ERROR("executeBuyOrder ????•Ïã≤Í∞?Åî??? {}", e.what());
        return false;
    }
}


void TradingEngine::monitorPositions() {

    static int log_counter = 0;
    bool should_log = (log_counter++ % 10 == 0);

    // 1. ??????Íæ©Î£ÜÔß?ù∞Ï≠?????????§„àá??????????¨Í≥£Î≤ÄÂ´?????Íæ§Îô¥????????????∫Ïñò???????????∫Ïñú???¨Í≥£Î´ñÈáâÎ©ßÎ≤ß?øÎó™??¥â??????????´Î??????ÄÎ∂æÍµù???????≤„É´????Î∏êÏ≠ç?Ô¶âÏï∏???????
    auto positions = risk_manager_->getAllPositions();

    if (!positions.empty() && risk_manager_->isDrawdownExceeded()) {
        LOG_ERROR("Max drawdown exceeded. Forcing protective exits for all open positions.");
        for (const auto& pos : positions) {
            const double price = getCurrentPrice(pos.market);
            if (price <= 0.0) {
                LOG_WARN("{} protective exit skipped: invalid current price", pos.market);
                continue;
            }
            executeSellOrder(pos.market, pos, "max_drawdown", price);
        }
        return;
    }
    
    // 2. ??????®Î∫§Î•???? ??????¨Í≥£Î≤ÄÂ´?????Íæ§Îô¥???????????ÍπÖÎºÇ?????????????Ô¶???Ë™ò„àë?Ë¢Å‚ë•????????Íæ©Î£ÜÔß?ù∞Ï≠??????∫Ïñò??????????????®ÎöÆÎº??∞Î????(Batch ??????????????????Í≤∏Îµõ?????????????≤ÎÄ???Á≠?
    std::set<std::string> market_set;
    for (const auto& pos : positions) {
        market_set.insert(pos.market);
    }

    std::vector<std::shared_ptr<strategy::IStrategy>> strategies;
    if (strategy_manager_) {
        strategies = strategy_manager_->getStrategies();
        for (const auto& strategy : strategies) {
            for (const auto& market : strategy->getActiveMarkets()) {
                market_set.insert(market);
            }
        }
    }

    if (market_set.empty()) {
        return;
    }

    std::vector<std::string> markets;
    markets.reserve(market_set.size());
    for (const auto& market : market_set) {
        markets.push_back(market);
    }
    
    if (should_log) {
        LOG_INFO("===== ??????È•îÎÇÖ?????????îÎá°????????®Î∫§???({}???????øÌã¢?? =====", markets.size());
    }

    // 3. [????? ?????????HTTP ????Ô¶??????????????∫Ïñò???????????∫Ïñú????????????ÍπÖÎºÇ????????Íæ©Î£ÜÔß?ù∞Ï≠?????????´ÏëÑ?????????õ¬Ä ?????????????(Batch Processing)
    std::map<std::string, double> price_map;
    
    try {
        // MarketScanner??????????????Êø°„Çç?êÊè∂??getTickerBatch ?????
        auto tickers = http_client_->getTickerBatch(markets);
        
        for (const auto& t : tickers) {
            std::string market_code = t["market"].get<std::string>();
            double trade_price = t["trade_price"].get<double>();
            price_map[market_code] = trade_price;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("?????????????????????®ÏÄ´Î????????????§Ïä£?? {}", e.what());
        return; // ????????????????????????????∞ÎÆù???????????? ????∫Ïñò?ÅÁ≠å?Ôº??????? (???????????∫Ïñò??????????Î∂∫Î™≠?????????Ë´õÎ™ÉÎß??ÊΩÅÎ∫õÍπ∫Ëã°?)
    }

    // 4. ????∫Ïñò?????????????Ë´õÎ™ÉÎß????????????ÄÏ≤???????????? ????????????????????????????????∑Îß£?´Î™≠????(??????????????¥Î™ñ?? ????∫Ïñò??????????????ÂΩ??Î£∞ÌÅø???
    for (auto& pos : positions) {
        // ??????????∫Ïñò??????????????????????????ÍπÖÎºÇ?????????´Î?????????∫Ïñò??????????Í±?Î™???????
        if (price_map.find(pos.market) == price_map.end()) {
            LOG_WARN("{} ticker data missing", pos.market);
            continue;
        }

        double current_price = price_map[pos.market];
        
        // RiskManager ??????Â´?????????????Í≥∑ÎôÉ???????ÑÏèÖÏ±∂Ôßç???(????∫Ïñò?????????????Ë´õÎ™ÉÎß???????????Ô¶´ÎöÆ????
        risk_manager_->updatePosition(pos.market, current_price);
        
        // [????????Ë¢Å‚ë£?????????????????±ÏÇ©???? ??????Íæ©Î£ÜÔß?ù∞Ï≠??????"?????????Í≥∑ÎôÉ???????ÑÏèÖÏ±∂Ôßç?????" ????????????????∫Ïñò??????ÍøîÍ∫Ç?????Ë´õ„Öª????===================
        std::shared_ptr<strategy::IStrategy> strategy;
        if (strategy_manager_) {
            // ??????????????®ÎöÆÎº??∞Î????????????≤Îê±Îπ??????∑Î™ÉÎ£??????????Íæ©Î£ÜÔß?ù∞Ï≠??????∫Ïñò??????????Í±?Î™???????(pos.strategy_name ????
            strategy = strategy_manager_->getStrategy(pos.strategy_name);
            
            if (strategy) {
                // ????????????IStrategy????????Ë¢Å‚ë£?????updateState ???ÄÎ∂æÍµù???????
                // ???????´ÎµÅ???????????Íæ©Î£ÜÔß?ù∞Ï≠?????????Í≥åÎñΩ?âÎ∂æ????????????????ÄÏ≤?È•îÎÇÖ????Ô¶´ÎöÆ???Ïæ?????????´ÎµÅ????????Î£∏‚Ñì??????∫Ïñò????????????∞Î??????§Ïä¢??????????????????Ô¶âÏï∏???Á≠???????®ÏÄ´Î?????
                // ?????ÍøîÍ∫Ç?????Î≥?Öâ????????Íæ©Î£ÜÔß?ù∞Ï≠?????????Í≥åÎñΩ?âÎ∂æ??????????´ÎµÅ???????????????????????á¬Ä?????????§Î≤°???????????Î°?æ∏?Ê§????§Ï±∂????(???????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ????
                strategy->updateState(pos.market, current_price);
            }
        }

        // ???????´Î????????????????????????Íæ®Íµ¥?????????´Î??????ÄÎ∂æÍµù???????≤„É´????Î∏êÏ≠ç?Ô¶âÏï∏???????(??????®ÎöÆÎº??∞Î?????????????????????????ÄÎ∂æÍµù??????????
        auto* updated_pos = risk_manager_->getPosition(pos.market);
        if (!updated_pos) continue;
        
        if (should_log) {
            LOG_INFO("  {} - ?????Ë´õÎ™ÉÎß?? {} - ?È•îÎÇÖ??????? {:.0f}, ?????Ë´õÎ™ÉÎß?? {:.0f}, ????? {:.0f} ({:+.2f}%)",
                     pos.market, updated_pos->strategy_name, updated_pos->entry_price, current_price,
                     updated_pos->unrealized_pnl, updated_pos->unrealized_pnl_pct * 100.0);
        }

        // --- ????∫Ïñò??????????Î∂∫Î™≠???????????‰∫?ªãÍº?∂≠???(??????Íæ©Î£ÜÔß?ù∞Ï≠??????????????????????????????) ---

        // ??????Íæ©Î£ÜÔß?ù∞Ï≠??????????????????????????(?????????????????
        if (strategy) {
            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            double holding_time_seconds = (now_ms - updated_pos->entry_time) / 1000.0;

            if (strategy->shouldExit(pos.market, updated_pos->entry_price, current_price, holding_time_seconds)) {
                LOG_INFO("?????Ë´õÎ™ÉÎß??????????®ÏÄ´Î??????????•‚ñ≤Íµ?Î®™Ï∂Ø????? {} (?????Ë´õÎ™ÉÎß?? {})", pos.market, updated_pos->strategy_name);
                executeSellOrder(pos.market, *updated_pos, "strategy_exit", current_price);
                continue;
            }
        }

        // 1???????????∫Ïñò???????????(50% ????
        if (!updated_pos->half_closed && current_price >= updated_pos->take_profit_1) {
            LOG_INFO("1???????????®ÏÄ´Î??????????•‚ñ≤Íµ?Î®™Ï∂Ø????? (?????∞Í∂ΩÎ∏îÎÄ???{:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            executePartialSell(pos.market, *updated_pos, current_price);
            continue; // ???????±ÏÇ©????????∫Ïñò??????????Î∂∫Î™≠?????????????éÏ≤é?Â´???????????ÍπÖÎºÇ?????????
        }
        
        // ??????Íæ©Î£ÜÔß?ù∞Ï≠??????????∫Ïñò???????????(?????or 2???????
        if (risk_manager_->shouldExitPosition(pos.market)) {
            std::string reason = "unknown";
            
            // ???????? ????????
            if (current_price <= updated_pos->stop_loss) {
                reason = "stop_loss";
                LOG_INFO("?????????®ÏÄ´Î??????????•‚ñ≤Íµ?Î®™Ï∂Ø?????(?????∞Í∂ΩÎ∏îÎÄ???{:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            } else if (current_price >= updated_pos->take_profit_2) {
                reason = "take_profit";
                LOG_INFO("2????????È•îÎÇÖ??????Â£???Î≥•Ïòñ?? ????®ÏÄ´Î??????????•‚ñ≤Íµ?Î®™Ï∂Ø?????(?????∞Í∂ΩÎ∏îÎÄ???{:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            } else {
                reason = "strategy_exit"; // ??????Íæ©Î£ÜÔß?ù∞Ï≠???????????????????âÎ†±????????®Î∫§Î•????(TS ??
            }
            
            executeSellOrder(pos.market, *updated_pos, reason, current_price);
        }
    }

    if (!strategies.empty()) {
        for (const auto& strategy : strategies) {
            auto active_markets = strategy->getActiveMarkets();
            for (const auto& market : active_markets) {
                if (risk_manager_->getPosition(market)) {
                    continue;
                }

                auto price_it = price_map.find(market);
                if (price_it == price_map.end()) {
                    continue;
                }

                strategy->updateState(market, price_it->second);
            }
        }

        for (const auto& strategy : strategies) {
            auto order_requests = strategy->drainOrderRequests();
            for (const auto& request : order_requests) {
                autolife::strategy::OrderResult result;
                result.market = request.market;
                result.side = request.side;
                result.level_id = request.level_id;
                result.reason = request.reason;

                double order_amount = request.price * request.quantity;
                if (order_amount < config_.min_order_krw) {
                    LOG_WARN("?????Ï¢äÌã£??????????πÎ¶Ñ??≤„É´????¥Ïö∞?????‰∫?ÇÉÏΩ?????????????Îºø¬Ä???{:.0f} < {:.0f}", order_amount, config_.min_order_krw);
                    strategy->onOrderResult(result);
                    continue;
                }

                if (request.side == autolife::strategy::OrderSide::BUY) {
                    if (risk_manager_->isDailyLossLimitExceeded()) {
                        LOG_WARN("{} skipped: daily loss limit exceeded", request.market);
                        strategy->onOrderResult(result);
                        continue;
                    }

                    double reserved_capital = risk_manager_->getReservedGridCapital(request.market);
                    if (reserved_capital <= 0.0 || order_amount > reserved_capital) {
                        LOG_WARN("{} ?????Ï¢äÌã£?????È•îÎÇÖ????????È•îÎÇÖ????∞Í∑•????ÔΩãÍ∂ô???? ??????úÔßü????????????Îºø¬Ä???({:.0f} < {:.0f})",
                                 request.market, reserved_capital, order_amount);
                        strategy->onOrderResult(result);
                        continue;
                    }
                }

                if (config_.mode == TradingMode::LIVE && !config_.dry_run && !config_.allow_live_orders) {
                    LOG_WARN(
                        "{} grid order blocked: trading.allow_live_orders=false, fallback to simulation",
                        request.market
                    );
                }

                if (isLiveRealOrderEnabled()) {
                    if (request.side == autolife::strategy::OrderSide::BUY) {
                        auto exec = executeLimitBuyOrder(
                            request.market,
                            request.price,
                            request.quantity,
                            0,
                            500
                        );
                        result.success = exec.success;
                        result.executed_price = exec.executed_price;
                        result.executed_volume = exec.executed_volume;
                    } else {
                        auto exec = executeLimitSellOrder(
                            request.market,
                            request.price,
                            request.quantity,
                            0,
                            500
                        );
                        result.success = exec.success;
                        result.executed_price = exec.executed_price;
                        result.executed_volume = exec.executed_volume;
                    }
                } else {
                    result.success = true;
                    result.executed_price = request.price;
                    result.executed_volume = request.quantity;
                }

                if (result.success && result.executed_volume > 0.0) {
                    risk_manager_->applyGridFill(
                        result.market,
                        result.side,
                        result.executed_price,
                        result.executed_volume
                    );
                }

                strategy->onOrderResult(result);
            }

            auto released_markets = strategy->drainReleasedMarkets();
            for (const auto& market : released_markets) {
                risk_manager_->releaseGridCapital(market);
            }
        }
    }
}

bool TradingEngine::executeSellOrder(
    const std::string& market,
    const risk::Position& position,
    const std::string& reason,
    double current_price
) {
    LOG_INFO("?????Ë´õÎ™ÉÎß???È•îÎÇÖ??????‰∫?ªãÍº?∂≠??????????: {} (????: {})", market, reason);
    
    //double current_price = getCurrentPrice(market);
    if (current_price <= 0) {
        LOG_ERROR("?????Ë´õÎ™ÉÎß????®ÏÄ™Îçß???????õ¬Ä ????®ÏÄ´Î????????????§Ïä£?? {}", market);
        return false;
    }
    
    // ??????Íæ©Î£ÜÔß?ù∞Ï≠??????∫Ïñò??????????Î∂∫Î™≠????
    double sell_quantity = std::floor(position.quantity * 0.9999 * 100000000.0) / 100000000.0;
    double invest_amount = sell_quantity * current_price;
    
    // 1. ????∫Ïñò????????????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠????????¥ÏèÇÍµ????????????∫Ïñò???????????(????∫Ïñò?ÅÁ≠å?Ôº???????????∫Ïñò?????????? 5,000 KRW)
    if (invest_amount < config_.min_order_krw) {
        LOG_WARN("?È•îÎÇÖ??????‰∫?ªãÍº?∂≠?????‰∫?ÇÉÏΩ?????????????Îºø¬Ä???{:.0f} < {:.0f} (?È•îÎÇÖÏ±∑Á∂≠???????È•îÎÇÖ??????Â£??", invest_amount, config_.min_order_krw);
        return false;
    }
    
    // 2. ???????????????®Î∫§Î•??????std::to_string ?????????? ?????????stringstream ????(????∫Ïñò??????????????¨Í≥£Î≤ÄÂ´?????Íæ§Îô¥???
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << sell_quantity;
        std::string quantity_str = ss.str();

    // 2-1. ???????∫Ïñò????????????????∫Ïñò??????????????∫Ïñò??????????Î∂∫Î™≠??????? ????????????(????∫Ïñò????????? ????∫Ïñò??????????Î∂∫Î™≠??????????????≤ÎÄ??
    double sell_price = current_price;
    try {
        auto orderbook = http_client_->getOrderBook(market);
        sell_price = calculateOptimalSellPrice(market, current_price, orderbook);
        double slippage_pct = estimateOrderbookSlippagePct(
            orderbook,
            sell_quantity,
            false,
            current_price
        );

        if (slippage_pct > config_.max_slippage_pct) {
            LOG_WARN("{} ?????? ????? {:.3f}% > {:.3f}% (?????È•îÎÇÖ????∞Í∑•????ÔΩãÍ∂ô????",
                     market, slippage_pct * 100.0, config_.max_slippage_pct * 100.0);
            return false;
        }
        LOG_INFO("?È•îÎÇÖ??????‰∫?ªãÍº?∂≠????? ?È•îÎÇÖ??????Â£??? {} (?????Ë´õÎ™ÉÎß????®ÏÄ™Îçß???????õ¬Ä: {})", sell_price, current_price);
    } catch (const std::exception& e) {
        LOG_WARN("??? ????®ÏÄ´Î????????????§Ïä£??(?????Ë´õÎ™ÉÎß????®ÏÄ™Îçß???????õ¬Ä ????: {}", e.what());
        sell_price = current_price;
    }

    // 2. ???????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ?????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠?????????? (????∫Ïñò????????? ????????? ?????Ë´õÎ™ÉÎßàÔ¶´???????
    double executed_price = current_price;
    if (config_.mode == TradingMode::LIVE) {
        if (config_.dry_run || !config_.allow_live_orders) {
            if (!config_.dry_run && !config_.allow_live_orders) {
                LOG_WARN("{} live sell blocked: trading.allow_live_orders=false, using simulation", market);
            }
            LOG_WARN("DRY RUN: sell simulation completed");
        } else {
            auto order_result = executeLimitSellOrder(
                market,
                sell_price,
                sell_quantity,
                0,
                500
            );

            if (!order_result.success || order_result.executed_volume <= 0.0) {
                LOG_ERROR("?È•îÎÇÖ??????‰∫?ªãÍº?∂≠???È•îÎÇÖ???????????????§Ïä£?? {}", order_result.error_message);
                return false;
            }

            executed_price = order_result.executed_price;
            sell_quantity = order_result.executed_volume;  // [Phase 3] ???????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ???????∫Ïñò???????????????Ë´õÎ™ÉÎßàÔ¶´???????
            
            // [Phase 3] ???????±ÏÇ©????????∫Ïñò????????????????´Î????
            double fill_ratio = order_result.executed_volume / position.quantity;
            if (fill_ratio < 0.999) {
                LOG_WARN("?????Îºø¬Ä????È•îÎÇÖ?????????????´Îäâ???: {:.8f}/{:.8f} ({:.1f}%)",
                         order_result.executed_volume, position.quantity, fill_ratio * 100.0);
            }
            
            LOG_INFO("?È•îÎÇÖ??????‰∫?ªãÍº?∂≠???È•îÎÇÖ??????????ËΩÖÎ∂Ω????????? ??? {:.0f} (?????{})",
                     executed_price, order_result.retry_count);
        }
    }
    
    // 3. ??????®ÎöÆÎº??∞Î?????????????????
    double realized_qty = sell_quantity;
    
    // 4. [Phase 3] ???????±ÏÇ©????????∫Ïñò?????????vs ??????Íæ©Î£ÜÔß?ù∞Ï≠??????∫Ïñò???????????????????Ï´???
    double fill_ratio = sell_quantity / position.quantity;
    const bool fully_closed = fill_ratio >= 0.999;
    if (fully_closed) {
        realized_qty = position.quantity;
        // ??????Íæ©Î£ÜÔß?ù∞Ï≠??????∫Ïñò??????????????????????????‰∫?ªãÍº?∂≠???(???????????Íæ©Î£ÜÔß?ù∞Ï≠??????
        risk_manager_->exitPosition(market, executed_price, reason);
        nlohmann::json close_payload;
        close_payload["exit_price"] = executed_price;
        close_payload["quantity"] = realized_qty;
        close_payload["reason"] = reason;
        close_payload["gross_pnl"] = (executed_price - position.entry_price) * realized_qty;
        appendJournalEvent(
            core::JournalEventType::POSITION_CLOSED,
            market,
            "sell_exit",
            close_payload
        );
    } else {
        // Partial fill must realize PnL/fees using actual fill price.
        const bool applied = risk_manager_->applyPartialSellFill(
            market,
            executed_price,
            sell_quantity,
            reason + "_partial_fill"
        );
        if (!applied) {
            LOG_ERROR("Partial fill accounting failed: {} qty {:.8f} @ {:.0f}",
                      market, sell_quantity, executed_price);
            return false;
        }
        nlohmann::json reduce_payload;
        reduce_payload["exit_price"] = executed_price;
        reduce_payload["quantity"] = sell_quantity;
        reduce_payload["reason"] = reason + "_partial_fill";
        reduce_payload["gross_pnl"] = (executed_price - position.entry_price) * realized_qty;
        appendJournalEvent(
            core::JournalEventType::POSITION_REDUCED,
            market,
            "sell_partial",
            reduce_payload
        );
    }
    
    // 5. [???????????®ÎöÆÎº??∞Î???? StrategyManager?????????????Íæ©Î£ÜÔß?ù∞Ï≠???????∫Ïñò??????????????????????????Í≥∑ÎôÉ???????ÑÏèÖÏ±∂Ôßç???& ?????Â£§Íµø???????????????¥Î™ù??
    if (fully_closed && strategy_manager_ && !position.strategy_name.empty()) {
        // Position ???????????????????ÁππÎ®ÆÍµûÂΩõ?????????Î≥ßÎÄ??Á≠åÎ®≤???????strategy_name("Advanced Scalping" ????????????????Íæ©Î£ÜÔß?ù∞Ï≠??????∫Ïñò??????????Í±?Î™???????
        auto strategy = strategy_manager_->getStrategy(position.strategy_name);
        
        if (strategy) {
            // Strategy stats are aligned to the same fee-inclusive basis as trade_history.
            const double fee_rate = Config::getInstance().getFeeRate();
            const double exit_value = executed_price * position.quantity;
            const double entry_fee = position.invested_amount * fee_rate;
            const double exit_fee = exit_value * fee_rate;
            const double net_pnl = exit_value - position.invested_amount - entry_fee - exit_fee;
            strategy->updateStatistics(market, net_pnl > 0.0, net_pnl);
            LOG_INFO("?Ë¢Å‚ë•??{}) ????????ÖÏëì??Íæ®Î±ú ?????????Í≥∏Ï†´", position.strategy_name);
        } else if (position.strategy_name != "RECOVERED") {
            // RECOVERED ?????? ????ÍøîÍ∫Ç???????????Œ≤Îº?è¥???? ?????????ñ¬Ä????Ô¶âÏï∏???Á≠????????Ô¶âÏï∏???Á≠????
            LOG_WARN("Sell complete but strategy object missing: {}", position.strategy_name);
        }
    }
    
    LOG_INFO("????????????øÌã¢???????Ë´õÎ™ÉÎß?? {} (????? {:.0f} KRW)",
             market, (executed_price - position.entry_price) * realized_qty);
    
    return true;
}


bool TradingEngine::executePartialSell(const std::string& market, const risk::Position& position, double current_price) {

    //double current_price = getCurrentPrice(market);

        if (current_price <= 0) {
        LOG_ERROR("?????Ë´õÎ™ÉÎß????®ÏÄ™Îçß???????õ¬Ä ????®ÏÄ´Î????????????§Ïä£?? {}", market);
        return false;
    }
    
    // 50% ??????????????????
    double sell_quantity = std::floor(position.quantity * 0.5 * 100000000.0) / 100000000.0;
    double invest_amount = sell_quantity * current_price;
    
    // 1. ????∫Ïñò????????????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠????????¥ÏèÇÍµ????????????∫Ïñò?????????????????(????∫Ïñò?ÅÁ≠å?Ôº???????????∫Ïñò?????????? 5,000 KRW)
    // 1. ????∫Ïñò????????????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠????????¥ÏèÇÍµ????????????∫Ïñò?????????????????(????∫Ïñò?ÅÁ≠å?Ôº???????????∫Ïñò?????????? 5,000 KRW)
    if (invest_amount < config_.min_order_krw) {
        LOG_WARN("?????Îºø¬Ä??????•¬Ä?´Ï∏ß???????‰∫?ÇÉÏΩ?????????????Îºø¬Ä???{:.0f} < ?È•îÎÇÖ??????Â£??{:.0f}) -> ?????Îºø¬Ä???????????ÍøîÍ∫Ç?????(Half Closed ?È•îÎÇÖ????Â£§Íµø??Ôßí„èêÍ¥??", 
                 invest_amount, config_.min_order_krw);
        
        // [Fix] ??????¥ÏèÇÍµ??????????????????È•îÎÇÖ????Ô¶´ÎöÆ???Ïæ?????????????±ÏÇ©?????????????È•îÎÇÖ??????????Ô¶âÏï∏???Á≠????????????ÔßèÍªäÍ¥??????????????????????Î∫§Î™ù???Á≠åÎ§æ??Íø±Ï????? (????ÍøîÍ∫Ç????????????∑Îß£?´Î™≠?????????Ë´õÎ™ÉÎß??ÊΩÅÎ∫õÍπ∫Ëã°?)
        // RiskManager??????∫Ïñò??????????????????????È•îÎÇÖ????????half_closed = true ???????éÏ≤é?Â´???
        if (risk_manager_) {
            // Safe method call
            risk_manager_->setHalfClosed(market, true);
        }
        return true; // ?????Êø??Ä??????§Ïä¢??∂ÌÅ∫?????????????∫Ïñò???????????Ô¶´ÎöÆ??∑Î≥†ÍΩ????È•îÎÇÖ?????????????????∫Ïñò?????????
    }
    
    LOG_INFO("?????Îºø¬Ä????È•îÎÇÖ??????‰∫?ªãÍº?∂≠?????????? (50%): {}", market);
    
    LOG_INFO("  ?È•îÎÇÖ?????????: {:.0f}, ??????ÊøöÎ∞∏≈¶???: {:.0f}, ?????Îºø¬Ä??????•¬Ä?´Ï∏ß???????Ë´õÎ™ÉÎß?? {:.8f}",
             position.entry_price, current_price, sell_quantity);
    
    // 2. ???????????????®Î∫§Î•??????std::to_string ?????????? ?????????stringstream ????(????∫Ïñò??????????????¨Í≥£Î≤ÄÂ´?????Íæ§Îô¥???
    std::stringstream ss;
    ss << std::fixed << std::setprecision(8) << sell_quantity;
    std::string quantity_str = ss.str();

    // 2-1. ???????∫Ïñò????????????????∫Ïñò??????????????∫Ïñò??????????Î∂∫Î™≠??????? ????????????(????∫Ïñò????????? ????∫Ïñò??????????Î∂∫Î™≠??????????????≤ÎÄ??
    double sell_price = current_price;
    try {
        auto orderbook = http_client_->getOrderBook(market);
        sell_price = calculateOptimalSellPrice(market, current_price, orderbook);
        double slippage_pct = estimateOrderbookSlippagePct(
            orderbook,
            sell_quantity,
            false,
            current_price
        );

        if (slippage_pct > config_.max_slippage_pct) {
            LOG_WARN("{} ?????? ????? {:.3f}% > {:.3f}% (?????Îºø¬Ä????????È•îÎÇÖ????∞Í∑•????ÔΩãÍ∂ô????",
                     market, slippage_pct * 100.0, config_.max_slippage_pct * 100.0);
            return false;
        }
    } catch (const std::exception&) {
        sell_price = current_price;
    }

    auto apply_partial_fill = [&](double fill_price, double fill_qty, const std::string& reason_tag) -> bool {
        const bool applied = risk_manager_->applyPartialSellFill(market, fill_price, fill_qty, reason_tag);
        if (!applied) {
            LOG_ERROR("Partial sell accounting failed: {} qty {:.8f} @ {:.0f}",
                      market, fill_qty, fill_price);
            return false;
        }
        risk_manager_->setHalfClosed(market, true);
        risk_manager_->moveStopToBreakeven(market);
        return true;
    };

    // 2. ???????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ?????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠?????????? (????∫Ïñò????????? ????????? ?????Ë´õÎ™ÉÎßàÔ¶´???????
    if (config_.mode == TradingMode::LIVE) {
        if (config_.dry_run || !config_.allow_live_orders) {
            if (!config_.dry_run && !config_.allow_live_orders) {
                LOG_WARN("{} live partial-sell blocked: trading.allow_live_orders=false, using simulation", market);
            }
            LOG_WARN("DRY RUN: partial sell simulated");
            if (!apply_partial_fill(current_price, sell_quantity, "partial_take_profit_dry_run")) {
                return false;
            }
            nlohmann::json reduce_payload;
            reduce_payload["exit_price"] = current_price;
            reduce_payload["quantity"] = sell_quantity;
            reduce_payload["reason"] = "partial_take_profit_dry_run";
            appendJournalEvent(
                core::JournalEventType::POSITION_REDUCED,
                market,
                "partial_sell",
                reduce_payload
            );
            return true;
        }

        auto order_result = executeLimitSellOrder(
            market,
            sell_price,
            sell_quantity,
            0,
            500
        );

        if (!order_result.success || order_result.executed_volume <= 0.0) {
            LOG_ERROR("?????Îºø¬Ä????È•îÎÇÖ??????‰∫?ªãÍº?∂≠???È•îÎÇÖ???????????????§Ïä£?? {}", order_result.error_message);
            return false;
        }

        LOG_INFO("?????Îºø¬Ä????È•îÎÇÖ??????‰∫?ªãÍº?∂≠???È•îÎÇÖ??????????ËΩÖÎ∂Ω????????? ??? {:.0f} (?????{})",
                 order_result.executed_price, order_result.retry_count);
        if (!apply_partial_fill(order_result.executed_price, order_result.executed_volume, "partial_take_profit")) {
            return false;
        }
        nlohmann::json reduce_payload;
        reduce_payload["exit_price"] = order_result.executed_price;
        reduce_payload["quantity"] = order_result.executed_volume;
        reduce_payload["reason"] = "partial_take_profit";
        appendJournalEvent(
            core::JournalEventType::POSITION_REDUCED,
            market,
            "partial_sell",
            reduce_payload
        );
        return true;
    }

    // Paper Trading
    if (!apply_partial_fill(current_price, sell_quantity, "partial_take_profit_paper")) {
        return false;
    }
    nlohmann::json reduce_payload;
    reduce_payload["exit_price"] = current_price;
    reduce_payload["quantity"] = sell_quantity;
    reduce_payload["reason"] = "partial_take_profit_paper";
    appendJournalEvent(
        core::JournalEventType::POSITION_REDUCED,
        market,
        "partial_sell",
        reduce_payload
    );
    return true;
}

// ===== ??????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠??????=====

double TradingEngine::calculateOptimalBuyPrice(
    const std::string& market,
    double base_price,
    const nlohmann::json& orderbook
) {
    (void)market;
    nlohmann::json units;

    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0]["orderbook_units"];
    } else if (orderbook.contains("orderbook_units")) {
        units = orderbook["orderbook_units"];
    }

    if (units.is_array() && !units.empty()) {
        double ask = units[0].value("ask_price", base_price);
        return common::roundUpToTickSize(ask);  // [Phase 3] ??? ????????®Î∫§ÍΩ?????
    }

    return common::roundUpToTickSize(base_price);
}

double TradingEngine::calculateOptimalSellPrice(
    const std::string& market,
    double base_price,
    const nlohmann::json& orderbook
) {
    (void)market;
    nlohmann::json units;

    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0]["orderbook_units"];
    } else if (orderbook.contains("orderbook_units")) {
        units = orderbook["orderbook_units"];
    }

    if (units.is_array() && !units.empty()) {
        double bid = units[0].value("bid_price", base_price);
        return common::roundDownToTickSize(bid);  // [Phase 3] ??? ????????®Î∫§ÍΩ??????
    }

    return common::roundDownToTickSize(base_price);
}

double TradingEngine::estimateOrderbookVWAPPrice(
    const nlohmann::json& orderbook,
    double target_volume,
    bool is_buy
) const {
    if (target_volume <= 0.0) {
        return 0.0;
    }

    nlohmann::json units;
    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0].value("orderbook_units", nlohmann::json::array());
    } else {
        units = orderbook.value("orderbook_units", nlohmann::json::array());
    }

    if (!units.is_array() || units.empty()) {
        return 0.0;
    }

    double reference_price = 0.0;
    if (!units.empty()) {
        reference_price = units[0].value(is_buy ? "ask_price" : "bid_price", 0.0);
    }

    if (reference_price <= 0.0) {
        return 0.0;
    }

    double target_notional = reference_price * target_volume;
    return analytics::OrderbookAnalyzer::estimateVWAPForNotional(
        units,
        target_notional,
        is_buy
    );
}

double TradingEngine::estimateOrderbookSlippagePct(
    const nlohmann::json& orderbook,
    double target_volume,
    bool is_buy,
    double reference_price
) const {
    if (reference_price <= 0.0) {
        return 0.0;
    }

    nlohmann::json units;
    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0].value("orderbook_units", nlohmann::json::array());
    } else {
        units = orderbook.value("orderbook_units", nlohmann::json::array());
    }

    if (!units.is_array() || units.empty()) {
        return 0.0;
    }

    double target_notional = reference_price * target_volume;
    double slippage = analytics::OrderbookAnalyzer::estimateSlippagePctForNotional(
        units,
        target_notional,
        is_buy,
        reference_price
    );

    return std::max(0.0, slippage);
}

TradingEngine::OrderFillInfo TradingEngine::verifyOrderFill(
    const std::string& uuid,
    const std::string& market,
    double order_volume
) {
    OrderFillInfo info{};
    (void)market;
    (void)order_volume;

    auto toDouble = [](const nlohmann::json& value) -> double {
        try {
            if (value.is_string()) {
                return std::stod(value.get<std::string>());
            }
            if (value.is_number()) {
                return value.get<double>();
            }
        } catch (...) {
        }
        return 0.0;
    };

    try {
        auto check = http_client_->getOrder(uuid);
        std::string state = check.value("state", "");

        double total_funds = 0.0;
        double total_vol = 0.0;

        if (check.contains("trades") && check["trades"].is_array()) {
            for (const auto& trade : check["trades"]) {
                double trade_vol = toDouble(trade["volume"]);
                double trade_price = toDouble(trade["price"]);
                total_vol += trade_vol;
                total_funds += trade_vol * trade_price;
            }
        } else if (check.contains("executed_volume")) {
            total_vol = toDouble(check["executed_volume"]);
        }

        if (total_vol > 0.0 && total_funds > 0.0) {
            info.avg_price = total_funds / total_vol;
        } else if (check.contains("price")) {
            info.avg_price = toDouble(check["price"]);
        }

        info.filled_volume = total_vol;
        info.is_filled = (state == "done") && total_vol > 0.0;
        info.is_partially_filled = (!info.is_filled && total_vol > 0.0);
        info.fee = 0.0;
    } catch (const std::exception& e) {
        LOG_WARN("??????πÎ¶Ñ??≤„É´????¥Ïö∞???È•îÎÇÖ??????????ËΩÖÎ∂Ω????????????????§Ïä£?? {}", e.what());
    }

    return info;
}

TradingEngine::LimitOrderResult TradingEngine::executeLimitBuyOrder(
    const std::string& market,
    double entry_price,
    double quantity,
    int max_retries,
    int retry_wait_ms
) {
    LimitOrderResult result{};
    result.success = false;
    result.retry_count = 0;
    result.executed_price = 0.0;
    result.executed_volume = 0.0;

    double remaining = quantity;
    double total_filled = 0.0;
    double total_funds = 0.0;

    int max_attempts = max_retries > 0 ? max_retries : 3;
    int attempt_count = 0;

    while (running_ && remaining > 0.00000001 && attempt_count < max_attempts) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << remaining;
        std::string vol_str = ss.str();
        // [Phase 3] ??? ????????®Î∫§ÍΩ?????+ ??? ???????????????®Î∫§Î•????
        double tick_price = common::roundUpToTickSize(entry_price);
        std::string price_str = common::priceToString(tick_price);

        nlohmann::json order_res;
        try {
            order_res = http_client_->placeOrder(market, "bid", vol_str, price_str, "limit");
        } catch (const std::exception& e) {
            result.error_message = e.what();
            return result;
        }

        if (!order_res.contains("uuid")) {
            result.error_message = "No UUID returned";
            return result;
        }

        std::string uuid = order_res["uuid"].get<std::string>();
        LOG_INFO("?È•îÎÇÖ?????????????πÎ¶Ñ??≤„É´????¥Ïö∞???????Ë´õÎ™ÉÎßàÔ¶´??πæ?Í≥†Îíå?(UUID: {}, ?????´Îäâ????{:.0f}, ?????? {})", uuid, entry_price, vol_str);

        // 10???????????ÎΩ∞‚îª ????∫Ïñò????????????ÄÎ∂æÍµù??????????(500ms * 20)
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto fill = verifyOrderFill(uuid, market, remaining);

            if (fill.filled_volume > 0.0) {
                total_filled += fill.filled_volume;
                total_funds += fill.avg_price * fill.filled_volume;
                remaining -= fill.filled_volume;
            }

            if (fill.is_filled || remaining <= 0.00000001) {
                break;
            }
        }

        if (remaining <= 0.00000001) {
            break;
        }

        // ??????∞Ïó®????Êø°„Çç?êÊè∂????Î£∏‚Ñì???????????Ô¶???? ??????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠???????????????????
        try {
            http_client_->cancelOrder(uuid);
            LOG_WARN("?È•îÎÇÖ???????????Î∏çÌàñ???éÍªäÍ∞???Î£∏≈???????10?? -> ??????πÎ¶Ñ??≤„É´????¥Ïö∞???????????????????");
        } catch (const std::exception& e) {
            LOG_WARN("?È•îÎÇÖ?????????????πÎ¶Ñ??≤„É´????¥Ïö∞???????????????§Ïä£?? {}", e.what());
        }

        attempt_count++;
        result.retry_count = attempt_count;

        if (retry_wait_ms > 0 && attempt_count < max_attempts) {
            int wait_ms = retry_wait_ms * (1 + attempt_count);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }

        try {
            auto orderbook = http_client_->getOrderBook(market);
            entry_price = calculateOptimalBuyPrice(market, entry_price, orderbook);
        } catch (const std::exception& e) {
            LOG_WARN("????????????????¨Í≥ª?¢Â§∑?®Îñ≥?Î≤?Ä???? ????®ÏÄ´Î????????????§Ïä£?? {}", e.what());
        }
    }

    if (total_filled > 0.0) {
        result.success = true;
        result.executed_volume = total_filled;
        result.executed_price = total_funds / total_filled;
    } else {
        result.error_message = (attempt_count >= max_attempts) ? "Max retries exceeded" : "No fills";
    }

    return result;
}

TradingEngine::LimitOrderResult TradingEngine::executeLimitSellOrder(
    const std::string& market,
    double exit_price,
    double quantity,
    int max_retries,
    int retry_wait_ms
) {
    LimitOrderResult result{};
    result.success = false;
    result.retry_count = 0;
    result.executed_price = 0.0;
    result.executed_volume = 0.0;

    double remaining = quantity;
    double total_filled = 0.0;
    double total_funds = 0.0;

    int max_attempts = max_retries > 0 ? max_retries : 3;
    int attempt_count = 0;

    while (running_ && remaining > 0.00000001 && attempt_count < max_attempts) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << remaining;
        std::string vol_str = ss.str();
        // [Phase 3] ??? ????????®Î∫§ÍΩ??????+ ??? ???????????????®Î∫§Î•????
        double tick_price = common::roundDownToTickSize(exit_price);
        std::string price_str = common::priceToString(tick_price);

        nlohmann::json order_res;
        try {
            order_res = http_client_->placeOrder(market, "ask", vol_str, price_str, "limit");
        } catch (const std::exception& e) {
            result.error_message = e.what();
            return result;
        }

        if (!order_res.contains("uuid")) {
            result.error_message = "No UUID returned";
            return result;
        }

        std::string uuid = order_res["uuid"].get<std::string>();
        LOG_INFO("?È•îÎÇÖ??????‰∫?ªãÍº?∂≠????????πÎ¶Ñ??≤„É´????¥Ïö∞???????Ë´õÎ™ÉÎßàÔ¶´??πæ?Í≥†Îíå?(UUID: {}, ?????´Îäâ????{:.0f}, ?????? {})", uuid, exit_price, vol_str);

        // 10???????????ÎΩ∞‚îª ????∫Ïñò????????????ÄÎ∂æÍµù??????????(500ms * 20)
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto fill = verifyOrderFill(uuid, market, remaining);

            if (fill.filled_volume > 0.0) {
                total_filled += fill.filled_volume;
                total_funds += fill.avg_price * fill.filled_volume;
                remaining -= fill.filled_volume;
            }

            if (fill.is_filled || remaining <= 0.00000001) {
                break;
            }
        }

        if (remaining <= 0.00000001) {
            break;
        }

        // ??????∞Ïó®????Êø°„Çç?êÊè∂????Î£∏‚Ñì???????????Ô¶???? ??????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠???????????????????
        try {
            http_client_->cancelOrder(uuid);
            LOG_WARN("?È•îÎÇÖ??????‰∫?ªãÍº?∂≠??????Î∏çÌàñ???éÍªäÍ∞???Î£∏≈???????10?? -> ??????πÎ¶Ñ??≤„É´????¥Ïö∞???????????????????");
        } catch (const std::exception& e) {
            LOG_WARN("?È•îÎÇÖ??????‰∫?ªãÍº?∂≠????????πÎ¶Ñ??≤„É´????¥Ïö∞???????????????§Ïä£?? {}", e.what());
        }

        attempt_count++;
        result.retry_count = attempt_count;

        if (retry_wait_ms > 0 && attempt_count < max_attempts) {
            int wait_ms = retry_wait_ms * (1 + attempt_count);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }

        try {
            auto orderbook = http_client_->getOrderBook(market);
            exit_price = calculateOptimalSellPrice(market, exit_price, orderbook);
        } catch (const std::exception& e) {
            LOG_WARN("????????????????¨Í≥ª?¢Â§∑?®Îñ≥?Î≤?Ä???? ????®ÏÄ´Î????????????§Ïä£?? {}", e.what());
        }
    }

    if (total_filled > 0.0) {
        result.success = true;
        result.executed_volume = total_filled;
        result.executed_price = total_funds / total_filled;
    } else {
        result.error_message = (attempt_count >= max_attempts) ? "Max retries exceeded" : "No fills";
    }

    return result;
}


// ===== ????∫Ïñò??????????Ô¶´ÎöÆ??????????????Í≥∑ÎôÉ???????ÑÏèÖÏ±∂Ôßç???=====

void TradingEngine::updateMetrics() {
    auto metrics = risk_manager_->getRiskMetrics();

    LOG_INFO("===== Metrics =====");
    LOG_INFO("Total capital: {:.0f} KRW ({:+.2f}%)",
             metrics.total_capital + metrics.invested_capital,
             metrics.total_pnl_pct * 100);
    LOG_INFO("Available/Invested: {:.0f} / {:.0f}",
             metrics.available_capital, metrics.invested_capital);
    LOG_INFO("Realized/Unrealized PnL: {:.0f} / {:.0f}",
             metrics.realized_pnl, metrics.unrealized_pnl);
    LOG_INFO("Trades: {} (W {} / L {}, WinRate {:.1f}%)",
             metrics.total_trades,
             metrics.winning_trades,
             metrics.losing_trades,
             metrics.win_rate * 100);
    LOG_INFO("Active positions: {}/{}, Drawdown: {:.2f}%",
             metrics.active_positions,
             metrics.max_positions,
             metrics.current_drawdown * 100);
    LOG_INFO("====================");
}
risk::RiskManager::RiskMetrics TradingEngine::getMetrics() const {
    return risk_manager_->getRiskMetrics();
}

std::vector<risk::Position> TradingEngine::getPositions() const {
    return risk_manager_->getAllPositions();
}

std::vector<risk::TradeHistory> TradingEngine::getTradeHistory() const {
    return risk_manager_->getTradeHistory();
}

// ===== ????≤Îê±Îπ??????âÎùø????±Îã™????????=====

void TradingEngine::manualScan() {
    LOG_INFO("???Ê±ùÎ∑¥????∫Îôº?øÎ°´??????Ô¶´ÎöÆ????????????");
    scanMarkets();
    generateSignals();
}

// void TradingEngine::manualClosePosition(const std::string& market) {
//     LOG_INFO("????≤Îê±Îπ??????âÎùø????±Îã™?????? {}", market);
    
//     auto* pos = risk_manager_->getPosition(market);
//     if (!pos) {
//         LOG_WARN("??????????????¥Î™ñ?? {}", market);
//         return;
//     }
    
//     executeSellOrder(market, *pos, "manual", current_price);
// }

// void TradingEngine::manualCloseAll() {
//     LOG_INFO("??????Íæ©Î£ÜÔß?ù∞Ï≠???????????≤Îê±Îπ??????âÎùø????±Îã™??????);
    
//     auto positions = risk_manager_->getAllPositions();
//     for (const auto& pos : positions) {
//         executeSellOrder(pos.market, pos, "manual_all", current_price);
//     }
// }

// ===== ?????????(??????®ÎöÆÎº??∞Î???? =====

double TradingEngine::getCurrentPrice(const std::string& market) {
    try {
        auto ticker = http_client_->getTicker(market);
        if (ticker.empty()) {
            return 0;
        }
        
        // 2. nlohmann/json ?????????????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ??????????????®Î∫§Î•????
        if (ticker.is_array() && !ticker.empty()) {
            return ticker[0].value("trade_price", 0.0);
        }

        if (ticker.contains("trade_price") && !ticker["trade_price"].is_null()) {
            return ticker.value("trade_price", 0.0);
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        LOG_ERROR("?????Ë´õÎ™ÉÎß????®ÏÄ™Îçß???????õ¬Ä ????®ÏÄ´Î????????????§Ïä£?? {} - {}", market, e.what());
        return 0;
    }
}

bool TradingEngine::hasEnoughBalance(double required_krw) {
    auto metrics = risk_manager_->getRiskMetrics();
    return metrics.available_capital >= required_krw;
}

void TradingEngine::logPerformance() {
    auto metrics = risk_manager_->getRiskMetrics();
    auto history = risk_manager_->getTradeHistory();

    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    double runtime_hours = (now - start_time_) / (1000.0 * 60.0 * 60.0);

    LOG_INFO("========================================");
    LOG_INFO("Final performance report");
    LOG_INFO("========================================");
    LOG_INFO("Runtime: {:.1f} hours", runtime_hours);
    LOG_INFO("Scans: {}, Signals: {}, Trades: {}",
             total_scans_, total_signals_, metrics.total_trades);
    LOG_INFO("Capital summary");
    LOG_INFO("Initial capital: {:.0f} KRW", config_.initial_capital);
    LOG_INFO("Final capital: {:.0f} KRW", metrics.total_capital);
    LOG_INFO("Total PnL: {:.0f} KRW ({:+.2f}%)",
             metrics.total_pnl, metrics.total_pnl_pct * 100);

    LOG_INFO("Trade performance");
    LOG_INFO("Win rate: {:.1f}% ({}/{})",
             metrics.win_rate * 100,
             metrics.winning_trades,
             metrics.total_trades);
    LOG_INFO("Profit Factor: {:.2f}", metrics.profit_factor);
    LOG_INFO("Sharpe Ratio: {:.2f}", metrics.sharpe_ratio);
    LOG_INFO("Max Drawdown: {:.2f}%", metrics.max_drawdown * 100);

    LOG_INFO("Recent trades (last 10)");
    if (!history.empty()) {
        int count = 0;
        for (auto it = history.rbegin(); it != history.rend() && count < 10; ++it, ++count) {
            std::string status_mark = (it->profit_loss > 0) ? "WIN" : "LOSS";
            LOG_INFO("  {} {} | entry: {:.0f}, exit: {:.0f} | {:+.2f}% | {}",
                     status_mark, it->market,
                     it->entry_price,
                     it->exit_price,
                     it->profit_loss_pct * 100,
                     it->exit_reason);
        }
    } else {
        LOG_INFO("  No trade history");
    }

    LOG_INFO("Realtime monitoring metrics");
    LOG_INFO("Dynamic filter value: {:.3f}", dynamic_filter_value_);
    LOG_INFO("Position scale multiplier: {:.2f}", position_scale_multiplier_);
    LOG_INFO("Cumulative buy/sell orders: {} / {}",
             prometheus_metrics_.total_buy_orders,
             prometheus_metrics_.total_sell_orders);

    auto prom_metrics = exportPrometheusMetrics();
    LOG_INFO("Prometheus metrics preview: {}", prom_metrics.substr(0, 200) + "...");
    LOG_INFO("========================================");
}
void TradingEngine::syncAccountState() {
    LOG_INFO("??È∂?Ö∫???????????????∞≈?çÆ???????????????ÍøîÍ∫Ç???ÂΩ±¬Ä??∞ƒ?..");

    try {
        auto accounts = http_client_->getAccounts();
        bool krw_found = false;
        std::set<std::string> wallet_markets;
        const std::size_t reconcile_candidates = pending_reconcile_positions_.size();
        std::size_t restored_positions = 0;
        std::size_t external_closes = 0;

        for (const auto& acc : accounts) {
            std::string currency = acc["currency"].get<std::string>();
            double balance = std::stod(acc["balance"].get<std::string>());
            double locked = std::stod(acc["locked"].get<std::string>());
            
            // 1. [??????®ÎöÆÎº??∞Î???? KRW(???????????? ????∫Ïñò???????????Ô¶´ÎöÆ??∑Î≥†ÍΩ?????????‰∫?ªãÍº?∂≠?????????Ë¢Å‚ë£???
            if (currency == "KRW") {
                double total_cash = balance + locked; // ???????????+ ??????∞Ïó®????Êø°„Çç?êÊè∂????Î£∏‚Ñì?????????????≤„É´?????
                
                // RiskManager???????????®ÎöÆ???????????????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ?????????????????????????????Ï¢äÌã£????
                risk_manager_->resetCapital(total_cash);
                // (2) [????????Ë¢Å‚ë£???] ????Ô¶??????????éÏ≤é?Â´???????????ÄÏ≤??'??????Á≠åÎ°´??????????????????éÏ≤é?Â´?ÊøöÎ∞∏ÏÆ?????????????????®Î∫§Î•????
                config_.initial_capital = total_cash;
                krw_found = true;
                LOG_INFO("?????¨Í≥•????????????????????? {:.0f} KRW (?????´Îäâ????{:.0f})", total_cash, balance);
                continue; // ????????????????∫Ïñò???????????Ô¶´ÎöÆ??∑Î≥†ÍΩ??????È•îÎÇÖ??????????????Ë¢Å‚ë£??????????éÏ≤é?Â´??????????
            }

            // 2. ??????®Î∫§Î•???? ??????ÑÏèÖÏ±∂Ôßç????????∫Ïñò???????????Ô¶´ÎöÆ??∑Î≥†ÍΩ??(???????????????‰∫?ªãÍº?∂≠??????)
            // ????∫Ïñò??????????????ÑÏèÖÏ±∂Ôßç???????????πÎïüÔßíÎÖπ????(?? BTC -> KRW-BTC)
            std::string market = "KRW-" + currency;
            wallet_markets.insert(market);
            
            double avg_buy_price = std::stod(acc["avg_buy_price"].get<std::string>());
            
            // ????∫Ïñò??????ÍøîÍ∫Ç????????????Dust) ????ÍøîÍ∫Ç???????(????∫Ïñò?ÅÁ≠å?Ôº???????????∫Ïñò????????????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠???????¥ÏèÇÍµ???????????????ÑÏèÖÏ±∂Ôßç???
            if (balance * avg_buy_price < 5000) continue;

            // ???? RiskManager?????????éÏ≤é?Â´?Êø°„Çç???∫Î∏ç??????????ìÌÖ§???â¬Ä??Á≠??
            if (risk_manager_->getPosition(market) != nullptr) continue;

            LOG_INFO("?????????????∞ÎÆõ???? ?????Î∞∏Î∏∂?????????πÎïüÔßíÎÖπ???´ÎîÜÎßöÔ¶´?Á≠?? {} (?????? {:.8f}, ???????: {:.0f})", 
                     market, balance, avg_buy_price);

            // [Phase 4] ?????????ÄÏ≤?????Ô¶???Ë™ò„àë?Ë¢Å‚ë•????????È•îÎÇÖ??????????SL/TP ??????®Î∫§Î•??≤„É´?????????È•îÎÇÖ???????
            const PersistedPosition* persisted = nullptr;
            for (const auto& pp : pending_reconcile_positions_) {
                if (pp.market == market && pp.stop_loss > 0.0) {
                    persisted = &pp;
                    break;
                }
            }

            double safe_stop_loss;
            double tp1, tp2;
            double be_trigger = 0.0, trail_start = 0.0;
            bool half_closed = false;

            if (persisted) {
                // [Phase 4] ???????Î≥ßÎÄ??Á≠åÎ®≤??????????????????Íæ©Î£ÜÔß?ù∞Ï≠????????(?????)
                safe_stop_loss = persisted->stop_loss;
                tp1 = persisted->take_profit_1;
                tp2 = persisted->take_profit_2;
                be_trigger = persisted->breakeven_trigger;
                trail_start = persisted->trailing_start;
                half_closed = persisted->half_closed;
                LOG_INFO("??????????∞ÎÆõ?Á≠åÎ?Í∫??ª„?Ë¢Å‚ë¶?????È•îÎÇÖ???????: {} SL={:.0f} TP1={:.0f} TP2={:.0f} BE={:.0f} TS={:.0f}",
                         market, safe_stop_loss, tp1, tp2, be_trigger, trail_start);
            } else {
                // ?????????ÄÏ≤????????????????????¥Î™ñ??????????????(????∫Ïñò?????????????????? ?????????≤Îê±Îπ??????âÎùø????±Îã™??????∫Ïñò????????
                double target_sl = avg_buy_price * 0.97;
                double upbit_limit_sl = config_.min_order_krw / (balance > 0 ? balance : 1e-9);

                if (upbit_limit_sl > avg_buy_price * 0.99) {
                    LOG_WARN("{} balance too small; using conservative stop-loss (qty {:.6f}, upbit_min_sl {:.0f})",
                             market, balance, upbit_limit_sl);
                    safe_stop_loss = target_sl;
                } else {
                    safe_stop_loss = std::max(target_sl, upbit_limit_sl);
                }
                tp1 = avg_buy_price * 1.010;
                tp2 = avg_buy_price * 1.015;
                LOG_WARN("??????????∞ÎÆõ?Á≠åÎ?Í∫??ª„?Ë¢Å‚ë¶??????????????: {} SL={:.0f} TP1={:.0f} TP2={:.0f} (?È•îÎÇÖ???????????????????????§Îú§??",
                         market, safe_stop_loss, tp1, tp2);
            }

            std::string recovered_strategy = "RECOVERED";
            auto recovered_it = recovered_strategy_map_.find(market);
            if (recovered_it != recovered_strategy_map_.end()) {
                recovered_strategy = recovered_it->second;
            }

            // ???????????®Î∫§Î•??≤„É´??????????ΩÁ∂≠????
            risk_manager_->enterPosition(
                market,
                avg_buy_price,
                balance,
                safe_stop_loss,
                tp1,
                tp2,
                recovered_strategy,
                be_trigger,
                trail_start
            );
            ++restored_positions;

            // [Phase 4] ???????±ÏÇ©??????????????Â´??????????®Î∫§Î•??≤„É´??????
            if (half_closed) {
                auto* pos = risk_manager_->getPosition(market);
                if (pos) {
                    pos->half_closed = true;
                    LOG_INFO("  ?????Îºø¬Ä??????•¬Ä?´Ï∏ß??????????∞≈?çÆ???????∞ÎÆõ?Á≠åÎ?Í∫??? {} (half_closed=true)", market);
                }
            }
        }
        
        if (!krw_found) {
            LOG_WARN("??È∂?Ö∫??????????KRW?????´Îäâ?? ??????ÍπÖÏ¶Ω?????????ÅÏ°Ñ! (??????Î©∏Í¥ú???????0????????????•Ïã≤Í∞?Åî???");
            risk_manager_->resetCapital(0.0);
        }

        // ===== ??????Â´???????ÄÎ∂æÍµù????????(????∫Ïñò???????????????) =====
        if (!pending_reconcile_positions_.empty()) {
            std::vector<std::string> missing_markets;
            for (const auto& pos : pending_reconcile_positions_) {
                if (!pos.market.empty() && wallet_markets.find(pos.market) == wallet_markets.end()) {
                    missing_markets.push_back(pos.market);
                }
            }

            std::map<std::string, double> price_map;
            if (!missing_markets.empty()) {
                try {
                    auto tickers = http_client_->getTickerBatch(missing_markets);
                    for (const auto& t : tickers) {
                        std::string market_code = t["market"].get<std::string>();
                        double trade_price = t["trade_price"].get<double>();
                        price_map[market_code] = trade_price;
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("????Î∏çÌàñ???‰∫?ªãÍº?∂≠?????????ËΩÖÎ∂Ω????????????????????®ÏÄ´Î????????????§Ïä£?? {}", e.what());
                }
            }

            const double fee_rate = Config::getInstance().getFeeRate();
            for (const auto& pos : pending_reconcile_positions_) {
                if (pos.market.empty()) {
                    continue;
                }
                if (wallet_markets.find(pos.market) != wallet_markets.end()) {
                    continue;
                }

                double exit_price = pos.entry_price;
                auto price_it = price_map.find(pos.market);
                if (price_it != price_map.end()) {
                    exit_price = price_it->second;
                }

                double entry_value = pos.entry_price * pos.quantity;
                double exit_value = exit_price * pos.quantity;
                double entry_fee = entry_value * fee_rate;
                double exit_fee = exit_value * fee_rate;
                double profit_loss = exit_value - entry_value - entry_fee - exit_fee;

                risk::TradeHistory trade;
                trade.market = pos.market;
                trade.entry_price = pos.entry_price;
                trade.exit_price = exit_price;
                trade.quantity = pos.quantity;
                trade.entry_time = pos.entry_time;
                trade.exit_time = getCurrentTimestampMs();
                trade.strategy_name = pos.strategy_name;
                trade.exit_reason = "manual_external";
                trade.signal_filter = pos.signal_filter;
                trade.signal_strength = pos.signal_strength;
                trade.market_regime = pos.market_regime;
                trade.liquidity_score = pos.liquidity_score;
                trade.volatility = pos.volatility;
                trade.expected_value = pos.expected_value;
                trade.reward_risk_ratio = pos.reward_risk_ratio;
                trade.profit_loss = profit_loss;
                trade.profit_loss_pct = (pos.entry_price > 0.0)
                    ? (exit_price - pos.entry_price) / pos.entry_price
                    : 0.0;
                trade.fee_paid = entry_fee + exit_fee;

                risk_manager_->appendTradeHistory(trade);

                nlohmann::json close_payload;
                close_payload["exit_price"] = exit_price;
                close_payload["quantity"] = pos.quantity;
                close_payload["reason"] = "manual_external";
                close_payload["gross_pnl"] = profit_loss;
                close_payload["strategy_name"] = pos.strategy_name;
                appendJournalEvent(
                    core::JournalEventType::POSITION_CLOSED,
                    pos.market,
                    "reconcile_external_close",
                    close_payload
                );

                if (strategy_manager_ && !pos.strategy_name.empty()) {
                    auto strategy_ptr = strategy_manager_->getStrategy(pos.strategy_name);
                    if (strategy_ptr) {
                        strategy_ptr->updateStatistics(pos.market, profit_loss > 0.0, profit_loss);
                    }
                }

                LOG_INFO("????Î∏çÌàñ???‰∫?ªãÍº?∂≠?????????ËΩÖÎ∂Ω????????? {} (?????Ë´õÎ™ÉÎß?? {}, ????? {:.0f})",
                         pos.market, pos.strategy_name, profit_loss);
                ++external_closes;
            }
        }

        LOG_INFO(
            "Account state sync summary: wallet_markets={}, reconcile_candidates={}, restored_positions={}, external_closes={}",
            wallet_markets.size(),
            reconcile_candidates,
            restored_positions,
            external_closes
        );
        pending_reconcile_positions_.clear();

        // ????∫Ïñò?????????????´Î??????Â£§Íµø?????èñ????????ÍøîÍ∫Ç?????????∫Ïñò??????????????????∞Î???????
        for (auto it = recovered_strategy_map_.begin(); it != recovered_strategy_map_.end(); ) {
            if (wallet_markets.find(it->first) == wallet_markets.end()) {
                it = recovered_strategy_map_.erase(it);
            } else {
                ++it;
            }
        }
        
        LOG_INFO("Account state synchronization completed");

    } catch (const std::exception& e) {
        LOG_ERROR("??È∂?Ö∫???????????????∞≈?çÆ???????????????????§Ïä£?? {}", e.what());
    }
}

void TradingEngine::runStatePersistence() {
    using namespace std::chrono_literals;
    while (state_persist_running_) {
        std::this_thread::sleep_for(30s);
        if (!state_persist_running_) {
            break;
        }
        saveState();
    }
}

void TradingEngine::appendJournalEvent(
    core::JournalEventType type,
    const std::string& market,
    const std::string& entity_id,
    const nlohmann::json& payload
) {
    if (!event_journal_) {
        return;
    }

    core::JournalEvent event;
    event.ts_ms = getCurrentTimestampMs();
    event.type = type;
    event.market = market;
    event.entity_id = entity_id;
    event.payload = payload;

    if (!event_journal_->append(event)) {
        LOG_WARN("Event journal append failed: type={}, market={}",
                 static_cast<int>(type), market);
    }
}

void TradingEngine::loadLearningState() {
    try {
        if (!learning_state_store_) {
            return;
        }

        auto loaded = learning_state_store_->load();
        if (!loaded.has_value()) {
            return;
        }

        const auto& policy = loaded->policy_params;
        if (policy.contains("dynamic_filter_value")) {
            dynamic_filter_value_ = std::clamp(
                policy.value("dynamic_filter_value", dynamic_filter_value_),
                0.35,
                0.70
            );
        }
        if (policy.contains("position_scale_multiplier")) {
            position_scale_multiplier_ = std::clamp(
                policy.value("position_scale_multiplier", position_scale_multiplier_),
                0.5,
                2.5
            );
        }
        if (policy.contains("market_hostility_ewma")) {
            market_hostility_ewma_ = std::clamp(
                policy.value("market_hostility_ewma", market_hostility_ewma_),
                0.0,
                1.0
            );
        }
        if (policy.contains("hostile_pause_scans_remaining")) {
            hostile_pause_scans_remaining_ = std::clamp(
                policy.value("hostile_pause_scans_remaining", hostile_pause_scans_remaining_),
                0,
                64
            );
        }

        LOG_INFO(
            "Learning state loaded (schema v{}, saved_at={}, filter={:.3f}, scale={:.2f}, hostility_ewma={:.3f}, pause_remaining={})",
            loaded->schema_version,
            loaded->saved_at_ms,
            dynamic_filter_value_,
            position_scale_multiplier_,
            market_hostility_ewma_,
            hostile_pause_scans_remaining_
        );
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load learning state: {}", e.what());
    }
}

void TradingEngine::saveLearningState() {
    try {
        if (!learning_state_store_) {
            return;
        }

        core::LearningStateSnapshot snapshot;
        snapshot.schema_version = core::LearningStateStoreJson::kCurrentSchemaVersion;
        snapshot.saved_at_ms = getCurrentTimestampMs();
        snapshot.policy_params = nlohmann::json::object();
        snapshot.policy_params["dynamic_filter_value"] = dynamic_filter_value_;
        snapshot.policy_params["position_scale_multiplier"] = position_scale_multiplier_;
        snapshot.policy_params["market_hostility_ewma"] = market_hostility_ewma_;
        snapshot.policy_params["hostile_pause_scans_remaining"] = hostile_pause_scans_remaining_;

        nlohmann::json bucket_stats = nlohmann::json::array();
        if (performance_store_) {
            for (const auto& [key, stats] : performance_store_->byBucket()) {
                nlohmann::json row;
                row["strategy_name"] = key.strategy_name;
                row["regime"] = static_cast<int>(key.regime);
                row["liquidity_bucket"] = key.liquidity_bucket;
                row["entry_archetype"] = key.entry_archetype;
                row["trades"] = stats.trades;
                row["wins"] = stats.wins;
                row["gross_profit"] = stats.gross_profit;
                row["gross_loss_abs"] = stats.gross_loss_abs;
                row["net_profit"] = stats.net_profit;
                bucket_stats.push_back(std::move(row));
            }
        }
        snapshot.bucket_stats = std::move(bucket_stats);
        snapshot.rollback_point = nlohmann::json::object();
        snapshot.rollback_point["dynamic_filter_value"] = dynamic_filter_value_;
        snapshot.rollback_point["position_scale_multiplier"] = position_scale_multiplier_;
        snapshot.rollback_point["market_hostility_ewma"] = market_hostility_ewma_;
        snapshot.rollback_point["hostile_pause_scans_remaining"] = hostile_pause_scans_remaining_;

        if (!learning_state_store_->save(snapshot)) {
            LOG_WARN("Learning state save failed");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save learning state: {}", e.what());
    }
}

void TradingEngine::saveState() {
    try {
        nlohmann::json state;
        state["version"] = 1;
        state["timestamp"] = getCurrentTimestampMs();
        state["snapshot_last_event_seq"] = event_journal_ ? event_journal_->lastSeq() : 0;
        state["dynamic_filter_value"] = dynamic_filter_value_;
        state["position_scale_multiplier"] = position_scale_multiplier_;
        state["market_hostility_ewma"] = market_hostility_ewma_;
        state["hostile_pause_scans_remaining"] = hostile_pause_scans_remaining_;

        // Trade history
        nlohmann::json history = nlohmann::json::array();
        for (const auto& trade : risk_manager_->getTradeHistory()) {
            nlohmann::json item;
            item["market"] = trade.market;
            item["entry_price"] = trade.entry_price;
            item["exit_price"] = trade.exit_price;
            item["quantity"] = trade.quantity;
            item["profit_loss"] = trade.profit_loss;
            item["profit_loss_pct"] = trade.profit_loss_pct;
            item["fee_paid"] = trade.fee_paid;
            item["entry_time"] = trade.entry_time;
            item["exit_time"] = trade.exit_time;
            item["strategy_name"] = trade.strategy_name;
            item["exit_reason"] = trade.exit_reason;
            item["signal_filter"] = trade.signal_filter;
            item["signal_strength"] = trade.signal_strength;
            item["market_regime"] = static_cast<int>(trade.market_regime);
            item["liquidity_score"] = trade.liquidity_score;
            item["volatility"] = trade.volatility;
            item["expected_value"] = trade.expected_value;
            item["reward_risk_ratio"] = trade.reward_risk_ratio;
            history.push_back(item);
        }
        state["trade_history"] = history;

        // Strategy stats
        nlohmann::json stats_json;
        if (strategy_manager_) {
            auto stats_map = strategy_manager_->getAllStatistics();
            for (const auto& [name, stats] : stats_map) {
                nlohmann::json s;
                s["total_signals"] = stats.total_signals;
                s["winning_trades"] = stats.winning_trades;
                s["losing_trades"] = stats.losing_trades;
                s["total_profit"] = stats.total_profit;
                s["total_loss"] = stats.total_loss;
                s["win_rate"] = stats.win_rate;
                s["avg_profit"] = stats.avg_profit;
                s["avg_loss"] = stats.avg_loss;
                s["profit_factor"] = stats.profit_factor;
                s["sharpe_ratio"] = stats.sharpe_ratio;
                stats_json[name] = s;
            }
        }
        state["strategy_stats"] = stats_json;

        // Position strategy mapping
        nlohmann::json position_map;
        for (const auto& pos : risk_manager_->getAllPositions()) {
            if (!pos.strategy_name.empty()) {
                position_map[pos.market] = pos.strategy_name;
            }
        }
        state["position_strategy_map"] = position_map;

        // Open positions (for external close reconciliation)
        nlohmann::json open_positions = nlohmann::json::array();
        for (const auto& pos : risk_manager_->getAllPositions()) {
            nlohmann::json p;
            p["market"] = pos.market;
            p["strategy_name"] = pos.strategy_name;
            p["entry_price"] = pos.entry_price;
            p["quantity"] = pos.quantity;
            p["entry_time"] = pos.entry_time;
            p["signal_filter"] = pos.signal_filter;
            p["signal_strength"] = pos.signal_strength;
            p["market_regime"] = static_cast<int>(pos.market_regime);
            p["liquidity_score"] = pos.liquidity_score;
            p["volatility"] = pos.volatility;
            p["expected_value"] = pos.expected_value;
            p["reward_risk_ratio"] = pos.reward_risk_ratio;
            // [Phase 4] ?????????????ÄÎ∂æÍµù????????????????????????ÄÏ≤???
            p["stop_loss"] = pos.stop_loss;
            p["take_profit_1"] = pos.take_profit_1;
            p["take_profit_2"] = pos.take_profit_2;
            p["breakeven_trigger"] = pos.breakeven_trigger;
            p["trailing_start"] = pos.trailing_start;
            p["half_closed"] = pos.half_closed;
            open_positions.push_back(p);
        }
        state["open_positions"] = open_positions;

        auto snapshot_path = utils::PathUtils::resolveRelativePath("state/snapshot_state.json");
        auto legacy_path = utils::PathUtils::resolveRelativePath("state/state.json");

        std::filesystem::create_directories(snapshot_path.parent_path());

        auto write_json = [&](const std::filesystem::path& path) {
            std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                throw std::runtime_error("Failed to open state file: " + path.string());
            }
            out << state.dump(2);
        };

        // Stage 3 deepening: primary snapshot path.
        write_json(snapshot_path);
        // Backward compatibility with existing tooling/loaders.
        write_json(legacy_path);

        LOG_INFO(
            "State snapshot saved: snapshot={}, legacy={}, last_event_seq={}",
            snapshot_path.string(),
            legacy_path.string(),
            state.value("snapshot_last_event_seq", 0)
        );

        saveLearningState();
    } catch (const std::exception& e) {
        LOG_ERROR("??????∞≈?çÆ?????????????§Ïä£?? {}", e.what());
    }
}

void TradingEngine::loadState() {
    try {
        auto snapshot_path = utils::PathUtils::resolveRelativePath("state/snapshot_state.json");
        auto legacy_path = utils::PathUtils::resolveRelativePath("state/state.json");
        std::filesystem::path state_path;

        if (std::filesystem::exists(snapshot_path)) {
            state_path = snapshot_path;
        } else if (std::filesystem::exists(legacy_path)) {
            state_path = legacy_path;
        } else {
            return;
        }

        std::ifstream in(state_path.string(), std::ios::binary);
        if (!in.is_open()) {
            return;
        }
        nlohmann::json state;
        in >> state;

        const long long snapshot_ts_ms = state.value("timestamp", 0LL);
        const std::uint64_t snapshot_last_seq = state.value(
            "snapshot_last_event_seq",
            static_cast<std::uint64_t>(0)
        );

        LOG_INFO(
            "State snapshot loaded: path={}, timestamp={}, last_event_seq={}",
            state_path.string(),
            snapshot_ts_ms,
            snapshot_last_seq
        );

        dynamic_filter_value_ = state.value("dynamic_filter_value", dynamic_filter_value_);
        position_scale_multiplier_ = state.value("position_scale_multiplier", position_scale_multiplier_);
        market_hostility_ewma_ = std::clamp(
            state.value("market_hostility_ewma", market_hostility_ewma_),
            0.0,
            1.0
        );
        hostile_pause_scans_remaining_ = std::clamp(
            state.value("hostile_pause_scans_remaining", hostile_pause_scans_remaining_),
            0,
            64
        );

        if (state.contains("trade_history") && state["trade_history"].is_array()) {
            std::vector<risk::TradeHistory> history;
            for (const auto& item : state["trade_history"]) {
                risk::TradeHistory trade;
                trade.market = item.value("market", "");
                trade.entry_price = item.value("entry_price", 0.0);
                trade.exit_price = item.value("exit_price", 0.0);
                trade.quantity = item.value("quantity", 0.0);
                trade.profit_loss = item.value("profit_loss", 0.0);
                trade.profit_loss_pct = item.value("profit_loss_pct", 0.0);
                trade.fee_paid = item.value("fee_paid", 0.0);
                trade.entry_time = item.value("entry_time", 0LL);
                trade.exit_time = item.value("exit_time", 0LL);
                trade.strategy_name = item.value("strategy_name", "");
                trade.exit_reason = item.value("exit_reason", "");
                trade.signal_filter = item.value("signal_filter", 0.5);
                trade.signal_strength = item.value("signal_strength", 0.0);
                trade.market_regime = static_cast<analytics::MarketRegime>(item.value("market_regime", static_cast<int>(analytics::MarketRegime::UNKNOWN)));
                trade.liquidity_score = item.value("liquidity_score", 0.0);
                trade.volatility = item.value("volatility", 0.0);
                trade.expected_value = item.value("expected_value", 0.0);
                trade.reward_risk_ratio = item.value("reward_risk_ratio", 0.0);
                history.push_back(trade);
            }
            risk_manager_->replaceTradeHistory(history);
            LOG_INFO("State restore: trade history {} loaded", history.size());
        }

        if (state.contains("strategy_stats") && state["strategy_stats"].is_object() && strategy_manager_) {
            for (const auto& strategy : strategy_manager_->getStrategies()) {
                auto info = strategy->getInfo();
                if (!state["strategy_stats"].contains(info.name)) {
                    continue;
                }
                const auto& s = state["strategy_stats"][info.name];
                strategy::IStrategy::Statistics stats;
                stats.total_signals = s.value("total_signals", 0);
                stats.winning_trades = s.value("winning_trades", 0);
                stats.losing_trades = s.value("losing_trades", 0);
                stats.total_profit = s.value("total_profit", 0.0);
                stats.total_loss = s.value("total_loss", 0.0);
                stats.win_rate = s.value("win_rate", 0.0);
                stats.avg_profit = s.value("avg_profit", 0.0);
                stats.avg_loss = s.value("avg_loss", 0.0);
                stats.profit_factor = s.value("profit_factor", 0.0);
                stats.sharpe_ratio = s.value("sharpe_ratio", 0.0);
                strategy->setStatistics(stats);
            }
            LOG_INFO("State restore: strategy statistics loaded");
        }

        recovered_strategy_map_.clear();
        if (state.contains("position_strategy_map") && state["position_strategy_map"].is_object()) {
            for (auto it = state["position_strategy_map"].begin(); it != state["position_strategy_map"].end(); ++it) {
                recovered_strategy_map_[it.key()] = it.value().get<std::string>();
            }
        }

        pending_reconcile_positions_.clear();
        if (state.contains("open_positions") && state["open_positions"].is_array()) {
            for (const auto& p : state["open_positions"]) {
                PersistedPosition pos;
                pos.market = p.value("market", "");
                pos.strategy_name = p.value("strategy_name", "");
                pos.entry_price = p.value("entry_price", 0.0);
                pos.quantity = p.value("quantity", 0.0);
                pos.entry_time = p.value("entry_time", 0LL);
                pos.signal_filter = p.value("signal_filter", 0.5);
                pos.signal_strength = p.value("signal_strength", 0.0);
                pos.market_regime = static_cast<analytics::MarketRegime>(p.value("market_regime", static_cast<int>(analytics::MarketRegime::UNKNOWN)));
                pos.liquidity_score = p.value("liquidity_score", 0.0);
                pos.volatility = p.value("volatility", 0.0);
                pos.expected_value = p.value("expected_value", 0.0);
                pos.reward_risk_ratio = p.value("reward_risk_ratio", 0.0);
                // [Phase 4] ?????????????ÄÎ∂æÍµù?????????????????????®Î∫§Î•??≤„É´??????
                pos.stop_loss = p.value("stop_loss", 0.0);
                pos.take_profit_1 = p.value("take_profit_1", 0.0);
                pos.take_profit_2 = p.value("take_profit_2", 0.0);
                pos.breakeven_trigger = p.value("breakeven_trigger", 0.0);
                pos.trailing_start = p.value("trailing_start", 0.0);
                pos.half_closed = p.value("half_closed", false);
                if (!pos.market.empty() && pos.entry_price > 0.0 && pos.quantity > 0.0) {
                    pending_reconcile_positions_.push_back(pos);
                }
            }
        }

        if (event_journal_) {
            const std::uint64_t replay_start_seq = (snapshot_last_seq > 0) ? (snapshot_last_seq + 1) : 1;
            auto replay_events = event_journal_->readFrom(replay_start_seq);

            std::map<std::string, PersistedPosition> open_map;
            for (const auto& pos : pending_reconcile_positions_) {
                if (!pos.market.empty()) {
                    open_map[pos.market] = pos;
                }
            }
            std::map<std::string, std::string> strategy_map = recovered_strategy_map_;
            std::vector<risk::TradeHistory> replay_trades;

            auto upsert_position = [&](const std::string& market) -> PersistedPosition& {
                auto it = open_map.find(market);
                if (it == open_map.end()) {
                    PersistedPosition pos;
                    pos.market = market;
                    it = open_map.emplace(market, std::move(pos)).first;
                }
                return it->second;
            };

            std::size_t replay_applied = 0;
            for (const auto& event : replay_events) {
                // Legacy snapshots do not carry seq watermark. In that case, replay only newer events by timestamp.
                if (snapshot_last_seq == 0 && snapshot_ts_ms > 0 && event.ts_ms <= snapshot_ts_ms) {
                    continue;
                }
                if (event.market.empty()) {
                    continue;
                }

                const auto& payload = event.payload;
                switch (event.type) {
                    case core::JournalEventType::POSITION_OPENED: {
                        PersistedPosition& pos = upsert_position(event.market);
                        pos.market = event.market;
                        pos.entry_price = payload.value("entry_price", pos.entry_price);
                        pos.quantity = payload.value("quantity", pos.quantity);
                        pos.entry_time = (pos.entry_time > 0) ? pos.entry_time : event.ts_ms;
                        const std::string strategy_name = payload.value("strategy_name", pos.strategy_name);
                        if (!strategy_name.empty()) {
                            pos.strategy_name = strategy_name;
                            strategy_map[event.market] = strategy_name;
                        }
                        if (payload.contains("stop_loss")) pos.stop_loss = payload.value("stop_loss", pos.stop_loss);
                        if (payload.contains("take_profit_1")) pos.take_profit_1 = payload.value("take_profit_1", pos.take_profit_1);
                        if (payload.contains("take_profit_2")) pos.take_profit_2 = payload.value("take_profit_2", pos.take_profit_2);
                        if (payload.contains("breakeven_trigger")) pos.breakeven_trigger = payload.value("breakeven_trigger", pos.breakeven_trigger);
                        if (payload.contains("trailing_start")) pos.trailing_start = payload.value("trailing_start", pos.trailing_start);
                        if (payload.contains("half_closed")) pos.half_closed = payload.value("half_closed", pos.half_closed);
                        ++replay_applied;
                        break;
                    }
                    case core::JournalEventType::FILL_APPLIED: {
                        std::string side = payload.value("side", "");
                        side = toLowerCopy(side);
                        if (!side.empty() && side != "buy") {
                            break;
                        }
                        const double fill_qty = payload.value("filled_volume", 0.0);
                        const double avg_price = payload.value("avg_price", 0.0);
                        if (fill_qty <= 0.0 || avg_price <= 0.0) {
                            break;
                        }
                        if (open_map.find(event.market) != open_map.end()) {
                            break;
                        }

                        PersistedPosition& pos = upsert_position(event.market);
                        pos.market = event.market;
                        pos.entry_price = avg_price;
                        pos.quantity = fill_qty;
                        pos.entry_time = event.ts_ms;
                        const std::string strategy_name = payload.value("strategy_name", pos.strategy_name);
                        if (!strategy_name.empty()) {
                            pos.strategy_name = strategy_name;
                            strategy_map[event.market] = strategy_name;
                        }
                        pos.stop_loss = payload.value("stop_loss", pos.stop_loss);
                        pos.take_profit_1 = payload.value("take_profit_1", pos.take_profit_1);
                        pos.take_profit_2 = payload.value("take_profit_2", pos.take_profit_2);
                        ++replay_applied;
                        break;
                    }
                    case core::JournalEventType::POSITION_REDUCED: {
                        auto it = open_map.find(event.market);
                        if (it == open_map.end()) {
                            break;
                        }
                        const double reduced_qty = payload.value("quantity", 0.0);
                        const double exit_price = payload.value("exit_price", 0.0);
                        double applied_qty = reduced_qty;
                        if (applied_qty > 0.0 && applied_qty <= it->second.quantity + 1e-12) {
                            if (exit_price > 0.0 && it->second.entry_price > 0.0) {
                                risk::TradeHistory trade;
                                trade.market = event.market;
                                trade.entry_price = it->second.entry_price;
                                trade.exit_price = exit_price;
                                trade.quantity = applied_qty;
                                trade.entry_time = it->second.entry_time;
                                trade.exit_time = event.ts_ms;
                                trade.strategy_name = it->second.strategy_name;
                                trade.exit_reason = payload.value("reason", std::string("position_reduced_replay"));
                                trade.signal_filter = it->second.signal_filter;
                                trade.signal_strength = it->second.signal_strength;
                                trade.market_regime = it->second.market_regime;
                                trade.liquidity_score = it->second.liquidity_score;
                                trade.volatility = it->second.volatility;
                                trade.expected_value = it->second.expected_value;
                                trade.reward_risk_ratio = it->second.reward_risk_ratio;
                                trade.profit_loss = payload.value(
                                    "gross_pnl",
                                    (exit_price - it->second.entry_price) * applied_qty
                                );
                                trade.profit_loss_pct = (it->second.entry_price > 0.0)
                                    ? ((exit_price - it->second.entry_price) / it->second.entry_price)
                                    : 0.0;
                                trade.fee_paid = 0.0;
                                replay_trades.push_back(std::move(trade));
                            }
                            it->second.quantity = std::max(0.0, it->second.quantity - applied_qty);
                        }
                        if (it->second.quantity <= 1e-12) {
                            open_map.erase(it);
                            strategy_map.erase(event.market);
                        }
                        ++replay_applied;
                        break;
                    }
                    case core::JournalEventType::POSITION_CLOSED: {
                        auto it = open_map.find(event.market);
                        if (it != open_map.end()) {
                            double close_qty = payload.value("quantity", 0.0);
                            if (close_qty <= 0.0 || close_qty > it->second.quantity + 1e-12) {
                                close_qty = it->second.quantity;
                            }
                            const double exit_price = payload.value("exit_price", 0.0);
                            if (close_qty > 0.0 && exit_price > 0.0 && it->second.entry_price > 0.0) {
                                risk::TradeHistory trade;
                                trade.market = event.market;
                                trade.entry_price = it->second.entry_price;
                                trade.exit_price = exit_price;
                                trade.quantity = close_qty;
                                trade.entry_time = it->second.entry_time;
                                trade.exit_time = event.ts_ms;
                                trade.strategy_name = it->second.strategy_name;
                                trade.exit_reason = payload.value("reason", std::string("position_closed_replay"));
                                trade.signal_filter = it->second.signal_filter;
                                trade.signal_strength = it->second.signal_strength;
                                trade.market_regime = it->second.market_regime;
                                trade.liquidity_score = it->second.liquidity_score;
                                trade.volatility = it->second.volatility;
                                trade.expected_value = it->second.expected_value;
                                trade.reward_risk_ratio = it->second.reward_risk_ratio;
                                trade.profit_loss = payload.value(
                                    "gross_pnl",
                                    (exit_price - it->second.entry_price) * close_qty
                                );
                                trade.profit_loss_pct = (it->second.entry_price > 0.0)
                                    ? ((exit_price - it->second.entry_price) / it->second.entry_price)
                                    : 0.0;
                                trade.fee_paid = 0.0;
                                replay_trades.push_back(std::move(trade));
                            }
                            open_map.erase(it);
                        }
                        strategy_map.erase(event.market);
                        ++replay_applied;
                        break;
                    }
                    default:
                        break;
                }
            }

            for (const auto& trade : replay_trades) {
                risk_manager_->appendTradeHistory(trade);
            }

            pending_reconcile_positions_.clear();
            for (const auto& [market, pos] : open_map) {
                if (market.empty() || pos.entry_price <= 0.0 || pos.quantity <= 0.0) {
                    continue;
                }
                pending_reconcile_positions_.push_back(pos);
            }
            recovered_strategy_map_ = std::move(strategy_map);

            if (replay_applied > 0) {
                LOG_INFO(
                    "State restore: journal replay applied {} event(s), reconstructed trades={}, open positions={}",
                    replay_applied,
                    replay_trades.size(),
                    pending_reconcile_positions_.size()
                );
            } else {
                LOG_INFO(
                    "State restore: no replay events applied (start_seq={}, journal_last_seq={})",
                    replay_start_seq,
                    event_journal_->lastSeq()
                );
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("??????∞≈?çÆ???????∞ÎÆõ?Á≠åÎ?Í∫??ª„?Ë¢Å‚ë¶???????????§Ïä£?? {}", e.what());
    }
}

// ===== [NEW] ?????Œº?úÂ™õ?Í±?Á≠åÎöÆÏ±∂Â§∑???????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤?????????????????(??????®Î∫§Î•??????????????????????????? =====

double TradingEngine::calculateDynamicFilterValue() {
    if (scanned_markets_.empty()) {
        return dynamic_filter_value_;
    }

    double total_volatility = 0.0;
    for (const auto& metrics : scanned_markets_) {
        total_volatility += metrics.volatility;
    }
    const double avg_volatility = total_volatility / static_cast<double>(scanned_markets_.size());

    // Base filter from market volatility.
    double new_filter_value = 0.40;
    if (avg_volatility < 0.3) {
        new_filter_value = 0.40 + (0.3 - avg_volatility) * 0.1667;  // tighter in calm markets
    } else if (avg_volatility > 0.7) {
        new_filter_value = 0.40 - (avg_volatility - 0.7) * 0.1667;  // looser in volatile markets
    }
    new_filter_value = std::clamp(new_filter_value, 0.35, 0.45);

    // Performance overlay from recent net PnL (already fee/slippage realized).
    auto history = risk_manager_->getTradeHistory();
    if (history.size() >= 20) {
        const size_t sample_n = std::min<size_t>(60, history.size());
        double gross_profit = 0.0;
        double gross_loss_abs = 0.0;
        double sum_pnl = 0.0;
        int wins = 0;

        size_t seen = 0;
        for (auto it = history.rbegin(); it != history.rend() && seen < sample_n; ++it, ++seen) {
            const double pnl = it->profit_loss;
            sum_pnl += pnl;
            if (pnl > 0.0) {
                gross_profit += pnl;
                ++wins;
            } else if (pnl < 0.0) {
                gross_loss_abs += std::abs(pnl);
            }
        }

        const double expectancy = sum_pnl / static_cast<double>(sample_n);
        const double profit_factor =
            (gross_loss_abs > 1e-12) ? (gross_profit / gross_loss_abs) : ((gross_profit > 1e-12) ? 99.9 : 0.0);
        const double win_rate = static_cast<double>(wins) / static_cast<double>(sample_n);

        if (expectancy < 0.0 || profit_factor < 1.0) {
            new_filter_value += 0.02; // tighten entries when edge is negative
        } else if (expectancy > 0.0 && profit_factor > 1.2 && win_rate >= 0.50) {
            new_filter_value -= 0.01;
        }
    }

    new_filter_value = std::clamp(new_filter_value, 0.35, 0.55);
    if (std::abs(new_filter_value - dynamic_filter_value_) > 0.01) {
        LOG_INFO("Dynamic filter update: {:.3f} -> {:.3f} (avg_vol {:.3f})",
                 dynamic_filter_value_, new_filter_value, avg_volatility);
    }

    dynamic_filter_value_ = new_filter_value;
    return dynamic_filter_value_;
}

// ===== [NEW] ???????? ?????Ë´õÎ™ÉÎßàÔ¶´???????????????????(Win Rate & Profit Factor ??????????????? =====

double TradingEngine::calculatePositionScaleMultiplier() {
    // ????????????????:
    // Win Rate >= 60% AND Profit Factor >= 1.5 ?????????? ???????éÏ≤é?Â´???®Î£∏Ïµ??
    // 
    // ??????????Ë´õÎ™ÉÎßàÔ¶´?????????Ô¶âÏï∏???Á≠????
    // - WR < 45% || PF < 1.0: 0.5??(??????????≤ÎÄ????????Ë¢Å‚ë£????
    // - 45% <= WR < 50% || 1.0 <= PF < 1.2: 0.75??(??????®Î∫§Î•????
    // - 50% <= WR < 60% || 1.2 <= PF < 1.5: 1.0??(???)
    // - WR >= 60% && PF >= 1.5: 1.5??2.5??(???)
    
    auto metrics = risk_manager_->getRiskMetrics();
    
    // ????∫Ïñò?ÅÁ≠å?Ôº???????????????????±ÏÇ©?????????????????âÎ®Æ??????????? ?????Ë´õÎ™ÉÎßàÔ¶´??????????
    if (metrics.total_trades < 20) {
        LOG_INFO("Not enough trades for position scaling ({}/20). keep 1.0x", metrics.total_trades);
        return 1.0;
    }
    
    double win_rate = metrics.win_rate;
    double profit_factor = metrics.profit_factor;
    
    double new_multiplier;
    
    if (win_rate < 0.45 || profit_factor < 1.0) {
        // ?????Êø??Ä??????§Ïä¢??∂ÌÅ∫???????????æÎ?ÎÆ??? ???????Ê∫êÎÇÜ???????????????≤ÎÄ????????Ë¢Å‚ë£????
        new_multiplier = 0.5;
    } else if (win_rate < 0.50 || profit_factor < 1.2) {
        // ??????®Î∫§Î•??????????Êø??Ä??????§Ïä¢??∂ÌÅ∫??????????????????????
        new_multiplier = 0.75;
    } else if (win_rate < 0.60 || profit_factor < 1.5) {
        // ??? ?????Êø??Ä??????§Ïä¢??∂ÌÅ∫?????????????????
        new_multiplier = 1.0;
    } else {
        // ??????????????Êø??Ä??????§Ïä¢??∂ÌÅ∫??????? ???????´Î?????
        // PF?? WR???????????????????È•îÎÇÖ?????????????Ë´õÎ™ÉÎßàÔ¶´?????????Ô¶âÏï∏???Á≠????
        // WR 60%~75%: 1.5??2.0?? PF 1.5~2.5: ??????Ë¢Å‚ë£??? 0.25??
        double wr_bonus = (win_rate - 0.60) * 10.0;  // 0~1.5
        double pf_bonus = std::min(0.5, (profit_factor - 1.5) * 0.5);  // 0~0.5
        new_multiplier = 1.5 + wr_bonus + pf_bonus;
        new_multiplier = std::min(2.5, new_multiplier);  // ????∫Ïñò????????? 2.5??
    }
    
    // ???????
    if (std::abs(new_multiplier - position_scale_multiplier_) > 0.01) {
        LOG_INFO("Position scale update: {:.2f}x -> {:.2f}x (WR: {:.1f}%, PF: {:.2f}, trades: {})",
                 position_scale_multiplier_, new_multiplier,
                 win_rate * 100.0, profit_factor, metrics.total_trades);
    }
    
    position_scale_multiplier_ = new_multiplier;
    return new_multiplier;
}

// ===== [NEW] ML ???????????????????∫Ïñò????????????????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤?????????? =====

void TradingEngine::learnOptimalFilterValue() {
    // historical P&L ???????????????????????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤?????????????Êø??Ä?Ô¶?????øÌèë??Î®•Îµæ???????????Ï´???
    // ?????????
    // 1. ????∫Ïñò?ÅÁ≠å?Ôº???????????????signal_filter ??????????????????????????∫Ïñò?ÅÁ≠å?Ôº????????????????Ï´???????
    // 2. ????????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤?????????????????Êø??Ä?Ô¶?????øÌèë??Î®•Îµæ?????∫Ïñò????????????????????(Win Rate, Profit Factor, Sharpe Ratio)
    // 3. ????∫Ïñò???????????????Êø??Ä??????§Ïä¢??∂ÌÅ∫????????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤???????????Ë¢Å‚ë£????
    
    auto history = risk_manager_->getTradeHistory();
    
    if (history.size() < 50) {
        LOG_INFO("Not enough samples for filter learning ({}/50), skipping", history.size());
        return;
    }
    
    // ??????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤????????????∫Ïñò?ÅÁ≠å?Ôº????????????????Ï´??????????????Êø??Ä?Ô¶?????øÌèë??Î®•Îµæ?????????????
    std::map<double, std::vector<TradeHistory>> trades_by_filter;
    std::map<double, std::vector<double>> returns_by_filter;  // Sharpe Ratio ?????????????
    
    // ??????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤????????????(0.45 ~ 0.55, 0.01 ????????®Î∫§ÍΩ?
    for (double filter = 0.45; filter <= 0.55; filter += 0.01) {
        trades_by_filter[filter] = std::vector<TradeHistory>();
        returns_by_filter[filter] = std::vector<double>();
    }
    
    // 1. ????∫Ïñò?ÅÁ≠å?Ôº??????????????????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤?????????????????????Ï´???????
    for (const auto& trade : history) {
        // signal_filter?????????´Î????????????´Î????????????????0.01 ????????®Î∫§ÍΩ?????????Ë´õÎ™ÉÎßàÔ¶´?????????‰∫?ÇÉÏΩ?????
        double rounded_filter = std::round(trade.signal_filter * 100.0) / 100.0;
        
        // ???????????????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤???????????????ÄÎ∂æÍµù??????????
        if (rounded_filter >= 0.45 && rounded_filter <= 0.55) {
            trades_by_filter[rounded_filter].push_back(trade);
            returns_by_filter[rounded_filter].push_back(trade.profit_loss_pct);
        }
    }
    
    // 2. ????????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤?????????????????Êø??Ä?Ô¶?????øÌèë??Î®•Îµæ???????????Ï´???
    struct FilterPerformance {
        double filter_value;
        int trade_count;
        double win_rate;
        double avg_return;
        double profit_factor;
        double sharpe_ratio;
        double total_pnl;
        
        FilterPerformance()
            : filter_value(0), trade_count(0), win_rate(0)
            , avg_return(0), profit_factor(0), sharpe_ratio(0), total_pnl(0)
        {}
    };
    
    std::map<double, FilterPerformance> performances;
    double best_sharpe = -999.0;
    double best_filter = 0.5;
    
    for (auto& [filter_val, trades] : trades_by_filter) {
        if (trades.empty()) continue;
        
        FilterPerformance perf;
        perf.filter_value = filter_val;
        perf.trade_count = static_cast<int>(trades.size());
        
        // Win Rate ????????????
        int winning_trades = 0;
        double total_profit = 0.0;
        double total_loss = 0.0;  // ??????∫Î∏ç????????????
        
        for (const auto& trade : trades) {
            if (trade.profit_loss > 0) {
                winning_trades++;
                total_profit += trade.profit_loss;
            } else {
                total_loss += std::abs(trade.profit_loss);  // ??????∫Î∏ç?????????????????´Î????????
            }
        }
        
        perf.win_rate = static_cast<double>(winning_trades) / trades.size();
        perf.total_pnl = total_profit - total_loss;
        
        // Profit Factor ????????????(????????®ÎöÆÎº??∞Î????/ ???????
        perf.profit_factor = (total_loss > 0) ? (total_profit / total_loss) : total_profit;
        
        // ???????????®ÎöÆÎº??∞Î?????
        perf.avg_return = perf.total_pnl / trades.size();
        
        // Sharpe Ratio ????????????(??????Ï¢äÌã£?????Ê∫êÎÇÜÏ°??????????????????????®ÎöÆÎº??∞Î?????
        const auto& returns = returns_by_filter[filter_val];
        if (returns.size() > 1) {
            double mean_return = 0.0;
            for (double ret : returns) {
                mean_return += ret;
            }
            mean_return /= returns.size();
            
            // ??????ÄÎ∂æÍµù?????????íÎ∫£???????????????
            double variance = 0.0;
            for (double ret : returns) {
                double diff = ret - mean_return;
                variance += diff * diff;
            }
            variance /= returns.size();
            double std_dev = std::sqrt(variance);
            
            // Sharpe Ratio = (???????????®ÎöÆÎº??∞Î?????- ????ÍøîÍ∫Ç???????????? / ??????ÄÎ∂æÍµù?????????íÎ∫£???
            // ????ÍøîÍ∫Ç????????????0???????????????´Î?????
            perf.sharpe_ratio = (std_dev > 0.0001) ? (mean_return / std_dev) : 0.0;
        }
        
        performances[filter_val] = perf;
        
        // ????∫Ïñò????????????????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤?????????È∂?Ö∫????????(Sharpe Ratio ???????)
        if (perf.sharpe_ratio > best_sharpe) {
            best_sharpe = perf.sharpe_ratio;
            best_filter = filter_val;
        }
        
        LOG_INFO("Filter {:.2f}: trades {}, win {:.1f}%, PF {:.2f}, Sharpe {:.3f}, net {:.0f}",
                 filter_val, perf.trade_count, perf.win_rate * 100.0, 
                 perf.profit_factor, perf.sharpe_ratio, perf.total_pnl);
    }
    
    // 3. ??Ô¶âÏï∏???Á≠??????????????Ï´???????????Ë¢Å‚ë£????
    // ??????Ë¢Å‚ë£??? ??????????????? Win Rate >= 50% ??Profit Factor >= 1.2 ??????????≤ÎÄ??(???????éÏ≤é?Â´????
    std::vector<double> qualified_filters;
    for (auto& [filter_val, perf] : performances) {
        if (perf.win_rate >= 0.50 && perf.profit_factor >= 1.2 && perf.trade_count >= 10) {
            qualified_filters.push_back(filter_val);
        }
    }
    
    if (!qualified_filters.empty()) {
        // ????????????????????≤ÎÄ????????¨Í≥£Î≤ÄÂ´?????Íæ§Îô¥????Sharpe Ratio ????∫Ïñò????????????????È∂?Ö∫????????
        double best_qualified_sharpe = -999.0;
        for (double f : qualified_filters) {
            if (performances[f].sharpe_ratio > best_qualified_sharpe) {
                best_qualified_sharpe = performances[f].sharpe_ratio;
                best_filter = f;
            }
        }
        
        LOG_INFO("ML filter learning (qualified set):");
        LOG_INFO("  best filter {:.2f} (Sharpe {:.3f}, win {:.1f}%, PF {:.2f})",
                 best_filter, best_qualified_sharpe,
                 performances[best_filter].win_rate * 100.0,
                 performances[best_filter].profit_factor);
    } else {
        // ????????????????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤????????????õ¬Ä ?????????¥Î™ñ??ÍøîÍ∫Ç?????Ë£ïÎºò????????Íæ©Î£ÜÔß?ù∞Ï≠??????Sharpe ????∫Ïñò????????????
        LOG_WARN("ML filter learning fallback (no qualified set).");
        LOG_WARN("  best Sharpe filter {:.2f} (Sharpe {:.3f})", best_filter, best_sharpe);
    }
    
    // [FIX] ?????Œº?úÂ™õ?Í±?Á≠åÎöÆÏ±∂Â§∑???????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤??????????????Í≥∑ÎôÉ???????ÑÏèÖÏ±∂Ôßç???(????∫Ïñò????????????ÁππÎ®Æ?èÂ†â????§Ïä¢???????????Ë´õÎ™ÉÎßàÔ¶´???????
    if (std::abs(best_filter - dynamic_filter_value_) > 0.001) {
        double direction = (best_filter > dynamic_filter_value_) ? 1.0 : -1.0;
        dynamic_filter_value_ += direction * 0.01; // 0.01???????
        dynamic_filter_value_ = std::clamp(dynamic_filter_value_, 0.45, 0.55);
        
        LOG_INFO("Dynamic filter nudged: {:.2f} -> {:.2f}", 
                 dynamic_filter_value_ - (direction * 0.01), dynamic_filter_value_);
    }
    
    // ??????????≤ÎÄ???????Êø??Ä?Ô¶?????øÌèë??Î®•Îµæ??????????(??????Ë¢Å‚ë£??????????????Ï´????
    filter_performance_history_[best_filter] = performances[best_filter].win_rate;
}

// ===== [NEW] Prometheus ????∫Ïñò??????????Ô¶´ÎöÆ????????ÄÎ∂æÍµù???????=====

std::string TradingEngine::exportPrometheusMetrics() const {
    // Prometheus ???ÄÎ∂æÍµù???????????§Ôºò???????∫Ïñò??????????Ô¶´ÎöÆ?????????????????????πÎïüÔßíÎÖπ????
    // Grafana?? ???????¨Í≥£Î™?????È•îÎÇÖ????????????????Íæ®Íµ¥???????∫Ïñò???????????∫Ïñú???????????ªÍππ???????∫Ïñò????????
    
    auto metrics = risk_manager_->getRiskMetrics();
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    std::ostringstream oss;
    
    // ????∫Ïñò????????????????(??? ??????Ô¶´ÎöÆ???êÏ≠ï???????Ë¢Å‚ë£???)
    oss << "# HELP autolife_state AutoLife ????∫Ïñò?ÅÁ≠å?Ôº??????????Ô¶?????????Â´???????ÄÎ∂æÍµù?????????n";
    oss << "# TYPE autolife_state gauge\n";

    // ????????????§„àá??????????Ô¶´ÎöÆ???êÏ≠ï?
    oss << "# HELP autolife_capital_total ???????(KRW)\n";
    oss << "# TYPE autolife_capital_total gauge\n";
    oss << "# HELP autolife_capital_available ???????????´Î?????????Œ≥?Î≥•Í±ô??????????????????????´Î?????????? KRW)\n";
    oss << "# TYPE autolife_capital_available gauge\n";
    oss << "# HELP autolife_capital_invested ??????????¨Í≥£Î≤ÄÂ´?????Íæ§Îô¥?????????¥ÏèÇÍµ??????????????È•îÎÇÖ?????????????????????? KRW)\n";
    oss << "# TYPE autolife_capital_invested gauge\n";

    // ????????????§„àá??????????Ô¶´ÎöÆ???êÏ≠ï?
    oss << "# HELP autolife_pnl_realized ??????????????????Íæ©Î£ÜÔß?ù∞Ï≠?? KRW)\n";
    oss << "# TYPE autolife_pnl_realized gauge\n";
    oss << "# HELP autolife_pnl_unrealized ??????∞Ïó®????Êø°„Çç?êÊè∂???????????????Íæ©Î£ÜÔß?ù∞Ï≠??????????????, KRW)\n";
    oss << "# TYPE autolife_pnl_unrealized gauge\n";
    oss << "# HELP autolife_pnl_total ????????????????????∞Ïó®????Êø°„Çç?êÊè∂???? KRW)\n";
    oss << "# TYPE autolife_pnl_total gauge\n";
    oss << "# HELP autolife_pnl_total_pct ??????Íæ©Î£ÜÔß?ù∞Ï≠?????????????????®ÎöÆÎº??∞Î?????%)\n";
    oss << "# TYPE autolife_pnl_total_pct gauge\n";

    // ??????Ï¢äÌã£?????Ê∫êÎÇÜÏ°??????????§„àá??????????Ô¶´ÎöÆ???êÏ≠ï?
    oss << "# HELP autolife_drawdown_max ????∫Ïñò????????? ??????Íæ©Î£ÜÔß?ù∞Ï≠?????????????????????\n";
    oss << "# TYPE autolife_drawdown_max gauge\n";
    oss << "# HELP autolife_drawdown_current ??????Íæ©Î£ÜÔß?ù∞Ï≠?????????????????????\n";
    oss << "# TYPE autolife_drawdown_current gauge\n";

    // ????????????§„àá??????????Ô¶´ÎöÆ???êÏ≠ï?
    oss << "# HELP autolife_positions_active ??????Íæ©Î£ÜÔß?ù∞Ï≠????????®Î∫§Î•???? ???????n";
    oss << "# TYPE autolife_positions_active gauge\n";
    oss << "# HELP autolife_positions_max ???????éÏ≤é?Â´???®Î£∏Ïµ??????∫Ïñò????????? ???????n";
    oss << "# TYPE autolife_positions_max gauge\n";

    // ????∫Ïñò?ÅÁ≠å?Ôº?????????????????Ô¶´ÎöÆ???êÏ≠ï?
    oss << "# HELP autolife_trades_total ??????Íæ©Î£ÜÔß?ù∞Ï≠??????∫Ïñò?ÅÁ≠å?Ôº????????n";
    oss << "# TYPE autolife_trades_total counter\n";
    oss << "# HELP autolife_trades_winning ??????Íæ©Î£ÜÔß?ù∞Ï≠????????®ÎöÆÎº??∞Î????????∫Ïñò?ÅÁ≠å?Ôº????????n";
    oss << "# TYPE autolife_trades_winning counter\n";
    oss << "# HELP autolife_trades_losing ??????Íæ©Î£ÜÔß?ù∞Ï≠???????????∫Ïñò?ÅÁ≠å?Ôº????????n";
    oss << "# TYPE autolife_trades_losing counter\n";

    // ?????Êø??Ä??????§Ïä¢??∂ÌÅ∫??????∫Ïñò??????????????Ô¶´ÎöÆ???êÏ≠ï?
    oss << "# HELP autolife_winrate ??????Î∞∏Î∏∂Á≠åÎ??????????0~1)\n";
    oss << "# TYPE autolife_winrate gauge\n";
    oss << "# HELP autolife_profit_factor ??????®ÎöÆÎº??∞Î???????Ô¶???Profit Factor)\n";
    oss << "# TYPE autolife_profit_factor gauge\n";
    oss << "# HELP autolife_sharpe_ratio ???????????∫Ïñò?????????????Êø??Ä??????§Ïä¢??∂ÌÅ∫??????∫Ïñò?????????\n";
    oss << "# TYPE autolife_sharpe_ratio gauge\n";

    // ????Ô¶?????????Â´??????????Ô¶´ÎöÆ???êÏ≠ï?
    oss << "# HELP autolife_engine_running ????Ô¶??????????? ??????Â´????1=??????????0=???)\n";
    oss << "# TYPE autolife_engine_running gauge\n";
    oss << "# HELP autolife_engine_scans_total ??????????????ÍøîÍ∫Ç??????????????øÎê∞–©\n";
    oss << "# TYPE autolife_engine_scans_total counter\n";
    oss << "# HELP autolife_engine_signals_total ???????πÎïüÔßíÎÖπ??????????????âÎ†±????????îÁëó????Ï∂??n";
    oss << "# TYPE autolife_engine_signals_total counter\n";

    // ?????Œº?úÂ™õ?Í±?Á≠åÎöÆÏ±∂Â§∑???????????≤ÎÄ?????????????Ô¶´ÎöÆ???êÏ≠ï?
    oss << "# HELP autolife_filter_value_dynamic ?????Œº?úÂ™õ?Í±?Á≠åÎöÆÏ±∂Â§∑???????????≤ÎÄ?????âÎ®Æ??Í≥ïÍ≤≤?????(0~1)\n";
    oss << "# TYPE autolife_filter_value_dynamic gauge\n";
    oss << "# HELP autolife_position_scale_multiplier ???????? ?????Ë´õÎ™ÉÎßàÔ¶´???????n";
    oss << "# TYPE autolife_position_scale_multiplier gauge\n";

    // ????Ô¶???????∫Ïñò?ÅÁ≠å?Ôº??????????∫Ïñò??????????Ô¶´ÎöÆ???????????Ô¶´ÎöÆ???êÏ≠ï?
    oss << "# HELP autolife_buy_orders_total ??????Íæ©Î£ÜÔß?ù∞Ï≠??????∫Ïñò??????????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠????n";
    oss << "# TYPE autolife_buy_orders_total counter\n";
    oss << "# HELP autolife_sell_orders_total ??????Íæ©Î£ÜÔß?ù∞Ï≠??????∫Ïñò??????????Î∂∫Î™≠??????????Ë´õÎ™Ñ???ËΩÖÎ∂Ω??????Í≥∏Îí≠????n";
    oss << "# TYPE autolife_sell_orders_total counter\n";
    oss << "# HELP autolife_pnl_cumulative ??????Íæ©Î£ÜÔß?ù∞Ï≠???????????????????????????ÍπÖÎºÇ??????????∑Î™ÑÍµ???? KRW)\n";
    oss << "# TYPE autolife_pnl_cumulative gauge\n";
    
    // 1. ????????????§„àá????????∫Ïñò??????????Ô¶´ÎöÆ?????
    oss << "autolife_capital_total{mode=\"" 
        << (config_.mode == TradingMode::LIVE ? "LIVE" : "PAPER") << "\"} "
        << metrics.total_capital << " " << timestamp_ms << "\n";
    
    oss << "autolife_capital_available{} " << metrics.available_capital << " " << timestamp_ms << "\n";
    oss << "autolife_capital_invested{} " << metrics.invested_capital << " " << timestamp_ms << "\n";
    
    // 2. ????????????§„àá????????∫Ïñò??????????Ô¶´ÎöÆ?????
    oss << "autolife_pnl_realized{} " << metrics.realized_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_unrealized{} " << metrics.unrealized_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_total{} " << metrics.total_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_total_pct{} " << metrics.total_pnl_pct << " " << timestamp_ms << "\n";
    
    // 3. ??????Ï¢äÌã£?????Ê∫êÎÇÜÏ°??????????§„àá????????∫Ïñò??????????Ô¶´ÎöÆ?????
    oss << "autolife_drawdown_max{} " << metrics.max_drawdown << " " << timestamp_ms << "\n";
    oss << "autolife_drawdown_current{} " << metrics.current_drawdown << " " << timestamp_ms << "\n";
    
    // 4. ????????????§„àá????????∫Ïñò??????????Ô¶´ÎöÆ?????
    oss << "autolife_positions_active{} " << metrics.active_positions << " " << timestamp_ms << "\n";
    oss << "autolife_positions_max{} " << config_.max_positions << " " << timestamp_ms << "\n";
    
    // 5. ????∫Ïñò?ÅÁ≠å?Ôº???????????
    oss << "autolife_trades_total{} " << metrics.total_trades << " " << timestamp_ms << "\n";
    oss << "autolife_trades_winning{} " << metrics.winning_trades << " " << timestamp_ms << "\n";
    oss << "autolife_trades_losing{} " << metrics.losing_trades << " " << timestamp_ms << "\n";
    
    // 6. ????∫Ïñò?ÅÁ≠å?Ôº???????????Êø??Ä??????§Ïä¢??∂ÌÅ∫??????∫Ïñò????????
    oss << "autolife_winrate{} " << metrics.win_rate << " " << timestamp_ms << "\n";
    oss << "autolife_profit_factor{} " << metrics.profit_factor << " " << timestamp_ms << "\n";
    oss << "autolife_sharpe_ratio{} " << metrics.sharpe_ratio << " " << timestamp_ms << "\n";
    
    // 7. ????Ô¶?????????Â´????????∫Ïñò??????????Ô¶´ÎöÆ?????
    oss << "autolife_engine_running{} " << (running_ ? 1 : 0) << " " << timestamp_ms << "\n";
    oss << "autolife_engine_scans_total{} " << total_scans_ << " " << timestamp_ms << "\n";
    oss << "autolife_engine_signals_total{} " << total_signals_ << " " << timestamp_ms << "\n";
    
    // 8. [NEW] ?????Œº?úÂ™õ?Í±?Á≠åÎöÆÏ±∂Â§∑???????????≤ÎÄ???????????? ????∫Ïñò??????????Ô¶´ÎöÆ?????
    oss << "autolife_filter_value_dynamic{} " << dynamic_filter_value_ << " " << timestamp_ms << "\n";
    oss << "autolife_position_scale_multiplier{} " << position_scale_multiplier_ << " " << timestamp_ms << "\n";
    
    // 9. [NEW] ????∫Ïñò?ÅÁ≠å?Ôº??????????Ô¶???????∫Ïñò??????????Ô¶´ÎöÆ?????
    oss << "autolife_buy_orders_total{} " << prometheus_metrics_.total_buy_orders << " " << timestamp_ms << "\n";
    oss << "autolife_sell_orders_total{} " << prometheus_metrics_.total_sell_orders << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_cumulative{} " << prometheus_metrics_.cumulative_realized_pnl << " " << timestamp_ms << "\n";
    
    oss << "# End of AutoLife Metrics\n";
    
    return oss.str();
}

// [NEW] Prometheus HTTP ???È•îÎÇÖ????????È∂?????????ÔßèÍªäÎ±?
void TradingEngine::runPrometheusHttpServer(int port) {
    prometheus_server_port_ = port;
    prometheus_server_running_ = true;
    
    LOG_INFO("Prometheus HTTP ???ÍøîÍ∫Ç??????ÂΩ????ÍøîÍ∫Ç???ÂΩ±¬Ä??∞ƒ?(???? {})", port);
    
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        LOG_ERROR("WSAStartup failed");
        prometheus_server_running_ = false;
        return;
    }
    
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        LOG_ERROR("Socket creation failed");
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    // ????????????????éÏ≤é?Â´???(TIME_WAIT ??????Â´????????????????????????´Î?????
    int reuse = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, 
                   reinterpret_cast<char*>(&reuse), sizeof(reuse)) < 0) {
        LOG_WARN("setsockopt(SO_REUSEADDR) failed");
    }
    
    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<u_short>(port));
    
    // Use inet_pton instead of deprecated inet_addr
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) != 1) {
        LOG_ERROR("inet_pton failed");
        closesocket(listen_socket);
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        LOG_ERROR("bind ???????§Ïä£??(???? {})", port);
        closesocket(listen_socket);
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    if (listen(listen_socket, 5) == SOCKET_ERROR) {
        LOG_ERROR("listen failed");
        closesocket(listen_socket);
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    LOG_INFO("Prometheus ?È•îÎÇÖ?????????ÔßéÎ©∏??????ÍøîÍ∫Ç??????ÂΩ??????Íæ®„Åç?Á≠åÏöå?Ä????é≥????????Ë´õÎ™ÉÎß??(http://localhost:{}/metrics)", port);
    
    // ???È•îÎÇÖ????????È∂??????∑Îß£?´Î™≠????
    while (prometheus_server_running_) {
        sockaddr_in client_addr = {};
        int client_addr_size = sizeof(client_addr);
        
        // 5??????????Íæ©Î£ÜÔß?ù∞Ï≠??????????éÏ≤é?Â´???
        timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_socket, &read_fds);
        
        int select_result = select(0, &read_fds, nullptr, nullptr, &timeout);
        if (select_result == 0) {
            // ????????Íæ©Î£ÜÔß?ù∞Ï≠???- ????????Íæ®Íµ¥??????∫Ïñò???????????
            continue;
        }
        if (select_result == SOCKET_ERROR) {
            LOG_WARN("select failed");
            break;
        }
        
        SOCKET client_socket = accept(listen_socket, 
                                      reinterpret_cast<sockaddr*>(&client_addr), 
                                      &client_addr_size);
        if (client_socket == INVALID_SOCKET) {
            LOG_WARN("accept failed");
            continue;
        }
        
        // HTTP ????Ô¶????????????ôÎÄ????
        char buffer[4096] = {0};
        int recv_result = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (recv_result > 0) {
            buffer[recv_result] = '\0';
            std::string request(buffer);
            
            // GET /metrics ???ÄÎ∂æÍµù??????????
            if (request.find("GET /metrics") == 0) {
                // Prometheus ????∫Ïñò??????????Ô¶´ÎöÆ????????????πÎïüÔßíÎÖπ????
                std::string metrics = exportPrometheusMetrics();
                
                // HTTP ?????????????
                std::ostringstream response;
                response << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                         << "Content-Length: " << metrics.length() << "\r\n"
                         << "Connection: close\r\n"
                         << "\r\n"
                         << metrics;
                
                std::string response_str = response.str();
                send(client_socket, response_str.c_str(), static_cast<int>(response_str.length()), 0);
            } 
            else if (request.find("GET /health") == 0) {
                // ????????∫Ïñò???????????????Ô¶???????
                std::string health_response = "OK";
                std::ostringstream response;
                response << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/plain; charset=utf-8\r\n"
                         << "Content-Length: " <<health_response.length() << "\r\n"
                         << "Connection: close\r\n"
                         << "\r\n"
                         << health_response;
                
                std::string response_str = response.str();
                send(client_socket, response_str.c_str(), static_cast<int>(response_str.length()), 0);
            }
            else {
                // 404 ???????
                std::string error_response = "Not Found";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n"
                         << "Content-Type: text/plain; charset=utf-8\r\n"
                         << "Content-Length: " << error_response.length() << "\r\n"
                         << "Connection: close\r\n"
                         << "\r\n"
                         << error_response;
                
                std::string response_str = response.str();
                send(client_socket, response_str.c_str(), static_cast<int>(response_str.length()), 0);
            }
        }
        
        closesocket(client_socket);
    }
    
    closesocket(listen_socket);
    WSACleanup();
    
    LOG_INFO("Prometheus HTTP server stopped");
}

} // namespace engine
} // namespace autolife
