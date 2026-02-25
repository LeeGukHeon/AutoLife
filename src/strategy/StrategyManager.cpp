#include "strategy/StrategyManager.h"
#include "strategy/FoundationRiskPipeline.h"
#include "common/Logger.h"
#include "common/Config.h"
#include "common/RuntimeDiagnosticsShared.h"
#include "risk/RiskManager.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
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

            if (!std::isfinite(signal.expected_value)) {
                signal.expected_value = 0.0;
            }

            // Keep strategy-provided EV as source of truth.
            // Fallback only when strategy did not provide a usable EV.
            if (std::abs(signal.expected_value) <= 1e-12) {
                if (signal.expected_return_pct > 0.0 && signal.expected_risk_pct > 0.0) {
                    const double implied_win = std::clamp(signal.strength, 0.0, 1.0);
                    const double expected_gross_pct =
                        (implied_win * signal.expected_return_pct) -
                        ((1.0 - implied_win) * signal.expected_risk_pct);
                    const double fee_round_trip_pct =
                        std::max(0.0, autolife::Config::getInstance().getFeeRate()) * 2.0;
                    signal.expected_value = expected_gross_pct - fee_round_trip_pct;
                } else {
                    signal.expected_value = 0.0;
                }
            }

            signal.score = signal.strength;
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
        const bool off_trend_regime = false;
        const bool hostile_regime =
            regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            regime == analytics::MarketRegime::TRENDING_DOWN;

        PerformanceGate gate;
        if (role != StrategyRole::FOUNDATION) {
            gate.min_win_rate = 1.0;
            gate.min_profit_factor = 10.0;
            gate.min_sample_trades = std::numeric_limits<int>::max();
        } else {
            const auto& manager_policy = signal.phase3.manager_filter;
            const auto manager_defaults = Signal::Phase3PolicySnapshot::ManagerFilterPolicy{};
            const bool use_manager_policy = manager_policy.enabled;
            const auto manager_pick = [&](double policy_value, double fallback_value) {
                return use_manager_policy ? policy_value : fallback_value;
            };
            const auto manager_pick_int = [&](int policy_value, int fallback_value) {
                return use_manager_policy ? policy_value : fallback_value;
            };

            gate.min_win_rate = manager_pick(
                manager_policy.history_gate_min_win_rate_base,
                manager_defaults.history_gate_min_win_rate_base
            );
            gate.min_profit_factor = manager_pick(
                manager_policy.history_gate_min_profit_factor_base,
                manager_defaults.history_gate_min_profit_factor_base
            );
            gate.min_sample_trades = std::max(
                1,
                manager_pick_int(
                    manager_policy.history_gate_min_sample_trades_base,
                    manager_defaults.history_gate_min_sample_trades_base
                )
            );

            if (regime == analytics::MarketRegime::TRENDING_DOWN) {
                gate.min_win_rate += manager_pick(
                    manager_policy.history_gate_win_rate_add_trending_down,
                    manager_defaults.history_gate_win_rate_add_trending_down
                );
                gate.min_profit_factor += manager_pick(
                    manager_policy.history_gate_profit_factor_add_trending_down,
                    manager_defaults.history_gate_profit_factor_add_trending_down
                );
            } else if (regime == analytics::MarketRegime::HIGH_VOLATILITY) {
                gate.min_win_rate += manager_pick(
                    manager_policy.history_gate_win_rate_add_high_volatility,
                    manager_defaults.history_gate_win_rate_add_high_volatility
                );
                gate.min_profit_factor += manager_pick(
                    manager_policy.history_gate_profit_factor_add_high_volatility,
                    manager_defaults.history_gate_profit_factor_add_high_volatility
                );
            }

            gate.min_win_rate = std::clamp(gate.min_win_rate, 0.0, 1.0);
            gate.min_profit_factor = std::max(0.0, gate.min_profit_factor);
        }

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

StrategyManager::StrategyRole StrategyManager::detectStrategyRole(const std::string& strategy_name) const {
    const std::string n = toLowerCopy(strategy_name);
    if (n.find("probabilistic primary") != std::string::npos) {
        return StrategyRole::FOUNDATION;
    }
    if (n.find("foundation") != std::string::npos) {
        return StrategyRole::FOUNDATION;
    }
    return StrategyRole::OTHER;
}

StrategyManager::RegimePolicy StrategyManager::getRegimePolicy(
    StrategyRole role,
    analytics::MarketRegime regime
) const {
    (void)regime;
    if (role == StrategyRole::FOUNDATION) {
        return RegimePolicy::ALLOW;
    }
    return RegimePolicy::BLOCK;
}

} // namespace strategy
} // namespace autolife






