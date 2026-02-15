#include "backtest/BacktestEngine.h"
#include "common/Logger.h"
#include "common/PathUtils.h"
#include "core/execution/OrderLifecycleStateMachine.h"
#include "core/execution/ExecutionUpdateSchema.h"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>

#include "strategy/ScalpingStrategy.h"
#include "strategy/MomentumStrategy.h"
#include "strategy/BreakoutStrategy.h"
#include "strategy/MeanReversionStrategy.h"
#include "strategy/GridTradingStrategy.h"

namespace autolife {
namespace backtest {

namespace {
// Core execution plane should not be penalized versus legacy in backtest cost modeling.
constexpr double CORE_ENTRY_SLIPPAGE_PCT = 0.00018;  // 0.018%
constexpr double CORE_EXIT_SLIPPAGE_PCT = 0.00026;   // 0.026%
constexpr double CORE_STOP_SLIPPAGE_PCT = 0.00095;   // 0.095%
constexpr double LEGACY_ENTRY_SLIPPAGE_PCT = 0.0002; // 0.02%
constexpr double LEGACY_EXIT_SLIPPAGE_PCT = 0.0003;  // 0.03%
constexpr double LEGACY_STOP_SLIPPAGE_PCT = 0.0010;  // 0.10%
// Live scanner feeds strategies with up to 200 bars on 1m timeframe.
// Keep backtest 1m window aligned for live-like parity.
constexpr size_t BACKTEST_CANDLE_WINDOW = 200;
constexpr size_t TF_5M_MAX_BARS = 120;
constexpr size_t TF_1H_MAX_BARS = 120;
constexpr size_t TF_4H_MAX_BARS = 90;
constexpr size_t TF_1D_MAX_BARS = 60;

long long getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

long long normalizeTimestampMs(long long ts) {
    // Second-based timestamps are converted to millisecond scale.
    if (ts > 0 && ts < 1000000000000LL) {
        return ts * 1000LL;
    }
    return ts;
}

std::string toLowerCopy(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool containsToken(const std::string& value, const std::string& token) {
    return value.find(token) != std::string::npos;
}

double entrySlippagePct(const engine::EngineConfig& cfg) {
    const bool execution_plane_enabled = cfg.enable_core_plane_bridge && cfg.enable_core_execution_plane;
    return execution_plane_enabled ? CORE_ENTRY_SLIPPAGE_PCT : LEGACY_ENTRY_SLIPPAGE_PCT;
}

double exitSlippagePct(const engine::EngineConfig& cfg) {
    const bool execution_plane_enabled = cfg.enable_core_plane_bridge && cfg.enable_core_execution_plane;
    return execution_plane_enabled ? CORE_EXIT_SLIPPAGE_PCT : LEGACY_EXIT_SLIPPAGE_PCT;
}

double stopSlippagePct(const engine::EngineConfig& cfg) {
    const bool execution_plane_enabled = cfg.enable_core_plane_bridge && cfg.enable_core_execution_plane;
    return execution_plane_enabled ? CORE_STOP_SLIPPAGE_PCT : LEGACY_STOP_SLIPPAGE_PCT;
}

std::filesystem::path executionUpdateArtifactPath() {
    return autolife::utils::PathUtils::resolveRelativePath("logs/execution_updates_backtest.jsonl");
}

void resetExecutionUpdateArtifact() {
    const auto path = executionUpdateArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
}

void appendExecutionUpdateArtifact(const autolife::core::ExecutionUpdate& update) {
    const auto path = executionUpdateArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        LOG_WARN("Execution update artifact open failed: {}", path.string());
        return;
    }
    out << autolife::core::execution::toJson(update).dump() << "\n";
}

bool passesRegimeGate(analytics::MarketRegime regime, const engine::EngineConfig& cfg) {
    if (cfg.avoid_high_volatility && regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        return false;
    }
    if (cfg.avoid_trending_down && regime == analytics::MarketRegime::TRENDING_DOWN) {
        return false;
    }
    return true;
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

bool rebalanceSignalRiskReward(strategy::Signal& signal, const engine::EngineConfig& cfg) {
    if (signal.entry_price <= 0.0 || signal.stop_loss <= 0.0 || signal.stop_loss >= signal.entry_price) {
        return false;
    }

    const double risk_price = signal.entry_price - signal.stop_loss;
    if (risk_price <= 0.0) {
        return false;
    }

    if (signal.take_profit_1 <= signal.entry_price) {
        signal.take_profit_1 = signal.entry_price + risk_price * 1.05;
    }
    if (signal.take_profit_2 <= signal.entry_price) {
        signal.take_profit_2 = signal.take_profit_1;
    }

    const double base_target_rr = computeTargetRewardRisk(signal.strength, cfg);
    const double target_rr = computeCostAwareRewardRiskFloor(signal, cfg, base_target_rr);
    const double current_rr = (signal.take_profit_2 - signal.entry_price) / risk_price;
    if (current_rr + 1e-9 < target_rr) {
        signal.take_profit_2 = signal.entry_price + risk_price * target_rr;
    }

    const double risk_pct = risk_price / std::max(1e-9, signal.entry_price);
    const double round_trip_cost_pct = computeEffectiveRoundTripCostPct(signal, cfg);
    const double tp1_cost_cover_rr = ((round_trip_cost_pct * 0.65) + 0.00015) / std::max(1e-9, risk_pct);
    const double min_tp1_rr = std::max({1.0, target_rr * 0.60, tp1_cost_cover_rr});
    const double min_tp1 = signal.entry_price + risk_price * min_tp1_rr;
    if (signal.take_profit_1 < min_tp1) {
        signal.take_profit_1 = min_tp1;
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

double computeStrategyHistoryWinProbPrior(const strategy::Signal& signal, const engine::EngineConfig& cfg) {
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
        case analytics::MarketRegime::TRENDING_UP:
            win_prob += 0.02;
            break;
        case analytics::MarketRegime::RANGING:
            win_prob -= 0.02;
            break;
        case analytics::MarketRegime::HIGH_VOLATILITY:
            win_prob -= 0.04;
            break;
        case analytics::MarketRegime::TRENDING_DOWN:
            win_prob -= 0.05;
            break;
        default:
            break;
    }

    // Archetype-level priors from loss-cluster diagnostics.
    // Keep it market-agnostic: apply only by strategy/archetype/regime.
    const bool is_breakout_cont = (signal.entry_archetype == "BREAKOUT_CONTINUATION");
    const bool is_scalping = (signal.strategy_name == "Advanced Scalping");
    const bool is_momentum = (signal.strategy_name == "Advanced Momentum");
    const bool is_breakout_strategy = (signal.strategy_name == "Breakout Strategy");
    const bool is_consolidation_break = (signal.entry_archetype == "CONSOLIDATION_BREAK");

    if (is_scalping && is_breakout_cont) {
        if (signal.market_regime == analytics::MarketRegime::TRENDING_UP) {
            win_prob -= 0.14;
        } else if (signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
            win_prob -= 0.18;
        }
    }
    if (is_momentum && is_breakout_cont) {
        if (signal.market_regime == analytics::MarketRegime::RANGING) {
            win_prob -= 0.14;
        } else if (signal.market_regime == analytics::MarketRegime::TRENDING_UP) {
            win_prob -= 0.08;
        }
    }
    if (is_breakout_strategy && is_consolidation_break &&
        signal.market_regime == analytics::MarketRegime::TRENDING_UP) {
        win_prob -= 0.08;
    }

    win_prob += computeStrategyHistoryWinProbPrior(signal, cfg);
    win_prob = std::clamp(win_prob, 0.12, 0.88);

    const double round_trip_cost_pct = computeEffectiveRoundTripCostPct(signal, cfg);
    const double expected_gross_pct = (win_prob * gross_reward_pct) - ((1.0 - win_prob) * gross_risk_pct);
    return expected_gross_pct - round_trip_cost_pct;
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
};

std::map<std::string, StrategyEdgeStats> buildStrategyEdgeStats(const std::vector<risk::TradeHistory>& history) {
    std::map<std::string, StrategyEdgeStats> out;
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

std::string makeStrategyRegimeKey(const std::string& strategy_name, analytics::MarketRegime regime) {
    return strategy_name + "|" + std::to_string(static_cast<int>(regime));
}

std::string makeMarketStrategyRegimeKey(
    const std::string& market,
    const std::string& strategy_name,
    analytics::MarketRegime regime
) {
    return market + "|" + strategy_name + "|" + std::to_string(static_cast<int>(regime));
}

std::map<std::string, StrategyEdgeStats> buildStrategyRegimeEdgeStats(
    const std::vector<risk::TradeHistory>& history
) {
    std::map<std::string, StrategyEdgeStats> out;
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
    const std::vector<risk::TradeHistory>& history
) {
    std::map<std::string, StrategyEdgeStats> out;
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

const char* regimeToLabel(analytics::MarketRegime regime) {
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP: return "TRENDING_UP";
        case analytics::MarketRegime::TRENDING_DOWN: return "TRENDING_DOWN";
        case analytics::MarketRegime::RANGING: return "RANGING";
        case analytics::MarketRegime::HIGH_VOLATILITY: return "HIGH_VOLATILITY";
        default: return "UNKNOWN";
    }
}

std::string strengthBucket(double strength) {
    if (strength < 0.55) return "strength_low";
    if (strength < 0.70) return "strength_mid";
    return "strength_high";
}

std::string expectedValueBucket(double expected_value) {
    // expected_value is tracked as calibrated net edge after fee/slippage.
    // Use stricter cutoffs so ev_high reflects meaningful post-cost expectancy.
    if (!std::isfinite(expected_value)) return "ev_negative";
    if (expected_value < 0.0) return "ev_negative";
    if (expected_value < 0.0015) return "ev_neutral";
    if (expected_value < 0.0035) return "ev_positive";
    return "ev_high";
}

std::string rewardRiskBucket(double rr) {
    if (rr < 1.20) return "rr_low";
    if (rr < 1.60) return "rr_mid";
    return "rr_high";
}

bool requiresTypedArchetype(const std::string& strategy_name) {
    return strategy_name == "Advanced Scalping" ||
           strategy_name == "Advanced Momentum";
}

void normalizeSignalStopLossByRegime(strategy::Signal& signal, analytics::MarketRegime regime) {
    if (signal.entry_price <= 0.0) {
        return;
    }

    double min_risk_pct = 0.0035;
    double max_risk_pct = 0.0100;
    if (regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        min_risk_pct = 0.0060;
        max_risk_pct = 0.0150;
    } else if (regime == analytics::MarketRegime::TRENDING_DOWN) {
        min_risk_pct = 0.0050;
        max_risk_pct = 0.0120;
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

bool passesEntryQualityGate(const strategy::Signal& signal, const engine::EngineConfig& cfg) {
    const double entry_price = signal.entry_price;
    const double take_profit_price = (signal.take_profit_2 > 0.0) ? signal.take_profit_2 : signal.take_profit_1;
    const double stop_loss_price = signal.stop_loss;

    if (entry_price <= 0.0 || take_profit_price <= 0.0 || stop_loss_price <= 0.0 ||
        take_profit_price <= entry_price || stop_loss_price >= entry_price) {
        return false;
    }

    const double gross_reward_pct = (take_profit_price - entry_price) / entry_price;
    const double gross_risk_pct = (entry_price - stop_loss_price) / entry_price;
    const double reward_risk_ratio = (gross_risk_pct > 1e-8) ? (gross_reward_pct / gross_risk_pct) : 0.0;

    const double calibrated_expected_edge_pct = computeCalibratedExpectedEdgePct(signal, cfg);

    return reward_risk_ratio >= cfg.min_reward_risk &&
           calibrated_expected_edge_pct >= cfg.min_expected_edge_pct;
}

bool passesSecondStageEntryConfirmation(
    const strategy::Signal& signal,
    const engine::EngineConfig& cfg,
    double reward_risk_ratio,
    double rr_gate,
    double calibrated_expected_edge_pct,
    double edge_gate
) {
    if (!(reward_risk_ratio >= rr_gate && calibrated_expected_edge_pct >= edge_gate)) {
        return false;
    }

    const double round_trip_cost_pct = computeEffectiveRoundTripCostPct(signal, cfg);
    double min_rr_margin = 0.05;
    double min_edge_margin = std::max(0.00008, round_trip_cost_pct * 0.12);

    if (signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
        min_rr_margin += 0.07;
        min_edge_margin += 0.00012;
    }
    if (signal.liquidity_score > 0.0 && signal.liquidity_score < 55.0) {
        min_rr_margin += 0.04;
        min_edge_margin += 0.00008;
    }
    if (signal.strategy_trade_count >= std::max(8, cfg.min_strategy_trades_for_ev / 2) &&
        (signal.strategy_win_rate < 0.45 ||
         (signal.strategy_profit_factor > 0.0 && signal.strategy_profit_factor < 0.95))) {
        min_rr_margin += 0.03;
        min_edge_margin += 0.00006;
    }
    if (signal.market_regime == analytics::MarketRegime::TRENDING_UP &&
        signal.strength >= 0.74 &&
        signal.liquidity_score >= 62.0) {
        min_rr_margin -= 0.02;
        min_edge_margin -= 0.00003;
    }

    min_rr_margin = std::clamp(min_rr_margin, 0.02, 0.22);
    min_edge_margin = std::clamp(min_edge_margin, 0.00004, 0.00045);

    const double rr_margin = reward_risk_ratio - rr_gate;
    const double edge_margin = calibrated_expected_edge_pct - edge_gate;
    return rr_margin >= min_rr_margin && edge_margin >= min_edge_margin;
}

void applyArchetypeRiskAdjustments(
    const strategy::Signal& signal,
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
    const bool is_consolidation_break = (signal.entry_archetype == "CONSOLIDATION_BREAK");

    if (is_scalping && is_breakout_cont) {
        if (signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
            signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
            regime_pattern_block = true;
            return;
        }
    }

    if (is_momentum && is_breakout_cont &&
        signal.market_regime == analytics::MarketRegime::RANGING) {
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
        signal.market_regime == analytics::MarketRegime::TRENDING_UP) {
        // Current hostile dataset consistently shows poor payoff for this pattern.
        // Revisit when broader market sample confirms a robust edge.
        regime_pattern_block = true;
        return;
    }
    if (is_breakout && is_consolidation_break &&
        signal.market_regime == analytics::MarketRegime::TRENDING_UP) {
        regime_pattern_block = true;
        return;
    }
}
}

BacktestEngine::BacktestEngine() 
    : balance_krw_(0), balance_asset_(0), 
      max_balance_(0), max_drawdown_(0), 
      total_trades_(0), winning_trades_(0) {
      
      // Initialize with dummy/mock client for backtest
      // In a real scenario, we might want a MockUpbitHttpClient
      http_client_ = std::make_shared<network::UpbitHttpClient>("BACKTEST_KEY", "BACKTEST_SECRET"); 
      
      // Initialize Components
      strategy_manager_ = std::make_unique<strategy::StrategyManager>(http_client_);
      regime_detector_ = std::make_unique<analytics::RegimeDetector>();
      policy_controller_ = std::make_unique<engine::AdaptivePolicyController>();
      performance_store_ = std::make_unique<engine::PerformanceStore>();
      // RiskManager will be initialized in init() with config capital
}

void BacktestEngine::init(const Config& config) {

    engine_config_ = config.getEngineConfig();
    resetExecutionUpdateArtifact();
    balance_krw_ = config.getInitialCapital();
    balance_asset_ = 0.0;
    max_balance_ = balance_krw_;
    loaded_tf_cursors_.clear();
    entry_funnel_ = Result::EntryFunnelSummary{};
    strategy_generated_counts_.clear();
    strategy_selected_best_counts_.clear();
    strategy_blocked_by_risk_manager_counts_.clear();
    strategy_entries_executed_counts_.clear();
    intrabar_stop_tp_collision_count_ = 0;
    intrabar_collision_by_strategy_.clear();
    
    // Reset Risk Manager with initial capital
    risk_manager_ = std::make_unique<risk::RiskManager>(balance_krw_);
    // 백테스트에서는 실시간 쿨다운 비활성화 (실시간이 아닌 시뮬레이션 시간 사용)
    risk_manager_->setMinReentryInterval(0);
    risk_manager_->setMaxDailyTrades(1000);
    
    std::set<std::string> enabled;
    for (const auto& s : engine_config_.enabled_strategies) {
        enabled.insert(s);
    }
    auto should_register = [&](const std::string& name) {
        if (enabled.empty()) {
            return true;
        }
        if (enabled.count(name) > 0) {
            return true;
        }
        if (name == "grid_trading" && enabled.count("grid") > 0) {
            return true;
        }
        return false;
    };

    if (should_register("scalping")) {
        auto scalping = std::make_shared<strategy::ScalpingStrategy>(http_client_);
        strategy_manager_->registerStrategy(scalping);
    }

    if (should_register("momentum")) {
        auto momentum = std::make_shared<strategy::MomentumStrategy>(http_client_);
        strategy_manager_->registerStrategy(momentum);
    }

    if (should_register("breakout")) {
        auto breakout = std::make_shared<strategy::BreakoutStrategy>(http_client_);
        strategy_manager_->registerStrategy(breakout);
    }

    if (should_register("mean_reversion")) {
        auto mean_rev = std::make_shared<strategy::MeanReversionStrategy>(http_client_);
        strategy_manager_->registerStrategy(mean_rev);
    }

    if (should_register("grid_trading")) {
        auto grid = std::make_shared<strategy::GridTradingStrategy>(http_client_);
        strategy_manager_->registerStrategy(grid);
    }
    
    LOG_INFO(
        "BacktestEngine initialized (core_bridge={}, core_policy={}, core_risk={}, core_execution={})",
        engine_config_.enable_core_plane_bridge ? "on" : "off",
        engine_config_.enable_core_policy_plane ? "on" : "off",
        engine_config_.enable_core_risk_plane ? "on" : "off",
        engine_config_.enable_core_execution_plane ? "on" : "off"
    );
}

long long BacktestEngine::toMsTimestamp(long long ts) {
    return normalizeTimestampMs(ts);
}

void BacktestEngine::normalizeTimestampsToMs(std::vector<Candle>& candles) {
    for (auto& candle : candles) {
        candle.timestamp = toMsTimestamp(candle.timestamp);
    }
    std::sort(candles.begin(), candles.end(), [](const Candle& a, const Candle& b) {
        return a.timestamp < b.timestamp;
    });
}

std::vector<Candle> BacktestEngine::aggregateCandles(
    const std::vector<Candle>& candles_1m,
    int timeframe_minutes,
    size_t max_bars
) {
    if (candles_1m.empty()) {
        return {};
    }
    if (timeframe_minutes <= 1) {
        if (candles_1m.size() <= max_bars) {
            return candles_1m;
        }
        return std::vector<Candle>(candles_1m.end() - static_cast<std::ptrdiff_t>(max_bars), candles_1m.end());
    }

    const long long bucket_ms = static_cast<long long>(timeframe_minutes) * 60LL * 1000LL;
    std::vector<Candle> aggregated;
    aggregated.reserve(candles_1m.size() / static_cast<size_t>(timeframe_minutes) + 2);

    Candle current{};
    long long current_bucket = std::numeric_limits<long long>::min();
    bool has_current = false;

    for (const auto& src : candles_1m) {
        const long long ts_ms = toMsTimestamp(src.timestamp);
        const long long bucket = (ts_ms / bucket_ms) * bucket_ms;
        if (!has_current || bucket != current_bucket) {
            if (has_current) {
                aggregated.push_back(current);
            }
            current_bucket = bucket;
            current.timestamp = bucket;
            current.open = src.open;
            current.high = src.high;
            current.low = src.low;
            current.close = src.close;
            current.volume = src.volume;
            has_current = true;
            continue;
        }

        current.high = std::max(current.high, src.high);
        current.low = std::min(current.low, src.low);
        current.close = src.close;
        current.volume += src.volume;
    }

    if (has_current) {
        aggregated.push_back(current);
    }

    if (aggregated.size() > max_bars) {
        aggregated.erase(
            aggregated.begin(),
            aggregated.end() - static_cast<std::ptrdiff_t>(max_bars)
        );
    }
    return aggregated;
}

void BacktestEngine::loadCompanionTimeframes(const std::string& file_path) {
    loaded_tf_candles_.clear();
    loaded_tf_cursors_.clear();

    std::filesystem::path primary(file_path);
    if (!primary.has_parent_path() || !std::filesystem::exists(primary.parent_path())) {
        return;
    }

    const std::string stem_lower = toLowerCopy(primary.stem().string());
    const std::string prefix = "upbit_";
    const std::string pivot = "_1m_";
    if (!startsWith(stem_lower, prefix) || !containsToken(stem_lower, pivot)) {
        return;
    }

    const size_t market_begin = prefix.size();
    const size_t market_end = stem_lower.find(pivot, market_begin);
    if (market_end == std::string::npos || market_end <= market_begin) {
        return;
    }
    const std::string market_token = stem_lower.substr(market_begin, market_end - market_begin);

    auto findCompanion = [&](const std::vector<std::string>& tf_tokens) -> std::filesystem::path {
        const auto parent = primary.parent_path();
        for (const auto& entry : std::filesystem::directory_iterator(parent)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (toLowerCopy(entry.path().extension().string()) != ".csv") {
                continue;
            }

            const std::string candidate_stem = toLowerCopy(entry.path().stem().string());
            for (const auto& token : tf_tokens) {
                const std::string expected_prefix = "upbit_" + market_token + "_" + token + "_";
                if (startsWith(candidate_stem, expected_prefix)) {
                    return entry.path();
                }
            }
        }
        return {};
    };

    struct TfSpec {
        std::string tf_key;
        std::vector<std::string> tokens;
    };

    const std::vector<TfSpec> specs = {
        {"5m", {"5m"}},
        {"1h", {"60m"}},
        {"4h", {"240m"}},
        {"1d", {"1d", "1440m"}}
    };

    for (const auto& spec : specs) {
        const auto companion = findCompanion(spec.tokens);
        if (companion.empty()) {
            continue;
        }

        auto candles = DataHistory::loadCSV(companion.string());
        if (candles.empty()) {
            continue;
        }
        normalizeTimestampsToMs(candles);
        loaded_tf_candles_[spec.tf_key] = std::move(candles);
        loaded_tf_cursors_[spec.tf_key] = 0;
        LOG_INFO("Backtest companion timeframe loaded: tf={} rows={} file={}",
                 spec.tf_key,
                 loaded_tf_candles_[spec.tf_key].size(),
                 companion.string());
    }
}

std::vector<Candle> BacktestEngine::getTimeframeCandles(
    const std::string& timeframe,
    long long current_timestamp,
    int fallback_minutes,
    size_t max_bars
) {
    const long long ts_ms = toMsTimestamp(current_timestamp);
    auto loaded_it = loaded_tf_candles_.find(timeframe);
    if (loaded_it != loaded_tf_candles_.end()) {
        const auto& source = loaded_it->second;
        size_t& cursor = loaded_tf_cursors_[timeframe];
        while (cursor < source.size() &&
               toMsTimestamp(source[cursor].timestamp) <= ts_ms) {
            ++cursor;
        }

        if (cursor > 0) {
            const size_t start = (cursor > max_bars) ? (cursor - max_bars) : 0;
            return std::vector<Candle>(source.begin() + static_cast<std::ptrdiff_t>(start),
                                       source.begin() + static_cast<std::ptrdiff_t>(cursor));
        }
    }

    return aggregateCandles(current_candles_, fallback_minutes, max_bars);
}

void BacktestEngine::loadData(const std::string& file_path) {
    if (file_path.find(".json") != std::string::npos) {
        history_data_ = DataHistory::loadJSON(file_path);
    } else {
        history_data_ = DataHistory::loadCSV(file_path);
    }

    normalizeTimestampsToMs(history_data_);
    loadCompanionTimeframes(file_path);

    const std::string stem_lower = toLowerCopy(std::filesystem::path(file_path).stem().string());
    const std::string prefix = "upbit_";
    const std::string pivot = "_1m_";
    if (startsWith(stem_lower, prefix) && containsToken(stem_lower, pivot)) {
        const size_t market_begin = prefix.size();
        const size_t market_end = stem_lower.find(pivot, market_begin);
        if (market_end != std::string::npos && market_end > market_begin) {
            std::string token = stem_lower.substr(market_begin, market_end - market_begin);
            std::replace(token.begin(), token.end(), '_', '-');
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            market_name_ = token;
        }
    }

    LOG_INFO("Backtest data loaded: market={}, rows={}, tf_companions={}",
             market_name_,
             history_data_.size(),
             loaded_tf_candles_.size());
}

void BacktestEngine::run() {
    LOG_INFO("Starting Backtest with {} candles.", history_data_.size());
    
    for (const auto& candle : history_data_) {
        processCandle(candle);
    }
    
    LOG_INFO("Backtest Completed.");
    if (risk_manager_) {
        auto metrics = risk_manager_->getRiskMetrics();
        LOG_INFO("Final Balance: {}", metrics.total_capital);
    } else {
        LOG_INFO("Final Balance: {}", balance_krw_ + (balance_asset_ * history_data_.back().close));
    }
}

void BacktestEngine::processCandle(const Candle& candle) {
    // 1. Accumulate History
    current_candles_.push_back(candle);
    if (current_candles_.size() > BACKTEST_CANDLE_WINDOW) {
        current_candles_.erase(current_candles_.begin()); // Keep window size
    }
    
    // Let each strategy enforce its own warm-up requirement.
    // A global 100-candle gate prevents short fixture datasets from producing any signal.
    if (current_candles_.size() < 30) return;

    double current_price = candle.close;
    auto notifyStrategyClosed = [&](const risk::Position& closed_position, double exit_price) {
        if (!strategy_manager_ || closed_position.strategy_name.empty()) {
            return;
        }
        auto strategy = strategy_manager_->getStrategy(closed_position.strategy_name);
        if (!strategy) {
            return;
        }
        const double fee_rate = Config::getInstance().getFeeRate();
        const double exit_value = exit_price * closed_position.quantity;
        const double entry_fee = closed_position.invested_amount * fee_rate;
        const double exit_fee = exit_value * fee_rate;
        const double net_pnl = exit_value - closed_position.invested_amount - entry_fee - exit_fee;
        strategy->updateStatistics(closed_position.market, net_pnl > 0.0, net_pnl);
    };
    
    // 2. Market/Regime Analysis
    auto regime = regime_detector_->analyzeRegime(current_candles_);
    analytics::CoinMetrics metrics;
    metrics.market = market_name_;
    metrics.candles = current_candles_;
    metrics.candles_by_tf["1m"] = metrics.candles;
    metrics.candles_by_tf["5m"] = getTimeframeCandles("5m", candle.timestamp, 5, TF_5M_MAX_BARS);
    metrics.candles_by_tf["1h"] = getTimeframeCandles("1h", candle.timestamp, 60, TF_1H_MAX_BARS);
    metrics.candles_by_tf["4h"] = getTimeframeCandles("4h", candle.timestamp, 240, TF_4H_MAX_BARS);
    metrics.candles_by_tf["1d"] = getTimeframeCandles("1d", candle.timestamp, 1440, TF_1D_MAX_BARS);
    metrics.current_price = current_price;
    metrics.volatility = regime.atr_pct;
    
    // Calculate price_change_rate from previous candle
    if (current_candles_.size() >= 2) {
        double prev_close = current_candles_[current_candles_.size() - 2].close;
        if (prev_close > 0) {
            metrics.price_change_rate = ((current_price - prev_close) / prev_close) * 100.0;
        }
    }
    
    // Estimate volume surge and rolling average volume
    double avg_vol = 0.0;
    if (current_candles_.size() >= 20) {
        for (size_t vi = current_candles_.size() - 20; vi < current_candles_.size() - 1; ++vi) {
            avg_vol += current_candles_[vi].volume;
        }
        avg_vol /= 19.0;
        metrics.volume_surge_ratio = (avg_vol > 0) ? (candle.volume / avg_vol) : 1.0;
    } else {
        metrics.volume_surge_ratio = 1.0;
    }
    
    // Dynamic synthetic microstructure from recent candles
    const double candle_range_pct = (current_price > 0.0)
        ? ((candle.high - candle.low) / current_price) * 100.0
        : 0.0;

    double momentum_5 = 0.0;
    if (current_candles_.size() >= 6) {
        const double base = current_candles_[current_candles_.size() - 6].close;
        if (base > 0.0) {
            momentum_5 = ((current_price - base) / base) * 100.0;
        }
    }

    // Liquidity drops when range is wide and volume baseline is weak
    const double vol_baseline_t = std::clamp((avg_vol - 10.0) / 120.0, 0.0, 1.0);
    const double spread_stress_t = std::clamp((candle_range_pct - 0.15) / 1.5, 0.0, 1.0);
    const double surge_bonus = std::clamp((metrics.volume_surge_ratio - 1.0) * 8.0, -10.0, 15.0);
    metrics.liquidity_score = std::clamp(
        30.0 + (vol_baseline_t * 55.0) - (spread_stress_t * 25.0) + surge_bonus,
        10.0, 95.0
    );

    const double momentum_t = std::clamp(momentum_5 / 2.5, -1.0, 1.0);
    const double surge_t = std::clamp((metrics.volume_surge_ratio - 1.0) / 2.0, -1.0, 1.0);
    metrics.order_book_imbalance = std::clamp((momentum_t * 0.45) + (surge_t * 0.25), -0.7, 0.7);
    metrics.buy_pressure = std::clamp(50.0 + (metrics.order_book_imbalance * 35.0), 10.0, 90.0);
    metrics.sell_pressure = std::clamp(100.0 - metrics.buy_pressure, 10.0, 90.0);
    metrics.price_momentum = momentum_5;
    
    // Generate synthetic orderbook_units (5 levels), with spread tied to regime/liquidity
    {
        nlohmann::json units = nlohmann::json::array();
        const double spread_pct = std::clamp(
            0.01 + (metrics.volatility * 0.20) + ((100.0 - metrics.liquidity_score) * 0.0015),
            0.01, 0.40
        );
        const double base_size = std::clamp(avg_vol / 80.0, 0.2, 3.0);
        
        for (int level = 0; level < 5; ++level) {
            double offset = spread_pct * (level + 1) / 100.0;
            double bid_price = current_price * (1.0 - offset);
            double ask_price = current_price * (1.0 + offset);
            
            const double level_bias = (5.0 - static_cast<double>(level)) / 5.0;
            const double flow_bias = metrics.order_book_imbalance * 0.5;
            const double wave = std::sin((candle.timestamp + level * 17) * 0.001) * 0.08;
            const double bid_size = base_size * (1.0 + level_bias * 0.25 + flow_bias + wave);
            const double ask_size = base_size * (1.0 + level_bias * 0.20 - flow_bias - wave);
            
            nlohmann::json unit;
            unit["ask_price"] = ask_price;
            unit["bid_price"] = bid_price;
            unit["ask_size"] = std::max(0.05, ask_size);
            unit["bid_size"] = std::max(0.05, bid_size);
            units.push_back(unit);
        }
        metrics.orderbook_units = units;
    }

    // 3. Monitor & Exit Positions
    // In TradingEngine this is done by iterating active positions
    // Here we check if we have an open position in RiskManager
    auto position = risk_manager_->getPosition(market_name_);
    if (position) {
        risk_manager_->updatePosition(market_name_, current_price);
        position = risk_manager_->getPosition(market_name_);
    }
    if (risk_manager_->isDrawdownExceeded()) {
        if (position) {
            const risk::Position closed_position = *position;
            const double forced_exit = current_price * (1.0 - exitSlippagePct(engine_config_));
            Order dd_order;
            dd_order.market = market_name_;
            dd_order.side = OrderSide::SELL;
            dd_order.volume = position->quantity;
            dd_order.price = forced_exit;
            dd_order.strategy_name = position->strategy_name;
            executeOrder(dd_order, forced_exit);
            risk_manager_->exitPosition(market_name_, forced_exit, "MaxDrawdown");
            notifyStrategyClosed(closed_position, forced_exit);
            position = nullptr;
        }
    }

    if (position) {
        auto strategy = strategy_manager_->getStrategy(position->strategy_name);
        if (strategy) {
            // Keep backtest exit mechanics aligned with live monitoring flow.
            auto scalping_strategy = std::dynamic_pointer_cast<strategy::ScalpingStrategy>(strategy);
            if (scalping_strategy && position->strategy_name == "Advanced Scalping") {
                if (position->breakeven_trigger > 0.0 &&
                    current_price >= position->breakeven_trigger &&
                    position->stop_loss < position->entry_price) {
                    risk_manager_->moveStopToBreakeven(market_name_);
                }

                if (position->trailing_start > 0.0 && current_price >= position->trailing_start) {
                    const double new_stop = scalping_strategy->updateTrailingStop(
                        position->entry_price,
                        position->highest_price,
                        current_price
                    );
                    risk_manager_->updateStopLoss(market_name_, new_stop, "trailing");
                }
                position = risk_manager_->getPosition(market_name_);
            }

            // Check Exit Condition
            bool should_exit = strategy->shouldExit(
                market_name_,
                position->entry_price,
                current_price,
                (candle.timestamp - position->entry_time) / 1000.0 // holding seconds
            );
            
            // Check Stop Loss / Take Profit (RiskManager handled?)
            // RiskManager::managePositions usually checks SL/TP. 
            // We should simulate that here.
            // Simplified:
            // Conservative intrabar execution model:
            // - If stop and take-profit touched in same candle, stop is prioritized.
            // - TP1 is partial exit, TP2 is full exit.
            const bool stop_touched = (candle.low <= position->stop_loss);
            const bool tp1_touched = (candle.high >= position->take_profit_1);
            const bool tp2_touched = (candle.high >= position->take_profit_2);
            if (stop_touched && (tp1_touched || tp2_touched)) {
                intrabar_stop_tp_collision_count_++;
                intrabar_collision_by_strategy_[position->strategy_name.empty() ? "unknown" : position->strategy_name]++;
            }

            if (stop_touched) {
                const risk::Position closed_position = *position;
                const double stop_fill = position->stop_loss * (1.0 - stopSlippagePct(engine_config_));
                Order sl_order;
                sl_order.market = market_name_;
                sl_order.side = OrderSide::SELL;
                sl_order.volume = position->quantity;
                sl_order.price = stop_fill;
                sl_order.strategy_name = position->strategy_name;
                executeOrder(sl_order, stop_fill);
                risk_manager_->exitPosition(market_name_, stop_fill, "StopLoss");
                notifyStrategyClosed(closed_position, stop_fill);
                position = nullptr;
            } else {
                if (!position->half_closed && tp1_touched) {
                    const double tp1_fill = position->take_profit_1 * (1.0 - exitSlippagePct(engine_config_));
                    const double partial_qty = position->quantity * 0.5;

                    Order tp1_order;
                    tp1_order.market = market_name_;
                    tp1_order.side = OrderSide::SELL;
                    tp1_order.volume = partial_qty;
                    tp1_order.price = tp1_fill;
                    tp1_order.strategy_name = position->strategy_name;
                    executeOrder(tp1_order, tp1_fill);
                    risk_manager_->partialExit(market_name_, tp1_fill);
                    position = risk_manager_->getPosition(market_name_);
                }

                if (position) {
                    const bool tp2_touched_after_partial = tp2_touched || (candle.high >= position->take_profit_2);
                    if (tp2_touched_after_partial) {
                        const risk::Position closed_position = *position;
                        const double tp2_fill = position->take_profit_2 * (1.0 - exitSlippagePct(engine_config_));
                        Order tp2_order;
                        tp2_order.market = market_name_;
                        tp2_order.side = OrderSide::SELL;
                        tp2_order.volume = position->quantity;
                        tp2_order.price = tp2_fill;
                        tp2_order.strategy_name = position->strategy_name;
                        executeOrder(tp2_order, tp2_fill);
                        risk_manager_->exitPosition(market_name_, tp2_fill, "TakeProfit2");
                        notifyStrategyClosed(closed_position, tp2_fill);
                        position = nullptr;
                    } else if (should_exit) {
                        const risk::Position closed_position = *position;
                        // Execute Sell
                        Order order;
                        order.market = market_name_;
                        order.side = OrderSide::SELL;
                        order.volume = position->quantity;
                        order.price = current_price * (1.0 - exitSlippagePct(engine_config_));
                        order.strategy_name = position->strategy_name;
                        executeOrder(order, order.price);
                        risk_manager_->exitPosition(market_name_, order.price, "StrategyExit");
                        notifyStrategyClosed(closed_position, order.price);
                        position = nullptr;
                    }
                }
            }
        }
    }
    
    // 4. Generate Entry Signals (only if no position)
    if (!position) {
        entry_funnel_.entry_rounds++;
        bool entry_executed = false;
        auto signals = strategy_manager_->collectSignals(
            market_name_,
            metrics,
            current_candles_,
            current_price,
            risk_manager_->getRiskMetrics().available_capital,
            regime
        );
        if (signals.empty()) {
            entry_funnel_.no_signal_generated++;
        }
        for (const auto& signal : signals) {
            if (!signal.strategy_name.empty()) {
                strategy_generated_counts_[signal.strategy_name]++;
            }
        }
        
        // Dynamic Filter Simulation (Self-learning stub)
        // If we had recent losses, filter might increase.
        double filter_threshold = dynamic_filter_value_;
        const auto bt_metrics = risk_manager_->getRiskMetrics();
        const bool small_seed_mode =
            bt_metrics.total_capital > 0.0 &&
            bt_metrics.total_capital <= engine_config_.small_account_tier2_capital_krw;
        const bool core_bridge_enabled = engine_config_.enable_core_plane_bridge;
        const bool core_policy_enabled = core_bridge_enabled && engine_config_.enable_core_policy_plane;
        const bool core_risk_enabled = core_bridge_enabled && engine_config_.enable_core_risk_plane;
        const double hostility_alpha = std::clamp(engine_config_.hostility_ewma_alpha, 0.01, 0.99);
        const double hostile_threshold = std::clamp(engine_config_.hostility_hostile_threshold, 0.0, 1.0);
        const double severe_threshold = std::clamp(
            std::max(engine_config_.hostility_severe_threshold, hostile_threshold),
            0.0,
            1.0
        );
        const double extreme_threshold = std::clamp(
            std::max(engine_config_.hostility_extreme_threshold, severe_threshold),
            0.0,
            1.0
        );
        const int pause_candles_base = std::clamp(engine_config_.backtest_hostility_pause_candles, 1, 2000);
        const int pause_candles_extreme = std::clamp(
            std::max(engine_config_.backtest_hostility_pause_candles_extreme, pause_candles_base),
            pause_candles_base,
            4000
        );
        const int pause_sample_min = std::clamp(engine_config_.hostility_pause_recent_sample_min, 1, 100);
        const double pause_expectancy_threshold = engine_config_.hostility_pause_recent_expectancy_krw;
        const double pause_win_rate_threshold = std::clamp(engine_config_.hostility_pause_recent_win_rate, 0.0, 1.0);
        if (small_seed_mode) {
            filter_threshold = std::clamp(filter_threshold + 0.08, 0.35, 0.90);
        }

        double hostility_now = 0.0;
        switch (regime.regime) {
            case analytics::MarketRegime::HIGH_VOLATILITY:
                hostility_now += 0.72;
                break;
            case analytics::MarketRegime::TRENDING_DOWN:
                hostility_now += 0.62;
                break;
            case analytics::MarketRegime::RANGING:
                hostility_now += 0.34;
                break;
            case analytics::MarketRegime::TRENDING_UP:
                hostility_now += 0.12;
                break;
            default:
                hostility_now += 0.28;
                break;
        }
        if (metrics.volatility > 0.0) {
            hostility_now += std::clamp((metrics.volatility - 1.8) / 6.0, 0.0, 0.28);
        }
        if (metrics.liquidity_score > 0.0) {
            hostility_now += std::clamp((55.0 - metrics.liquidity_score) / 90.0, 0.0, 0.20);
        }
        if (metrics.orderbook_snapshot.valid) {
            const double spread_pct = metrics.orderbook_snapshot.spread_pct * 100.0;
            hostility_now += std::clamp((spread_pct - 0.18) / 0.40, 0.0, 0.18);
        }
        hostility_now = std::clamp(hostility_now, 0.0, 1.0);
        if (market_hostility_ewma_ <= 1e-9) {
            market_hostility_ewma_ = hostility_now;
        } else {
            market_hostility_ewma_ = std::clamp(
                market_hostility_ewma_ * (1.0 - hostility_alpha) + hostility_now * hostility_alpha,
                0.0,
                1.0
            );
        }
        const double effective_hostility = std::max(hostility_now, market_hostility_ewma_);
        const bool hostile_market = effective_hostility >= hostile_threshold;
        const bool severe_hostile_market = effective_hostility >= severe_threshold;

        double recent_expectancy_krw = 0.0;
        double recent_win_rate = 0.5;
        int recent_sample = 0;
        {
            const auto trade_history = risk_manager_->getTradeHistory();
            double recent_profit_sum = 0.0;
            int recent_wins = 0;
            for (auto it = trade_history.rbegin(); it != trade_history.rend() && recent_sample < 20; ++it) {
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
        if (severe_hostile_market &&
            recent_sample >= pause_sample_min &&
            recent_expectancy_krw < pause_expectancy_threshold &&
            recent_win_rate < pause_win_rate_threshold) {
            const int base_pause_candles =
                (effective_hostility >= extreme_threshold) ? pause_candles_extreme : pause_candles_base;
            hostile_entry_pause_candles_ = std::max(hostile_entry_pause_candles_, base_pause_candles);
        }
        
        double min_expected_value = 0.0;
        if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            filter_threshold = std::max(filter_threshold, 0.48);
        } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
            filter_threshold = std::max(filter_threshold, 0.52);
        } else if (regime.regime == analytics::MarketRegime::RANGING) {
            filter_threshold = std::max(filter_threshold, 0.43);
        }

        // Regime-aware minimum activation:
        // when there are no entries for a long time, ease thresholds slightly only in
        // non-bear regimes to avoid staying fully idle.
        if (no_entry_streak_candles_ >= 45 &&
            regime.regime == analytics::MarketRegime::TRENDING_UP &&
            metrics.liquidity_score >= 55.0 &&
            metrics.volume_surge_ratio >= 1.2 &&
            metrics.price_change_rate >= 0.10) {
            filter_threshold = std::max(0.34, filter_threshold - 0.07);
            min_expected_value = std::max(0.0, min_expected_value - 0.00005);
        } else if (no_entry_streak_candles_ >= 70 &&
                   regime.regime == analytics::MarketRegime::RANGING &&
                   metrics.liquidity_score >= 50.0) {
            filter_threshold = std::max(0.35, filter_threshold - 0.05);
            min_expected_value = std::max(0.0, min_expected_value - 0.00003);
        }
        if (hostile_market) {
            filter_threshold += std::clamp((effective_hostility - hostile_threshold) * 0.18, 0.0, 0.08);
            min_expected_value += std::clamp((effective_hostility - hostile_threshold) * 0.0008, 0.0, 0.00035);
        }
        if (hostile_entry_pause_candles_ > 0) {
            filter_threshold = std::max(filter_threshold, 0.96);
            min_expected_value = std::max(min_expected_value, 0.0040);
            hostile_entry_pause_candles_--;
            LOG_INFO(
                "Backtest hostile-entry pause active: remaining_candles={}, hostility_now={:.3f}, hostility_ewma={:.3f}",
                hostile_entry_pause_candles_,
                hostility_now,
                market_hostility_ewma_
            );
        }
        filter_threshold = std::clamp(filter_threshold, 0.35, 0.98);
        min_expected_value = std::clamp(min_expected_value, -0.0002, 0.0050);

        auto filtered_signals = strategy_manager_->filterSignals(
            signals, filter_threshold, min_expected_value, regime.regime
        );
        if (!signals.empty() && filtered_signals.empty()) {
            entry_funnel_.filtered_out_by_manager++;
        }
        std::vector<strategy::Signal> candidate_signals = filtered_signals;
        if (core_policy_enabled && policy_controller_) {
            if (performance_store_) {
                performance_store_->rebuild(risk_manager_->getTradeHistory());
            }

            engine::PolicyInput policy_input;
            policy_input.candidates = filtered_signals;
            policy_input.small_seed_mode = small_seed_mode;
            policy_input.max_new_orders_per_scan = 1;
            policy_input.dominant_regime = regime.regime;
            if (performance_store_) {
                policy_input.strategy_stats = &performance_store_->byStrategy();
                policy_input.bucket_stats = &performance_store_->byBucket();
            }

            auto policy_output = policy_controller_->selectCandidates(policy_input);
            candidate_signals = std::move(policy_output.selected_candidates);
        }
        if (core_policy_enabled && !filtered_signals.empty() && candidate_signals.empty()) {
            entry_funnel_.filtered_out_by_policy++;
        }

        strategy::Signal best_signal;
        if (!candidate_signals.empty()) {
            if (core_bridge_enabled) {
                best_signal = strategy_manager_->selectRobustSignal(candidate_signals, regime.regime);
            } else {
                best_signal = strategy_manager_->selectBestSignal(candidate_signals);
            }
        }
        if (!candidate_signals.empty() && best_signal.type == strategy::SignalType::NONE) {
            entry_funnel_.no_best_signal++;
        }
        if (best_signal.type != strategy::SignalType::NONE && !best_signal.strategy_name.empty()) {
            strategy_selected_best_counts_[best_signal.strategy_name]++;
        }
        const auto trade_history = risk_manager_->getTradeHistory();
        const auto strategy_edge = buildStrategyEdgeStats(trade_history);
        const auto strategy_regime_edge = buildStrategyRegimeEdgeStats(trade_history);
        const auto market_strategy_regime_edge = buildMarketStrategyRegimeEdgeStats(trade_history);

        if (best_signal.type != strategy::SignalType::NONE) {
            best_signal.market_regime = regime.regime;
            const bool archetype_required = requiresTypedArchetype(best_signal.strategy_name);
            const bool archetype_fallback_allowed =
                best_signal.strength >= 0.74 &&
                best_signal.expected_value >= 0.0005;
            const bool archetype_ready =
                !archetype_required ||
                (!best_signal.entry_archetype.empty() && best_signal.entry_archetype != "UNSPECIFIED") ||
                archetype_fallback_allowed;
            bool strategy_ev_ok = true;
            double adaptive_rr_add = 0.0;
            double adaptive_edge_add = 0.0;
            double required_strength_floor = filter_threshold;
            bool regime_pattern_block = false;
            auto stat_it = strategy_edge.find(best_signal.strategy_name);
            if (core_risk_enabled && stat_it != strategy_edge.end()) {
                const auto& stat = stat_it->second;
                if (stat.trades >= engine_config_.min_strategy_trades_for_ev) {
                    const double exp_krw = stat.expectancy();
                    const double pf = stat.profitFactor();
                    if (exp_krw < engine_config_.min_strategy_expectancy_krw ||
                        pf < engine_config_.min_strategy_profit_factor) {
                        required_strength_floor += 0.04;
                        adaptive_rr_add += 0.12;
                        adaptive_edge_add += 0.00025;
                        if (exp_krw < (engine_config_.min_strategy_expectancy_krw - 15.0) &&
                            pf < std::max(0.75, engine_config_.min_strategy_profit_factor - 0.20)) {
                            strategy_ev_ok = false;
                        }
                    }
                }
                if (stat.trades >= 8) {
                    const double wr = stat.winRate();
                    const double pf = stat.profitFactor();
                    const double exp_krw = stat.expectancy();
                    if (wr < 0.42) {
                        adaptive_rr_add += 0.35;
                        adaptive_edge_add += 0.0006;
                    } else if (wr < 0.48) {
                        adaptive_rr_add += 0.20;
                        adaptive_edge_add += 0.0003;
                    }
                    if (pf < 0.90) {
                        adaptive_rr_add += 0.25;
                        adaptive_edge_add += 0.0005;
                    } else if (pf < 1.00) {
                        adaptive_rr_add += 0.12;
                        adaptive_edge_add += 0.0002;
                    }
                    if (exp_krw < 0.0) {
                        adaptive_rr_add += std::clamp((-exp_krw) / 1000.0, 0.0, 0.25);
                        adaptive_edge_add += std::clamp((-exp_krw) / 80000.0, 0.0, 0.0004);
                    }
                }
            }
            auto regime_it = strategy_regime_edge.find(
                makeStrategyRegimeKey(best_signal.strategy_name, best_signal.market_regime)
            );
            if (core_risk_enabled && regime_it != strategy_regime_edge.end()) {
                const auto& stat = regime_it->second;
                if (stat.trades >= 6) {
                    const double wr = stat.winRate();
                    const double pf = stat.profitFactor();
                    const double exp_krw = stat.expectancy();
                    if (exp_krw < -20.0 || (wr < 0.30 && pf < 0.78)) {
                        regime_pattern_block = true;
                    }
                    if (stat.trades >= 12 &&
                        exp_krw < -15.0 &&
                        (wr < 0.40 || pf < 0.90)) {
                        required_strength_floor = std::max(required_strength_floor, 0.62);
                        adaptive_rr_add += 0.20;
                        adaptive_edge_add += 0.0003;
                    }
                    if (stat.trades >= 18 &&
                        exp_krw < -22.0 &&
                        wr < 0.20) {
                        regime_pattern_block = true;
                    }
                    if (exp_krw < 0.0) {
                        required_strength_floor += std::clamp((-exp_krw) / 2500.0, 0.0, 0.08);
                        adaptive_rr_add += std::clamp((-exp_krw) / 1800.0, 0.0, 0.20);
                        adaptive_edge_add += std::clamp((-exp_krw) / 200000.0, 0.0, 0.0004);
                    } else if (exp_krw > 8.0 && wr >= 0.58 && pf >= 1.15) {
                        required_strength_floor -= 0.02;
                        adaptive_rr_add -= 0.08;
                        adaptive_edge_add -= 0.00015;
                    }
                    if (wr < 0.42) {
                        required_strength_floor += 0.03;
                        adaptive_rr_add += 0.10;
                        adaptive_edge_add += 0.0002;
                    }
                    if (pf < 0.95) {
                        required_strength_floor += 0.02;
                        adaptive_rr_add += 0.08;
                        adaptive_edge_add += 0.0002;
                    }
                }
            }
            auto market_regime_it = market_strategy_regime_edge.find(
                makeMarketStrategyRegimeKey(
                    best_signal.market,
                    best_signal.strategy_name,
                    best_signal.market_regime
                )
            );
            if (core_risk_enabled && market_regime_it != market_strategy_regime_edge.end()) {
                const auto& stat = market_regime_it->second;
                if (stat.trades >= 4) {
                    const double wr = stat.winRate();
                    const double pf = stat.profitFactor();
                    const double exp_krw = stat.expectancy();

                    if (exp_krw < -40.0 && wr < 0.25 && pf < 0.75) {
                        regime_pattern_block = true;
                    }
                    if (exp_krw < -20.0 && wr < 0.35 && pf < 0.85) {
                        required_strength_floor += 0.05;
                        adaptive_rr_add += 0.15;
                        adaptive_edge_add += 0.0003;
                    } else if (exp_krw < -10.0) {
                        required_strength_floor += 0.02;
                        adaptive_rr_add += 0.08;
                        adaptive_edge_add += 0.00015;
                    } else if (exp_krw > 10.0 && wr >= 0.60 && pf >= 1.20) {
                        required_strength_floor -= 0.02;
                        adaptive_rr_add -= 0.06;
                        adaptive_edge_add -= 0.0001;
                    }
                }
            }
            applyArchetypeRiskAdjustments(
                best_signal,
                required_strength_floor,
                adaptive_rr_add,
                adaptive_edge_add,
                regime_pattern_block
            );
            required_strength_floor = std::clamp(required_strength_floor, 0.35, 0.92);
            const bool pattern_strength_ok =
                !regime_pattern_block &&
                archetype_ready &&
                best_signal.strength >= required_strength_floor;
            if (!pattern_strength_ok && core_risk_enabled) {
                if (!archetype_ready) {
                    LOG_INFO("{} pattern gate blocked [{}]: missing typed archetype",
                             market_name_, best_signal.strategy_name);
                } else if (archetype_required &&
                           (best_signal.entry_archetype.empty() || best_signal.entry_archetype == "UNSPECIFIED")) {
                    LOG_INFO("{} archetype fallback accepted [{}]: strength {:.3f}, ev {:.5f}",
                             market_name_, best_signal.strategy_name, best_signal.strength, best_signal.expected_value);
                } else {
                    LOG_INFO("{} pattern gate blocked [{}]: strength {:.3f} < floor {:.3f} or severe regime loss",
                             market_name_, best_signal.strategy_name, best_signal.strength, required_strength_floor);
                }
            }

            engine::EngineConfig tuned_cfg = engine_config_;
            if (core_risk_enabled && small_seed_mode) {
                const bool favorable_small_seed_signal =
                    best_signal.market_regime == analytics::MarketRegime::TRENDING_UP &&
                    best_signal.strength >= 0.72 &&
                    best_signal.liquidity_score >= 60.0;
                double rr_add = 0.16;
                double edge_mul = 1.30;
                if (best_signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                    best_signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
                    rr_add = 0.22;
                    edge_mul = 1.45;
                } else if (favorable_small_seed_signal) {
                    rr_add = 0.08;
                    edge_mul = 1.10;
                }
                tuned_cfg.min_reward_risk = engine_config_.min_reward_risk + rr_add;
                tuned_cfg.min_expected_edge_pct = engine_config_.min_expected_edge_pct * edge_mul;
            }
            if (core_risk_enabled) {
                tuned_cfg.min_reward_risk = std::clamp(
                    tuned_cfg.min_reward_risk + adaptive_rr_add,
                    tuned_cfg.min_reward_risk,
                    tuned_cfg.min_reward_risk + 0.45
                );
                tuned_cfg.min_expected_edge_pct = std::clamp(
                    tuned_cfg.min_expected_edge_pct + adaptive_edge_add,
                    tuned_cfg.min_expected_edge_pct,
                    tuned_cfg.min_expected_edge_pct + 0.0012
                );
            }
            if (core_risk_enabled &&
                no_entry_streak_candles_ >= 45 &&
                (best_signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
                 best_signal.market_regime == analytics::MarketRegime::RANGING) &&
                best_signal.strength >= 0.72 &&
                best_signal.liquidity_score >= 60.0) {
                // Entry-frequency recovery path: in favorable regimes, ease gates slightly
                // only after prolonged no-entry streak and only for high-quality signals.
                tuned_cfg.min_reward_risk = std::max(1.05, tuned_cfg.min_reward_risk - 0.10);
                tuned_cfg.min_expected_edge_pct = std::max(
                    engine_config_.min_expected_edge_pct * 0.80,
                    tuned_cfg.min_expected_edge_pct * 0.82
                );
            }

            normalizeSignalStopLossByRegime(best_signal, best_signal.market_regime);
            const bool rr_rebalance_ok = rebalanceSignalRiskReward(best_signal, tuned_cfg);
            const bool risk_gate_ok =
                !core_risk_enabled ||
                (strategy_ev_ok &&
                 pattern_strength_ok &&
                 passesRegimeGate(best_signal.market_regime, engine_config_) &&
                 passesEntryQualityGate(best_signal, tuned_cfg));
            const double entry_price = best_signal.entry_price;
            const double take_profit_price =
                (best_signal.take_profit_2 > 0.0) ? best_signal.take_profit_2 : best_signal.take_profit_1;
            const double stop_loss_price = best_signal.stop_loss;
            const double gross_reward_pct =
                (entry_price > 0.0) ? (take_profit_price - entry_price) / entry_price : 0.0;
            const double gross_risk_pct =
                (entry_price > 0.0) ? (entry_price - stop_loss_price) / entry_price : 0.0;
            const double reward_risk_ratio =
                (gross_risk_pct > 1e-8) ? (gross_reward_pct / gross_risk_pct) : 0.0;
            const double calibrated_expected_edge_pct = computeCalibratedExpectedEdgePct(best_signal, tuned_cfg);
            const bool second_stage_ok =
                !core_risk_enabled ||
                (rr_rebalance_ok &&
                 passesSecondStageEntryConfirmation(
                     best_signal,
                     tuned_cfg,
                     reward_risk_ratio,
                     tuned_cfg.min_reward_risk,
                     calibrated_expected_edge_pct,
                     tuned_cfg.min_expected_edge_pct));
            if (!pattern_strength_ok) {
                entry_funnel_.blocked_pattern_gate++;
            } else if (!rr_rebalance_ok) {
                entry_funnel_.blocked_rr_rebalance++;
            } else if (!risk_gate_ok || !second_stage_ok) {
                entry_funnel_.blocked_risk_gate++;
            } else if (pattern_strength_ok && rr_rebalance_ok && risk_gate_ok && second_stage_ok) {
                const double min_order_krw = std::max(5000.0, engine_config_.min_order_krw);
                const double available_cash_for_gate = risk_manager_->getRiskMetrics().available_capital;
                const bool has_min_order_capital = available_cash_for_gate >= min_order_krw;
                if (!has_min_order_capital) {
                    entry_funnel_.blocked_min_order_or_capital++;
                } else {
                    const double min_position_size_for_order = min_order_krw / available_cash_for_gate;
                    const double effective_position_size = std::clamp(
                        std::max(best_signal.position_size, min_position_size_for_order),
                        0.0,
                        1.0
                    );
                    // Validate Risk
                    const bool can_enter_position = risk_manager_->canEnterPosition(
                        market_name_,
                        current_price * (1.0 + entrySlippagePct(engine_config_)),
                        effective_position_size,
                        best_signal.strategy_name
                    );
                    if (!can_enter_position) {
                        entry_funnel_.blocked_risk_manager++;
                        if (!best_signal.strategy_name.empty()) {
                            strategy_blocked_by_risk_manager_counts_[best_signal.strategy_name]++;
                        }
                    } else {
                    // Execute Buy
                    double available_cash = risk_manager_->getRiskMetrics().available_capital;
                    double fill_price = current_price * (1.0 + entrySlippagePct(engine_config_));
                    const double fee_rate = Config::getInstance().getFeeRate();
                    const double fee_reserve = std::clamp(engine_config_.order_fee_reserve_pct, 0.0, 0.02);
                    const double spendable_capital = available_cash / (1.0 + fee_reserve);
                    const bool capital_gate_ok = spendable_capital >= min_order_krw;

                    if (capital_gate_ok) {
                        double desired_order_krw = available_cash * effective_position_size;
                        double max_order_krw = std::min(engine_config_.max_order_krw, spendable_capital);
                        if (available_cash <= engine_config_.small_account_tier1_capital_krw) {
                            const double tier_cap = std::clamp(engine_config_.small_account_tier1_max_order_pct, 0.01, 1.0);
                            max_order_krw = std::min(max_order_krw, std::max(min_order_krw, available_cash * tier_cap));
                        } else if (available_cash <= engine_config_.small_account_tier2_capital_krw) {
                            const double tier_cap = std::clamp(engine_config_.small_account_tier2_max_order_pct, 0.01, 1.0);
                            max_order_krw = std::min(max_order_krw, std::max(min_order_krw, available_cash * tier_cap));
                        }

                        const int max_lots = std::max(1, static_cast<int>(std::floor(max_order_krw / min_order_krw)));
                        int desired_lots = static_cast<int>(std::floor(desired_order_krw / min_order_krw));
                        desired_lots = std::clamp(desired_lots, 1, max_lots);
                        const double order_amount = static_cast<double>(desired_lots) * min_order_krw;
                        const double quantity = order_amount / (fill_price * (1.0 + fee_rate));
                        const double fee = quantity * fill_price * fee_rate;
                        const bool order_sizing_ok =
                            quantity > 0.0 && available_cash >= (quantity * fill_price) + fee;

                        if (order_sizing_ok) {
                            Order order;
                            order.market = market_name_;
                            order.side = OrderSide::BUY;
                            order.price = fill_price;
                            order.volume = quantity;
                            order.strategy_name = best_signal.strategy_name;

                            executeOrder(order, fill_price);

                            // Register with Risk Manager
                            risk_manager_->enterPosition(
                                market_name_,
                                fill_price,
                                quantity,
                                best_signal.stop_loss,
                                best_signal.take_profit_1,
                                best_signal.take_profit_2,
                                best_signal.strategy_name,
                                best_signal.breakeven_trigger,
                                best_signal.trailing_start
                            );
                            if (auto* entered_position = risk_manager_->getPosition(market_name_)) {
                                // Backtest holding-time logic must be candle-clock based.
                                entered_position->entry_time = toMsTimestamp(candle.timestamp);
                            }
                            const double reward_pct = (best_signal.take_profit_2 - best_signal.entry_price) / std::max(1e-9, best_signal.entry_price);
                            const double risk_pct = (best_signal.entry_price - best_signal.stop_loss) / std::max(1e-9, best_signal.entry_price);
                            const double rr = (risk_pct > 1e-9) ? (reward_pct / risk_pct) : 0.0;
                            const double round_trip_cost = computeEffectiveRoundTripCostPct(best_signal, tuned_cfg);
                            const double net_edge = reward_pct - round_trip_cost;
                            const double calibrated_edge = computeCalibratedExpectedEdgePct(best_signal, tuned_cfg);
                            const double tracked_expected_edge =
                                (std::isfinite(calibrated_edge))
                                    ? calibrated_edge
                                    : ((best_signal.expected_value != 0.0) ? best_signal.expected_value : net_edge);
                            risk_manager_->setPositionSignalInfo(
                                market_name_,
                                best_signal.signal_filter,
                                best_signal.strength,
                                best_signal.market_regime,
                                best_signal.liquidity_score,
                                best_signal.volatility,
                                tracked_expected_edge,
                                rr,
                                best_signal.entry_archetype
                            );
                            auto selected_strategy = strategy_manager_->getStrategy(best_signal.strategy_name);
                            if (selected_strategy &&
                                !selected_strategy->onSignalAccepted(best_signal, order_amount)) {
                                LOG_WARN("{} strategy accepted backtest entry but state registration was skipped: {}",
                                         market_name_, best_signal.strategy_name);
                            }
                            entry_executed = true;
                            entry_funnel_.entries_executed++;
                            if (!best_signal.strategy_name.empty()) {
                                strategy_entries_executed_counts_[best_signal.strategy_name]++;
                            }
                        } else {
                            entry_funnel_.blocked_order_sizing++;
                        }
                    } else {
                        entry_funnel_.blocked_min_order_or_capital++;
                    }
                    }
                }
            }
        }

        if (entry_executed) {
            no_entry_streak_candles_ = 0;
        } else {
            no_entry_streak_candles_++;
        }
    } else {
        entry_funnel_.skipped_due_to_open_position++;
    }

    // Execute pending order lifecycle transitions for this candle.
    checkOrders(candle);

    // 5. Update Portfolio Value for Drawdown calculation
    double current_equity = risk_manager_->getRiskMetrics().total_capital;
    
    if (current_equity > max_balance_) {
        max_balance_ = current_equity;
    }
    
    double drawdown = (max_balance_ > 0) ? (max_balance_ - current_equity) / max_balance_ : 0.0;
    if (drawdown > max_drawdown_) {
        max_drawdown_ = drawdown;
    }
    
    // 6. Self-Learning Update
    updateDynamicFilter();
}

void BacktestEngine::checkOrders(const Candle& candle) {
    (void)candle;
    if (pending_orders_.empty()) {
        return;
    }

    const double fee_rate = Config::getInstance().getFeeRate();
    for (const auto& pending : pending_orders_) {
        const auto transitioned = core::execution::OrderLifecycleStateMachine::transition(
            "filled",
            0.0,
            pending.order.volume,
            pending.order.volume,
            0.0
        );
        const double trade_amount = pending.requested_price * transitioned.filled_volume;
        const double fee = trade_amount * fee_rate;
        (void)fee;

        LOG_INFO(
            "Execution lifecycle: source={}, event={}, order_id={}, market={}, side={}, status={}, filled={:.8f}, volume={:.8f}, terminal={}",
            "backtest",
            "filled",
            pending.order.order_id,
            pending.order.market,
            core::execution::orderSideToString(pending.order.side),
            core::execution::orderStatusToString(transitioned.status),
            transitioned.filled_volume,
            pending.order.volume,
            transitioned.terminal ? "true" : "false"
        );
        appendExecutionUpdateArtifact(
            core::execution::makeExecutionUpdate(
                "backtest",
                "filled",
                pending.order.order_id,
                pending.order.market,
                pending.order.side,
                transitioned.status,
                transitioned.filled_volume,
                pending.order.volume,
                pending.requested_price,
                pending.order.strategy_name,
                transitioned.terminal,
                getCurrentTimeMs()
            )
        );

        if (transitioned.terminal &&
            (pending.order.side == OrderSide::BUY || pending.order.side == OrderSide::SELL)) {
            total_trades_++;
        }
    }

    pending_orders_.clear();
}

void BacktestEngine::executeOrder(const Order& order, double price) {
    Order queued = order;
    if (queued.order_id.empty()) {
        queued.order_id = "bt-" + std::to_string(++backtest_order_seq_);
    }

    const auto submitted = core::execution::OrderLifecycleStateMachine::transition(
        "submitted",
        0.0,
        queued.volume,
        0.0,
        queued.volume
    );
    LOG_INFO(
        "Execution lifecycle: source={}, event={}, order_id={}, market={}, side={}, status={}, filled={:.8f}, volume={:.8f}, terminal={}",
        "backtest",
        "submitted",
        queued.order_id,
        queued.market,
        core::execution::orderSideToString(queued.side),
        core::execution::orderStatusToString(submitted.status),
        submitted.filled_volume,
        queued.volume,
        submitted.terminal ? "true" : "false"
    );
    appendExecutionUpdateArtifact(
        core::execution::makeExecutionUpdate(
            "backtest",
            "submitted",
            queued.order_id,
            queued.market,
            queued.side,
            submitted.status,
            submitted.filled_volume,
            queued.volume,
            price,
            queued.strategy_name,
            submitted.terminal,
            getCurrentTimeMs()
        )
    );

    PendingBacktestOrder pending;
    pending.order = queued;
    pending.requested_price = price;
    pending.enqueued_at_ms = getCurrentTimeMs();
    pending_orders_.push_back(std::move(pending));
}

BacktestEngine::Result BacktestEngine::getResult() const {
    Result result{};
    double final_equity = balance_krw_;
    if (risk_manager_) {
        final_equity = risk_manager_->getRiskMetrics().total_capital;
    } else if (!history_data_.empty()) {
        final_equity += balance_asset_ * history_data_.back().close;
    }

    int closed_trades = 0;
    int wins = 0;
    int losses = 0;
    double gross_profit = 0.0;
    double gross_loss_abs = 0.0;
    double total_holding_minutes = 0.0;
    double total_fees = 0.0;
    std::map<std::string, int> exit_reason_counts;
    std::map<std::string, Result::StrategySummary> strategy_map;
    struct PatternAgg {
        int total_trades = 0;
        int winning_trades = 0;
        int losing_trades = 0;
        double gross_profit = 0.0;
        double gross_loss_abs = 0.0;
        double total_profit = 0.0;
    };
    std::map<std::string, PatternAgg> pattern_map;
    if (risk_manager_) {
        const auto history = risk_manager_->getTradeHistory();
        closed_trades = static_cast<int>(history.size());
        for (const auto& trade : history) {
            const std::string strategy_name = trade.strategy_name.empty() ? "unknown" : trade.strategy_name;
            auto& ss = strategy_map[strategy_name];
            ss.strategy_name = strategy_name;
            ss.total_trades++;
            ss.total_profit += trade.profit_loss;
            total_fees += std::max(0.0, trade.fee_paid);
            if (trade.exit_time > trade.entry_time) {
                total_holding_minutes += static_cast<double>(trade.exit_time - trade.entry_time) / 60000.0;
            }
            const std::string reason = trade.exit_reason.empty() ? "unknown" : trade.exit_reason;
            exit_reason_counts[reason]++;

            if (trade.profit_loss > 0.0) {
                ++wins;
                gross_profit += trade.profit_loss;
                ss.winning_trades++;
                ss.avg_win_krw += trade.profit_loss; // temp as gross win accumulator
            } else if (trade.profit_loss < 0.0) {
                ++losses;
                gross_loss_abs += std::abs(trade.profit_loss);
                ss.losing_trades++;
                ss.avg_loss_krw += std::abs(trade.profit_loss); // temp as gross loss accumulator
            }

            const std::string regime_label = regimeToLabel(trade.market_regime);
            const std::string archetype_label =
                trade.entry_archetype.empty() ? "UNSPECIFIED" : trade.entry_archetype;
            const std::string strength_bucket = strengthBucket(trade.signal_strength);
            const double entry_notional = trade.entry_price * trade.quantity;
            const double realized_net_edge = (entry_notional > 1e-9)
                ? (trade.profit_loss / entry_notional)
                : trade.profit_loss_pct;
            // Keep EV buckets conservative: realized outcome may demote the bucket,
            // but a single good exit should not promote a weak expected edge signal.
            double realized_aligned_edge = trade.expected_value;
            if (std::isfinite(trade.expected_value)) {
                const double blended_edge =
                    (trade.expected_value * 0.80) + (realized_net_edge * 0.20);
                realized_aligned_edge = std::min(trade.expected_value, blended_edge);
            } else {
                realized_aligned_edge = realized_net_edge;
            }
            const std::string ev_bucket = expectedValueBucket(realized_aligned_edge);
            const std::string rr_bucket = rewardRiskBucket(trade.reward_risk_ratio);
            const std::string pattern_key =
                strategy_name + "|" + archetype_label + "|" + regime_label + "|" +
                strength_bucket + "|" + ev_bucket + "|" + rr_bucket;
            auto& pa = pattern_map[pattern_key];
            pa.total_trades++;
            pa.total_profit += trade.profit_loss;
            if (trade.profit_loss > 0.0) {
                pa.winning_trades++;
                pa.gross_profit += trade.profit_loss;
            } else if (trade.profit_loss < 0.0) {
                pa.losing_trades++;
                pa.gross_loss_abs += std::abs(trade.profit_loss);
            }
        }

        for (auto& [_, ss] : strategy_map) {
            ss.win_rate = (ss.total_trades > 0)
                ? (static_cast<double>(ss.winning_trades) / static_cast<double>(ss.total_trades))
                : 0.0;
            const double gross_win = ss.avg_win_krw;
            const double gross_loss = ss.avg_loss_krw;
            ss.avg_win_krw = (ss.winning_trades > 0) ? (gross_win / static_cast<double>(ss.winning_trades)) : 0.0;
            ss.avg_loss_krw = (ss.losing_trades > 0) ? (gross_loss / static_cast<double>(ss.losing_trades)) : 0.0;
            if (gross_loss > 1e-12) {
                ss.profit_factor = gross_win / gross_loss;
            } else {
                ss.profit_factor = (gross_win > 1e-12) ? 99.9 : 0.0;
            }
            result.strategy_summaries.push_back(ss);
        }
        std::sort(result.strategy_summaries.begin(), result.strategy_summaries.end(),
            [](const Result::StrategySummary& a, const Result::StrategySummary& b) {
                return a.total_profit > b.total_profit;
            });

        for (const auto& [key, pa] : pattern_map) {
            const size_t p1 = key.find('|');
            const size_t p2 = (p1 == std::string::npos) ? std::string::npos : key.find('|', p1 + 1);
            const size_t p3 = (p2 == std::string::npos) ? std::string::npos : key.find('|', p2 + 1);
            const size_t p4 = (p3 == std::string::npos) ? std::string::npos : key.find('|', p3 + 1);
            const size_t p5 = (p4 == std::string::npos) ? std::string::npos : key.find('|', p4 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos ||
                p4 == std::string::npos || p5 == std::string::npos) {
                continue;
            }
            Result::PatternSummary ps;
            ps.strategy_name = key.substr(0, p1);
            ps.entry_archetype = key.substr(p1 + 1, p2 - p1 - 1);
            ps.regime = key.substr(p2 + 1, p3 - p2 - 1);
            ps.strength_bucket = key.substr(p3 + 1, p4 - p3 - 1);
            ps.expected_value_bucket = key.substr(p4 + 1, p5 - p4 - 1);
            ps.reward_risk_bucket = key.substr(p5 + 1);
            ps.total_trades = pa.total_trades;
            ps.winning_trades = pa.winning_trades;
            ps.losing_trades = pa.losing_trades;
            ps.win_rate = (pa.total_trades > 0)
                ? (static_cast<double>(pa.winning_trades) / static_cast<double>(pa.total_trades))
                : 0.0;
            ps.total_profit = pa.total_profit;
            ps.avg_profit_krw = (pa.total_trades > 0)
                ? (pa.total_profit / static_cast<double>(pa.total_trades))
                : 0.0;
            if (pa.gross_loss_abs > 1e-12) {
                ps.profit_factor = pa.gross_profit / pa.gross_loss_abs;
            } else {
                ps.profit_factor = (pa.gross_profit > 1e-12) ? 99.9 : 0.0;
            }
            result.pattern_summaries.push_back(std::move(ps));
        }
        std::sort(result.pattern_summaries.begin(), result.pattern_summaries.end(),
            [](const Result::PatternSummary& a, const Result::PatternSummary& b) {
                if (a.strategy_name != b.strategy_name) return a.strategy_name < b.strategy_name;
                if (a.entry_archetype != b.entry_archetype) return a.entry_archetype < b.entry_archetype;
                if (a.regime != b.regime) return a.regime < b.regime;
                return a.total_trades > b.total_trades;
            });
    } else {
        closed_trades = total_trades_;
        wins = winning_trades_;
        losses = std::max(0, closed_trades - wins);
    }

    const double win_rate = (closed_trades > 0)
        ? static_cast<double>(wins) / static_cast<double>(closed_trades)
        : 0.0;
    const double avg_win_krw = (wins > 0) ? (gross_profit / static_cast<double>(wins)) : 0.0;
    const double avg_loss_krw = (losses > 0) ? (gross_loss_abs / static_cast<double>(losses)) : 0.0;
    const double profit_factor =
        (gross_loss_abs > 1e-12) ? (gross_profit / gross_loss_abs) : ((gross_profit > 1e-12) ? 99.9 : 0.0);
    const double expectancy_krw = (closed_trades > 0)
        ? ((gross_profit - gross_loss_abs) / static_cast<double>(closed_trades))
        : 0.0;

    result.final_balance = final_equity;
    result.total_profit = final_equity - Config::getInstance().getInitialCapital();
    result.max_drawdown = max_drawdown_;
    result.total_trades = closed_trades;
    result.winning_trades = wins;
    result.losing_trades = losses;
    result.win_rate = win_rate;
    result.avg_win_krw = risk_manager_ ? avg_win_krw : 0.0;
    result.avg_loss_krw = risk_manager_ ? avg_loss_krw : 0.0;
    result.profit_factor = risk_manager_ ? profit_factor : 0.0;
    result.expectancy_krw = risk_manager_ ? expectancy_krw : 0.0;
    result.avg_holding_minutes = (closed_trades > 0) ? (total_holding_minutes / static_cast<double>(closed_trades)) : 0.0;
    result.avg_fee_krw = (closed_trades > 0) ? (total_fees / static_cast<double>(closed_trades)) : 0.0;
    result.exit_reason_counts = std::move(exit_reason_counts);
    result.intrabar_stop_tp_collision_count = intrabar_stop_tp_collision_count_;
    result.intrabar_collision_by_strategy = intrabar_collision_by_strategy_;
    std::set<std::string> strategy_names;
    for (const auto& [name, _] : strategy_generated_counts_) {
        strategy_names.insert(name);
    }
    for (const auto& [name, _] : strategy_selected_best_counts_) {
        strategy_names.insert(name);
    }
    for (const auto& [name, _] : strategy_blocked_by_risk_manager_counts_) {
        strategy_names.insert(name);
    }
    for (const auto& [name, _] : strategy_entries_executed_counts_) {
        strategy_names.insert(name);
    }
    for (const auto& strategy_name : strategy_names) {
        Result::StrategySignalFunnel item;
        item.strategy_name = strategy_name;
        item.generated_signals = strategy_generated_counts_.count(strategy_name)
            ? strategy_generated_counts_.at(strategy_name) : 0;
        item.selected_best = strategy_selected_best_counts_.count(strategy_name)
            ? strategy_selected_best_counts_.at(strategy_name) : 0;
        item.blocked_by_risk_manager = strategy_blocked_by_risk_manager_counts_.count(strategy_name)
            ? strategy_blocked_by_risk_manager_counts_.at(strategy_name) : 0;
        item.entries_executed = strategy_entries_executed_counts_.count(strategy_name)
            ? strategy_entries_executed_counts_.at(strategy_name) : 0;
        result.strategy_signal_funnel.push_back(std::move(item));
    }
    std::sort(result.strategy_signal_funnel.begin(), result.strategy_signal_funnel.end(),
        [](const Result::StrategySignalFunnel& a, const Result::StrategySignalFunnel& b) {
            if (a.generated_signals != b.generated_signals) {
                return a.generated_signals > b.generated_signals;
            }
            return a.strategy_name < b.strategy_name;
        });
    result.entry_funnel = entry_funnel_;
    return result;
}



void BacktestEngine::updateDynamicFilter() {
    auto history = risk_manager_->getTradeHistory();
    if (history.size() < 20) {
        return;
    }

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
        dynamic_filter_value_ = std::min(0.70, dynamic_filter_value_ + 0.01);
    } else if (expectancy > 0.0 && profit_factor > 1.15 && win_rate >= 0.50) {
        dynamic_filter_value_ = std::max(0.35, dynamic_filter_value_ - 0.01);
    } else {
        if (dynamic_filter_value_ > 0.50) {
            dynamic_filter_value_ = std::max(0.35, dynamic_filter_value_ - 0.003);
        } else if (dynamic_filter_value_ < 0.50) {
            dynamic_filter_value_ = std::min(0.70, dynamic_filter_value_ + 0.003);
        }
    }
}

} // namespace backtest
} // namespace autolife
