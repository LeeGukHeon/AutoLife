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
        diagnostics->frontier_decision_samples.clear();
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
        if (diagnostics != nullptr && decision.frontier_enabled) {
            StrategyManager::FilterDiagnostics::FrontierDecisionSample sample;
            sample.market = signal.market;
            sample.strategy_name = signal.strategy_name;
            sample.regime = autolife::common::runtime_diag::marketRegimeLabel(signal.market_regime);
            sample.frontier_enabled = decision.frontier_enabled;
            sample.frontier_pass = decision.frontier_pass;
            sample.expected_value_pass = decision.expected_value_pass;
            sample.margin_pass = decision.margin_pass;
            sample.ev_confidence_pass = decision.ev_confidence_pass;
            sample.cost_tail_pass = decision.cost_tail_pass;
            sample.manager_pass = decision.pass;
            sample.expected_value_observed = signal.expected_value;
            sample.required_expected_value = decision.required_expected_value;
            sample.expected_value_slack = signal.expected_value - decision.required_expected_value;
            sample.margin_observed = std::clamp(signal.probabilistic_h5_margin, -1.0, 1.0);
            sample.margin_floor = std::clamp(signal.phase3.frontier_margin_floor, -1.0, 1.0);
            sample.margin_slack = sample.margin_observed - sample.margin_floor;
            sample.ev_confidence_observed = std::clamp(decision.ev_confidence, 0.0, 1.0);
            sample.ev_confidence_floor = std::clamp(signal.phase3.frontier_ev_confidence_floor, 0.0, 1.0);
            sample.ev_confidence_slack = sample.ev_confidence_observed - sample.ev_confidence_floor;
            sample.cost_tail_observed = std::max(0.0, decision.cost_tail_term);
            sample.cost_tail_limit = std::clamp(signal.phase3.frontier_cost_tail_reject_threshold_pct, 0.0, 1.0);
            sample.cost_tail_slack = sample.cost_tail_limit - sample.cost_tail_observed;
            diagnostics->frontier_decision_samples.push_back(std::move(sample));
        }
        const double ranging_margin_min = std::max(
            0.0,
            signal.phase3.manager_filter.margin_min_ranging
        );
        const double h5_margin = std::isfinite(signal.probabilistic_h5_margin)
            ? std::clamp(signal.probabilistic_h5_margin, -1.0, 1.0)
            : -1.0;
        const std::string ranging_margin_mode = toLowerCopy(
            signal.phase3.manager_filter.margin_min_ranging_mode
        );
        const bool ranging_margin_observe_only =
            ranging_margin_mode == "observe_only" ||
            ranging_margin_mode == "observe" ||
            ranging_margin_mode == "telemetry_only";
        const bool reject_ranging_margin_insufficient =
            regime == analytics::MarketRegime::RANGING &&
            ranging_margin_min > 0.0 &&
            h5_margin < ranging_margin_min;
        const bool reject_expected_edge_negative =
            std::isfinite(signal.expected_value) && signal.expected_value < 0.0;

        if (decision.pass && reject_ranging_margin_insufficient) {
            incrementCounter(rejection_bucket, "reject_ranging_margin_insufficient_count", 1);
            incrementCounter(rejection_bucket, "ranging_margin_insufficient_observed_count", 1);
            incrementCounter(rejection_bucket, "ranging_margin_insufficient_would_reject_count", 1);
            if (!ranging_margin_observe_only) {
                recordPhase3DiagnosticsV2(rejection_bucket, signal, false);
                incrementRejectionReason(rejection_bucket, "reject_ranging_margin_insufficient");
                continue;
            }
            incrementCounter(rejection_bucket, "ranging_margin_observe_only_mode_count", 1);
        }

        if (decision.pass && reject_expected_edge_negative) {
            recordPhase3DiagnosticsV2(rejection_bucket, signal, false);
            incrementCounter(rejection_bucket, "reject_expected_edge_negative_count", 1);
            incrementRejectionReason(rejection_bucket, "reject_expected_edge_negative");
            continue;
        }

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






