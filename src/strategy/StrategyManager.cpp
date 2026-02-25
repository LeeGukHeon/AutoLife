#include "strategy/StrategyManager.h"
#include "strategy/FoundationRiskPipeline.h"
#include "common/Logger.h"
#include "common/Config.h"
#include "common/RuntimeDiagnosticsShared.h"
#include "risk/RiskManager.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#undef max
#undef min

namespace autolife {
namespace strategy {
namespace {
std::string toLowerCopy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
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
    double profitFactor() const {
        return (gross_loss_abs > 1e-12) ? (gross_profit / gross_loss_abs) : 0.0;
    }
    double winRate() const {
        return (trades > 0) ? (static_cast<double>(wins) / static_cast<double>(trades)) : 0.0;
    }
};

double estimateSignalRoundTripCostPctForManager(const Signal& signal) {
    const double fee_rate_per_side = autolife::Config::getInstance().getFeeRate();
    double slippage_per_side = std::clamp(
        autolife::Config::getInstance().getMaxSlippagePct() * 0.35,
        0.00045,
        0.00120
    );

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

double computeImpliedWinProbForManager(const Signal& signal) {
    double implied_win = std::clamp(signal.strength, 0.35, 0.75);

    if (signal.strategy_trade_count >= 8) {
        const double sample_weight = std::clamp(
            (static_cast<double>(signal.strategy_trade_count) - 8.0) / 40.0,
            0.0,
            1.0
        );
        const double history_win = std::clamp(signal.strategy_win_rate, 0.20, 0.82);
        implied_win = ((1.0 - sample_weight) * implied_win) + (sample_weight * history_win);

        if (signal.strategy_profit_factor > 0.0) {
            implied_win += std::clamp((signal.strategy_profit_factor - 1.0) * 0.03, -0.04, 0.04);
        }
    }

    return std::clamp(implied_win, 0.18, 0.85);
}

double computeRewardRiskRatioForManager(const Signal& signal) {
    if (!(signal.entry_price > 0.0) || !(signal.stop_loss > 0.0)) {
        return 0.0;
    }
    const double take_profit = signal.getPrimaryTakeProfit();
    if (!(take_profit > 0.0)) {
        return 0.0;
    }
    const double risk = std::abs(signal.entry_price - signal.stop_loss);
    if (risk <= 1e-12) {
        return 0.0;
    }
    const double reward = std::abs(take_profit - signal.entry_price);
    return reward / risk;
}

double computeExpectedValueTighteningForManager(
    const Signal& signal,
    analytics::MarketRegime regime
) {
    double tighten = 0.0;

    if (regime == analytics::MarketRegime::TRENDING_DOWN) {
        tighten += 0.00012;
    } else if (regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        tighten += 0.00010;
    } else if (regime == analytics::MarketRegime::RANGING) {
        tighten += 0.00003;
    }

    if (signal.liquidity_score > 0.0 && signal.liquidity_score < 55.0) {
        tighten += std::clamp((55.0 - signal.liquidity_score) * 0.0000030, 0.0, 0.00018);
    }
    if (signal.volatility > 4.5) {
        tighten += std::clamp((signal.volatility - 4.5) * 0.000025, 0.0, 0.00012);
    }

    const double reward_risk_ratio = computeRewardRiskRatioForManager(signal);
    if (reward_risk_ratio > 0.0) {
        if (reward_risk_ratio < 1.10) {
            tighten += 0.00010;
        } else if (reward_risk_ratio < 1.25) {
            tighten += 0.00006;
        }
    }

    if (signal.strategy_trade_count >= 20 &&
        (signal.strategy_win_rate < 0.45 ||
         (signal.strategy_profit_factor > 0.0 && signal.strategy_profit_factor < 1.00))) {
        tighten += 0.00012;
    }

    return std::clamp(tighten, 0.0, 0.00040);
}

double estimateLossToWinRatioFromWinRateAndPf(double win_rate, double profit_factor) {
    if (profit_factor <= 1e-9 || win_rate <= 1e-6 || win_rate >= (1.0 - 1e-6)) {
        return 0.0;
    }
    const double loss_rate = 1.0 - win_rate;
    if (loss_rate <= 1e-9) {
        return 0.0;
    }
    return win_rate / (loss_rate * profit_factor);
}

bool coreSignalOwnershipEnabledForManager() {
    const auto& cfg = Config::getInstance().getEngineConfig();
    return cfg.enable_core_plane_bridge && cfg.enable_core_risk_plane;
}

void incrementRejectionReason(std::map<std::string, int>* bucket, const char* reason) {
    if (bucket == nullptr || reason == nullptr || reason[0] == '\0') {
        return;
    }
    (*bucket)[reason]++;
}

void incrementCounter(std::map<std::string, int>* bucket, const std::string& key, int delta = 1) {
    if (bucket == nullptr || key.empty() || delta <= 0) {
        return;
    }
    (*bucket)[key] += delta;
}

void recordPhase3SliceCounter(
    std::map<std::string, int>* bucket,
    const std::string& prefix,
    const std::string& label,
    bool passed
) {
    if (bucket == nullptr) {
        return;
    }
    const std::string safe_label = label.empty() ? "UNKNOWN" : label;
    incrementCounter(bucket, prefix + "::" + safe_label + "::candidate", 1);
    if (passed) {
        incrementCounter(bucket, prefix + "::" + safe_label + "::pass", 1);
    }
}

void recordPhase3DiagnosticsV2(
    std::map<std::string, int>* bucket,
    const Signal& signal,
    bool passed
) {
    if (bucket == nullptr || !signal.phase3.diagnostics_v2_enabled) {
        return;
    }
    incrementCounter(bucket, "candidate_total", 1);
    if (passed) {
        incrementCounter(bucket, "pass_total", 1);
    }

    recordPhase3SliceCounter(
        bucket,
        "pass_rate_by_regime",
        autolife::common::runtime_diag::marketRegimeLabel(signal.market_regime),
        passed
    );
    recordPhase3SliceCounter(
        bucket,
        "pass_rate_by_vol_bucket",
        autolife::common::runtime_diag::volatilityBucket(signal.volatility),
        passed
    );
    recordPhase3SliceCounter(
        bucket,
        "pass_rate_by_liquidity_bucket",
        autolife::common::runtime_diag::liquidityBucket(signal.liquidity_score),
        passed
    );
    recordPhase3SliceCounter(
        bucket,
        "pass_rate_edge_regressor_present_vs_fallback",
        signal.phase3.edge_regressor_used ? "edge_regressor_present" : "edge_fallback",
        passed
    );
    recordPhase3SliceCounter(
        bucket,
        "pass_rate_by_cost_mode",
        signal.phase3.cost_mode,
        passed
    );
}

void recordPhase3RejectDiagnosticsV2(
    std::map<std::string, int>* bucket,
    const foundation::FilterDecision& decision,
    const Signal& signal
) {
    if (bucket == nullptr || !signal.phase3.diagnostics_v2_enabled) {
        return;
    }
    if (!decision.margin_pass) {
        incrementCounter(bucket, "reject_margin_insufficient", 1);
    }
    if (!decision.strength_pass) {
        incrementCounter(bucket, "reject_strength_fail", 1);
    }
    if (!decision.expected_value_pass) {
        incrementCounter(bucket, "reject_expected_value_fail", 1);
    }
    if (decision.frontier_enabled && !decision.frontier_pass) {
        incrementCounter(bucket, "reject_frontier_fail", 1);
    }
    if (!decision.ev_confidence_pass) {
        incrementCounter(bucket, "reject_ev_confidence_low", 1);
    }
    if (signal.phase3.cost_tail_enabled && !decision.cost_tail_pass) {
        incrementCounter(bucket, "reject_cost_tail_fail", 1);
    }
}

}

StrategyManager::StrategyManager(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
{
    LOG_INFO("StrategyManager initialized");
}

void StrategyManager::registerStrategy(std::shared_ptr<IStrategy> strategy) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto info = strategy->getInfo();
    strategies_.push_back(strategy);

    LOG_INFO("Strategy registered: {} (expected win rate: {:.1f}%, risk: {}/10)",
             info.name, info.expected_winrate * 100, info.risk_level);
}

std::shared_ptr<IStrategy> StrategyManager::getStrategy(const std::string& name) {
    for (const auto& strategy : strategies_) {
        if (strategy->getInfo().name == name) {
            return strategy;
        }
    }
    return nullptr;
}

Signal StrategyManager::processStrategySignal(
    std::shared_ptr<IStrategy> strategy,
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime
) {
    Signal signal;
    try {
        if (!strategy->isEnabled()) {
            return signal;
        }

        signal = strategy->generateSignal(market, metrics, candles, current_price, available_capital, regime);

        if (signal.type != SignalType::NONE) {
            auto stats = strategy->getStatistics();

            signal.strategy_win_rate = stats.win_rate;
            signal.strategy_profit_factor = stats.profit_factor;
            signal.strategy_trade_count = stats.total_signals;
            signal.liquidity_score = metrics.liquidity_score;
            signal.volatility = metrics.volatility;

            if (signal.entry_price > 0 && signal.take_profit_2 > 0) {
                signal.expected_return_pct = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
            }

            if (signal.entry_price > 0 && signal.stop_loss > 0) {
                signal.expected_risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
            }

            const double implied_win = computeImpliedWinProbForManager(signal);
            if (signal.expected_return_pct > 0.0 && signal.expected_risk_pct > 0.0) {
                const double expected_gross_pct =
                    (implied_win * signal.expected_return_pct) -
                    ((1.0 - implied_win) * signal.expected_risk_pct);
                // Pre-filter stage stays slightly permissive; final gate uses full cost in engine.
                const double prefilter_cost_pct = estimateSignalRoundTripCostPctForManager(signal) * 0.55;
                signal.expected_value = expected_gross_pct - prefilter_cost_pct;
            } else {
                signal.expected_value = 0.0;
            }

            signal.score = calculateSignalScore(signal);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("{} - {} analysis failed: {}", market, strategy->getInfo().name, e.what());
        signal.reason = "strategy_execution_exception";
    }

    return signal;
}

std::vector<Signal> StrategyManager::collectSignals(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime,
    CollectDiagnostics* diagnostics
) {
    std::vector<Signal> signals;
    if (diagnostics != nullptr) {
        diagnostics->strategy_total = 0;
        diagnostics->strategy_enabled = 0;
        diagnostics->generated_signal_count = 0;
        diagnostics->skipped_disabled_count = 0;
        diagnostics->no_signal_count = 0;
        diagnostics->exception_count = 0;
        diagnostics->generated_by_strategy.clear();
        diagnostics->skipped_disabled_by_strategy.clear();
        diagnostics->no_signal_by_strategy.clear();
        diagnostics->no_signal_reason_counts.clear();
        diagnostics->exception_by_strategy.clear();
    }

    // Execute sequentially to avoid API burst/race in live mode.
    for (auto& strategy : strategies_) {
        const std::string strategy_name = strategy ? strategy->getInfo().name : std::string("unknown");
        if (diagnostics != nullptr) {
            diagnostics->strategy_total++;
        }
        if (!strategy || !strategy->isEnabled()) {
            if (diagnostics != nullptr) {
                diagnostics->skipped_disabled_count++;
                diagnostics->skipped_disabled_by_strategy[strategy_name]++;
            }
            continue;
        }
        if (diagnostics != nullptr) {
            diagnostics->strategy_enabled++;
        }
        try {
            Signal signal = processStrategySignal(
                strategy, market, metrics, candles, current_price, available_capital, regime
            );
            if (signal.type != SignalType::NONE) {
                signals.push_back(signal);
                if (diagnostics != nullptr) {
                    diagnostics->generated_signal_count++;
                    diagnostics->generated_by_strategy[strategy_name]++;
                }
                LOG_INFO(
                    "{} - {} signal generated: strength {:.2f}, score {:.2f}, tf5m_preloaded={}, tf1h_preloaded={}, fallback={}",
                    market,
                    signal.strategy_name,
                    signal.strength,
                    signal.score,
                    signal.used_preloaded_tf_5m ? "Y" : "N",
                    signal.used_preloaded_tf_1h ? "Y" : "N",
                    signal.used_resampled_tf_fallback ? "Y" : "N");
            } else {
                if (diagnostics != nullptr) {
                    diagnostics->no_signal_count++;
                    diagnostics->no_signal_by_strategy[strategy_name]++;
                    const std::string no_signal_reason = signal.reason.empty()
                        ? "no_signal_reason_unspecified"
                        : signal.reason;
                    diagnostics->no_signal_reason_counts[no_signal_reason]++;
                }
            }
        } catch (const std::exception& e) {
            if (diagnostics != nullptr) {
                diagnostics->exception_count++;
                diagnostics->exception_by_strategy[strategy_name]++;
            }
            LOG_ERROR("Strategy execution exception ({}): {}", strategy->getInfo().name, e.what());
        }
    }

    if (!signals.empty()) {
        LOG_INFO("{} - strategy analysis complete ({} signals)", market, signals.size());
    }

    return signals;
}

std::vector<Signal> StrategyManager::filterSignalsWithDiagnostics(
    const std::vector<Signal>& signals,
    double min_strength,
    double min_expected_value,
    analytics::MarketRegime regime,
    FilterDiagnostics* diagnostics
) {
    std::vector<Signal> filtered;
    const bool core_signal_ownership = coreSignalOwnershipEnabledForManager();
    if (diagnostics != nullptr) {
        diagnostics->input_count = static_cast<int>(signals.size());
        diagnostics->output_count = 0;
        diagnostics->rejection_reason_counts.clear();
    }
    auto* rejection_bucket =
        diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr;

    // Rebuilt from baseline: deterministic, decomposed filter pipeline.
    for (const auto& signal : signals) {
        const StrategyRole role = detectStrategyRole(signal.strategy_name);
        const RegimePolicy policy = getRegimePolicy(role, regime);
        const bool trend_role = (role == StrategyRole::MOMENTUM || role == StrategyRole::BREAKOUT);
        const bool off_trend_regime = trend_role && regime != analytics::MarketRegime::TRENDING_UP;
        const bool hostile_regime =
            regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            regime == analytics::MarketRegime::TRENDING_DOWN;

        const PerformanceGate gate = getPerformanceGate(role, regime);

        foundation::FilterInput input;
        input.signal = &signal;
        input.regime = regime;
        input.min_strength = min_strength;
        input.min_expected_value = min_expected_value;
        input.core_signal_ownership = core_signal_ownership;
        input.policy_block = (policy == RegimePolicy::BLOCK) && !core_signal_ownership;
        input.policy_hold = (policy == RegimePolicy::HOLD) ||
            ((policy == RegimePolicy::BLOCK) && core_signal_ownership);
        input.off_trend_regime = off_trend_regime;
        input.hostile_regime = hostile_regime;
        input.min_history_sample = gate.min_sample_trades;
        input.min_history_win_rate = gate.min_win_rate;
        input.min_history_profit_factor = gate.min_profit_factor;

        const foundation::FilterDecision decision = foundation::evaluateFilter(input);
        if (decision.pass) {
            filtered.push_back(signal);
            recordPhase3DiagnosticsV2(rejection_bucket, signal, true);
            continue;
        }

        recordPhase3DiagnosticsV2(rejection_bucket, signal, false);
        recordPhase3RejectDiagnosticsV2(rejection_bucket, decision, signal);
        incrementRejectionReason(
            rejection_bucket,
            decision.reject_reason.empty()
                ? "filtered_out_by_manager_unknown"
                : decision.reject_reason.c_str()
        );
    }

    if (diagnostics != nullptr) {
        diagnostics->output_count = static_cast<int>(filtered.size());
    }
    return filtered;
}

std::map<std::string, IStrategy::Statistics> StrategyManager::getAllStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::map<std::string, IStrategy::Statistics> stats_map;
    for (const auto& strategy : strategies_) {
        auto info = strategy->getInfo();
        stats_map[info.name] = strategy->getStatistics();
    }
    return stats_map;
}

std::vector<std::shared_ptr<IStrategy>> StrategyManager::getStrategies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return strategies_;
}

void StrategyManager::refreshStrategyStatesFromHistory(
    const std::vector<risk::TradeHistory>& history,
    analytics::MarketRegime dominant_regime,
    bool avoid_high_volatility,
    bool avoid_trending_down,
    int min_trades_for_ev,
    double min_expectancy_krw,
    double min_profit_factor
) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::map<std::string, StrategyEdgeStats> edge;
    for (const auto& trade : history) {
        if (trade.strategy_name.empty()) {
            continue;
        }
        auto& s = edge[trade.strategy_name];
        s.trades++;
        s.net_profit += trade.profit_loss;
        if (trade.profit_loss > 0.0) {
            s.wins++;
            s.gross_profit += trade.profit_loss;
        } else if (trade.profit_loss < 0.0) {
            s.gross_loss_abs += std::abs(trade.profit_loss);
        }
    }

    for (auto& strategy : strategies_) {
        const auto info = strategy->getInfo();
        const std::string strategy_name = info.name;
        const StrategyRole role = detectStrategyRole(strategy_name);
        bool enable = true;

        if (avoid_high_volatility && dominant_regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            enable = false;
        }
        if (avoid_trending_down && dominant_regime == analytics::MarketRegime::TRENDING_DOWN) {
            enable = false;
        }

        const RegimePolicy policy = getRegimePolicy(role, dominant_regime);
        if (policy == RegimePolicy::BLOCK) {
            enable = false;
        }

        const auto stat_it = edge.find(strategy_name);
        if (stat_it != edge.end()) {
            const auto& stat = stat_it->second;
            if (stat.trades >= min_trades_for_ev) {
                const bool ev_ok =
                    (stat.expectancy() >= min_expectancy_krw) &&
                    (stat.profitFactor() >= min_profit_factor);
                if (!ev_ok) {
                    enable = false;
                }
            }

            const PerformanceGate gate = getPerformanceGate(role, dominant_regime);
            if (stat.trades >= gate.min_sample_trades) {
                if (stat.winRate() < gate.min_win_rate ||
                    stat.profitFactor() < gate.min_profit_factor) {
                    enable = false;
                }
            }
        }

        if (strategy->isEnabled() != enable) {
            strategy->setEnabled(enable);
            LOG_INFO("Strategy state changed: {} -> {} (manager policy)",
                     strategy_name, enable ? "ON" : "OFF");
        }
    }
}

double StrategyManager::calculateSignalScore(const Signal& signal) const {
    double score = signal.strength;

    switch (signal.type) {
        case SignalType::STRONG_BUY:
        case SignalType::STRONG_SELL:
            score *= 1.5;
            break;
        case SignalType::BUY:
        case SignalType::SELL:
            score *= 1.0;
            break;
        default:
            score *= 0.5;
            break;
    }

    double tp = signal.getPrimaryTakeProfit();
    if (signal.entry_price > 0 && signal.stop_loss > 0 && tp > 0) {
        double risk = std::abs(signal.entry_price - signal.stop_loss);
        double reward = std::abs(tp - signal.entry_price);

        if (risk > 0) {
            double rr_ratio = reward / risk;
            score *= std::min(2.0, rr_ratio / 2.0);
        }
    }

    if (signal.strategy_trade_count >= 30) {
        if (signal.strategy_profit_factor >= 1.5) {
            score *= 1.15;
        } else if (signal.strategy_profit_factor < 1.1) {
            score *= 0.90;
        }

        if (signal.strategy_win_rate >= 0.60) {
            score *= 1.10;
        } else if (signal.strategy_win_rate < 0.45) {
            score *= 0.90;
        }
    }

    if (signal.liquidity_score > 0.0) {
        if (signal.liquidity_score < 30.0) {
            score *= 0.75;
        } else if (signal.liquidity_score < 50.0) {
            score *= 0.90;
        } else if (signal.liquidity_score >= 80.0) {
            score *= 1.05;
        }
    }

    if (signal.volatility > 0.0) {
        if (signal.volatility < 0.6) {
            score *= 0.85;
        } else if (signal.volatility > 6.0) {
            score *= 0.90;
        }
    }

    if (signal.expected_value != 0.0) {
        double ev_boost = std::clamp(signal.expected_value * 10.0, -0.5, 0.5);
        score *= (1.0 + ev_boost);
    }

    if (signal.probabilistic_runtime_applied) {
        const bool hostile_regime =
            signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
        const double margin_conf =
            std::clamp((signal.probabilistic_h5_margin + 0.02) / 0.16, 0.0, 1.0);
        const double calibrated_conf =
            std::clamp((signal.probabilistic_h5_calibrated - 0.50) / 0.25, 0.0, 1.0);
        double boost = 1.0 + (margin_conf * 0.30) + (calibrated_conf * 0.10);
        if (hostile_regime) {
            boost = 1.0 + ((boost - 1.0) * 0.70);
        }
        score *= std::clamp(boost, 0.90, 1.35);
    }

    return score;
}

StrategyManager::StrategyRole StrategyManager::detectStrategyRole(const std::string& strategy_name) const {
    const std::string n = toLowerCopy(strategy_name);
    if (n.find("probabilistic primary") != std::string::npos) {
        return StrategyRole::FOUNDATION;
    }
    if (n.find("foundation") != std::string::npos) {
        return StrategyRole::FOUNDATION;
    }
    if (n.find("scalping") != std::string::npos) {
        return StrategyRole::SCALPING;
    }
    if (n.find("momentum") != std::string::npos) {
        return StrategyRole::MOMENTUM;
    }
    if (n.find("breakout") != std::string::npos) {
        return StrategyRole::BREAKOUT;
    }
    if (n.find("mean reversion") != std::string::npos || n.find("mean_reversion") != std::string::npos) {
        return StrategyRole::MEAN_REVERSION;
    }
    if (n.find("grid") != std::string::npos) {
        return StrategyRole::GRID;
    }
    return StrategyRole::OTHER;
}

StrategyManager::RegimePolicy StrategyManager::getRegimePolicy(
    StrategyRole role,
    analytics::MarketRegime regime
) const {
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            switch (role) {
                case StrategyRole::FOUNDATION:
                case StrategyRole::BREAKOUT:
                case StrategyRole::MOMENTUM:
                case StrategyRole::SCALPING:
                    return RegimePolicy::ALLOW;
                case StrategyRole::MEAN_REVERSION:
                case StrategyRole::GRID:
                case StrategyRole::OTHER:
                    return RegimePolicy::HOLD;
            }
            break;

        case analytics::MarketRegime::TRENDING_DOWN:
            switch (role) {
                case StrategyRole::FOUNDATION:
                    return RegimePolicy::HOLD;
                case StrategyRole::MOMENTUM:
                case StrategyRole::BREAKOUT:
                case StrategyRole::GRID:
                    return RegimePolicy::BLOCK;
                case StrategyRole::SCALPING:
                case StrategyRole::MEAN_REVERSION:
                case StrategyRole::OTHER:
                    return RegimePolicy::HOLD;
            }
            break;

        case analytics::MarketRegime::RANGING:
            switch (role) {
                case StrategyRole::FOUNDATION:
                    return RegimePolicy::ALLOW;
                case StrategyRole::MEAN_REVERSION:
                case StrategyRole::GRID:
                    return RegimePolicy::ALLOW;
                case StrategyRole::SCALPING:
                case StrategyRole::MOMENTUM:
                case StrategyRole::BREAKOUT:
                case StrategyRole::OTHER:
                    return RegimePolicy::HOLD;
            }
            break;

        case analytics::MarketRegime::HIGH_VOLATILITY:
            switch (role) {
                case StrategyRole::FOUNDATION:
                    return RegimePolicy::HOLD;
                case StrategyRole::MOMENTUM:
                case StrategyRole::BREAKOUT:
                    return RegimePolicy::BLOCK;
                case StrategyRole::SCALPING:
                case StrategyRole::MEAN_REVERSION:
                case StrategyRole::GRID:
                case StrategyRole::OTHER:
                    return RegimePolicy::HOLD;
            }
            break;

        default:
            return RegimePolicy::HOLD;
    }

    return RegimePolicy::HOLD;
}

StrategyManager::PerformanceGate StrategyManager::getPerformanceGate(
    StrategyRole role,
    analytics::MarketRegime regime
) const {
    PerformanceGate gate;
    gate.min_sample_trades = 20;

    // Objective separation by strategy role.
    switch (role) {
        case StrategyRole::FOUNDATION:
            gate.min_win_rate = 0.50;
            gate.min_profit_factor = 1.10;
            gate.min_sample_trades = 16;
            break;
        case StrategyRole::SCALPING:
            gate.min_win_rate = 0.60;      // high win-rate, lower R
            gate.min_profit_factor = 1.05;
            break;
        case StrategyRole::BREAKOUT:
            gate.min_win_rate = 0.42;      // low frequency, high R
            gate.min_profit_factor = 1.25;
            gate.min_sample_trades = 12;
            break;
        case StrategyRole::MEAN_REVERSION:
            gate.min_win_rate = 0.57;      // ranging specialist
            gate.min_profit_factor = 1.10;
            break;
        case StrategyRole::MOMENTUM:
            gate.min_win_rate = 0.50;
            gate.min_profit_factor = 1.15;
            break;
        case StrategyRole::GRID:
            gate.min_win_rate = 0.56;
            gate.min_profit_factor = 1.08;
            break;
        case StrategyRole::OTHER:
            gate.min_win_rate = 0.52;
            gate.min_profit_factor = 1.10;
            break;
    }

    // Regime-specific dual-gate tuning.
    if (regime == analytics::MarketRegime::TRENDING_DOWN) {
        gate.min_win_rate += 0.03;
        gate.min_profit_factor += 0.05;
    } else if (regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        gate.min_win_rate += 0.02;
        gate.min_profit_factor += 0.04;
    } else if (regime == analytics::MarketRegime::RANGING) {
        if (role == StrategyRole::MEAN_REVERSION || role == StrategyRole::GRID) {
            gate.min_win_rate = std::max(0.0, gate.min_win_rate - 0.02);
            gate.min_profit_factor = std::max(1.0, gate.min_profit_factor - 0.03);
        } else {
            gate.min_profit_factor += 0.03;
        }
    } else if (regime == analytics::MarketRegime::TRENDING_UP) {
        if (role == StrategyRole::BREAKOUT || role == StrategyRole::MOMENTUM) {
            gate.min_win_rate = std::max(0.0, gate.min_win_rate - 0.02);
            gate.min_profit_factor += 0.02;
        }
    }

    return gate;
}

} // namespace strategy
} // namespace autolife






