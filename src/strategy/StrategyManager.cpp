#include "strategy/StrategyManager.h"
#include "common/Logger.h"
#include "common/Config.h"
#include "common/RuntimeDiagnosticsShared.h"
#include "risk/RiskManager.h"
#include <algorithm>
#include <map>

#undef max
#undef min

namespace autolife {
namespace strategy {
namespace {
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
        diagnostics->liq_vol_gate_observation_count = 0;
        diagnostics->liq_vol_gate_pass_count = 0;
        diagnostics->liq_vol_gate_fail_count = 0;
        diagnostics->liq_vol_gate_low_conf_triggered_count = 0;
        diagnostics->liq_vol_gate_observed_sum = 0.0;
        diagnostics->liq_vol_gate_threshold_sum = 0.0;
        diagnostics->liq_vol_gate_mode = "legacy_fixed";
        diagnostics->liq_vol_gate_quantile_q = 0.0;
        diagnostics->liq_vol_gate_window_minutes = 0;
        diagnostics->liq_vol_gate_min_samples_required = 0;
        diagnostics->liq_vol_gate_low_conf_action = "hold";
        diagnostics->liq_vol_gate_samples.clear();
        diagnostics->structure_gate_observation_count = 0;
        diagnostics->structure_gate_fail_count_total = 0;
        diagnostics->structure_gate_fail_count_by_regime.clear();
        diagnostics->structure_gate_pass_count_by_regime.clear();
        diagnostics->structure_gate_mode =
            metrics.foundation_structure_gate_policy.enabled &&
                    metrics.foundation_structure_gate_policy.mode == "trend_only_relax"
                ? "trend_only_relax"
                : "legacy_fixed";
        diagnostics->structure_gate_relax_delta =
            metrics.foundation_structure_gate_policy.enabled &&
                    metrics.foundation_structure_gate_policy.mode == "trend_only_relax"
                ? std::clamp(metrics.foundation_structure_gate_policy.relax_delta, 0.0, 1.0)
                : 0.0;
        diagnostics->structure_gate_observed_score_sum = 0.0;
        diagnostics->structure_gate_threshold_before_sum = 0.0;
        diagnostics->structure_gate_threshold_after_sum = 0.0;
        diagnostics->structure_gate_samples.clear();
        diagnostics->bear_rebound_observation_count = 0;
        diagnostics->bear_rebound_pass_count = 0;
        diagnostics->bear_rebound_fail_count = 0;
        diagnostics->bear_rebound_low_conf_triggered_count = 0;
        diagnostics->bear_rebound_fail_count_by_regime.clear();
        diagnostics->bear_rebound_pass_count_by_regime.clear();
        diagnostics->bear_rebound_mode =
            metrics.bear_rebound_guard_policy.enabled &&
                    metrics.bear_rebound_guard_policy.mode == "quantile_dynamic"
                ? "quantile_dynamic"
                : "legacy_fixed";
        diagnostics->bear_rebound_quantile_q =
            metrics.bear_rebound_guard_policy.enabled &&
                    metrics.bear_rebound_guard_policy.mode == "quantile_dynamic"
                ? std::clamp(metrics.bear_rebound_guard_policy.quantile_q, 0.0, 1.0)
                : 0.0;
        diagnostics->bear_rebound_window_minutes =
            metrics.bear_rebound_guard_policy.enabled &&
                    metrics.bear_rebound_guard_policy.mode == "quantile_dynamic"
                ? std::max(1, metrics.bear_rebound_guard_policy.window_minutes)
                : 0;
        diagnostics->bear_rebound_min_samples_required =
            metrics.bear_rebound_guard_policy.enabled &&
                    metrics.bear_rebound_guard_policy.mode == "quantile_dynamic"
                ? std::max(1, metrics.bear_rebound_guard_policy.min_samples_required)
                : 0;
        diagnostics->bear_rebound_low_conf_action =
            metrics.bear_rebound_guard_policy.enabled &&
                    metrics.bear_rebound_guard_policy.mode == "quantile_dynamic"
                ? metrics.bear_rebound_guard_policy.low_conf_action
                : "hold";
        diagnostics->bear_rebound_observed_sum = 0.0;
        diagnostics->bear_rebound_threshold_sum = 0.0;
        diagnostics->bear_rebound_samples.clear();
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
            if (diagnostics != nullptr && signal.phase3.liq_vol_gate_telemetry_valid) {
                diagnostics->liq_vol_gate_observation_count++;
                if (signal.phase3.liq_vol_gate_pass) {
                    diagnostics->liq_vol_gate_pass_count++;
                } else {
                    diagnostics->liq_vol_gate_fail_count++;
                }
                if (signal.phase3.liq_vol_gate_low_conf_triggered) {
                    diagnostics->liq_vol_gate_low_conf_triggered_count++;
                }
                diagnostics->liq_vol_gate_observed_sum += signal.phase3.liq_vol_gate_observed;
                diagnostics->liq_vol_gate_threshold_sum += signal.phase3.liq_vol_gate_threshold_dynamic;
                diagnostics->liq_vol_gate_mode = signal.phase3.liq_vol_gate_mode;
                diagnostics->liq_vol_gate_quantile_q = signal.phase3.liq_vol_gate_quantile_q;
                diagnostics->liq_vol_gate_window_minutes = signal.phase3.liq_vol_gate_window_minutes;
                diagnostics->liq_vol_gate_min_samples_required =
                    signal.phase3.liq_vol_gate_min_samples_required;
                diagnostics->liq_vol_gate_low_conf_action =
                    signal.phase3.liq_vol_gate_low_conf_action;
                if (diagnostics->liq_vol_gate_samples.size() < 10) {
                    StrategyManager::CollectDiagnostics::LiqVolGateSample sample;
                    sample.market = signal.market;
                    sample.observed = signal.phase3.liq_vol_gate_observed;
                    sample.threshold_dynamic = signal.phase3.liq_vol_gate_threshold_dynamic;
                    sample.history_count = signal.phase3.liq_vol_gate_history_count;
                    sample.pass = signal.phase3.liq_vol_gate_pass;
                    sample.low_conf = signal.phase3.liq_vol_gate_low_conf_triggered;
                    diagnostics->liq_vol_gate_samples.push_back(std::move(sample));
                }
            }
            if (diagnostics != nullptr && signal.phase3.structure_gate_telemetry_valid) {
                diagnostics->structure_gate_observation_count++;
                const std::string regime_label =
                    autolife::common::runtime_diag::marketRegimeLabel(signal.market_regime);
                if (signal.phase3.structure_gate_pass) {
                    diagnostics->structure_gate_pass_count_by_regime[regime_label]++;
                } else {
                    diagnostics->structure_gate_fail_count_total++;
                    diagnostics->structure_gate_fail_count_by_regime[regime_label]++;
                }
                diagnostics->structure_gate_observed_score_sum +=
                    signal.phase3.structure_gate_observed_score;
                diagnostics->structure_gate_threshold_before_sum +=
                    signal.phase3.structure_gate_threshold_before;
                diagnostics->structure_gate_threshold_after_sum +=
                    signal.phase3.structure_gate_threshold_after;
                if (diagnostics->structure_gate_samples.size() < 10) {
                    StrategyManager::CollectDiagnostics::StructureGateSample sample;
                    sample.market = signal.market;
                    sample.regime = regime_label;
                    sample.observed_score = signal.phase3.structure_gate_observed_score;
                    sample.threshold_before = signal.phase3.structure_gate_threshold_before;
                    sample.threshold_after = signal.phase3.structure_gate_threshold_after;
                    sample.pass = signal.phase3.structure_gate_pass;
                    sample.relax_applied = signal.phase3.structure_gate_relax_applied;
                    diagnostics->structure_gate_samples.push_back(std::move(sample));
                }
            }
            if (diagnostics != nullptr && signal.phase3.bear_rebound_guard_telemetry_valid) {
                diagnostics->bear_rebound_observation_count++;
                const std::string regime_label =
                    autolife::common::runtime_diag::marketRegimeLabel(signal.market_regime);
                if (signal.phase3.bear_rebound_pass) {
                    diagnostics->bear_rebound_pass_count++;
                    diagnostics->bear_rebound_pass_count_by_regime[regime_label]++;
                } else {
                    diagnostics->bear_rebound_fail_count++;
                    diagnostics->bear_rebound_fail_count_by_regime[regime_label]++;
                }
                if (signal.phase3.bear_rebound_low_conf_triggered) {
                    diagnostics->bear_rebound_low_conf_triggered_count++;
                }
                diagnostics->bear_rebound_mode = signal.phase3.bear_rebound_guard_mode;
                diagnostics->bear_rebound_quantile_q = signal.phase3.bear_rebound_quantile_q;
                diagnostics->bear_rebound_window_minutes = signal.phase3.bear_rebound_window_minutes;
                diagnostics->bear_rebound_min_samples_required =
                    signal.phase3.bear_rebound_min_samples_required;
                diagnostics->bear_rebound_low_conf_action = signal.phase3.bear_rebound_low_conf_action;
                diagnostics->bear_rebound_observed_sum += signal.phase3.bear_rebound_observed;
                diagnostics->bear_rebound_threshold_sum += signal.phase3.bear_rebound_threshold_dynamic;
                if (diagnostics->bear_rebound_samples.size() < 10) {
                    StrategyManager::CollectDiagnostics::BearReboundGuardSample sample;
                    sample.market = signal.market;
                    sample.regime = regime_label;
                    sample.observed = signal.phase3.bear_rebound_observed;
                    sample.threshold_dynamic = signal.phase3.bear_rebound_threshold_dynamic;
                    sample.history_count = signal.phase3.bear_rebound_history_count;
                    sample.pass = signal.phase3.bear_rebound_pass;
                    sample.low_conf = signal.phase3.bear_rebound_low_conf_triggered;
                    diagnostics->bear_rebound_samples.push_back(std::move(sample));
                }
            }
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

} // namespace strategy
} // namespace autolife






