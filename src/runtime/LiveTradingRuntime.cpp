#include "runtime/LiveTradingRuntime.h"
#include "common/Logger.h"
#include "common/Config.h"
#include "strategy/FoundationAdaptiveStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include "analytics/OrderbookAnalyzer.h"
#include "analytics/ProbabilisticRuntimeFeatures.h"
#include "execution/ExecutionPlaneAdapter.h"
#include "engine/PolicyLearningPlaneAdapter.h"
#include "risk/UpbitComplianceAdapter.h"
#include "state/EventJournalJsonl.h"
#include "state/LearningStateStoreJson.h"
#include "risk/RiskManager.h"
#include "common/PathUtils.h"
#include "common/MarketDataWindowPolicy.h"
#include "common/TickSizeHelper.h"  // [Phase 3] ???????????? ???????????좎럡萸????????됰Ŧ??????????????
#include "common/RuntimeDiagnosticsShared.h"
#include "common/SignalPolicyShared.h"
#include "common/StrategyEdgeStatsShared.h"
#include "common/ExecutionGuardPolicyShared.h"
#include "common/ProbabilisticRegimeSpec.h"
#include "execution/ExecutionUpdateSchema.h"
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
std::atomic<unsigned long long> g_live_execution_local_seq{0};

long long getCurrentTimestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

long long normalizeTimestampMs(long long ts) {
    if (ts > 0 && ts < 1000000000000LL) {
        return ts * 1000LL;
    }
    return ts;
}

const char* regimeToString(autolife::analytics::MarketRegime regime) {
    return autolife::common::runtime_diag::marketRegimeLabel(regime);
}

using autolife::common::signal_policy::normalizeStrategyToken;
using autolife::common::signal_policy::isStrategyEnabledByConfig;
using autolife::common::signal_policy::isBreakoutContinuationArchetype;
using autolife::common::signal_policy::isTrendReaccelerationArchetype;
using autolife::common::signal_policy::isConsolidationBreakArchetype;
using autolife::common::signal_policy::isRangePullbackArchetype;
using autolife::common::signal_policy::isDefensiveFoundationArchetype;
using autolife::common::signal_policy::isTrendContinuationStyleSignal;
using autolife::common::signal_policy::computeTargetRewardRisk;
using autolife::common::signal_policy::computeImpliedLossToWinRatio;
using autolife::common::signal_policy::computeHistoryRewardRiskAsymmetryPressure;
using autolife::common::signal_policy::computeEffectiveRoundTripCostPct;
using autolife::common::signal_policy::computeCostAwareRewardRiskFloor;
using autolife::common::signal_policy::rebalanceSignalRiskReward;
using autolife::common::signal_policy::computeStrategyHistoryWinProbPrior;
using autolife::common::signal_policy::computeCalibratedExpectedEdgePct;
using autolife::common::execution_guard::computeLiveScanPrefilterThresholds;
using autolife::common::execution_guard::computeRealtimeEntryVetoThresholds;
using autolife::common::execution_guard::computeDynamicSlippageThresholds;
using autolife::common::runtime_diag::classifySignalRejectionGroup;

std::filesystem::path resolveLiveExecutionArtifactPath() {
    return autolife::utils::PathUtils::resolveRelativePath("logs/execution_updates_live.jsonl");
}

void appendLiveExecutionUpdateArtifact(const autolife::core::ExecutionUpdate& update) {
    static std::mutex file_mutex;
    const auto path = resolveLiveExecutionArtifactPath();
    std::lock_guard<std::mutex> lock(file_mutex);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out.is_open()) {
        LOG_WARN("Live execution update artifact open failed: {}", path.string());
        return;
    }
    out << autolife::core::execution::toJson(update).dump() << "\n";
}

std::string buildSyntheticLiveOrderId(const std::string& market, const std::string& tag) {
    const auto seq = ++g_live_execution_local_seq;
    return "live-sim-" + tag + "-" + market + "-" + std::to_string(getCurrentTimestampMs()) + "-" + std::to_string(seq);
}

void recordLiveExecutionLifecycle(
    const std::string& source,
    const std::string& event,
    const std::string& order_id,
    const std::string& market,
    autolife::OrderSide side,
    autolife::OrderStatus status,
    double filled_volume,
    double order_volume,
    double avg_price,
    const std::string& strategy_name,
    bool terminal
) {
    LOG_INFO(
        "Execution lifecycle: source={}, event={}, order_id={}, market={}, side={}, status={}, filled={:.8f}, volume={:.8f}, terminal={}",
        source,
        event,
        order_id,
        market,
        autolife::core::execution::orderSideToString(side),
        autolife::core::execution::orderStatusToString(status),
        filled_volume,
        order_volume,
        terminal ? "true" : "false"
    );
    appendLiveExecutionUpdateArtifact(
        autolife::core::execution::makeExecutionUpdate(
            source,
            event,
            order_id,
            market,
            side,
            status,
            filled_volume,
            order_volume,
            avg_price,
            strategy_name,
            terminal,
            getCurrentTimestampMs()
        )
    );
}

struct ProbabilisticRuntimeSnapshot {
    bool enabled = false;
    bool applied = false;
    double prob_h1_calibrated = 0.5;
    double prob_h5_calibrated = 0.5;
    double prob_h1_mean = 0.5;
    double prob_h5_mean = 0.5;
    double prob_h5_std = 0.0;
    int ensemble_member_count = 1;
    double threshold_h5 = 0.6;
    double margin_h5 = 0.0;
    double expected_edge_raw_pct = 0.0;
    double expected_edge_calibrated_pct = 0.0;
    double expected_edge_pct = 0.0;
    double ev_confidence = 1.0;
    bool edge_regressor_used = false;
    bool ev_calibration_applied = false;
    double cost_entry_pct = 0.0;
    double cost_exit_pct = 0.0;
    double cost_tail_pct = 0.0;
    double cost_used_pct = 0.0;
    std::string cost_mode = "mean_mode";
    bool phase3_frontier_enabled = false;
    bool phase3_ev_calibration_enabled = false;
    bool phase3_cost_tail_enabled = false;
    bool phase3_adaptive_ev_blend_enabled = false;
    bool phase3_diagnostics_v2_enabled = false;
    autolife::analytics::ProbabilisticRuntimeModel::Phase3FrontierPolicy phase3_frontier_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase3AdaptiveEvBlendPolicy phase3_blend_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase3PrimaryMinimumPolicy phase3_primary_minimum_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase3PrimaryPriorityPolicy phase3_primary_priority_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase3ManagerFilterPolicy phase3_manager_filter_policy{};
    double online_margin_bias = 0.0;
    double online_strength_gain = 1.0;
    autolife::common::probabilistic_regime::State regime_state =
        autolife::common::probabilistic_regime::State::NORMAL;
    double regime_volatility_zscore = 0.0;
    double regime_drawdown_speed_bps = 0.0;
    double regime_correlation_shock = 0.0;
    double regime_threshold_add = 0.0;
    double regime_size_multiplier = 1.0;
    bool regime_block_new_entries = false;
};

struct ProbabilisticOnlineLearningState {
    bool active = false;
    double margin_bias = 0.0;
    double strength_gain = 1.0;
};

ProbabilisticOnlineLearningState computeProbabilisticOnlineLearningState(
    const std::vector<autolife::risk::TradeHistory>& history,
    const autolife::engine::EngineConfig& cfg,
    const std::string& market,
    autolife::analytics::MarketRegime regime
) {
    ProbabilisticOnlineLearningState out;
    if (!cfg.probabilistic_runtime_online_learning_enabled ||
        !cfg.enable_probabilistic_runtime_model ||
        !cfg.probabilistic_runtime_primary_mode) {
        return out;
    }
    if (history.empty()) {
        return out;
    }

    const int window = std::max(10, cfg.probabilistic_runtime_online_learning_window);
    const int min_samples = std::max(3, cfg.probabilistic_runtime_online_learning_min_samples);
    const double max_bias = std::max(0.0, cfg.probabilistic_runtime_online_learning_max_margin_bias);
    const double gain_band = std::clamp(cfg.probabilistic_runtime_online_learning_strength_gain, 0.0, 1.0);

    int sampled = 0;
    int scanned = 0;
    double weighted_alignment_sum = 0.0;
    double weight_sum = 0.0;
    double recency_weight = 1.0;
    for (auto it = history.rbegin(); it != history.rend() && scanned < window; ++it, ++scanned) {
        if (!it->probabilistic_runtime_applied) {
            recency_weight *= 0.985;
            continue;
        }
        const double outcome = (it->profit_loss > 0.0) ? 1.0 : (it->profit_loss < 0.0 ? -1.0 : 0.0);
        if (std::abs(outcome) <= 1e-12) {
            recency_weight *= 0.985;
            continue;
        }
        const double margin_dir = (it->probabilistic_h5_margin >= 0.0) ? 1.0 : -1.0;
        const double alignment = outcome * margin_dir;

        double relevance = 1.0;
        if (it->market == market) {
            relevance *= 1.25;
        }
        if (it->market_regime == regime) {
            relevance *= 1.20;
        }
        const double pnl_weight = std::clamp(std::abs(it->profit_loss_pct) / 0.02, 0.40, 1.80);
        const double weight = recency_weight * relevance * pnl_weight;
        weighted_alignment_sum += alignment * weight;
        weight_sum += weight;
        sampled++;
        recency_weight *= 0.985;
    }

    if (sampled < min_samples || weight_sum <= 1e-9) {
        return out;
    }
    const double normalized_alignment = std::clamp(weighted_alignment_sum / weight_sum, -1.0, 1.0);
    out.active = true;
    out.margin_bias = std::clamp(normalized_alignment * max_bias, -max_bias, max_bias);
    out.strength_gain = std::clamp(
        1.0 + (normalized_alignment * gain_band),
        std::max(0.60, 1.0 - gain_band),
        1.0 + gain_band
    );
    return out;
}

double effectiveProbabilisticScanPrefilterMargin(
    const autolife::engine::EngineConfig& cfg,
    autolife::analytics::MarketRegime regime
) {
    double gate = cfg.probabilistic_runtime_scan_prefilter_margin;
    if (regime == autolife::analytics::MarketRegime::TRENDING_DOWN ||
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY) {
        gate += 0.015;
    } else if (regime == autolife::analytics::MarketRegime::TRENDING_UP) {
        gate -= 0.005;
    }
    return std::clamp(gate, -0.30, 0.15);
}

double probabilisticUncertaintySizeScale(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot
) {
    if (!cfg.probabilistic_uncertainty_ensemble_enabled || snapshot.ensemble_member_count < 2) {
        return 1.0;
    }
    const double u = std::max(0.0, snapshot.prob_h5_std);
    const double u_max = std::max(1e-6, cfg.probabilistic_uncertainty_u_max);
    double scale = 1.0;
    if (cfg.probabilistic_uncertainty_size_mode == "exp") {
        scale = std::exp(-std::max(0.0, cfg.probabilistic_uncertainty_exp_k) * u);
    } else {
        scale = 1.0 - (u / u_max);
    }
    return std::clamp(scale, cfg.probabilistic_uncertainty_min_scale, 1.0);
}

bool probabilisticUncertaintyBlocksEntry(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot
) {
    if (!cfg.probabilistic_uncertainty_ensemble_enabled ||
        !cfg.probabilistic_uncertainty_skip_when_high ||
        snapshot.ensemble_member_count < 2) {
        return false;
    }
    return snapshot.prob_h5_std > std::max(cfg.probabilistic_uncertainty_u_max, cfg.probabilistic_uncertainty_skip_u);
}

double resolveAdaptiveEvBlend(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot,
    const autolife::strategy::Signal& signal
) {
    (void)cfg;
    if (!snapshot.phase3_adaptive_ev_blend_enabled) {
        return 0.20;
    }
    const auto& p = snapshot.phase3_blend_policy;
    double blend = p.base;
    const bool hostile_regime =
        signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    if (!hostile_regime && signal.market_regime == autolife::analytics::MarketRegime::TRENDING_UP) {
        blend += p.trend_bonus;
    }
    if (signal.market_regime == autolife::analytics::MarketRegime::RANGING) {
        blend -= p.ranging_penalty;
    }
    if (hostile_regime) {
        blend -= p.hostile_penalty;
    }
    if (snapshot.edge_regressor_used) {
        blend += 0.02;
    }
    const double ev_confidence = std::clamp(snapshot.ev_confidence, 0.0, 1.0);
    if (ev_confidence >= 0.72) {
        blend += p.high_confidence_bonus;
    } else {
        blend -= (1.0 - ev_confidence) * p.low_confidence_penalty;
    }
    const double tail_excess_pct = std::max(0.0, snapshot.cost_tail_pct - snapshot.cost_used_pct);
    blend -= (tail_excess_pct * 100.0) * p.cost_penalty;
    return std::clamp(blend, p.min, p.max);
}

bool inferProbabilisticRuntimeSnapshot(
    const autolife::analytics::ProbabilisticRuntimeModel& model,
    const autolife::engine::EngineConfig& cfg,
    const autolife::analytics::CoinMetrics& metrics,
    const std::string& market,
    autolife::analytics::MarketRegime regime,
    const ProbabilisticOnlineLearningState* online_learning_state,
    ProbabilisticRuntimeSnapshot& out_snapshot,
    std::string* reject_reason
) {
    out_snapshot = ProbabilisticRuntimeSnapshot{};
    out_snapshot.enabled = cfg.enable_probabilistic_runtime_model && model.isLoaded();
    const bool strict_primary_mode =
        cfg.enable_probabilistic_runtime_model &&
        cfg.probabilistic_runtime_primary_mode;
    if (!out_snapshot.enabled) {
        if (strict_primary_mode) {
            if (reject_reason != nullptr) {
                *reject_reason = "probabilistic_runtime_bundle_unavailable";
            }
            return false;
        }
        return true;
    }
    if (!model.supportsMarket(market)) {
        if (strict_primary_mode) {
            if (reject_reason != nullptr) {
                *reject_reason = "probabilistic_market_not_supported";
            }
            return false;
        }
        return true;
    }

    autolife::strategy::Signal probe_signal;
    probe_signal.market = market;
    probe_signal.entry_price = metrics.current_price;
    probe_signal.market_regime = regime;
    std::vector<double> transformed_features;
    std::string feature_error;
    if (!autolife::analytics::buildProbabilisticTransformedFeatures(
            probe_signal,
            metrics,
            transformed_features,
            &feature_error)) {
        LOG_WARN("{} probabilistic prefilter feature build skipped: {}", market, feature_error);
        if (strict_primary_mode) {
            if (reject_reason != nullptr) {
                *reject_reason = "probabilistic_feature_build_failed";
            }
            return false;
        }
        return true;
    }

    if (transformed_features.size() != model.featureColumns().size()) {
        LOG_WARN(
            "{} probabilistic prefilter feature dimension mismatch: runtime={} bundle={}",
            market,
            transformed_features.size(),
            model.featureColumns().size()
        );
        if (strict_primary_mode) {
            if (reject_reason != nullptr) {
                *reject_reason = "probabilistic_feature_dimension_mismatch";
            }
            return false;
        }
        return true;
    }

    autolife::analytics::ProbabilisticInference inference;
    if (!model.infer(market, transformed_features, inference)) {
        LOG_WARN("{} probabilistic prefilter inference failed", market);
        if (strict_primary_mode) {
            if (reject_reason != nullptr) {
                *reject_reason = "probabilistic_inference_failed";
            }
            return false;
        }
        return true;
    }

    out_snapshot.applied = true;
    out_snapshot.prob_h1_calibrated = std::clamp(inference.prob_h1_calibrated, 0.0, 1.0);
    out_snapshot.prob_h5_calibrated = std::clamp(inference.prob_h5_calibrated, 0.0, 1.0);
    out_snapshot.prob_h1_mean = std::clamp(inference.prob_h1_mean, 0.0, 1.0);
    out_snapshot.prob_h5_mean = std::clamp(inference.prob_h5_mean, 0.0, 1.0);
    out_snapshot.prob_h5_std = std::max(0.0, inference.prob_h5_std);
    out_snapshot.ensemble_member_count = std::max(1, inference.ensemble_member_count);
    out_snapshot.threshold_h5 = std::clamp(inference.selection_threshold_h5, 0.0, 1.0);
    out_snapshot.expected_edge_raw_pct = std::clamp(inference.expected_edge_raw_bps / 10000.0, -0.05, 0.05);
    out_snapshot.expected_edge_calibrated_pct = std::clamp(
        inference.expected_edge_calibrated_bps / 10000.0,
        -0.05,
        0.05
    );
    out_snapshot.expected_edge_pct = std::clamp(inference.expected_edge_pct, -0.05, 0.05);
    out_snapshot.ev_confidence = std::clamp(inference.ev_confidence, 0.0, 1.0);
    out_snapshot.edge_regressor_used = inference.edge_regressor_used;
    out_snapshot.ev_calibration_applied = inference.ev_calibration_applied;
    out_snapshot.cost_entry_pct = std::clamp(inference.entry_cost_bps_estimate / 10000.0, 0.0, 0.10);
    out_snapshot.cost_exit_pct = std::clamp(inference.exit_cost_bps_estimate / 10000.0, 0.0, 0.10);
    out_snapshot.cost_tail_pct = std::clamp(inference.tail_cost_bps_estimate / 10000.0, 0.0, 0.10);
    out_snapshot.cost_used_pct = std::clamp(inference.cost_used_bps_estimate / 10000.0, 0.0, 0.10);
    out_snapshot.cost_mode = inference.cost_mode;
    const auto& phase3_policy = model.phase3Policy();
    out_snapshot.phase3_frontier_enabled = inference.phase3_frontier_enabled;
    out_snapshot.phase3_ev_calibration_enabled = inference.phase3_ev_calibration_enabled;
    out_snapshot.phase3_cost_tail_enabled = inference.phase3_cost_tail_enabled;
    out_snapshot.phase3_adaptive_ev_blend_enabled = inference.phase3_adaptive_ev_blend_enabled;
    out_snapshot.phase3_diagnostics_v2_enabled = inference.phase3_diagnostics_v2_enabled;
    out_snapshot.phase3_frontier_policy = phase3_policy.frontier;
    out_snapshot.phase3_blend_policy = phase3_policy.adaptive_ev_blend;
    out_snapshot.phase3_primary_minimum_policy = phase3_policy.primary_minimums;
    out_snapshot.phase3_primary_priority_policy = phase3_policy.primary_priority;
    out_snapshot.phase3_manager_filter_policy = phase3_policy.manager_filter;
    out_snapshot.margin_h5 = std::clamp(
        inference.prob_h5_calibrated - inference.selection_threshold_h5,
        -1.0,
        1.0
    );
    if (online_learning_state != nullptr && online_learning_state->active) {
        out_snapshot.online_margin_bias = online_learning_state->margin_bias;
        out_snapshot.online_strength_gain = online_learning_state->strength_gain;
        out_snapshot.margin_h5 = std::clamp(
            out_snapshot.margin_h5 + out_snapshot.online_margin_bias,
            -1.0,
            1.0
        );
    }

    const auto regime_ext = autolife::common::probabilistic_regime::analyze(
        metrics.candles,
        cfg,
        nullptr
    );
    out_snapshot.regime_state = regime_ext.state;
    out_snapshot.regime_volatility_zscore = regime_ext.volatility_zscore;
    out_snapshot.regime_drawdown_speed_bps = regime_ext.drawdown_speed_bps;
    out_snapshot.regime_correlation_shock = regime_ext.correlation_shock;
    out_snapshot.regime_threshold_add = autolife::common::probabilistic_regime::thresholdAdd(
        cfg,
        regime_ext.state
    );
    out_snapshot.regime_size_multiplier = autolife::common::probabilistic_regime::sizeMultiplier(
        cfg,
        regime_ext.state
    );
    out_snapshot.regime_block_new_entries = autolife::common::probabilistic_regime::blockNewEntries(
        cfg,
        regime_ext.state
    );

    if (cfg.probabilistic_runtime_scan_prefilter_enabled) {
        const double gate_margin = std::clamp(
            effectiveProbabilisticScanPrefilterMargin(cfg, regime) + out_snapshot.regime_threshold_add,
            -0.30,
            0.30
        );
        if (out_snapshot.margin_h5 < gate_margin) {
            if (reject_reason != nullptr) {
                *reject_reason =
                    (cfg.probabilistic_regime_spec_enabled &&
                     out_snapshot.regime_state != autolife::common::probabilistic_regime::State::NORMAL)
                    ? "probabilistic_regime_prefilter"
                    : "probabilistic_market_prefilter";
            }
            return false;
        }
    }
    return true;
}

void applyProbabilisticPrimaryDecisionProfile(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot,
    autolife::strategy::Signal& signal
);

void applyProbabilisticManagerFloors(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot,
    autolife::analytics::MarketRegime regime,
    double* min_strength,
    double* min_expected_value
);

bool applyProbabilisticRuntimeAdjustment(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot,
    autolife::strategy::Signal& signal,
    std::string* reject_reason
) {
    signal.phase3 = autolife::strategy::Signal::Phase3PolicySnapshot{};
    signal.probabilistic_runtime_applied = false;
    signal.probabilistic_h1_calibrated = 0.5;
    signal.probabilistic_h5_calibrated = 0.5;
    signal.probabilistic_h5_threshold = 0.6;
    signal.probabilistic_h5_margin = 0.0;
    signal.probabilistic_h5_uncertainty_std = 0.0;
    signal.probabilistic_ensemble_member_count = 1;
    if (!snapshot.applied) {
        return true;
    }

    const double margin = snapshot.margin_h5;
    const double effective_margin = std::clamp(
        margin * std::max(0.50, snapshot.online_strength_gain),
        -1.0,
        1.0
    );
    signal.probabilistic_runtime_applied = true;
    signal.probabilistic_h1_calibrated = snapshot.prob_h1_calibrated;
    signal.probabilistic_h5_calibrated = snapshot.prob_h5_calibrated;
    signal.probabilistic_h5_threshold = snapshot.threshold_h5;
    signal.probabilistic_h5_margin = effective_margin;
    signal.probabilistic_h5_uncertainty_std = std::max(0.0, snapshot.prob_h5_std);
    signal.probabilistic_ensemble_member_count = std::max(1, snapshot.ensemble_member_count);
    signal.phase3.frontier_enabled = snapshot.phase3_frontier_enabled;
    signal.phase3.ev_calibration_enabled = snapshot.phase3_ev_calibration_enabled;
    signal.phase3.cost_tail_enabled = snapshot.phase3_cost_tail_enabled;
    signal.phase3.adaptive_ev_blend_enabled = snapshot.phase3_adaptive_ev_blend_enabled;
    signal.phase3.diagnostics_v2_enabled = snapshot.phase3_diagnostics_v2_enabled;
    signal.phase3.edge_regressor_used = snapshot.edge_regressor_used;
    signal.phase3.ev_calibration_applied = snapshot.ev_calibration_applied;
    signal.phase3.ev_confidence = snapshot.ev_confidence;
    signal.phase3.expected_edge_raw_pct = snapshot.expected_edge_raw_pct;
    signal.phase3.expected_edge_calibrated_pct = snapshot.expected_edge_calibrated_pct;
    signal.phase3.cost_entry_pct = snapshot.cost_entry_pct;
    signal.phase3.cost_exit_pct = snapshot.cost_exit_pct;
    signal.phase3.cost_tail_pct = snapshot.cost_tail_pct;
    signal.phase3.cost_used_pct = snapshot.cost_used_pct;
    signal.phase3.cost_mode = snapshot.cost_mode;
    signal.phase3.frontier_k_margin = snapshot.phase3_frontier_policy.k_margin;
    signal.phase3.frontier_k_uncertainty = snapshot.phase3_frontier_policy.k_uncertainty;
    signal.phase3.frontier_k_cost_tail = snapshot.phase3_frontier_policy.k_cost_tail;
    signal.phase3.frontier_min_required_ev = snapshot.phase3_frontier_policy.min_required_ev;
    signal.phase3.frontier_max_required_ev = snapshot.phase3_frontier_policy.max_required_ev;
    signal.phase3.frontier_margin_floor = snapshot.phase3_frontier_policy.margin_floor;
    signal.phase3.frontier_ev_confidence_floor = snapshot.phase3_frontier_policy.ev_confidence_floor;
    signal.phase3.frontier_cost_tail_reject_threshold_pct =
        snapshot.phase3_frontier_policy.cost_tail_reject_threshold_pct;
    signal.phase3.primary_minimums_enabled = snapshot.phase3_primary_minimum_policy.enabled;
    signal.phase3.primary_min_h5_calibrated = snapshot.phase3_primary_minimum_policy.min_h5_calibrated;
    signal.phase3.primary_min_h5_margin = snapshot.phase3_primary_minimum_policy.min_h5_margin;
    signal.phase3.primary_min_liquidity_score = snapshot.phase3_primary_minimum_policy.min_liquidity_score;
    signal.phase3.primary_min_signal_strength = snapshot.phase3_primary_minimum_policy.min_signal_strength;
    signal.phase3.primary_priority.enabled = snapshot.phase3_primary_priority_policy.enabled;
    signal.phase3.primary_priority.margin_score_shift = snapshot.phase3_primary_priority_policy.margin_score_shift;
    signal.phase3.primary_priority.margin_score_scale = snapshot.phase3_primary_priority_policy.margin_score_scale;
    signal.phase3.primary_priority.edge_score_shift = snapshot.phase3_primary_priority_policy.edge_score_shift;
    signal.phase3.primary_priority.edge_score_scale = snapshot.phase3_primary_priority_policy.edge_score_scale;
    signal.phase3.primary_priority.prob_weight = snapshot.phase3_primary_priority_policy.prob_weight;
    signal.phase3.primary_priority.margin_weight = snapshot.phase3_primary_priority_policy.margin_weight;
    signal.phase3.primary_priority.liquidity_weight = snapshot.phase3_primary_priority_policy.liquidity_weight;
    signal.phase3.primary_priority.strength_weight = snapshot.phase3_primary_priority_policy.strength_weight;
    signal.phase3.primary_priority.edge_weight = snapshot.phase3_primary_priority_policy.edge_weight;
    signal.phase3.primary_priority.hostile_prob_weight = snapshot.phase3_primary_priority_policy.hostile_prob_weight;
    signal.phase3.primary_priority.hostile_margin_weight = snapshot.phase3_primary_priority_policy.hostile_margin_weight;
    signal.phase3.primary_priority.hostile_liquidity_weight = snapshot.phase3_primary_priority_policy.hostile_liquidity_weight;
    signal.phase3.primary_priority.hostile_strength_weight = snapshot.phase3_primary_priority_policy.hostile_strength_weight;
    signal.phase3.primary_priority.hostile_edge_weight = snapshot.phase3_primary_priority_policy.hostile_edge_weight;
    signal.phase3.primary_priority.strong_buy_bonus = snapshot.phase3_primary_priority_policy.strong_buy_bonus;
    signal.phase3.primary_priority.margin_bonus_scale = snapshot.phase3_primary_priority_policy.margin_bonus_scale;
    signal.phase3.primary_priority.margin_bonus_cap = snapshot.phase3_primary_priority_policy.margin_bonus_cap;
    signal.phase3.primary_priority.range_penalty = snapshot.phase3_primary_priority_policy.range_penalty;
    signal.phase3.primary_priority.range_bonus = snapshot.phase3_primary_priority_policy.range_bonus;
    signal.phase3.primary_priority.range_penalty_strength_floor =
        snapshot.phase3_primary_priority_policy.range_penalty_strength_floor;
    signal.phase3.primary_priority.range_penalty_margin_floor =
        snapshot.phase3_primary_priority_policy.range_penalty_margin_floor;
    signal.phase3.primary_priority.range_penalty_prob_floor =
        snapshot.phase3_primary_priority_policy.range_penalty_prob_floor;
    signal.phase3.primary_priority.range_bonus_margin_floor =
        snapshot.phase3_primary_priority_policy.range_bonus_margin_floor;
    signal.phase3.primary_priority.range_bonus_prob_floor =
        snapshot.phase3_primary_priority_policy.range_bonus_prob_floor;
    signal.phase3.primary_priority.uptrend_bonus = snapshot.phase3_primary_priority_policy.uptrend_bonus;
    signal.phase3.primary_priority.uptrend_bonus_margin_floor =
        snapshot.phase3_primary_priority_policy.uptrend_bonus_margin_floor;
    signal.phase3.primary_priority.uptrend_bonus_prob_floor =
        snapshot.phase3_primary_priority_policy.uptrend_bonus_prob_floor;
    signal.phase3.manager_filter.enabled = snapshot.phase3_manager_filter_policy.enabled;
    signal.phase3.manager_filter.required_strength_cap =
        snapshot.phase3_manager_filter_policy.required_strength_cap;
    signal.phase3.manager_filter.core_signal_ownership_strength_relief =
        snapshot.phase3_manager_filter_policy.core_signal_ownership_strength_relief;
    signal.phase3.manager_filter.core_signal_ownership_expected_value_floor =
        snapshot.phase3_manager_filter_policy.core_signal_ownership_expected_value_floor;
    signal.phase3.manager_filter.policy_hold_strength_add =
        snapshot.phase3_manager_filter_policy.policy_hold_strength_add;
    signal.phase3.manager_filter.policy_hold_expected_value_add_core =
        snapshot.phase3_manager_filter_policy.policy_hold_expected_value_add_core;
    signal.phase3.manager_filter.policy_hold_expected_value_add_other =
        snapshot.phase3_manager_filter_policy.policy_hold_expected_value_add_other;
    signal.phase3.manager_filter.off_trend_strength_add =
        snapshot.phase3_manager_filter_policy.off_trend_strength_add;
    signal.phase3.manager_filter.off_trend_expected_value_add_core =
        snapshot.phase3_manager_filter_policy.off_trend_expected_value_add_core;
    signal.phase3.manager_filter.off_trend_expected_value_add_other =
        snapshot.phase3_manager_filter_policy.off_trend_expected_value_add_other;
    signal.phase3.manager_filter.hostile_regime_strength_add =
        snapshot.phase3_manager_filter_policy.hostile_regime_strength_add;
    signal.phase3.manager_filter.hostile_regime_expected_value_add_core =
        snapshot.phase3_manager_filter_policy.hostile_regime_expected_value_add_core;
    signal.phase3.manager_filter.hostile_regime_expected_value_add_other =
        snapshot.phase3_manager_filter_policy.hostile_regime_expected_value_add_other;
    signal.phase3.manager_filter.probabilistic_confidence_strength_relief_scale =
        snapshot.phase3_manager_filter_policy.probabilistic_confidence_strength_relief_scale;
    signal.phase3.manager_filter.probabilistic_confidence_expected_value_relief_scale =
        snapshot.phase3_manager_filter_policy.probabilistic_confidence_expected_value_relief_scale;
    signal.phase3.manager_filter.history_min_sample_hostile =
        snapshot.phase3_manager_filter_policy.history_min_sample_hostile;
    signal.phase3.manager_filter.history_min_sample_calm =
        snapshot.phase3_manager_filter_policy.history_min_sample_calm;
    signal.phase3.manager_filter.history_guard_scale_base =
        snapshot.phase3_manager_filter_policy.history_guard_scale_base;
    signal.phase3.manager_filter.history_guard_scale_confidence_scale =
        snapshot.phase3_manager_filter_policy.history_guard_scale_confidence_scale;
    signal.phase3.manager_filter.history_guard_scale_min_hostile =
        snapshot.phase3_manager_filter_policy.history_guard_scale_min_hostile;
    signal.phase3.manager_filter.history_guard_scale_min_calm =
        snapshot.phase3_manager_filter_policy.history_guard_scale_min_calm;
    signal.phase3.manager_filter.history_guard_scale_max_hostile =
        snapshot.phase3_manager_filter_policy.history_guard_scale_max_hostile;
    signal.phase3.manager_filter.history_guard_scale_max_calm =
        snapshot.phase3_manager_filter_policy.history_guard_scale_max_calm;
    signal.phase3.manager_filter.history_strength_bump_prob =
        snapshot.phase3_manager_filter_policy.history_strength_bump_prob;
    signal.phase3.manager_filter.history_strength_bump_non_prob =
        snapshot.phase3_manager_filter_policy.history_strength_bump_non_prob;
    signal.phase3.manager_filter.history_edge_bump_core_prob =
        snapshot.phase3_manager_filter_policy.history_edge_bump_core_prob;
    signal.phase3.manager_filter.history_edge_bump_core_non_prob =
        snapshot.phase3_manager_filter_policy.history_edge_bump_core_non_prob;
    signal.phase3.manager_filter.history_edge_bump_other_prob =
        snapshot.phase3_manager_filter_policy.history_edge_bump_other_prob;
    signal.phase3.manager_filter.history_edge_bump_other_non_prob =
        snapshot.phase3_manager_filter_policy.history_edge_bump_other_non_prob;
    signal.phase3.manager_filter.rr_guard_floor_hostile =
        snapshot.phase3_manager_filter_policy.rr_guard_floor_hostile;
    signal.phase3.manager_filter.rr_guard_floor_calm =
        snapshot.phase3_manager_filter_policy.rr_guard_floor_calm;
    signal.phase3.manager_filter.rr_guard_skip_min_rr =
        snapshot.phase3_manager_filter_policy.rr_guard_skip_min_rr;
    signal.phase3.manager_filter.rr_guard_scale_base =
        snapshot.phase3_manager_filter_policy.rr_guard_scale_base;
    signal.phase3.manager_filter.rr_guard_scale_confidence_scale =
        snapshot.phase3_manager_filter_policy.rr_guard_scale_confidence_scale;
    signal.phase3.manager_filter.rr_guard_scale_min =
        snapshot.phase3_manager_filter_policy.rr_guard_scale_min;
    signal.phase3.manager_filter.rr_guard_scale_max =
        snapshot.phase3_manager_filter_policy.rr_guard_scale_max;
    signal.phase3.manager_filter.rr_guard_strength_add =
        snapshot.phase3_manager_filter_policy.rr_guard_strength_add;
    signal.phase3.manager_filter.rr_guard_expected_value_add_core =
        snapshot.phase3_manager_filter_policy.rr_guard_expected_value_add_core;
    signal.phase3.manager_filter.rr_guard_expected_value_add_other =
        snapshot.phase3_manager_filter_policy.rr_guard_expected_value_add_other;
    signal.score += std::clamp(
        effective_margin * std::clamp(cfg.probabilistic_runtime_score_weight, 0.0, 1.0),
        -0.12,
        0.12
    );
    signal.signal_filter = std::clamp(signal.signal_filter + (effective_margin * 0.10), 0.0, 1.0);
    signal.expected_value += effective_margin * std::clamp(
        cfg.probabilistic_runtime_expected_edge_weight,
        0.0,
        0.01
    );
    const double ev_blend = resolveAdaptiveEvBlend(cfg, snapshot, signal);
    signal.phase3.adaptive_ev_blend = ev_blend;
    if (std::isfinite(snapshot.expected_edge_pct) && std::abs(snapshot.expected_edge_pct) > 1e-9) {
        signal.expected_value =
            (signal.expected_value * (1.0 - ev_blend)) +
            (std::clamp(snapshot.expected_edge_pct, -0.05, 0.05) * ev_blend);
    }

    if (cfg.probabilistic_runtime_primary_mode) {
        const double blend = std::clamp(cfg.probabilistic_runtime_strength_blend, 0.0, 1.0);
        const double primary_nudge = effective_margin * (0.20 + (0.25 * blend));
        signal.strength = std::clamp(
            signal.strength + primary_nudge,
            0.0,
            1.0
        );
        signal.signal_filter = std::clamp(
            signal.signal_filter + (effective_margin * (0.10 + (0.10 * blend))),
            0.0,
            1.0
        );
    }
    applyProbabilisticPrimaryDecisionProfile(cfg, snapshot, signal);

    if (cfg.probabilistic_runtime_hard_gate &&
        effective_margin < cfg.probabilistic_runtime_hard_gate_margin) {
        if (reject_reason != nullptr) {
            *reject_reason = "probabilistic_runtime_hard_gate";
        }
        LOG_INFO(
            "{} rejected by probabilistic hard gate: p_h5_cal={:.4f}, threshold={:.4f}, margin={:.4f}",
            signal.market,
            snapshot.prob_h5_calibrated,
            snapshot.threshold_h5,
            effective_margin
        );
        return false;
    }

    return true;
}

double probabilisticPositionScaleForEntry(
    const autolife::engine::EngineConfig& cfg,
    const autolife::strategy::Signal& signal,
    const ProbabilisticRuntimeSnapshot* snapshot
) {
    if (!cfg.probabilistic_runtime_primary_mode || !signal.probabilistic_runtime_applied) {
        return 1.0;
    }
    const double weight = std::clamp(cfg.probabilistic_runtime_position_scale_weight, 0.0, 1.0);
    if (weight <= 1e-9) {
        return 1.0;
    }
    const double normalized_margin = std::clamp(signal.probabilistic_h5_margin / 0.10, -1.0, 1.0);
    double scale = 1.0 + (normalized_margin * 0.55 * weight);
    if ((signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN ||
         signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY) &&
        signal.probabilistic_h5_margin < 0.0) {
        scale *= 0.92;
    }
    int ensemble_member_count = signal.probabilistic_ensemble_member_count;
    double uncertainty_std = std::max(0.0, signal.probabilistic_h5_uncertainty_std);
    if (snapshot != nullptr) {
        ensemble_member_count = std::max(ensemble_member_count, snapshot->ensemble_member_count);
        uncertainty_std = std::max(uncertainty_std, snapshot->prob_h5_std);
    }
    if (cfg.probabilistic_uncertainty_ensemble_enabled && ensemble_member_count >= 2) {
        ProbabilisticRuntimeSnapshot temp_snapshot;
        temp_snapshot.prob_h5_std = uncertainty_std;
        temp_snapshot.ensemble_member_count = ensemble_member_count;
        scale *= probabilisticUncertaintySizeScale(cfg, temp_snapshot);
    }
    if (snapshot != nullptr && cfg.probabilistic_regime_spec_enabled) {
        scale *= std::clamp(snapshot->regime_size_multiplier, 0.01, 1.0);
    }
    const double min_scale = (cfg.probabilistic_uncertainty_ensemble_enabled && ensemble_member_count >= 2)
        ? cfg.probabilistic_uncertainty_min_scale
        : 0.45;
    return std::clamp(scale, min_scale, 1.45);
}

void applyProbabilisticPrimaryDecisionProfile(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot,
    autolife::strategy::Signal& signal
) {
    (void)snapshot;
    if (!cfg.probabilistic_runtime_primary_mode || !signal.probabilistic_runtime_applied) {
        return;
    }

    const bool hostile_regime =
        signal.market_regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    const bool range_pullback_archetype =
        signal.entry_archetype.find("FOUNDATION_RANGE_PULLBACK") != std::string::npos;
    const bool fragility_archetype = range_pullback_archetype;
    const double prob = std::clamp(signal.probabilistic_h5_calibrated, 0.0, 1.0);
    const double threshold = std::clamp(signal.probabilistic_h5_threshold, 0.0, 1.0);
    const double margin = std::clamp(signal.probabilistic_h5_margin, -1.0, 1.0);
    const double confidence = std::clamp(
        (std::clamp((prob - 0.50) / 0.20, 0.0, 1.0) * 0.65) +
        (std::clamp((margin + 0.01) / 0.08, 0.0, 1.0) * 0.35),
        0.0,
        1.0
    );

    const double target_strength = std::clamp(
        0.22 + (prob * 0.60) + (margin * 1.80),
        hostile_regime ? 0.26 : 0.18,
        0.98
    );
    signal.strength = std::clamp((signal.strength * 0.25) + (target_strength * 0.75), 0.0, 1.0);

    const double target_filter = std::clamp(
        0.42 + (prob * 0.40) + (margin * 0.70),
        0.20,
        0.95
    );
    signal.signal_filter = std::clamp((signal.signal_filter * 0.30) + (target_filter * 0.70), 0.0, 1.0);

    const double implied_win = std::clamp(
        prob + (margin * 0.45) + ((prob - threshold) * 0.35),
        hostile_regime ? 0.44 : 0.40,
        0.86
    );

    if (signal.entry_price > 0.0 &&
        signal.stop_loss > 0.0 &&
        signal.take_profit_2 > signal.entry_price &&
        signal.stop_loss < signal.entry_price) {
        const double expected_return = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
        const double expected_risk = (signal.entry_price - signal.stop_loss) / signal.entry_price;
        if (expected_return > 0.0 && expected_risk > 0.0) {
            const double model_ev =
                (implied_win * expected_return) -
                ((1.0 - implied_win) * expected_risk);
            const double ev_anchor_weight = std::clamp(signal.phase3.adaptive_ev_blend, 0.0, 1.0);
            const double ev_model_weight = 1.0 - ev_anchor_weight;
            signal.expected_value =
                (signal.expected_value * ev_anchor_weight) +
                (model_ev * ev_model_weight);
        }
    }
    const double probabilistic_edge_floor =
        (margin * 0.0012) +
        ((prob - 0.50) * 0.0010) -
        (hostile_regime ? 0.00045 : 0.00030);
    signal.expected_value = std::max(signal.expected_value, probabilistic_edge_floor);

    if (signal.entry_price > 0.0 && signal.stop_loss > 0.0 && signal.stop_loss < signal.entry_price) {
        const double current_risk_pct = std::clamp(
            (signal.entry_price - signal.stop_loss) / signal.entry_price,
            0.0010,
            0.0500
        );
        double target_risk_pct = hostile_regime
            ? (0.0026 + ((1.0 - confidence) * 0.0036))
            : (0.0028 + ((1.0 - confidence) * 0.0048));
        if (!hostile_regime && fragility_archetype) {
            target_risk_pct *= 0.78;
        }
        if (!hostile_regime && fragility_archetype && margin < -0.006) {
            target_risk_pct *= 0.90;
        }
        target_risk_pct = std::clamp(
            target_risk_pct,
            hostile_regime ? 0.0022 : 0.0024,
            hostile_regime ? 0.0075 : 0.0105
        );
        const double blended_risk_pct = std::clamp(
            (current_risk_pct * 0.35) + (target_risk_pct * 0.65),
            hostile_regime ? 0.0022 : 0.0024,
            hostile_regime ? 0.0080 : 0.0120
        );

        double rr_target = hostile_regime
            ? (1.10 + (confidence * 1.00) + (std::max(0.0, margin) * 2.0))
            : (1.20 + (confidence * 1.30) + (std::max(0.0, margin) * 2.8));
        if (!hostile_regime && fragility_archetype) {
            rr_target += 0.18;
        }
        rr_target = std::clamp(rr_target, hostile_regime ? 1.05 : 1.10, hostile_regime ? 2.40 : 3.20);
        signal.stop_loss = signal.entry_price * (1.0 - blended_risk_pct);
        signal.take_profit_2 = signal.entry_price * (1.0 + (blended_risk_pct * rr_target));
        signal.take_profit_1 = signal.entry_price * (1.0 + (blended_risk_pct * std::max(1.0, rr_target * 0.55)));
        double breakeven_mult = (!hostile_regime && fragility_archetype) ? 0.48 : 0.70;
        double trailing_mult = (!hostile_regime && fragility_archetype) ? 0.82 : 1.10;
        signal.breakeven_trigger = signal.entry_price * (1.0 + (blended_risk_pct * breakeven_mult));
        signal.trailing_start = signal.entry_price * (1.0 + (blended_risk_pct * trailing_mult));
        signal.expected_return_pct = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
        signal.expected_risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
        signal.expected_value =
            (implied_win * signal.expected_return_pct) -
            ((1.0 - implied_win) * signal.expected_risk_pct);
        signal.expected_value = std::max(signal.expected_value, probabilistic_edge_floor);
    }

    if (signal.position_size > 0.0) {
        double size_scale = 0.42 + (confidence * 0.80) + (std::max(0.0, margin) * 1.20);
        if (margin < 0.0) {
            size_scale *= 0.86;
        }
        if (hostile_regime) {
            size_scale *= 0.88;
        }
        signal.position_size *= std::clamp(size_scale, 0.30, 1.35);
    }
}

void applyProbabilisticManagerFloors(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot,
    autolife::analytics::MarketRegime regime,
    double* min_strength,
    double* min_expected_value
) {
    if (!cfg.probabilistic_runtime_primary_mode || min_strength == nullptr || min_expected_value == nullptr) {
        return;
    }
    const auto& policy = snapshot.phase3_manager_filter_policy;
    const auto defaults = autolife::analytics::ProbabilisticRuntimeModel::Phase3ManagerFilterPolicy{};
    const bool use_policy = policy.enabled;
    const auto pick = [&](double policy_value, double default_value) {
        return use_policy ? policy_value : default_value;
    };
    const bool hostile_regime =
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    if (!snapshot.applied) {
        *min_strength = std::min(
            *min_strength,
            hostile_regime
                ? pick(policy.no_snapshot_min_strength_hostile, defaults.no_snapshot_min_strength_hostile)
                : pick(policy.no_snapshot_min_strength_calm, defaults.no_snapshot_min_strength_calm)
        );
        *min_expected_value = std::min(
            *min_expected_value,
            hostile_regime
                ? pick(
                    policy.no_snapshot_min_expected_value_hostile,
                    defaults.no_snapshot_min_expected_value_hostile
                )
                : pick(
                    policy.no_snapshot_min_expected_value_calm,
                    defaults.no_snapshot_min_expected_value_calm
                )
        );
        return;
    }

    const double prob = std::clamp(snapshot.prob_h5_calibrated, 0.0, 1.0);
    const double margin = std::clamp(snapshot.margin_h5, -1.0, 1.0);
    const double conf_prob_shift = pick(policy.confidence_prob_shift, defaults.confidence_prob_shift);
    const double conf_prob_scale = std::max(1e-6, pick(policy.confidence_prob_scale, defaults.confidence_prob_scale));
    const double conf_margin_shift = pick(policy.confidence_margin_shift, defaults.confidence_margin_shift);
    const double conf_margin_scale = std::max(
        1e-6,
        pick(policy.confidence_margin_scale, defaults.confidence_margin_scale)
    );
    const double conf_prob_weight = std::max(0.0, pick(policy.confidence_prob_weight, defaults.confidence_prob_weight));
    const double conf_margin_weight = std::max(
        0.0,
        pick(policy.confidence_margin_weight, defaults.confidence_margin_weight)
    );
    const double conf_prob_component = std::clamp((prob - conf_prob_shift) / conf_prob_scale, 0.0, 1.0);
    const double conf_margin_component = std::clamp((margin + conf_margin_shift) / conf_margin_scale, 0.0, 1.0);
    const double conf_denom = std::max(1e-6, conf_prob_weight + conf_margin_weight);
    const double confidence = std::clamp(
        ((conf_prob_component * conf_prob_weight) +
         (conf_margin_component * conf_margin_weight)) / conf_denom,
        0.0,
        1.0
    );

    double target_strength = hostile_regime
        ? (pick(policy.target_strength_hostile_base, defaults.target_strength_hostile_base) -
           (confidence * pick(
               policy.target_strength_hostile_confidence_scale,
               defaults.target_strength_hostile_confidence_scale
           )))
        : (pick(policy.target_strength_calm_base, defaults.target_strength_calm_base) -
           (confidence * pick(
               policy.target_strength_calm_confidence_scale,
               defaults.target_strength_calm_confidence_scale
           )));
    double target_edge = hostile_regime
        ? (pick(
                policy.target_expected_value_hostile_base,
                defaults.target_expected_value_hostile_base
            ) -
           (confidence * pick(
               policy.target_expected_value_hostile_confidence_scale,
               defaults.target_expected_value_hostile_confidence_scale
           )))
        : (pick(policy.target_expected_value_calm_base, defaults.target_expected_value_calm_base) -
           (confidence * pick(
               policy.target_expected_value_calm_confidence_scale,
               defaults.target_expected_value_calm_confidence_scale
           )));
    if (margin < 0.0) {
        target_strength += hostile_regime
            ? pick(policy.negative_margin_strength_add_hostile, defaults.negative_margin_strength_add_hostile)
            : pick(policy.negative_margin_strength_add_calm, defaults.negative_margin_strength_add_calm);
        target_edge += hostile_regime
            ? pick(
                policy.negative_margin_expected_value_add_hostile,
                defaults.negative_margin_expected_value_add_hostile
            )
            : pick(
                policy.negative_margin_expected_value_add_calm,
                defaults.negative_margin_expected_value_add_calm
            );
    }

    target_strength = std::clamp(
        target_strength,
        hostile_regime
            ? pick(policy.target_strength_hostile_min, defaults.target_strength_hostile_min)
            : pick(policy.target_strength_calm_min, defaults.target_strength_calm_min),
        hostile_regime
            ? pick(policy.target_strength_hostile_max, defaults.target_strength_hostile_max)
            : pick(policy.target_strength_calm_max, defaults.target_strength_calm_max)
    );
    target_edge = std::clamp(
        target_edge,
        hostile_regime
            ? pick(policy.target_expected_value_hostile_min, defaults.target_expected_value_hostile_min)
            : pick(policy.target_expected_value_calm_min, defaults.target_expected_value_calm_min),
        hostile_regime
            ? pick(policy.target_expected_value_hostile_max, defaults.target_expected_value_hostile_max)
            : pick(policy.target_expected_value_calm_max, defaults.target_expected_value_calm_max)
    );
    *min_strength = std::min(*min_strength, target_strength);
    *min_expected_value = std::min(*min_expected_value, target_edge);
}

struct ProbabilisticPrimaryMinimums {
    double min_h5_calibrated = 0.5;
    double min_h5_margin = 0.0;
    double min_liquidity_score = 0.0;
    double min_signal_strength = 0.0;
};

ProbabilisticPrimaryMinimums effectiveProbabilisticPrimaryMinimums(
    const autolife::engine::EngineConfig& cfg,
    const autolife::strategy::Signal& signal,
    autolife::analytics::MarketRegime regime,
    const ProbabilisticRuntimeSnapshot* snapshot
) {
    ProbabilisticPrimaryMinimums out;
    if (signal.phase3.primary_minimums_enabled) {
        out.min_h5_calibrated = signal.phase3.primary_min_h5_calibrated;
        out.min_h5_margin = signal.phase3.primary_min_h5_margin;
        out.min_liquidity_score = signal.phase3.primary_min_liquidity_score;
        out.min_signal_strength = signal.phase3.primary_min_signal_strength;
    } else {
        out.min_h5_calibrated = cfg.probabilistic_primary_min_h5_calibrated;
        out.min_h5_margin = cfg.probabilistic_primary_min_h5_margin;
        out.min_liquidity_score = cfg.probabilistic_primary_min_liquidity_score;
        out.min_signal_strength = cfg.probabilistic_primary_min_signal_strength;
    }

    const bool hostile =
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    if (hostile) {
        out.min_h5_calibrated += 0.06;
        out.min_h5_margin += 0.03;
        out.min_liquidity_score += 10.0;
        out.min_signal_strength += 0.08;
    } else if (regime == autolife::analytics::MarketRegime::RANGING) {
        out.min_h5_calibrated += 0.01;
        out.min_h5_margin += 0.005;
        out.min_liquidity_score += 2.0;
        out.min_signal_strength += 0.02;
    } else if (regime == autolife::analytics::MarketRegime::TRENDING_UP) {
        out.min_h5_calibrated -= 0.02;
        out.min_h5_margin -= 0.01;
        out.min_liquidity_score -= 5.0;
        out.min_signal_strength -= 0.03;
    }

    if (snapshot != nullptr && cfg.probabilistic_regime_spec_enabled) {
        const double regime_add = std::max(0.0, snapshot->regime_threshold_add);
        out.min_h5_margin += regime_add;
        out.min_h5_calibrated += std::clamp(regime_add * 1.5, 0.0, 0.20);
        if (snapshot->regime_state == autolife::common::probabilistic_regime::State::VOLATILE) {
            out.min_signal_strength += 0.03;
        } else if (snapshot->regime_state == autolife::common::probabilistic_regime::State::HOSTILE) {
            out.min_liquidity_score += 8.0;
            out.min_signal_strength += 0.08;
        }
    }

    out.min_h5_calibrated = std::clamp(out.min_h5_calibrated, 0.0, 1.0);
    out.min_h5_margin = std::clamp(out.min_h5_margin, -0.50, 0.20);
    out.min_liquidity_score = std::clamp(out.min_liquidity_score, 0.0, 100.0);
    out.min_signal_strength = std::clamp(out.min_signal_strength, 0.0, 1.0);
    return out;
}

double probabilisticPrimaryPriorityScore(
    const autolife::engine::EngineConfig& cfg,
    const autolife::strategy::Signal& signal,
    autolife::analytics::MarketRegime regime
) {
    const auto& priority = signal.phase3.primary_priority;
    const bool use_policy = priority.enabled;
    const auto pick = [&](double policy_value, double fallback_value) {
        return use_policy ? policy_value : fallback_value;
    };

    const double prob = signal.probabilistic_runtime_applied
        ? std::clamp(signal.probabilistic_h5_calibrated, 0.0, 1.0)
        : 0.5;
    const double margin = signal.probabilistic_runtime_applied
        ? std::clamp(signal.probabilistic_h5_margin, -1.0, 1.0)
        : 0.0;
    const double margin_score_shift = pick(priority.margin_score_shift, 0.10);
    const double margin_score_scale = std::max(1e-6, pick(priority.margin_score_scale, 0.20));
    const double margin_score = std::clamp((margin + margin_score_shift) / margin_score_scale, 0.0, 1.0);
    const double liquidity_score = std::clamp(signal.liquidity_score / 100.0, 0.0, 1.0);
    const double strength_score = std::clamp(signal.strength, 0.0, 1.0);
    const double edge_score_shift = pick(priority.edge_score_shift, 0.0005);
    const double edge_score_scale = std::max(1e-6, pick(priority.edge_score_scale, 0.0025));
    const double expected_edge_score = std::clamp((signal.expected_value + edge_score_shift) / edge_score_scale, 0.0, 1.0);

    double prob_weight = pick(priority.prob_weight, 0.50);
    double margin_weight = pick(priority.margin_weight, 0.22);
    double liquidity_weight = pick(priority.liquidity_weight, 0.10);
    double strength_weight = pick(priority.strength_weight, 0.10);
    double edge_weight = pick(priority.edge_weight, 0.08);
    const bool hostile =
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    if (hostile) {
        prob_weight = pick(priority.hostile_prob_weight, 0.54);
        margin_weight = pick(priority.hostile_margin_weight, 0.22);
        liquidity_weight = pick(priority.hostile_liquidity_weight, 0.11);
        strength_weight = pick(priority.hostile_strength_weight, 0.09);
        edge_weight = pick(priority.hostile_edge_weight, 0.04);
    }
    double score =
        (prob * prob_weight) +
        (margin_score * margin_weight) +
        (liquidity_score * liquidity_weight) +
        (strength_score * strength_weight) +
        (expected_edge_score * edge_weight);

    if (signal.type == autolife::strategy::SignalType::STRONG_BUY) {
        score += pick(priority.strong_buy_bonus, 0.02);
    }
    if (cfg.probabilistic_runtime_primary_mode && signal.probabilistic_runtime_applied) {
        const double margin_bonus_scale = pick(priority.margin_bonus_scale, 0.08);
        const double margin_bonus_cap = std::max(0.0, pick(priority.margin_bonus_cap, 0.03));
        score += std::clamp(signal.probabilistic_h5_margin * margin_bonus_scale, -margin_bonus_cap, margin_bonus_cap);
    }

    const std::string& archetype = signal.entry_archetype;
    if (archetype.find("FOUNDATION_RANGE_PULLBACK") != std::string::npos) {
        const double range_penalty_strength_floor = pick(priority.range_penalty_strength_floor, 0.50);
        const double range_penalty_margin_floor = pick(priority.range_penalty_margin_floor, 0.008);
        const double range_penalty_prob_floor = pick(priority.range_penalty_prob_floor, 0.54);
        const double range_bonus_margin_floor = pick(priority.range_bonus_margin_floor, 0.012);
        const double range_bonus_prob_floor = pick(priority.range_bonus_prob_floor, 0.57);
        if (signal.strength < range_penalty_strength_floor &&
            (margin < range_penalty_margin_floor || prob < range_penalty_prob_floor)) {
            score -= std::max(0.0, pick(priority.range_penalty, 0.11));
        } else if (margin >= range_bonus_margin_floor && prob >= range_bonus_prob_floor) {
            score += pick(priority.range_bonus, 0.03);
        }
    } else if (archetype.find("FOUNDATION_UPTREND_CONTINUATION") != std::string::npos) {
        const double uptrend_bonus_margin_floor = pick(priority.uptrend_bonus_margin_floor, 0.0);
        const double uptrend_bonus_prob_floor = pick(priority.uptrend_bonus_prob_floor, 0.52);
        if (margin >= uptrend_bonus_margin_floor && prob >= uptrend_bonus_prob_floor) {
            score += pick(priority.uptrend_bonus, 0.03);
        }
    }

    return score;
}

bool passesProbabilisticPrimaryMinimums(
    const autolife::engine::EngineConfig& cfg,
    const autolife::strategy::Signal& signal,
    autolife::analytics::MarketRegime regime,
    const ProbabilisticRuntimeSnapshot* snapshot,
    std::string* reject_reason
) {
    if (!cfg.probabilistic_runtime_primary_mode) {
        return true;
    }
    if (!signal.probabilistic_runtime_applied) {
        if (reject_reason != nullptr) {
            *reject_reason = "blocked_probabilistic_primary_missing_overlay";
        }
        return false;
    }
    if (snapshot != nullptr &&
        cfg.probabilistic_regime_spec_enabled &&
        snapshot->regime_block_new_entries) {
        if (reject_reason != nullptr) {
            *reject_reason = "blocked_probabilistic_regime_hostile";
        }
        return false;
    }
    if (snapshot != nullptr && probabilisticUncertaintyBlocksEntry(cfg, *snapshot)) {
        if (reject_reason != nullptr) {
            *reject_reason = "blocked_probabilistic_uncertainty_high";
        }
        return false;
    }

    ProbabilisticPrimaryMinimums mins =
        effectiveProbabilisticPrimaryMinimums(cfg, signal, regime, snapshot);
    const bool hostile_regime = autolife::common::signal_policy::isHostileRegime(regime);
    const bool calm_context =
        !hostile_regime &&
        signal.volatility > 0.0 &&
        signal.volatility <= 3.0 &&
        signal.liquidity_score >= 20.0 &&
        signal.reason.find("foundation_adaptive_regime_entry") != std::string::npos;
    const bool strong_prob_context =
        !hostile_regime &&
        signal.probabilistic_h5_calibrated >= 0.54 &&
        signal.probabilistic_h5_margin >= -0.010;
    const bool probabilistic_supportive_context =
        !hostile_regime &&
        signal.probabilistic_h5_calibrated >= 0.46 &&
        signal.probabilistic_h5_margin >= -0.015 &&
        signal.liquidity_score >= 16.0;
    if (hostile_regime) {
        if (signal.probabilistic_h5_margin < -0.010) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_margin";
            }
            return false;
        }
        if (signal.probabilistic_h5_calibrated < 0.49 &&
            signal.probabilistic_h5_margin < 0.0) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_calibrated";
            }
            return false;
        }
    } else {
        if (signal.probabilistic_h5_margin < -0.045 &&
            signal.probabilistic_h5_calibrated < 0.40) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_margin";
            }
            return false;
        }
        if (signal.probabilistic_h5_calibrated < 0.35 &&
            signal.probabilistic_h5_margin < -0.030) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_calibrated";
            }
            return false;
        }
    }
    if (calm_context) {
        mins.min_h5_calibrated = std::max(0.0, mins.min_h5_calibrated - 0.03);
        mins.min_h5_margin = std::max(-0.50, mins.min_h5_margin - 0.015);
        mins.min_liquidity_score = std::max(0.0, mins.min_liquidity_score - 10.0);
        mins.min_signal_strength = std::max(0.0, mins.min_signal_strength - 0.08);
    }
    if (strong_prob_context) {
        mins.min_h5_calibrated = std::max(0.0, mins.min_h5_calibrated - 0.02);
        mins.min_h5_margin = std::max(-0.50, mins.min_h5_margin - 0.010);
        mins.min_signal_strength = std::max(0.0, mins.min_signal_strength - 0.04);
    }
    if (probabilistic_supportive_context) {
        mins.min_h5_calibrated = std::max(0.0, mins.min_h5_calibrated - 0.02);
        mins.min_h5_margin = std::max(-0.50, mins.min_h5_margin - 0.010);
        mins.min_liquidity_score = std::max(0.0, mins.min_liquidity_score - 10.0);
        mins.min_signal_strength = std::max(0.0, mins.min_signal_strength - 0.04);
    }
    const bool calibrated_fail = signal.probabilistic_h5_calibrated < mins.min_h5_calibrated;
    const bool margin_fail = signal.probabilistic_h5_margin < mins.min_h5_margin;
    const bool liquidity_fail = signal.liquidity_score < mins.min_liquidity_score;
    const bool strength_fail = signal.strength < mins.min_signal_strength;
    if (!(calibrated_fail || margin_fail || liquidity_fail || strength_fail)) {
        return true;
    }
    if (!hostile_regime) {
        const double calibrated_score = std::clamp(
            (signal.probabilistic_h5_calibrated - (mins.min_h5_calibrated - 0.14)) / 0.24,
            0.0,
            1.0
        );
        const double margin_score = std::clamp(
            (signal.probabilistic_h5_margin - (mins.min_h5_margin - 0.06)) / 0.14,
            0.0,
            1.0
        );
        const double liquidity_score = std::clamp(
            signal.liquidity_score / std::max(1.0, mins.min_liquidity_score + 20.0),
            0.0,
            1.0
        );
        const double strength_score = std::clamp(
            signal.strength / std::max(0.30, mins.min_signal_strength + 0.20),
            0.0,
            1.0
        );
        const double expected_edge_score = std::clamp(
            (signal.expected_value + 0.00055) / 0.00160,
            0.0,
            1.0
        );
        const double composite_score =
            (calibrated_score * 0.34) +
            (margin_score * 0.28) +
            (liquidity_score * 0.14) +
            (strength_score * 0.14) +
            (expected_edge_score * 0.10);
        const double composite_threshold = 0.48;
        if (composite_score >= composite_threshold &&
            signal.probabilistic_h5_margin >= (mins.min_h5_margin - 0.028)) {
            return true;
        }
        if (signal.probabilistic_h5_calibrated >= 0.40 &&
            signal.probabilistic_h5_margin >= -0.028 &&
            signal.liquidity_score >= 12.0 &&
            signal.strength >= 0.07 &&
            signal.expected_value >= -0.00014) {
            return true;
        }
    } else {
        const double hostile_score =
            (std::clamp((signal.probabilistic_h5_calibrated - 0.46) / 0.20, 0.0, 1.0) * 0.45) +
            (std::clamp((signal.probabilistic_h5_margin + 0.02) / 0.08, 0.0, 1.0) * 0.35) +
            (std::clamp(signal.liquidity_score / 70.0, 0.0, 1.0) * 0.10) +
            (std::clamp(signal.strength / 0.75, 0.0, 1.0) * 0.10);
        if (hostile_score >= 0.58 &&
            signal.probabilistic_h5_margin >= (mins.min_h5_margin - 0.012) &&
            signal.liquidity_score >= std::max(20.0, mins.min_liquidity_score - 6.0)) {
            return true;
        }
    }
    if (signal.probabilistic_h5_calibrated < mins.min_h5_calibrated) {
        if (reject_reason != nullptr) {
            *reject_reason = "blocked_probabilistic_primary_calibrated";
        }
        return false;
    }
    if (signal.probabilistic_h5_margin < mins.min_h5_margin) {
        if (reject_reason != nullptr) {
            *reject_reason = "blocked_probabilistic_primary_margin";
        }
        return false;
    }
    if (signal.liquidity_score < mins.min_liquidity_score) {
        if (reject_reason != nullptr) {
            *reject_reason = "blocked_probabilistic_primary_liquidity";
        }
        return false;
    }
    if (signal.strength < mins.min_signal_strength) {
        if (reject_reason != nullptr) {
            *reject_reason = "blocked_probabilistic_primary_strength";
        }
        return false;
    }
    return true;
}

using autolife::common::strategy_edge::StrategyEdgeStats;
using autolife::common::strategy_edge::buildStrategyEdgeStats;
using autolife::common::strategy_edge::makeStrategyRegimeKey;
using autolife::common::strategy_edge::makeMarketStrategyRegimeKey;
using autolife::common::strategy_edge::buildStrategyRegimeEdgeStats;
using autolife::common::strategy_edge::buildMarketStrategyRegimeEdgeStats;

void normalizeSignalStopLossByRegime(autolife::strategy::Signal& signal, autolife::analytics::MarketRegime regime) {
    autolife::common::signal_policy::normalizeSignalStopLossByRegime(signal, regime);
}

std::string toLowerCopy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

long long timeframeDurationMs(const std::string& timeframe_key) {
    if (timeframe_key == "1m") return 60LL * 1000LL;
    if (timeframe_key == "5m") return 5LL * 60LL * 1000LL;
    if (timeframe_key == "15m") return 15LL * 60LL * 1000LL;
    if (timeframe_key == "1h" || timeframe_key == "60m") return 60LL * 60LL * 1000LL;
    if (timeframe_key == "4h" || timeframe_key == "240m") return 4LL * 60LL * 60LL * 1000LL;
    if (timeframe_key == "1d" || timeframe_key == "day" || timeframe_key == "d") {
        return 24LL * 60LL * 60LL * 1000LL;
    }
    return 0LL;
}

size_t dropTrailingUnconfirmedCandles(
    std::map<std::string, std::vector<autolife::Candle>>& candles_by_tf,
    long long now_ms
) {
    size_t dropped = 0;
    if (now_ms <= 0) {
        return dropped;
    }

    for (auto& entry : candles_by_tf) {
        const long long duration_ms = timeframeDurationMs(entry.first);
        if (duration_ms <= 0) {
            continue;
        }
        auto& candles = entry.second;
        while (!candles.empty()) {
            const auto& last = candles.back();
            if (last.timestamp <= 0) {
                break;
            }
            const bool is_unconfirmed = (last.timestamp + duration_ms) > now_ms;
            if (!is_unconfirmed) {
                break;
            }
            candles.pop_back();
            dropped++;
        }
    }

    return dropped;
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
    // Integrity-first policy:
    // 15m must come from native timeframe collection, not in-runtime resampling.
    (void)metrics;
}

bool requiresTypedArchetype(const std::string& strategy_name) {
    return autolife::common::signal_policy::requiresTypedArchetype(strategy_name);
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

long long latestCandleTimestampMs(const autolife::analytics::CoinMetrics& metrics) {
    long long ts = 0;
    auto tf_it = metrics.candles_by_tf.find("1m");
    if (tf_it != metrics.candles_by_tf.end() && !tf_it->second.empty()) {
        ts = normalizeTimestampMs(tf_it->second.back().timestamp);
    } else if (!metrics.candles.empty()) {
        ts = normalizeTimestampMs(metrics.candles.back().timestamp);
    }
    return ts > 0 ? ts : 0;
}

long long resolvePolicyAuditTimestampMs(
    const std::vector<autolife::analytics::CoinMetrics>& scanned_markets,
    const std::vector<autolife::engine::PolicyDecisionRecord>& policy_decisions,
    const std::vector<autolife::strategy::Signal>& policy_input_signals
) {
    long long best_ts = 0;
    if (!policy_decisions.empty()) {
        std::set<std::string> decision_markets;
        for (const auto& decision : policy_decisions) {
            if (!decision.market.empty()) {
                decision_markets.insert(decision.market);
            }
        }
        for (const auto& metrics : scanned_markets) {
            if (decision_markets.find(metrics.market) == decision_markets.end()) {
                continue;
            }
            best_ts = std::max(best_ts, latestCandleTimestampMs(metrics));
        }
        if (best_ts > 0) {
            return best_ts;
        }
    }

    for (const auto& metrics : scanned_markets) {
        best_ts = std::max(best_ts, latestCandleTimestampMs(metrics));
    }
    if (best_ts > 0) {
        return best_ts;
    }

    for (const auto& signal : policy_input_signals) {
        best_ts = std::max(best_ts, normalizeTimestampMs(signal.timestamp));
    }
    if (best_ts > 0) {
        return best_ts;
    }
    return getCurrentTimestampMs();
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
    int max_new_orders_per_scan,
    long long decision_timestamp_ms
) {
    if (decisions.empty()) {
        return;
    }

    nlohmann::json line;
    line["ts"] = normalizeTimestampMs(decision_timestamp_ms);
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

void resetPolicyDecisionAuditArtifact() {
    auto path = autolife::utils::PathUtils::resolveRelativePath("logs/policy_decisions.jsonl");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARN("Policy decision audit reset failed: {}", path.string());
        return;
    }
    LOG_INFO("Policy decision audit reset: {}", path.string());
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
    const bool enable_my_order_ws =
        config.mode == TradingMode::LIVE &&
        !config.dry_run &&
        config.allow_live_orders;
    order_manager_ = std::make_unique<execution::OrderManager>(
        http_client,
        enable_my_order_ws
    );
    LOG_INFO("Order manager WS mode: {}", enable_my_order_ws ? "enabled" : "disabled");
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
    const bool foundation_enabled = isStrategyEnabledByConfig(config_, foundation->getInfo().name);
    foundation->setEnabled(foundation_enabled);
    strategy_manager_->registerStrategy(foundation);
    LOG_INFO("Registered strategy: foundation_adaptive (enabled={})",
             foundation_enabled ? "Y" : "N");

    if (config_.enable_probabilistic_runtime_model) {
        const auto bundle_path = utils::PathUtils::resolveRelativePath(
            config_.probabilistic_runtime_bundle_path
        );
        std::string error_message;
        probabilistic_runtime_model_loaded_ =
            probabilistic_runtime_model_.loadFromFile(bundle_path.string(), &error_message);
        if (probabilistic_runtime_model_loaded_) {
            LOG_INFO(
                "Probabilistic runtime bundle loaded: path={}, features={}, markets={}",
                bundle_path.string(),
                probabilistic_runtime_model_.featureColumns().size(),
                "runtime-ready"
            );
        } else {
            LOG_WARN(
                "Probabilistic runtime bundle load skipped: path={}, reason={}",
                bundle_path.string(),
                error_message.empty() ? std::string("unknown") : error_message
            );
        }
    }
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
    live_warmup_scans_completed_ = 0;
    live_warmup_done_ = !config_.enable_live_cache_warmup;
    if (config_.mode == TradingMode::LIVE) {
        resetPolicyDecisionAuditArtifact();
    }

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
                        double reserved_amount = order.price * order.volume;
                        auto reserve_it = pending_buy_reservation_by_order_id_.find(order.order_id);
                        if (reserve_it != pending_buy_reservation_by_order_id_.end()) {
                            reserved_amount = reserve_it->second;
                            pending_buy_reservation_by_order_id_.erase(reserve_it);
                        }
                        if (reserved_amount > 0.0) {
                            risk_manager_->releasePendingCapital(reserved_amount);
                        }

                        if (order.filled_volume <= 1e-10) {
                            LOG_INFO("Order terminal without fill: {} ({})", order.market, order.order_id);
                            pending_signal_metadata_by_market_.erase(order.market);
                            continue;
                        }

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

                        PendingSignalMetadata fill_meta;
                        auto pending_meta_it = pending_signal_metadata_by_market_.find(order.market);
                        if (pending_meta_it != pending_signal_metadata_by_market_.end()) {
                            fill_meta = pending_meta_it->second;
                            risk_manager_->setPositionSignalInfo(
                                order.market,
                                fill_meta.signal_filter,
                                fill_meta.signal_strength,
                                fill_meta.market_regime,
                                fill_meta.liquidity_score,
                                fill_meta.volatility,
                                fill_meta.expected_value,
                                fill_meta.reward_risk_ratio,
                                fill_meta.entry_archetype,
                                fill_meta.probabilistic_runtime_applied,
                                fill_meta.probabilistic_h1_calibrated,
                                fill_meta.probabilistic_h5_calibrated,
                                fill_meta.probabilistic_h5_threshold,
                                fill_meta.probabilistic_h5_margin
                            );
                            pending_signal_metadata_by_market_.erase(pending_meta_it);
                        }

                        nlohmann::json fill_payload;
                        fill_payload["side"] = "BUY";
                        fill_payload["filled_volume"] = order.filled_volume;
                        fill_payload["avg_price"] = order.price;
                        fill_payload["strategy_name"] = order.strategy_name;
                        fill_payload["stop_loss"] = order.stop_loss;
                        fill_payload["take_profit_1"] = order.take_profit_1;
                        fill_payload["take_profit_2"] = order.take_profit_2;
                        fill_payload["signal_filter"] = fill_meta.signal_filter;
                        fill_payload["signal_strength"] = fill_meta.signal_strength;
                        fill_payload["market_regime"] = static_cast<int>(fill_meta.market_regime);
                        fill_payload["liquidity_score"] = fill_meta.liquidity_score;
                        fill_payload["volatility"] = fill_meta.volatility;
                        fill_payload["expected_value"] = fill_meta.expected_value;
                        fill_payload["reward_risk_ratio"] = fill_meta.reward_risk_ratio;
                        fill_payload["entry_archetype"] = fill_meta.entry_archetype;
                        fill_payload["probabilistic_runtime_applied"] = fill_meta.probabilistic_runtime_applied;
                        fill_payload["probabilistic_h1_calibrated"] = fill_meta.probabilistic_h1_calibrated;
                        fill_payload["probabilistic_h5_calibrated"] = fill_meta.probabilistic_h5_calibrated;
                        fill_payload["probabilistic_h5_threshold"] = fill_meta.probabilistic_h5_threshold;
                        fill_payload["probabilistic_h5_margin"] = fill_meta.probabilistic_h5_margin;
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
                        position_payload["signal_filter"] = fill_meta.signal_filter;
                        position_payload["signal_strength"] = fill_meta.signal_strength;
                        position_payload["market_regime"] = static_cast<int>(fill_meta.market_regime);
                        position_payload["liquidity_score"] = fill_meta.liquidity_score;
                        position_payload["volatility"] = fill_meta.volatility;
                        position_payload["expected_value"] = fill_meta.expected_value;
                        position_payload["reward_risk_ratio"] = fill_meta.reward_risk_ratio;
                        position_payload["entry_archetype"] = fill_meta.entry_archetype;
                        position_payload["probabilistic_runtime_applied"] = fill_meta.probabilistic_runtime_applied;
                        position_payload["probabilistic_h1_calibrated"] = fill_meta.probabilistic_h1_calibrated;
                        position_payload["probabilistic_h5_calibrated"] = fill_meta.probabilistic_h5_calibrated;
                        position_payload["probabilistic_h5_threshold"] = fill_meta.probabilistic_h5_threshold;
                        position_payload["probabilistic_h5_margin"] = fill_meta.probabilistic_h5_margin;
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

                if (config_.mode == TradingMode::LIVE &&
                    config_.enable_live_cache_warmup &&
                    !live_warmup_done_) {
                    live_warmup_scans_completed_++;

                    int ready_markets = 0;
                    int evaluated_markets = 0;
                    for (const auto& coin : scanned_markets_) {
                        analytics::CoinMetrics warmup_metrics = coin;
                        if (warmup_metrics.candles_by_tf.find("1m") == warmup_metrics.candles_by_tf.end() &&
                            !warmup_metrics.candles.empty()) {
                            warmup_metrics.candles_by_tf["1m"] = warmup_metrics.candles;
                        }
                        ensureParityCompanionTimeframes(warmup_metrics);
                        if (config_.use_confirmed_candle_only_for_signals) {
                            dropTrailingUnconfirmedCandles(
                                warmup_metrics.candles_by_tf,
                                getCurrentTimestampMs()
                            );
                        }
                        common::trimCandlesByPolicy(warmup_metrics.candles_by_tf);
                        const auto window_check =
                            common::checkLiveEquivalentWindow(warmup_metrics.candles_by_tf);
                        evaluated_markets++;
                        if (window_check.pass) {
                            ready_markets++;
                        }
                    }

                    const double ready_ratio = (evaluated_markets > 0)
                        ? static_cast<double>(ready_markets) / static_cast<double>(evaluated_markets)
                        : 0.0;
                    const bool warmup_scan_ok =
                        live_warmup_scans_completed_ >= config_.live_cache_warmup_min_scans;
                    const bool warmup_ready_ok =
                        ready_ratio >= config_.live_cache_warmup_min_ready_ratio;

                    if (warmup_scan_ok && warmup_ready_ok) {
                        live_warmup_done_ = true;
                        LOG_INFO(
                            "Live cache warm-up complete: scans={} (min {}), ready_markets={}/{}, ready_ratio={:.2f} (min {:.2f})",
                            live_warmup_scans_completed_,
                            config_.live_cache_warmup_min_scans,
                            ready_markets,
                            evaluated_markets,
                            ready_ratio,
                            config_.live_cache_warmup_min_ready_ratio
                        );
                    } else {
                        LOG_INFO(
                            "Live cache warm-up in progress: scans={} (min {}), ready_markets={}/{}, ready_ratio={:.2f} (min {:.2f})",
                            live_warmup_scans_completed_,
                            config_.live_cache_warmup_min_scans,
                            ready_markets,
                            evaluated_markets,
                            ready_ratio,
                            config_.live_cache_warmup_min_ready_ratio
                        );
                        nlohmann::json payload;
                        payload["reason"] = "live_cache_warmup_active";
                        payload["scans_completed"] = live_warmup_scans_completed_;
                        payload["min_scans"] = config_.live_cache_warmup_min_scans;
                        payload["ready_markets"] = ready_markets;
                        payload["evaluated_markets"] = evaluated_markets;
                        payload["ready_ratio"] = ready_ratio;
                        payload["min_ready_ratio"] = config_.live_cache_warmup_min_ready_ratio;
                        appendJournalEvent(
                            core::JournalEventType::NO_TRADE,
                            "MULTI",
                            "warmup_guard",
                            payload
                        );
                        pending_signals_.clear();
                        last_scan_time = std::chrono::steady_clock::now();
                        updateMetrics();
                        continue;
                    }
                }

                generateSignals();
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
    const auto scan_prefilter_thresholds = computeLiveScanPrefilterThresholds(
        config_,
        scanned_markets_,
        market_hostility_ewma_
    );
    for (const auto& coin : scanned_markets_) {
        if (coin.volume_24h < scan_prefilter_thresholds.min_volume_krw) {
            continue;
        }
        if (!coin.orderbook_snapshot.valid) {
            continue;
        }
        if (coin.orderbook_snapshot.spread_pct > scan_prefilter_thresholds.max_spread_pct) {
            continue;
        }
        if (coin.orderbook_snapshot.best_bid <= 0.0 || coin.orderbook_snapshot.best_ask <= 0.0) {
            continue;
        }
        if (coin.orderbook_snapshot.ask_notional < scan_prefilter_thresholds.min_ask_notional_krw) {
            continue;
        }

        filtered.push_back(coin);
    }

    if (filtered.size() > 20) {
        filtered.resize(20);
    }

    scanned_markets_ = filtered;
    LOG_INFO("Scan prefilter(dynamic): min_vol={:.0f}, max_spread={:.3f}%, min_ask_notional={:.0f}, hostility={:.3f}",
             scan_prefilter_thresholds.min_volume_krw,
             scan_prefilter_thresholds.max_spread_pct * 100.0,
             scan_prefilter_thresholds.min_ask_notional_krw,
             market_hostility_ewma_);

    const long long now_ms = getCurrentTimestampMs();
    for (const auto& coin : scanned_markets_) {
        if (coin.orderbook_snapshot.best_ask <= 0.0) {
            continue;
        }
        recent_best_ask_by_market_[coin.market] = coin.orderbook_snapshot.best_ask;
        recent_best_ask_timestamp_by_market_[coin.market] = now_ms;
    }

    LOG_INFO("Scanned markets: {}", scanned_markets_.size());

    int count = 0;
    for (const auto& coin : scanned_markets_) {
        if (count++ >= 5) {
            break;
        }

        LOG_INFO("  #{} {} - score {:.1f}, 24h vol {:.0f}????? volatility {:.2f}%",
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
    std::map<std::string, int> last_scan_rejection_counts;
    std::map<std::string, int> last_scan_hint_adjustment_counts;
    auto markScanReject = [&](const std::string& reason, int count = 1) {
        if (reason.empty() || count <= 0) {
            return;
        }
        last_scan_rejection_counts[reason] += count;
        recordLiveSignalReject(reason, static_cast<long long>(count));
    };
    const auto probabilistic_online_history = risk_manager_->getTradeHistory();

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
            if (config_.use_confirmed_candle_only_for_signals) {
                const size_t dropped_unconfirmed = dropTrailingUnconfirmedCandles(
                    signal_metrics.candles_by_tf,
                    getCurrentTimestampMs()
                );
                if (dropped_unconfirmed > 0) {
                    LOG_INFO("{} - dropped {} trailing unconfirmed candle(s) before signal generation",
                             coin.market,
                             static_cast<int>(dropped_unconfirmed));
                }
            }
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
            ProbabilisticRuntimeSnapshot probabilistic_snapshot;
            const auto probabilistic_online_state = computeProbabilisticOnlineLearningState(
                probabilistic_online_history,
                config_,
                signal_metrics.market,
                regime.regime
            );
            std::string probabilistic_prefilter_reason;
            if (!inferProbabilisticRuntimeSnapshot(
                    probabilistic_runtime_model_,
                    config_,
                    signal_metrics,
                    signal_metrics.market,
                    regime.regime,
                    &probabilistic_online_state,
                    probabilistic_snapshot,
                    &probabilistic_prefilter_reason)) {
                live_signal_funnel_.no_signal_generated++;
                markScanReject(probabilistic_prefilter_reason.empty()
                                   ? std::string("probabilistic_market_prefilter")
                                   : probabilistic_prefilter_reason);
                continue;
            }

            strategy::StrategyManager::CollectDiagnostics collect_diag;
            auto signals = strategy_manager_->collectSignals(
                signal_metrics.market,
                signal_metrics,
                candles,
                current_price,
                risk_manager_->getRiskMetrics().available_capital,
                regime,
                &collect_diag
            );

            if (!signals.empty()) {
                std::vector<strategy::Signal> adjusted_signals;
                adjusted_signals.reserve(signals.size());
                for (auto& signal : signals) {
                    std::string reject_reason;
                    if (!applyProbabilisticRuntimeAdjustment(
                            config_,
                            probabilistic_snapshot,
                            signal,
                            &reject_reason)) {
                        markScanReject(reject_reason.empty()
                                           ? std::string("probabilistic_runtime_hard_gate")
                                           : reject_reason);
                        continue;
                    }
                    adjusted_signals.push_back(std::move(signal));
                }
                signals = std::move(adjusted_signals);
            }

            if (signals.empty()) {
                live_signal_funnel_.no_signal_generated++;
                markScanReject("no_signal_generated");
                for (const auto& kv : collect_diag.no_signal_reason_counts) {
                    if (kv.first.empty() || kv.second <= 0) {
                        continue;
                    }
                    markScanReject(kv.first, kv.second);
                }
                continue;
            }
            live_signal_funnel_.generated_signal_candidates += static_cast<long long>(signals.size());

            strategy::Signal best_signal;
            int candidate_count = static_cast<int>(signals.size());
            const auto& manager_policy = probabilistic_snapshot.phase3_manager_filter_policy;
            const auto manager_defaults =
                autolife::analytics::ProbabilisticRuntimeModel::Phase3ManagerFilterPolicy{};
            const bool use_manager_policy = manager_policy.enabled;
            const auto manager_pick = [&](double policy_value, double default_value) {
                return use_manager_policy ? policy_value : default_value;
            };
            double min_strength = manager_pick(
                manager_policy.base_min_strength_default,
                manager_defaults.base_min_strength_default
            );
            double min_expected_value = manager_pick(
                manager_policy.base_min_expected_value,
                manager_defaults.base_min_expected_value
            );
            if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
                min_strength = manager_pick(
                    manager_policy.base_min_strength_high_volatility,
                    manager_defaults.base_min_strength_high_volatility
                );
            } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
                min_strength = manager_pick(
                    manager_policy.base_min_strength_trending_down,
                    manager_defaults.base_min_strength_trending_down
                );
            } else if (regime.regime == analytics::MarketRegime::RANGING) {
                min_strength = manager_pick(
                    manager_policy.base_min_strength_ranging,
                    manager_defaults.base_min_strength_ranging
                );
            }
            applyProbabilisticManagerFloors(
                config_,
                probabilistic_snapshot,
                regime.regime,
                &min_strength,
                &min_expected_value
            );

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

            std::vector<strategy::Signal> ranked;
            ranked.reserve(filtered.size());
            for (auto& signal : filtered) {
                std::string primary_reject_reason;
                if (!passesProbabilisticPrimaryMinimums(
                        config_,
                        signal,
                        regime.regime,
                        &probabilistic_snapshot,
                        &primary_reject_reason)) {
                    markScanReject(
                        primary_reject_reason.empty()
                            ? std::string("filtered_out_by_probabilistic_primary_minimum")
                            : primary_reject_reason
                    );
                    continue;
                }
                ranked.push_back(std::move(signal));
            }
            if (ranked.empty()) {
                live_signal_funnel_.filtered_out_by_manager++;
                markScanReject("filtered_out_by_probabilistic_primary_minimum");
                continue;
            }
            std::stable_sort(
                ranked.begin(),
                ranked.end(),
                [&](const strategy::Signal& lhs, const strategy::Signal& rhs) {
                    return probabilisticPrimaryPriorityScore(config_, lhs, regime.regime) >
                           probabilisticPrimaryPriorityScore(config_, rhs, regime.regime);
                }
            );
            candidate_count = static_cast<int>(ranked.size());
            best_signal = ranked.front();
            live_signal_funnel_.selection_call_count++;
            live_signal_funnel_.selection_scored_candidate_count +=
                static_cast<long long>(candidate_count);
            LOG_INFO(
                "{} probabilistic-primary best selected: p_h5={:.4f}, margin={:+.4f}, liq={:.1f}, strength={:.3f}, ranked_candidates={}, regime_state={}, vol_z={:+.3f}, dd_speed_bps={:.2f}, ens_n={}, u_std={:.4f}, ev_blend={:.3f}, ev_conf={:.3f}, edge_raw={:+.4f}%, edge_cal={:+.4f}%, cost_mode={}, c_entry={:.4f}%, c_exit={:.4f}%, c_tail={:.4f}%",
                coin.market,
                best_signal.probabilistic_h5_calibrated,
                best_signal.probabilistic_h5_margin,
                best_signal.liquidity_score,
                best_signal.strength,
                candidate_count,
                autolife::common::probabilistic_regime::stateLabel(probabilistic_snapshot.regime_state),
                probabilistic_snapshot.regime_volatility_zscore,
                probabilistic_snapshot.regime_drawdown_speed_bps,
                probabilistic_snapshot.ensemble_member_count,
                probabilistic_snapshot.prob_h5_std,
                best_signal.phase3.adaptive_ev_blend,
                best_signal.phase3.ev_confidence,
                best_signal.phase3.expected_edge_raw_pct * 100.0,
                best_signal.phase3.expected_edge_calibrated_pct * 100.0,
                best_signal.phase3.cost_mode,
                best_signal.phase3.cost_entry_pct * 100.0,
                best_signal.phase3.cost_exit_pct * 100.0,
                best_signal.phase3.cost_tail_pct * 100.0
            );

            if (best_signal.type == strategy::SignalType::NONE) {
                live_signal_funnel_.no_best_signal++;
                markScanReject("no_best_signal");
                continue;
            }

            if (best_signal.type != strategy::SignalType::NONE) {
                if (config_.probabilistic_regime_spec_enabled) {
                    const double regime_scale = std::clamp(
                        probabilistic_snapshot.regime_size_multiplier,
                        0.01,
                        1.0
                    );
                    if (std::fabs(regime_scale - 1.0) > 1e-6) {
                        best_signal.position_size *= regime_scale;
                        LOG_INFO(
                            "{} probabilistic regime size scale: state={}, scale={:.3f}, pos={:.4f}",
                            coin.market,
                            autolife::common::probabilistic_regime::stateLabel(
                                probabilistic_snapshot.regime_state
                            ),
                            regime_scale,
                            best_signal.position_size
                        );
                    }
                }
                best_signal.market_regime = regime.regime;
                pending_signals_.push_back(best_signal);
                total_signals_++;
                live_signal_funnel_.selected_signal_candidates++;
                live_signal_funnel_.markets_with_selected_candidate++;

                LOG_INFO("Signal selected: {} - {} (strength: {:.2f}, candidates={}, tf5m_preloaded={}, tf1h_preloaded={}, fallback={})",
                         coin.market,
                         best_signal.type == strategy::SignalType::STRONG_BUY ? "STRONG_BUY" : "BUY",
                         best_signal.strength,
                         candidate_count,
                         best_signal.used_preloaded_tf_5m ? "Y" : "N",
                         best_signal.used_preloaded_tf_1h ? "Y" : "N",
                         best_signal.used_resampled_tf_fallback ? "Y" : "N");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Signal generation failed: {} - {}", coin.market, e.what());
        }
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

    const auto metrics_snapshot = risk_manager_->getRiskMetrics();
    const bool small_seed_mode =
        metrics_snapshot.total_capital > 0.0 &&
        metrics_snapshot.total_capital <= config_.small_account_tier2_capital_krw;
    int per_scan_buy_limit = small_seed_mode
        ? 1
        : std::max(1, config_.max_new_orders_per_scan);
    if (small_seed_mode) {
        LOG_INFO("Small-seed mode active: capital {:.0f}, per_scan_limit={}",
                 metrics_snapshot.total_capital,
                 per_scan_buy_limit);
    }
    const double fee_rate_for_capacity = Config::getInstance().getFeeRate();
    const double capacity_stop_guard_pct = 0.03;
    const double capacity_slippage_guard_pct = std::clamp(config_.max_slippage_pct * 1.5, 0.0005, 0.02);
    const double capacity_exit_retention = std::max(0.50, 1.0 - capacity_stop_guard_pct - capacity_slippage_guard_pct - fee_rate_for_capacity);
    const double min_capacity_order_krw = std::max(config_.min_order_krw, config_.min_order_krw / capacity_exit_retention);
    const double deployable_capital = std::max(0.0, metrics_snapshot.available_capital + metrics_snapshot.reserved_capital);
    const int configured_max_positions = std::max(1, config_.max_positions);
    int effective_max_positions = std::clamp(
        static_cast<int>(std::floor(deployable_capital / std::max(1.0, min_capacity_order_krw))),
        1,
        configured_max_positions
    );
    per_scan_buy_limit = std::clamp(per_scan_buy_limit, 1, effective_max_positions);
    if (effective_max_positions != configured_max_positions) {
        LOG_INFO("Dynamic max positions: {} -> {} (deployable {:.0f}, min_slot {:.0f})",
                 configured_max_positions, effective_max_positions, deployable_capital, min_capacity_order_krw);
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

    if (!small_seed_mode && effective_hostility >= (hostile_threshold + 0.13)) {
        per_scan_buy_limit = 1;
    }

    LOG_INFO(
        "Adaptive scan profile: hostility_now={:.3f}, hostility_ewma={:.3f} (up {:.2f}/range {:.2f}/down {:.2f}/hv {:.2f}), per_scan_limit={}, pause_remaining={}",
        market_hostility_score,
        market_hostility_ewma_,
        up_ratio,
        ranging_ratio,
        down_ratio,
        high_vol_ratio,
        per_scan_buy_limit,
        hostile_pause_scans_remaining_
    );
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
        pending_signals_.clear();
        return;
    }

    std::vector<strategy::Signal> execution_candidates = pending_signals_;
    if (config_.enable_core_plane_bridge &&
        config_.enable_core_policy_plane &&
        core_cycle_) {
        core::PolicyContext context;
        context.small_seed_mode = small_seed_mode;
        context.max_new_orders_per_scan = per_scan_buy_limit;
        context.dominant_regime = dominant_regime;

        auto decision_batch = core_cycle_->selectPolicyCandidates(pending_signals_, context);
        execution_candidates = std::move(decision_batch.selected_candidates);
        appendPolicyDecisionAudit(
            decision_batch.decisions,
            dominant_regime,
            small_seed_mode,
            per_scan_buy_limit,
            resolvePolicyAuditTimestampMs(
                scanned_markets_,
                decision_batch.decisions,
                pending_signals_
            )
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
        appendPolicyDecisionAudit(
            policy_output.decisions,
            dominant_regime,
            small_seed_mode,
            per_scan_buy_limit,
            resolvePolicyAuditTimestampMs(
                scanned_markets_,
                policy_output.decisions,
                pending_signals_
            )
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
    if (total_potential >= static_cast<size_t>(effective_max_positions)) {
        LOG_WARN("Position limit reached: {} / effective_max {} (configured {})", total_potential, effective_max_positions, configured_max_positions);
    }
    std::stable_sort(
        execution_candidates.begin(),
        execution_candidates.end(),
        [&](const strategy::Signal& lhs, const strategy::Signal& rhs) {
            const auto lhs_regime =
                lhs.market_regime == analytics::MarketRegime::UNKNOWN ? dominant_regime : lhs.market_regime;
            const auto rhs_regime =
                rhs.market_regime == analytics::MarketRegime::UNKNOWN ? dominant_regime : rhs.market_regime;
            return probabilisticPrimaryPriorityScore(config_, lhs, lhs_regime) >
                   probabilisticPrimaryPriorityScore(config_, rhs, rhs_regime);
        }
    );

    for (auto& signal : execution_candidates) {
        if (signal.type != strategy::SignalType::BUY &&
            signal.type != strategy::SignalType::STRONG_BUY) {
            continue;
        }
        const auto signal_regime =
            signal.market_regime == analytics::MarketRegime::UNKNOWN ? dominant_regime : signal.market_regime;

        if (executed_buys_this_scan >= per_scan_buy_limit) {
            LOG_WARN("Per-scan buy limit reached: {} / {}",
                     executed_buys_this_scan, per_scan_buy_limit);
            filtered_out++;
            continue;
        }

        total_potential = getTotalPotentialPositions();
        if (total_potential >= static_cast<size_t>(effective_max_positions)) {
            filtered_out++;
            continue;
        }

        normalizeSignalStopLossByRegime(signal, signal_regime);

        if (!rebalanceSignalRiskReward(signal, config_)) {
            LOG_WARN("{} skipped: invalid price levels for RR normalization", signal.market);
            filtered_out++;
            continue;
        }

        if (config_.avoid_high_volatility &&
            signal_regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            LOG_INFO("{} skipped by regime gate: {}", signal.market, regimeToString(signal_regime));
            filtered_out++;
            continue;
        }
        if (config_.avoid_trending_down &&
            signal_regime == analytics::MarketRegime::TRENDING_DOWN) {
            LOG_INFO("{} skipped by regime gate: {}", signal.market, regimeToString(signal_regime));
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
        const double calibrated_expected_edge_pct = computeCalibratedExpectedEdgePct(signal, config_);
        const double occupancy_ratio = (effective_max_positions > 0)
            ? std::clamp(static_cast<double>(total_potential) / static_cast<double>(effective_max_positions), 0.0, 1.5)
            : 1.0;
        auto runtime_metrics = risk_manager_->getRiskMetrics();
        const double capital_buffer_ratio = (runtime_metrics.total_capital > 1e-9)
            ? std::clamp(runtime_metrics.available_capital / runtime_metrics.total_capital, 0.0, 1.0)
            : 1.0;
        const double diversification_scale = std::clamp(1.0 - (0.45 * occupancy_ratio), 0.55, 1.0);
        const double capital_scale = std::clamp(0.65 + (0.35 * capital_buffer_ratio), 0.60, 1.0);
        const double auto_position_scale = std::clamp(diversification_scale * capital_scale, 0.45, 1.0);
        signal.position_size *= auto_position_scale;
        LOG_INFO("{} auto sizing: occupancy {:.2f}, capital_buffer {:.2f}, auto_scale {:.2f}x => pos {:.4f}",
                 signal.market, occupancy_ratio, capital_buffer_ratio, auto_position_scale, signal.position_size);

        const double strength_multiplier = std::clamp(0.5 + signal.strength, 0.75, 1.5);
        signal.position_size *= strength_multiplier;
        const double probabilistic_scale = probabilisticPositionScaleForEntry(config_, signal, nullptr);
        if (std::fabs(probabilistic_scale - 1.0) > 1e-6) {
            signal.position_size *= probabilistic_scale;
            LOG_INFO(
                "{} probabilistic entry size scale: margin={:+.4f}, scale {:.3f}x => pos {:.4f}",
                signal.market,
                signal.probabilistic_h5_margin,
                probabilistic_scale,
                signal.position_size
            );
        }

        LOG_INFO("Signal candidate - {} [{}] (strength {:.3f}, strength_mul {:.2f}x => pos {:.4f})",
                 signal.market, signal.strategy_name, signal.strength,
                 strength_multiplier, signal.position_size);

        double risk_pct = 0.0;
        if (signal.entry_price > 0.0 && signal.stop_loss > 0.0) {
            risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
        }
        risk_pct = std::clamp(risk_pct, 0.0, 0.25);

        const double fee_rate = Config::getInstance().getFeeRate();
        const auto dynamic_buy_slippage = computeDynamicSlippageThresholds(
            config_,
            market_hostility_ewma_,
            true,
            signal.market_regime,
            signal.strength,
            signal.liquidity_score,
            signal.expected_value
        );
        const double slippage_guard_pct = dynamic_buy_slippage.guard_slippage_pct;
        // Align with RiskManager entry survivability check (BASE_STOP_LOSS_PCT=3%)
        // to avoid internal gate mismatch where runtime "safe minimum" passes but
        // RiskManager rejects the same order as non-survivable at stop-loss exit.
        const double stop_guard_pct = std::clamp((risk_pct > 1e-9) ? risk_pct : 0.03, 0.03, 0.20);
        const double exit_retention = std::max(0.50, 1.0 - stop_guard_pct - slippage_guard_pct - fee_rate);
        const double min_required_krw = std::max(config_.min_order_krw, config_.min_order_krw / exit_retention);

        auto risk_metrics = runtime_metrics;
        const double risk_budget_krw = risk_metrics.total_capital * config_.risk_per_trade_pct;
        if (risk_metrics.available_capital < min_required_krw) {
            LOG_WARN("{} skipped - insufficient available capital (have {:.0f}, need {:.0f}, stop_guard {:.2f}%)",
                     signal.market, risk_metrics.available_capital, min_required_krw, stop_guard_pct * 100.0);
            continue;
        }

        double min_position_size = min_required_krw / risk_metrics.available_capital;
        if (signal.position_size < min_position_size) {
            LOG_INFO("{} raised position_size {:.4f} -> {:.4f} (safe min order {:.0f})",
                     signal.market, signal.position_size, min_position_size, min_required_krw);
            signal.position_size = min_position_size;
        }

        if (signal.position_size > 1.0) {
            LOG_WARN("{} position_size clamped: {:.4f} -> 1.0", signal.market, signal.position_size);
            signal.position_size = 1.0;
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

        if (signal.position_size + 1e-9 < min_position_size) {
            LOG_WARN("{} skipped - risk budget too small for safe minimum order (pos {:.4f} < min {:.4f})",
                     signal.market, signal.position_size, min_position_size);
            continue;
        }

        auto strategy_ptr = strategy_manager_ ? strategy_manager_->getStrategy(signal.strategy_name) : nullptr;
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
            LOG_WARN("{} skipped - computed entry amount below safe minimum ({:.0f} < {:.0f})",
                     signal.market, signal.entry_amount, min_required_krw);
            continue;
        }

        if (executeBuyOrder(signal.market, signal)) {
            PendingSignalMetadata pending_meta;
            pending_meta.signal_filter = signal.signal_filter;
            pending_meta.signal_strength = signal.strength;
            pending_meta.market_regime = signal.market_regime;
            pending_meta.liquidity_score = signal.liquidity_score;
            pending_meta.volatility = signal.volatility;
            pending_meta.expected_value = (std::isfinite(calibrated_expected_edge_pct)
                ? calibrated_expected_edge_pct
                : (signal.expected_value != 0.0 ? signal.expected_value : 0.0));
            pending_meta.reward_risk_ratio = reward_risk_ratio;
            pending_meta.entry_archetype = signal.entry_archetype.empty()
                ? "UNSPECIFIED"
                : signal.entry_archetype;
            pending_meta.probabilistic_runtime_applied = signal.probabilistic_runtime_applied;
            pending_meta.probabilistic_h1_calibrated = signal.probabilistic_h1_calibrated;
            pending_meta.probabilistic_h5_calibrated = signal.probabilistic_h5_calibrated;
            pending_meta.probabilistic_h5_threshold = signal.probabilistic_h5_threshold;
            pending_meta.probabilistic_h5_margin = signal.probabilistic_h5_margin;

            if (risk_manager_->getPosition(signal.market) != nullptr) {
                risk_manager_->setPositionSignalInfo(
                    signal.market,
                    pending_meta.signal_filter,
                    pending_meta.signal_strength,
                    pending_meta.market_regime,
                    pending_meta.liquidity_score,
                    pending_meta.volatility,
                    pending_meta.expected_value,
                    pending_meta.reward_risk_ratio,
                    pending_meta.entry_archetype,
                    pending_meta.probabilistic_runtime_applied,
                    pending_meta.probabilistic_h1_calibrated,
                    pending_meta.probabilistic_h5_calibrated,
                    pending_meta.probabilistic_h5_threshold,
                    pending_meta.probabilistic_h5_margin
                );
                pending_signal_metadata_by_market_.erase(signal.market);
            } else {
                pending_signal_metadata_by_market_[signal.market] = pending_meta;
                LOG_INFO("{} position metadata deferred until fill event", signal.market);
            }
            if (strategy_ptr) {
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
            LOG_INFO("Position counter updated: {} -> {} (effective_max {}, configured {})",
                     previous_total, total_potential, effective_max_positions, configured_max_positions);
        }
    }

    LOG_INFO("Signal execution done: executed {}, filtered {}", executed, filtered_out);
    if (executed_buys_this_scan <= 0) {
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
    }
    pending_signals_.clear();
}
bool TradingEngine::executeBuyOrder(
    const std::string& market,
    const strategy::Signal& signal
) {
    LOG_INFO("?????좎럩堉????????????遺얘턁??????? {} (??????????좎럥???????????좎룞???????傭?끆????좎럡?꾢뜝??? {:.2f})", market, signal.strength);
    
    try {
        // 1. [????? ?????Orderbook) ???????????????????좎럩堉???????????????좎럥큔???????????????????????????좎럥??????????
        //    ?????????????좎럡?썹땟戮녹???좎럩?????좎?留????????????????????????좎듅?(Ticker)??????????? ????????좎럡?썹땟戮녹???좎럩?????좎?留???? ??????????????????좎뜴泥???????????????????'??????좎럥큔?????????????됰엨??????1???'???????????⑤㈇??????
        auto orderbook = http_client_->getOrderBook(market);
        if (orderbook.empty()) {
            LOG_ERROR("??? ???????????????????????좎럥痢?? {}", market);
            return false;
        }
        
        // [??????????좎럥??????????좎럡??????? API ?????????좎럥??????????????????????????좎룞??????????????????????????좎럥큔?????????????饔낅떽??????좎럥梨?????
        nlohmann::json units;
        if (orderbook.is_array() && !orderbook.empty()) {
            units = orderbook[0]["orderbook_units"];
        } else if (orderbook.contains("orderbook_units")) {
            units = orderbook["orderbook_units"];
        } else {
            LOG_ERROR("{} - ??? ?????????orderbook_units)???????좎럩堉?????????????????嚥싲갭큔?????????????좎럥梨룟뜝? {}", market, orderbook.dump());
            return false;
        }

        if (config_.enable_realtime_entry_veto) {
            const long long now_ms = getCurrentTimestampMs();
            const int tracking_window_seconds = std::max(
                10,
                config_.realtime_entry_veto_tracking_window_seconds
            );
            const auto veto_thresholds = computeRealtimeEntryVetoThresholds(
                config_,
                signal,
                market_hostility_ewma_
            );
            const auto snapshot = analytics::OrderbookAnalyzer::analyze(
                units,
                std::max(config_.min_order_krw, 50000.0)
            );
            if (!snapshot.valid || snapshot.best_ask <= 0.0) {
                LOG_WARN("{} buy vetoed: invalid realtime orderbook snapshot", market);
                return false;
            }
            if (snapshot.spread_pct > veto_thresholds.max_spread_pct) {
                LOG_WARN(
                    "{} buy vetoed: spread {:.4f}% > {:.4f}% (dynamic)",
                    market,
                    snapshot.spread_pct * 100.0,
                    veto_thresholds.max_spread_pct * 100.0
                );
                return false;
            }
            if (snapshot.imbalance < veto_thresholds.min_orderbook_imbalance) {
                LOG_WARN(
                    "{} buy vetoed: orderbook imbalance {:.3f} < {:.3f} (dynamic)",
                    market,
                    snapshot.imbalance,
                    veto_thresholds.min_orderbook_imbalance
                );
                return false;
            }

            const double max_drop_pct = std::max(0.0001, veto_thresholds.max_drop_pct);
            const double drop_vs_signal = (signal.entry_price > 0.0)
                ? ((signal.entry_price - snapshot.best_ask) / signal.entry_price)
                : 0.0;
            if (drop_vs_signal > max_drop_pct) {
                LOG_WARN(
                    "{} buy vetoed: rapid drop vs signal price {:.4f}% > {:.4f}%",
                    market,
                    drop_vs_signal * 100.0,
                    max_drop_pct * 100.0
                );
                return false;
            }

            auto prev_price_it = recent_best_ask_by_market_.find(market);
            auto prev_ts_it = recent_best_ask_timestamp_by_market_.find(market);
            if (prev_price_it != recent_best_ask_by_market_.end() &&
                prev_ts_it != recent_best_ask_timestamp_by_market_.end() &&
                prev_price_it->second > 0.0) {
                const long long age_ms = now_ms - prev_ts_it->second;
                if (age_ms >= 0 &&
                    age_ms <= static_cast<long long>(tracking_window_seconds) * 1000LL) {
                    const double drop_vs_recent =
                        (prev_price_it->second - snapshot.best_ask) / prev_price_it->second;
                    if (drop_vs_recent > max_drop_pct) {
                        LOG_WARN(
                            "{} buy vetoed: rapid drop vs recent best ask {:.4f}% > {:.4f}% (age={}s)",
                            market,
                            drop_vs_recent * 100.0,
                            max_drop_pct * 100.0,
                            age_ms / 1000
                        );
                        return false;
                    }
                }
            }

            recent_best_ask_by_market_[market] = snapshot.best_ask;
            recent_best_ask_timestamp_by_market_[market] = now_ms;
        }

        double best_ask_price = calculateOptimalBuyPrice(market, signal.entry_price, orderbook); // ??????좎럥큔?????????????됰엨??????1???
        
        // 2. ????????????⑤즾??????????????????????????????
        auto metrics = risk_manager_->getRiskMetrics();
        
        // Keep execution minimum aligned with stop-loss survivability and exchange minimum.
        const double fee_rate = Config::getInstance().getFeeRate();
        const auto dynamic_buy_slippage = computeDynamicSlippageThresholds(
            config_,
            market_hostility_ewma_,
            true,
            signal.market_regime,
            signal.strength,
            signal.liquidity_score,
            signal.expected_value
        );
        double signal_risk_pct = 0.0;
        if (signal.entry_price > 0.0 && signal.stop_loss > 0.0 && signal.stop_loss < signal.entry_price) {
            signal_risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
        }
        signal_risk_pct = std::clamp(signal_risk_pct, 0.0, 0.25);
        // Keep stop-loss survivability floor aligned with RiskManager (3% baseline).
        const double stop_guard_pct = std::clamp((signal_risk_pct > 1e-9) ? signal_risk_pct : 0.03, 0.03, 0.20);
        const double slippage_guard_pct = dynamic_buy_slippage.guard_slippage_pct;
        const double exit_retention = std::max(0.50, 1.0 - stop_guard_pct - slippage_guard_pct - fee_rate);
        const double MIN_ORDER_BUFFER = std::max(config_.min_order_krw, config_.min_order_krw / exit_retention);
        double invest_amount = 0.0;
        double safe_position_size = 0.0;
        
        // [NEW] executeSignals????????????????????????됰Ŧ??????????????????⑤즾??????????????
        if (signal.entry_amount > 0.0) {
            invest_amount = signal.entry_amount;
            // ???? position_size ??????????좎룞彛????(?????????좎럥????????μ떜媛?걫??좎럩???????????좎럥큔?????????? ?????좎럥??????????????좎럥큔????????????????壤굿??좎?留??????
            if (metrics.available_capital > 0) {
                safe_position_size = invest_amount / metrics.available_capital;
            }
        } else {
            // Fallback (???????????????????븍툖??????
            safe_position_size = signal.position_size;
            if (safe_position_size > 1.0) safe_position_size = 1.0;
            invest_amount = metrics.available_capital * safe_position_size;
        }

        // [??????좎럥큔???????????????좎럥?℡뜝?濚밸Þ??????????좎럥큔????????????1] ?????????????????????????????좎럥큔?????????????????????????됰Ŧ????????????(???? ????????????????????????좎럩堉????????
        // ???????熬곣뫖利닷뜝???? signal.entry_amount????????????????????????????????????????????????????????????????汝뷴뜝??좎룞??????좎럥????????????좎럩??ル윻?????
        // ????????좎럡?썹땟戮녹???좎럩?????좎?留?????????? ???????????좎룞???????????????????????????좎럥큔??????? ??????됰Ŧ????????????????????좎럡?썹땟戮녹???좎럩?????좎?留??
        if (metrics.available_capital < invest_amount) {
             // ?????????????濚밸Ŧ???????? ??? ????????좎럡?썹땟戮녹???좎럩?????좎?留??????????筌롫㈇??????? ????????좎럡?썹땟戮녹???좎럩?????좎?留?????????좎럥梨룟뜝???????????熬곣뫖利닷뜝??좎룞?쇿뜝???
             // ??????좎럥큔??????????????????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝??????????좎럥踰▼뜝?????????????????좎럩堉??????饔낅떽????????????????????熬곣뫖利닷뜝??좎룞?쇿뜝??????????좎럩?쒎뜝??좎룞????좎럩??????
             if (metrics.available_capital < MIN_ORDER_BUFFER) {
                 LOG_WARN("{} - ????????좎룞???????????????????좎럩沅????{:.0f} < {:.0f} (?????좎럩堉???????????????????좎럩紐??)", 
                          market, metrics.available_capital, MIN_ORDER_BUFFER);
                 return false;
             }
             // ??? ???????????????????좎럡???????????좎럥큔???????????????????좎럥踰▼뜝????????????????????좎럥裕??????? ??? ???????????좎룞????????????????????좎럥큔?????????(Partial Entry)
             LOG_INFO("{} - ????????좎룞???????????????????좎럩沅????????????????????????????????????????? {:.0f} -> {:.0f}", 
                      market, invest_amount, metrics.available_capital);
             invest_amount = metrics.available_capital;
        }
        
        LOG_INFO("{} - [???????????? ????????????좎럩苡???????????좎럩猶욕뜝???????? {:.0f} KRW (????????좎룞??????{:.0f})", 
                     market, invest_amount, metrics.available_capital);
        
        if (invest_amount < MIN_ORDER_BUFFER) {
            LOG_WARN("{} - ?????좎럩堉?????????????????좎럥踰???????????좎럩沅???????????좎럩猶욕뜝???????? {:.0f}, ??????熬곣뫖利닷뜝??좎룞?쇿뜝?? {:.0f} KRW) [??????????????]", 
                     market, invest_amount, MIN_ORDER_BUFFER);
            return false;
        }
        
        // [?????????????좎럥??????????좎럡??????? ?????????좎럡萸??????position_size??RiskManager???????
        if (!risk_manager_->canEnterPosition(
            market,
            signal.entry_price,
            safe_position_size,
            signal.strategy_name
        )) {
            LOG_WARN("{} - ???????ル뵁??????濚밸Ŧ援욃뜝??????뀀맩鍮??????????????????좎럥痢??(???????ル뵁??????濚밸Ŧ援욃뜝???????????", market);
            return false;
        }

        if (invest_amount > config_.max_order_krw) invest_amount = config_.max_order_krw;
        
        // ??????좎럥큔?????????? ???????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝????????????????????(??????8????????耀붾굝?????????)
        double quantity = invest_amount / best_ask_price;

        // [NEW] ???????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝???????????????????????? ???????
        double slippage_pct = estimateOrderbookSlippagePct(
            orderbook,
            quantity,
            true,
            best_ask_price
        );

        if (slippage_pct > dynamic_buy_slippage.max_slippage_pct) {
            LOG_WARN("{} ?????? ????? {:.3f}% > {:.3f}% (?????좎럩堉?????????????좎럩堉???????좎럥??????鶯ㅺ동????좎룞?????",
                     market, slippage_pct * 100.0, dynamic_buy_slippage.max_slippage_pct * 100.0);
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
        
        // ??????????????????좎럡萸?????(?????????????됰Ŧ???????????????????????좎럥큔?????????????饔낅떽??????좎럥梨??????????????좎럩??щ엠??????????좎럥利????????????????????熬곣뫖利닷뜝????
        // [?????? ?????????????????(sprintf ?????stringstream ????
        char buffer[64];
        // ??????? ??????8????????耀붾굝?????????, ?????????????????????0 ??????????????????????????븍툖??????????????좎럡?썹땟戮녹???좎럩?????좎?留?????????????????좎룞彛???
        std::snprintf(buffer, sizeof(buffer), "%.8f", quantity); 
        std::string vol_str(buffer);
        
        LOG_INFO("  ???????꾩룆梨???耀붾굝????????⑤챶裕?????????좎럡?썼린?????좎뜫爰???????????? ??? {:.0f}, ??????{}, ???????좎럩猶욕뜝????????{:.0f}", 
                 best_ask_price, vol_str, invest_amount);

            // 3. [??????????????????좎뜴泥????? ??????????????????좎뜴泥???????????좎럥큔????????????????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝??(OrderManager ????????좎럡?썹땟戮녹???좎럩?????좎?留????????????
        if (config_.mode == TradingMode::LIVE && !config_.dry_run && !config_.allow_live_orders) {
            LOG_WARN("{} live buy blocked: trading.allow_live_orders=false, fallback to paper simulation", market);
        }

        if (isLiveRealOrderEnabled()) {
            
            // [NEW] OrderManager?????????????????좎듅?????????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝?????????좎럩堉????????
            // Strategy Metadata ????????좎럡?썹땟戮녹???좎럩?????좎?留???
            std::string submitted_order_id;
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
                signal.trailing_start,
                &submitted_order_id
            );

            if (submitted) {
                LOG_INFO("OrderManager ???????꾩룆梨???耀붾굝????????⑤챶裕??????遺얘턁?????????????熬곣뫖利닷뜝??좎룞?쇿뜝?? {}", market);
                risk_manager_->reservePendingCapital(invest_amount);  // ???????좎럩堉????????????????????????
                nlohmann::json order_payload;
                if (!submitted_order_id.empty()) {
                    pending_buy_reservation_by_order_id_[submitted_order_id] = invest_amount;
                }
                order_payload["side"] = "BUY";
                order_payload["price"] = best_ask_price;
                order_payload["volume"] = quantity;
                order_payload["strategy_name"] = signal.strategy_name;
                order_payload["entry_amount"] = invest_amount;
                order_payload["order_id"] = submitted_order_id;
                appendJournalEvent(
                    core::JournalEventType::ORDER_SUBMITTED,
                    market,
                    submitted_order_id.empty() ? "live_buy" : submitted_order_id,
                    order_payload
                );
                return true; // Async Success
            } else {
                LOG_ERROR("OrderManager submit failed");
                return false;
            }
        } 
        else {
            // Paper Trading (??????좎럥큔??????????????좎럥큔??????? - ???????????????????븍툖????????? (Simulation)
            // ... (Paper Mode Logic stays roughly same or we use OrderManager for Paper too?)
            // OrderManager currently hits API. So for Paper Mode we should NOT use OrderManager 
            // unless we Mock API.
            // Current Paper Mode simulates "Fill" immediately.
            
            // [???????????좎룞??????좎뜫爰?????獄쎼끏???????????????????
            double dynamic_stop_loss = best_ask_price * 0.975; // ?????????? -2.5%
            try {
                auto candles_json = http_client_->getCandles(market, "60", 200);
                if (!candles_json.empty() && candles_json.is_array()) {
                    auto candles = analytics::TechnicalIndicators::jsonToCandles(candles_json);
                    if (!candles.empty()) {
                        dynamic_stop_loss = risk_manager_->calculateDynamicStopLoss(best_ask_price, candles);
                        LOG_INFO("[PAPER] ????????좎뜴泥???癲ル슢??㎖?밤뀋??????????????????좎듅? ???????????? {:.0f} ({:.2f}%)", 
                                 dynamic_stop_loss, (dynamic_stop_loss - best_ask_price) / best_ask_price * 100.0);
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN("[PAPER] ????????좎뜴泥???癲ル슢??㎖?밤뀋??????????????????좎듅? ??????????????????????좎럥痢?? ??????????-2.5%) ???? {}", e.what());
            }

            double applied_stop_loss = dynamic_stop_loss;
            if (signal.stop_loss > 0.0) {
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

            const std::string paper_buy_order_id = buildSyntheticLiveOrderId(market, "buy");
            recordLiveExecutionLifecycle(
                "live_paper",
                "submitted",
                paper_buy_order_id,
                market,
                OrderSide::BUY,
                OrderStatus::SUBMITTED,
                0.0,
                quantity,
                best_ask_price,
                signal.strategy_name,
                false
            );
            recordLiveExecutionLifecycle(
                "live_paper",
                "filled",
                paper_buy_order_id,
                market,
                OrderSide::BUY,
                OrderStatus::FILLED,
                quantity,
                quantity,
                best_ask_price,
                signal.strategy_name,
                true
            );

            nlohmann::json paper_fill_payload;
            paper_fill_payload["side"] = "BUY";
            paper_fill_payload["filled_volume"] = quantity;
            paper_fill_payload["avg_price"] = best_ask_price;
            paper_fill_payload["strategy_name"] = signal.strategy_name;
            paper_fill_payload["signal_filter"] = signal.signal_filter;
            paper_fill_payload["signal_strength"] = signal.strength;
            paper_fill_payload["market_regime"] = static_cast<int>(signal.market_regime);
            paper_fill_payload["liquidity_score"] = signal.liquidity_score;
            paper_fill_payload["volatility"] = signal.volatility;
            paper_fill_payload["expected_value"] = signal.expected_value;
            paper_fill_payload["entry_archetype"] =
                signal.entry_archetype.empty() ? "UNSPECIFIED" : signal.entry_archetype;
            paper_fill_payload["probabilistic_runtime_applied"] = signal.probabilistic_runtime_applied;
            paper_fill_payload["probabilistic_h1_calibrated"] = signal.probabilistic_h1_calibrated;
            paper_fill_payload["probabilistic_h5_calibrated"] = signal.probabilistic_h5_calibrated;
            paper_fill_payload["probabilistic_h5_threshold"] = signal.probabilistic_h5_threshold;
            paper_fill_payload["probabilistic_h5_margin"] = signal.probabilistic_h5_margin;
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
            paper_pos_payload["signal_filter"] = signal.signal_filter;
            paper_pos_payload["signal_strength"] = signal.strength;
            paper_pos_payload["market_regime"] = static_cast<int>(signal.market_regime);
            paper_pos_payload["liquidity_score"] = signal.liquidity_score;
            paper_pos_payload["volatility"] = signal.volatility;
            paper_pos_payload["expected_value"] = signal.expected_value;
            paper_pos_payload["entry_archetype"] =
                signal.entry_archetype.empty() ? "UNSPECIFIED" : signal.entry_archetype;
            paper_pos_payload["probabilistic_runtime_applied"] = signal.probabilistic_runtime_applied;
            paper_pos_payload["probabilistic_h1_calibrated"] = signal.probabilistic_h1_calibrated;
            paper_pos_payload["probabilistic_h5_calibrated"] = signal.probabilistic_h5_calibrated;
            paper_pos_payload["probabilistic_h5_threshold"] = signal.probabilistic_h5_threshold;
            paper_pos_payload["probabilistic_h5_margin"] = signal.probabilistic_h5_margin;
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
        LOG_ERROR("executeBuyOrder ????????좎럩苡????? {}", e.what());
        return false;
    }
}


void TradingEngine::monitorPositions() {

    static int log_counter = 0;
    bool should_log = (log_counter++ % 10 == 0);

    // 1. ????????좎럡?썹땟戮녹???좎럩?????좎?留????????????좎럥諭??????????????좎럥??????????좎럡??????????????????좎럥큔??????????????좎럥큔????????좎럥釉뜹뜝???좎뜾異?에?吏?????좎럩肉???????????????????????됰Ŧ?????????耀붾굝?????????좎떊?곷퉲????좎럥?????????
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
    
    // 2. ?????????좎럡萸????? ??????????좎럥??????????좎럡???????????????嚥싲갭큔??????????????????????좎룞?쇿뜝??????좎뜾???????????좎럡?썹땟戮녹???좎럩?????좎?留????????좎럥큔??????????????????좎럥裕????????(Batch ????????????????????좎럡?볟뜝??????????????????????
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
        LOG_INFO("===== ??????????좎럩堉????????????좎럩堉????????????좎럡萸???({}????????繹먮굝??? =====", markets.size());
    }

    // 3. [????? ?????????HTTP ?????????????????????좎럥큔??????????????좎럥큔?????????????嚥싲갭큔????????????좎럡?썹땟戮녹???좎럩?????좎?留????????????????????????좎듅? ?????????????(Batch Processing)
    std::map<std::string, double> price_map;
    
    try {
        // MarketScanner???????????????汝뷴뜝??좎룞????좎뜦維뽩뜝??getTickerBatch ?????
        auto tickers = http_client_->getTickerBatch(markets);
        
        for (const auto& t : tickers) {
            std::string market_code = t["market"].get<std::string>();
            double trade_price = t["trade_price"].get<double>();
            price_map[market_code] = trade_price;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("????????????????????????????????????????좎럥痢?? {}", e.what());
        return; // ??????????????????????????????좎럥踰▼뜝???????????? ??????좎럥큔?????????????? (?????????????좎럥큔?????????????됰엨????????????熬곣뫖利닷뜝??좎룞?쇿뜝?????좎럥沅???좎????좎럡?볟젆??)
    }

    // 4. ??????좎럥큔???????????????熬곣뫖利닷뜝??좎룞?쇿뜝?????????????????????????? ??????????????????????????????????좎럥캇?????좎럡愿????(??????????????????? ??????좎럥큔????????????????????룰퀬??移???
    for (auto& pos : positions) {
        // ????????????좎럥큔???????????????????????????嚥싲갭큔???????????????????????좎럥큔?????????????????????
        if (price_map.find(pos.market) == price_map.end()) {
            LOG_WARN("{} ticker data missing", pos.market);
            continue;
        }

        double current_price = price_map[pos.market];
        
        // RiskManager ???????????????????????좎룞??????????ш끽維뽳쭩?뱀땡???좎뜴????(??????좎럥큔???????????????熬곣뫖利닷뜝??좎룞?쇿뜝????????????饔낅떽??????
        risk_manager_->updatePosition(pos.market, current_price);
        
        // [????????????좎룞彛?????????????????濚밸Ŧ?????? ????????좎럡?썹땟戮녹???좎럩?????좎?留??????"????????????좎룞??????????ш끽維뽳쭩?뱀땡???좎뜴??????" ??????????????????좎럥큔????????遺얘턁????????熬곣뫖??????===================
        std::shared_ptr<strategy::IStrategy> strategy;
        if (strategy_manager_) {
            // ?????????????????좎럥裕??????????????????嶺뚮씚維?????????좎럥梨룟뜝?????????????좎럡?썹땟戮녹???좎럩?????좎?留????????좎럥큔?????????????????????(pos.strategy_name ????
            strategy = strategy_manager_->getStrategy(pos.strategy_name);
            
            if (strategy) {
                // ????????????IStrategy????????????좎룞彛?????updateState ??????됰Ŧ?????????
                // ???????????좎룞??????????????좎럡?썹땟戮녹???좎럩?????좎?留????????????좎럩?????????????????????????????좎럩堉??????饔낅떽???????????????????좎룞???????????룸㎗?ルき????????좎럥큔???????????????????????좎럥痢?????????????????????좎럥??????????????????????
                // ??????遺얘턁????????????????????좎럡?썹땟戮녹???좎럩?????좎?留????????????좎럩????????????????????좎룞???????????????????????????좎듅?????????????좎럥異????????????????????????좎럥?????(??????????????????좎뜴泥??????
                strategy->updateState(pos.market, current_price);
            }
        }

        // ??????????????????????????????????좎럡?썲뜝????????????????????됰Ŧ?????????耀붾굝?????????좎떊?곷퉲????좎럥?????????(?????????좎럥裕????????????????????????????????됰Ŧ????????????
        auto* updated_pos = risk_manager_->getPosition(pos.market);
        if (!updated_pos) continue;
        
        if (should_log) {
            LOG_INFO("  {} - ??????熬곣뫖利닷뜝??좎룞?쇿뜝?? {} - ?????좎럩堉???????? {:.0f}, ??????熬곣뫖利닷뜝??좎룞?쇿뜝?? {:.0f}, ????? {:.0f} ({:+.2f}%)",
                     pos.market, updated_pos->strategy_name, updated_pos->entry_price, current_price,
                     updated_pos->unrealized_pnl, updated_pos->unrealized_pnl_pct * 100.0);
        }

        risk_manager_->applyAdaptiveRiskControls(pos.market);
        updated_pos = risk_manager_->getPosition(pos.market);
        if (!updated_pos) continue;

        // --- ??????좎럥큔?????????????됰엨?????????????????븍툖??????(????????좎럡?썹땟戮녹???좎럩?????좎?留??????????????????????????????) ---


        // ????????좎럡?썹땟戮녹???좎럩?????좎?留??????????????????????????(?????????????????
        if (strategy) {
            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            double holding_time_seconds = (now_ms - updated_pos->entry_time) / 1000.0;

            if (strategy->shouldExit(pos.market, updated_pos->entry_price, current_price, holding_time_seconds)) {
                LOG_INFO("??????熬곣뫖利닷뜝??좎룞?쇿뜝????????????????????????黎앸럽?????沃섃넄??????? {} (??????熬곣뫖利닷뜝??좎룞?쇿뜝?? {})", pos.market, updated_pos->strategy_name);
                executeSellOrder(pos.market, *updated_pos, "strategy_exit", current_price);
                continue;
            }
        }

        // 1?????????????좎럥큔????????????(50% ????
        if (!updated_pos->half_closed && current_price >= updated_pos->take_profit_1) {
            LOG_INFO("1?????????????????????????黎앸럽?????沃섃넄??????? (???????좎럥?℡뜝???좎룞??????{:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            executePartialSell(pos.market, *updated_pos, current_price);
            continue; // ???????濚밸Ŧ????????????좎럥큔?????????????됰엨????????????????????????????????嚥싲갭큔???????????
        }
        
        // ????????좎럡?썹땟戮녹???좎럩?????좎?留????????????좎럥큔????????????(?????or 2???????
        if (risk_manager_->shouldExitPosition(pos.market)) {
            std::string reason = "unknown";
            
            // ???????? ????????
            if (current_price <= updated_pos->stop_loss) {
                reason = "stop_loss";
                LOG_INFO("???????????????????????黎앸럽?????沃섃넄???????(???????좎럥?℡뜝???좎룞??????{:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            } else if (current_price >= updated_pos->take_profit_2) {
                reason = "take_profit";
                LOG_INFO("2????????????좎럩堉?????????????좎럡???좎럡??? ??????????????????黎앸럽?????沃섃넄???????(???????좎럥?℡뜝???좎룞??????{:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            } else {
                reason = "strategy_exit"; // ????????좎럡?썹땟戮녹???좎럩?????좎?留?????????????????????⑥ъ８???????????좎럡萸?????(TS ??
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
                    LOG_WARN("????????좎럥?????????????꾩룆梨???耀붾굝????????⑤챶裕?????????좎럩猶욕뜝????????????????좎럩沅????{:.0f} < {:.0f}", order_amount, config_.min_order_krw);
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
                        LOG_WARN("{} ????????좎럥???????????좎럩堉?????????????좎럩堉???????좎럥??????鶯ㅺ동????좎룞????? ????????????????????????좎럩沅????({:.0f} < {:.0f})",
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
    LOG_INFO("??????熬곣뫖利닷뜝??좎룞?쇿뜝???????좎럩堉???????????븍툖?????????????: {} (????: {})", market, reason);
    
    //double current_price = getCurrentPrice(market);
    if (current_price <= 0) {
        LOG_ERROR("??????熬곣뫖利닷뜝??좎룞?쇿뜝??????????좎럩紐??????????좎듅? ???????????????????????좎럥痢?? {}", market);
        return false;
    }
    
    // ????????좎럡?썹땟戮녹???좎럩?????좎?留????????좎럥큔?????????????됰엨??????
    double sell_quantity = std::floor(position.quantity * 0.9999 * 100000000.0) / 100000000.0;
    double invest_amount = sell_quantity * current_price;
    const auto dynamic_sell_slippage = computeDynamicSlippageThresholds(
        config_,
        market_hostility_ewma_,
        false,
        position.market_regime,
        position.signal_strength,
        position.liquidity_score,
        position.expected_value,
        reason
    );
    
    // 1. ??????좎럥큔??????????????????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝??????????⑤즾????????????????좎럥큔????????????(??????좎럥큔????????????????????좎럥큔??????????? 5,000 KRW)
    if (invest_amount < config_.min_order_krw) {
        LOG_WARN("?????좎럩堉???????????븍툖????????????좎럩猶욕뜝????????????????좎럩沅????{:.0f} < {:.0f} (?????좎럩堉??좎뜾異??????????????좎럩堉??????????", invest_amount, config_.min_order_krw);
        return false;
    }
    
    // 2. ??????????????????좎럡萸???????std::to_string ?????????? ?????????stringstream ????(??????좎럥큔???????????????????좎럥??????????좎럡???????
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << sell_quantity;
        std::string quantity_str = ss.str();

    // 2-1. ?????????좎럥큔???????????????????좎럥큔?????????????????좎럥큔?????????????됰엨????????? ????????????(??????좎럥큔?????????? ??????좎럥큔?????????????됰엨?????????????????????
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

        if (slippage_pct > dynamic_sell_slippage.max_slippage_pct) {
            LOG_WARN("{} ?????? ????? {:.3f}% > {:.3f}% (?????????좎럩堉???????좎럥??????鶯ㅺ동????좎룞?????",
                     market, slippage_pct * 100.0, dynamic_sell_slippage.max_slippage_pct * 100.0);
            return false;
        }
        LOG_INFO("?????좎럩堉???????????븍툖???????? ?????좎럩堉??????????? {} (??????熬곣뫖利닷뜝??좎룞?쇿뜝??????????좎럩紐??????????좎듅?: {})", sell_price, current_price);
    } catch (const std::exception& e) {
        LOG_WARN("??? ???????????????????????좎럥痢??(??????熬곣뫖利닷뜝??좎룞?쇿뜝??????????좎럩紐??????????좎듅? ????: {}", e.what());
        sell_price = current_price;
    }

    // 2. ??????????????????좎뜴泥????????????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝?????????? (??????좎럥큔?????????? ????????? ??????熬곣뫖利닷뜝??좎룞?쇿뜝????좎룞????????
    double executed_price = current_price;
    std::string execution_order_id;
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
                LOG_ERROR("?????좎럩堉???????????븍툖??????????좎럩堉???????????????????좎럥痢?? {}", order_result.error_message);
                return false;
            }

            executed_price = order_result.executed_price;
            execution_order_id = order_result.order_uuid;
            sell_quantity = order_result.executed_volume;  // [Phase 3] ??????????????????좎뜴泥???????????좎럥큔?????????????????熬곣뫖利닷뜝??좎룞?쇿뜝????좎룞????????
            
            // [Phase 3] ???????濚밸Ŧ????????????좎럥큔??????????????????????
            double fill_ratio = order_result.executed_volume / position.quantity;
            if (fill_ratio < 0.999) {
                LOG_WARN("????????좎럩沅?????????좎럩堉?????????????????좎룞?????: {:.8f}/{:.8f} ({:.1f}%)",
                         order_result.executed_volume, position.quantity, fill_ratio * 100.0);
            }
            
            LOG_INFO("?????좎럩堉???????????븍툖??????????좎럩堉?????????????좎럥???????????? ??? {:.0f} (?????{})",
                     executed_price, order_result.retry_count);
        }
    }
    
    // 3. ?????????좎럥裕?????????????????????
    if (execution_order_id.empty()) {
        execution_order_id = buildSyntheticLiveOrderId(market, "sell");
    }
    const bool real_live_sell = (config_.mode == TradingMode::LIVE && !config_.dry_run && config_.allow_live_orders);
    const std::string sell_source = real_live_sell ? "live" : "live_paper";
    recordLiveExecutionLifecycle(
        sell_source,
        "submitted",
        execution_order_id,
        market,
        OrderSide::SELL,
        OrderStatus::SUBMITTED,
        0.0,
        sell_quantity,
        executed_price,
        position.strategy_name,
        false
    );
    recordLiveExecutionLifecycle(
        sell_source,
        "filled",
        execution_order_id,
        market,
        OrderSide::SELL,
        OrderStatus::FILLED,
        sell_quantity,
        sell_quantity,
        executed_price,
        position.strategy_name,
        true
    );

    double realized_qty = sell_quantity;
    
    // 4. [Phase 3] ???????濚밸Ŧ????????????좎럥큔??????????vs ????????좎럡?썹땟戮녹???좎럩?????좎?留????????좎럥큔????????????????????????
    double fill_ratio = sell_quantity / position.quantity;
    const bool fully_closed = fill_ratio >= 0.999;
    if (fully_closed) {
        realized_qty = position.quantity;
        // ????????좎럡?썹땟戮녹???좎럩?????좎?留????????좎럥큔???????????????????????????????븍툖??????(?????????????좎럡?썹땟戮녹???좎럩?????좎?留??????
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
    
    // 5. [??????????????좎럥裕???????? StrategyManager???????????????좎럡?썹땟戮녹???좎럩?????좎?留?????????좎럥큔??????????????????????????????좎룞??????????ш끽維뽳쭩?뱀땡???좎뜴????& ?????????????????????????????
    if (fully_closed && strategy_manager_ && !position.strategy_name.empty()) {
        // Position ?????????????????????좎럩?쒎뜝??좎룞????좎럩?????????????좎럡?덂뜝?????좎뜫爰?????????strategy_name("Advanced Scalping" ??????????????????좎럡?썹땟戮녹???좎럩?????좎?留????????좎럥큔?????????????????????
        auto strategy = strategy_manager_->getStrategy(position.strategy_name);
        
        if (strategy) {
            // Strategy stats are aligned to the same fee-inclusive basis as trade_history.
            const double fee_rate = Config::getInstance().getFeeRate();
            const double exit_value = executed_price * position.quantity;
            const double entry_fee = position.invested_amount * fee_rate;
            const double exit_fee = exit_value * fee_rate;
            const double net_pnl = exit_value - position.invested_amount - entry_fee - exit_fee;
            strategy->updateStatistics(market, net_pnl > 0.0, net_pnl);
            LOG_INFO("Sell complete: strategy stats updated ({})", position.strategy_name);
        } else if (position.strategy_name != "RECOVERED") {
            // RECOVERED ?????? ?????遺얘턁?????????????癲됱빖???????? ???????????좎럩?????????좎럥?????????????????좎럥??????????
            LOG_WARN("Sell complete but strategy object missing: {}", position.strategy_name);
        }
    }
    
    LOG_INFO("?????????????繹먮굝?????????熬곣뫖利닷뜝??좎룞?쇿뜝?? {} (????? {:.0f} KRW)",
             market, (executed_price - position.entry_price) * realized_qty);
    
    return true;
}


bool TradingEngine::executePartialSell(const std::string& market, const risk::Position& position, double current_price) {

    //double current_price = getCurrentPrice(market);

        if (current_price <= 0) {
        LOG_ERROR("??????熬곣뫖利닷뜝??좎룞?쇿뜝??????????좎럩紐??????????좎듅? ???????????????????????좎럥痢?? {}", market);
        return false;
    }
    
    // Adaptive partial cadence: shared with backtest runtime.
    const double partial_ratio = std::clamp(
        risk_manager_->getAdaptivePartialExitRatio(market),
        0.35,
        0.80
    );
    double sell_quantity = std::floor(position.quantity * partial_ratio * 100000000.0) / 100000000.0;
    if (sell_quantity <= 0.0) {
        LOG_WARN("{} partial-sell skipped: computed quantity is zero (ratio {:.2f}, qty {:.8f})",
                 market, partial_ratio, position.quantity);
        return false;
    }
    double invest_amount = sell_quantity * current_price;
    const auto dynamic_partial_sell_slippage = computeDynamicSlippageThresholds(
        config_,
        market_hostility_ewma_,
        false,
        position.market_regime,
        position.signal_strength,
        position.liquidity_score,
        position.expected_value,
        "partial_take_profit"
    );
    // If partial amount cannot satisfy exchange minimum, avoid fake half-close.
    if (invest_amount < config_.min_order_krw) {
        const double full_notional = position.quantity * current_price;
        LOG_WARN("{} partial-sell unavailable: partial {:.0f} < min {:.0f} (full {:.0f})",
                 market, invest_amount, config_.min_order_krw, full_notional);

        if (full_notional >= config_.min_order_krw) {
            LOG_INFO("{} fallback: execute full take-profit exit because partial is below min order", market);
            return executeSellOrder(market, position, "take_profit_full_due_to_min_order", current_price);
        }

        // Both partial/full are below minimum. Keep position but arm protective state.
        if (risk_manager_) {
            risk_manager_->setHalfClosed(market, true);
            risk_manager_->moveStopToBreakeven(market);
        }
        nlohmann::json reduce_payload;
        reduce_payload["exit_price"] = current_price;
        reduce_payload["quantity"] = 0.0;
        reduce_payload["reason"] = "partial_take_profit_skipped_below_min_order";
        appendJournalEvent(
            core::JournalEventType::POSITION_REDUCED,
            market,
            "partial_sell_skipped",
            reduce_payload
        );
        return true;
    }
    
    LOG_INFO("{} partial take-profit triggered ({:.0f}%)", market, partial_ratio * 100.0);
    
    LOG_INFO("  ?????좎럩堉??????????: {:.0f}, ??????????좎뜴泥?????: {:.0f}, ????????좎럩沅?????????????????????熬곣뫖利닷뜝??좎룞?쇿뜝?? {:.8f}",
             position.entry_price, current_price, sell_quantity);
    
    // 2. ??????????????????좎럡萸???????std::to_string ?????????? ?????????stringstream ????(??????좎럥큔???????????????????좎럥??????????좎럡???????
    std::stringstream ss;
    ss << std::fixed << std::setprecision(8) << sell_quantity;
    std::string quantity_str = ss.str();

    // 2-1. ?????????좎럥큔???????????????????좎럥큔?????????????????좎럥큔?????????????됰엨????????? ????????????(??????좎럥큔?????????? ??????좎럥큔?????????????됰엨?????????????????????
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

        if (slippage_pct > dynamic_partial_sell_slippage.max_slippage_pct) {
            LOG_WARN("{} ?????? ????? {:.3f}% > {:.3f}% (????????좎럩沅?????????????좎럩堉???????좎럥??????鶯ㅺ동????좎룞?????",
                     market, slippage_pct * 100.0, dynamic_partial_sell_slippage.max_slippage_pct * 100.0);
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
    auto record_partial_execution = [&](double fill_price, double fill_qty, const std::string& source, const std::string& order_id_hint) {
        std::string order_id = order_id_hint;
        if (order_id.empty()) {
            order_id = buildSyntheticLiveOrderId(market, "partial_sell");
        }
        recordLiveExecutionLifecycle(
            source,
            "submitted",
            order_id,
            market,
            OrderSide::SELL,
            OrderStatus::SUBMITTED,
            0.0,
            fill_qty,
            fill_price,
            position.strategy_name,
            false
        );
        recordLiveExecutionLifecycle(
            source,
            "filled",
            order_id,
            market,
            OrderSide::SELL,
            OrderStatus::FILLED,
            fill_qty,
            fill_qty,
            fill_price,
            position.strategy_name,
            true
        );
    };

    // 2. ??????????????????좎뜴泥????????????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝?????????? (??????좎럥큔?????????? ????????? ??????熬곣뫖利닷뜝??좎룞?쇿뜝????좎룞????????
    if (config_.mode == TradingMode::LIVE) {
        if (config_.dry_run || !config_.allow_live_orders) {
            if (!config_.dry_run && !config_.allow_live_orders) {
                LOG_WARN("{} live partial-sell blocked: trading.allow_live_orders=false, using simulation", market);
            }
            LOG_WARN("DRY RUN: partial sell simulated");
            if (!apply_partial_fill(current_price, sell_quantity, "partial_take_profit_dry_run")) {
                return false;
            }
            record_partial_execution(current_price, sell_quantity, "live_paper", "");
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
            LOG_ERROR("????????좎럩沅?????????좎럩堉???????????븍툖??????????좎럩堉???????????????????좎럥痢?? {}", order_result.error_message);
            return false;
        }

        LOG_INFO("????????좎럩沅?????????좎럩堉???????????븍툖??????????좎럩堉?????????????좎럥???????????? ??? {:.0f} (?????{})",
                 order_result.executed_price, order_result.retry_count);
        if (!apply_partial_fill(order_result.executed_price, order_result.executed_volume, "partial_take_profit")) {
            return false;
        }
        record_partial_execution(order_result.executed_price, order_result.executed_volume, "live", order_result.order_uuid);
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
    record_partial_execution(current_price, sell_quantity, "live_paper", "");
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

// ===== ???????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝??????=====

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
        return common::roundUpToTickSize(ask);  // [Phase 3] ??? ???????????좎럡萸??????
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
        return common::roundDownToTickSize(bid);  // [Phase 3] ??? ???????????좎럡萸???????
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
        LOG_WARN("???????꾩룆梨???耀붾굝????????⑤챶裕???????좎럩堉?????????????좎럥??????????????????????좎럥痢?? {}", e.what());
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
        // [Phase 3] ??? ???????????좎럡萸??????+ ??? ??????????????????좎럡萸?????
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
        LOG_INFO("?????좎럩堉???????????????꾩룆梨???耀붾굝????????⑤챶裕????????熬곣뫖利닷뜝??좎룞?쇿뜝????좎룞????????좎럩履??(UUID: {}, ????????좎룞??????{:.0f}, ?????? {})", uuid, entry_price, vol_str);

        // 10???????????癲ル슪???용뙋????????좎럥큔????????????????됰Ŧ????????????(500ms * 20)
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

        // ????????좎럩??щ엠?????汝뷴뜝??좎룞????좎뜦維뽩뜝??????룸㎗?ルき???????????????? ???????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝???????????????????
        try {
            http_client_->cancelOrder(uuid);
            LOG_WARN("?????좎럩堉???????????????좎럩肉?????β뼯援??좎뜽琉?????룸ħ瑗???????10?? -> ???????꾩룆梨???耀붾굝????????⑤챶裕???????????????????");
        } catch (const std::exception& e) {
            LOG_WARN("?????좎럩堉???????????????꾩룆梨???耀붾굝????????⑤챶裕??????????????????좎럥痢?? {}", e.what());
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
            LOG_WARN("??????????????????????좎룞?????좎럩苑???????? ???????????????????????좎럥痢?? {}", e.what());
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
        // [Phase 3] ??? ???????????좎럡萸???????+ ??? ??????????????????좎럡萸?????
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
        LOG_INFO("?????좎럩堉???????????븍툖????????????꾩룆梨???耀붾굝????????⑤챶裕????????熬곣뫖利닷뜝??좎룞?쇿뜝????좎룞????????좎럩履??(UUID: {}, ????????좎룞??????{:.0f}, ?????? {})", uuid, exit_price, vol_str);

        // 10???????????癲ル슪???용뙋????????좎럥큔????????????????됰Ŧ????????????(500ms * 20)
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

        // ????????좎럩??щ엠?????汝뷴뜝??좎룞????좎뜦維뽩뜝??????룸㎗?ルき???????????????? ???????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝???????????????????
        try {
            http_client_->cancelOrder(uuid);
            LOG_WARN("?????좎럩堉???????????븍툖????????????좎럩肉?????β뼯援??좎뜽琉?????룸ħ瑗???????10?? -> ???????꾩룆梨???耀붾굝????????⑤챶裕???????????????????");
        } catch (const std::exception& e) {
            LOG_WARN("?????좎럩堉???????????븍툖????????????꾩룆梨???耀붾굝????????⑤챶裕??????????????????좎럥痢?? {}", e.what());
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
            LOG_WARN("??????????????????????좎룞?????좎럩苑???????? ???????????????????????좎럥痢?? {}", e.what());
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


// ===== ??????좎럥큔????????????饔낅떽???????????????????좎룞??????????ш끽維뽳쭩?뱀땡???좎뜴????=====

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

// ===== ??????嶺뚮씚維?????????⑥ル럵????濚밸Ŧ援앭뜝????????=====

void TradingEngine::manualScan() {
    LOG_INFO("?????좎럥留⒴뜝???????좎럥??????좎럥????????饔낅떽??????????????");
    scanMarkets();
    generateSignals();
}

// void TradingEngine::manualClosePosition(const std::string& market) {
//     LOG_INFO("??????嶺뚮씚維?????????⑥ル럵????濚밸Ŧ援앭뜝?????? {}", market);
    
//     auto* pos = risk_manager_->getPosition(market);
//     if (!pos) {
//         LOG_WARN("??????????????????? {}", market);
//         return;
//     }
    
//     executeSellOrder(market, *pos, "manual", current_price);
// }

// void TradingEngine::manualCloseAll() {
//     LOG_INFO("????????좎럡?썹땟戮녹???좎럩?????좎?留?????????????嶺뚮씚維?????????⑥ル럵????濚밸Ŧ援앭뜝??????);
    
//     auto positions = risk_manager_->getAllPositions();
//     for (const auto& pos : positions) {
//         executeSellOrder(pos.market, pos, "manual_all", current_price);
//     }
// }

// ===== ?????????(?????????좎럥裕???????? =====

double TradingEngine::getCurrentPrice(const std::string& market) {
    try {
        auto ticker = http_client_->getTicker(market);
        if (ticker.empty()) {
            return 0;
        }
        
        // 2. nlohmann/json ????????????????????????좎뜴泥???????????????????좎럡萸?????
        if (ticker.is_array() && !ticker.empty()) {
            return ticker[0].value("trade_price", 0.0);
        }

        if (ticker.contains("trade_price") && !ticker["trade_price"].is_null()) {
            return ticker.value("trade_price", 0.0);
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        LOG_ERROR("??????熬곣뫖利닷뜝??좎룞?쇿뜝??????????좎럩紐??????????좎듅? ???????????????????????좎럥痢?? {} - {}", market, e.what());
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
    LOG_INFO("Cumulative buy/sell orders: {} / {}",
             prometheus_metrics_.total_buy_orders,
             prometheus_metrics_.total_sell_orders);

    auto prom_metrics = exportPrometheusMetrics();
    LOG_INFO("Prometheus metrics preview: {}", prom_metrics.substr(0, 200) + "...");
    LOG_INFO("========================================");
}
void TradingEngine::syncAccountState() {
    LOG_INFO("???????????????????????????????????????遺얘턁????????좎룞???????..");

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
            
            // 1. [?????????좎럥裕???????? KRW(???????????? ??????좎럥큔?????????????饔낅떽??????좎럥梨????????????????븍툖????????????????좎룞彛???
            if (currency == "KRW") {
                double total_cash = balance + locked; // ???????????+ ????????좎럩??щ엠?????汝뷴뜝??좎룞????좎뜦維뽩뜝??????룸㎗?ルき?????????????耀붾굝???????
                
                // RiskManager??????????????좎럥裕??????????????????????????좎뜴泥??????????????????????????????????좎럥??????
                risk_manager_->resetCapital(total_cash);
                // (2) [????????????좎룞彛???] ????????????????????????????????????'????????좎뜫爰???????????????????????????????좎뜴泥??????????????????????좎럡萸?????
                config_.initial_capital = total_cash;
                krw_found = true;
                LOG_INFO("???????????????????????????? {:.0f} KRW (????????좎룞??????{:.0f})", total_cash, balance);
                continue; // ??????????????????좎럥큔?????????????饔낅떽??????좎럥梨?????????????좎럩堉???????????????????좎룞彛??????????????????????????
            }

            // 2. ?????????좎럡萸????? ???????ш끽維뽳쭩?뱀땡???좎뜴???????????좎럥큔?????????????饔낅떽??????좎럥梨?????(???????????????????븍툖?????????)
            // ??????좎럥큔????????????????ш끽維뽳쭩?뱀땡???좎뜴?????????????꾩룆梨띰쭕?뚢뵾???????(?? BTC -> KRW-BTC)
            std::string market = "KRW-" + currency;
            wallet_markets.insert(market);
            
            double avg_buy_price = std::stod(acc["avg_buy_price"].get<std::string>());
            
            // ??????좎럥큔????????遺얘턁??????????????Dust) ?????遺얘턁?????????(??????좎럥큔????????????????????좎럥큔??????????????????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝?????????⑤즾??????????????????ш끽維뽳쭩?뱀땡???좎뜴????
            if (balance * avg_buy_price < 5000) continue;

            // ???? RiskManager?????????????????汝뷴뜝??좎룞??????좎럥?????????????곌퇈猷????????????
            if (risk_manager_->getPosition(market) != nullptr) continue;

            LOG_INFO("???????????????좎럥踰▼뜝???? ???????좎럩猷놅㎖?곗춹??????????꾩룆梨띰쭕?뚢뵾?????????좎럥?뚳┼??뵰?????? {} (?????? {:.8f}, ???????: {:.0f})", 
                     market, balance, avg_buy_price);

            // [Phase 4] ???????????????????????좎룞?쇿뜝??????좎뜾?????????????좎럩堉???????????SL/TP ?????????좎럡萸???耀붾굝???????????????좎럩堉????????
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
                // [Phase 4] ?????????좎럡?덂뜝?????좎뜫爰??????????????????????좎럡?썹땟戮녹???좎럩?????좎?留????????(?????)
                safe_stop_loss = persisted->stop_loss;
                tp1 = persisted->take_profit_1;
                tp2 = persisted->take_profit_2;
                be_trigger = persisted->breakeven_trigger;
                trail_start = persisted->trailing_start;
                half_closed = persisted->half_closed;
                LOG_INFO("????????????좎럥踰▼뜝???좎뜫爰???????????좎뜾??????????좎럩堉????????: {} SL={:.0f} TP1={:.0f} TP2={:.0f} BE={:.0f} TS={:.0f}",
                         market, safe_stop_loss, tp1, tp2, be_trigger, trail_start);
            } else {
                // ????????????????????????????????????????????????(??????좎럥큔??????????????????? ???????????嶺뚮씚維?????????⑥ル럵????濚밸Ŧ援앭뜝????????좎럥큔?????????
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
                LOG_WARN("????????????좎럥踰▼뜝???좎뜫爰???????????좎뜾???????????????: {} SL={:.0f} TP1={:.0f} TP2={:.0f} (?????좎럩堉???????????????????????????좎럥泥??",
                         market, safe_stop_loss, tp1, tp2);
            }

            std::string recovered_strategy = "RECOVERED";
            auto recovered_it = recovered_strategy_map_.find(market);
            if (recovered_it != recovered_strategy_map_.end()) {
                recovered_strategy = recovered_it->second;
            }

            // ??????????????좎럡萸???耀붾굝?????????????獄쏅챶留????
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

            if (persisted) {
                risk_manager_->setPositionSignalInfo(
                    market,
                    persisted->signal_filter,
                    persisted->signal_strength,
                    persisted->market_regime,
                    persisted->liquidity_score,
                    persisted->volatility,
                    persisted->expected_value,
                    persisted->reward_risk_ratio,
                    persisted->entry_archetype,
                    persisted->probabilistic_runtime_applied,
                    persisted->probabilistic_h1_calibrated,
                    persisted->probabilistic_h5_calibrated,
                    persisted->probabilistic_h5_threshold,
                    persisted->probabilistic_h5_margin
                );
            }

            // [Phase 4] ???????濚밸Ŧ??????????????????????????????좎럡萸???耀붾굝????????
            if (half_closed) {
                auto* pos = risk_manager_->getPosition(market);
                if (pos) {
                    pos->half_closed = true;
                    LOG_INFO("  ????????좎럩沅???????????????????????????????????좎럥踰▼뜝???좎뜫爰?????? {} (half_closed=true)", market);
                }
            }
        }
        
        if (!krw_found) {
            LOG_WARN("???????????????KRW????????좎룞???? ??????嚥싲갭큔?????????????좎럥梨룟뜝? (??????癲ル슢??????????0????????????????좎럩苡?????");
            risk_manager_->resetCapital(0.0);
        }

        // ===== ?????????????????됰Ŧ??????????(??????좎럥큔????????????????) =====
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
                    LOG_WARN("???????좎럩肉????????븍툖??????????????좎럥??????????????????????????????????????????좎럥痢?? {}", e.what());
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
                trade.entry_archetype = pos.entry_archetype;
                trade.probabilistic_runtime_applied = pos.probabilistic_runtime_applied;
                trade.probabilistic_h1_calibrated = pos.probabilistic_h1_calibrated;
                trade.probabilistic_h5_calibrated = pos.probabilistic_h5_calibrated;
                trade.probabilistic_h5_threshold = pos.probabilistic_h5_threshold;
                trade.probabilistic_h5_margin = pos.probabilistic_h5_margin;
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

                LOG_INFO("???????좎럩肉????????븍툖??????????????좎럥???????????? {} (??????熬곣뫖利닷뜝??좎룞?쇿뜝?? {}, ????? {:.0f})",
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

        // ??????좎럥큔????????????????????????????????????????遺얘턁?????????????좎럥큔???????????????????????????
        for (auto it = recovered_strategy_map_.begin(); it != recovered_strategy_map_.end(); ) {
            if (wallet_markets.find(it->first) == wallet_markets.end()) {
                it = recovered_strategy_map_.erase(it);
            } else {
                ++it;
            }
        }
        
        LOG_INFO("Account state synchronization completed");

    } catch (const std::exception& e) {
        LOG_ERROR("?????????????????????????????????????????????좎럥痢?? {}", e.what());
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
            "Learning state loaded (schema v{}, saved_at={}, hostility_ewma={:.3f}, pause_remaining={})",
            loaded->schema_version,
            loaded->saved_at_ms,
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
            item["entry_archetype"] = trade.entry_archetype;
            item["probabilistic_runtime_applied"] = trade.probabilistic_runtime_applied;
            item["probabilistic_h1_calibrated"] = trade.probabilistic_h1_calibrated;
            item["probabilistic_h5_calibrated"] = trade.probabilistic_h5_calibrated;
            item["probabilistic_h5_threshold"] = trade.probabilistic_h5_threshold;
            item["probabilistic_h5_margin"] = trade.probabilistic_h5_margin;
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
            p["entry_archetype"] = pos.entry_archetype;
            p["probabilistic_runtime_applied"] = pos.probabilistic_runtime_applied;
            p["probabilistic_h1_calibrated"] = pos.probabilistic_h1_calibrated;
            p["probabilistic_h5_calibrated"] = pos.probabilistic_h5_calibrated;
            p["probabilistic_h5_threshold"] = pos.probabilistic_h5_threshold;
            p["probabilistic_h5_margin"] = pos.probabilistic_h5_margin;
            // [Phase 4] ????????????????됰Ŧ???????????????????????????????
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
        auto fallback_state_path = utils::PathUtils::resolveRelativePath("state/state.json");

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
        write_json(fallback_state_path);

        LOG_INFO(
            "State snapshot saved: snapshot={}, fallback={}, last_event_seq={}",
            snapshot_path.string(),
            fallback_state_path.string(),
            state.value("snapshot_last_event_seq", 0)
        );

        saveLearningState();
    } catch (const std::exception& e) {
        LOG_ERROR("?????????????????????????좎럥痢?? {}", e.what());
    }
}

void TradingEngine::loadState() {
    try {
        pending_signal_metadata_by_market_.clear();
        auto snapshot_path = utils::PathUtils::resolveRelativePath("state/snapshot_state.json");
        auto fallback_state_path = utils::PathUtils::resolveRelativePath("state/state.json");
        std::filesystem::path state_path;

        if (std::filesystem::exists(snapshot_path)) {
            state_path = snapshot_path;
        } else if (std::filesystem::exists(fallback_state_path)) {
            state_path = fallback_state_path;
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
                trade.entry_archetype = item.value("entry_archetype", std::string("UNSPECIFIED"));
                trade.probabilistic_runtime_applied = item.value("probabilistic_runtime_applied", false);
                trade.probabilistic_h1_calibrated = item.value("probabilistic_h1_calibrated", 0.5);
                trade.probabilistic_h5_calibrated = item.value("probabilistic_h5_calibrated", 0.5);
                trade.probabilistic_h5_threshold = item.value("probabilistic_h5_threshold", 0.6);
                trade.probabilistic_h5_margin = item.value("probabilistic_h5_margin", 0.0);
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
                pos.entry_archetype = p.value("entry_archetype", std::string("UNSPECIFIED"));
                pos.probabilistic_runtime_applied = p.value("probabilistic_runtime_applied", false);
                pos.probabilistic_h1_calibrated = p.value("probabilistic_h1_calibrated", 0.5);
                pos.probabilistic_h5_calibrated = p.value("probabilistic_h5_calibrated", 0.5);
                pos.probabilistic_h5_threshold = p.value("probabilistic_h5_threshold", 0.6);
                pos.probabilistic_h5_margin = p.value("probabilistic_h5_margin", 0.0);
                // [Phase 4] ????????????????됰Ŧ??????????????????????????좎럡萸???耀붾굝????????
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

            auto applyPersistedSignalMetadata = [&](PersistedPosition& pos, const nlohmann::json& meta) {
                if (meta.contains("signal_filter")) pos.signal_filter = meta.value("signal_filter", pos.signal_filter);
                if (meta.contains("signal_strength")) pos.signal_strength = meta.value("signal_strength", pos.signal_strength);
                if (meta.contains("market_regime")) {
                    pos.market_regime = static_cast<analytics::MarketRegime>(
                        meta.value("market_regime", static_cast<int>(pos.market_regime))
                    );
                }
                if (meta.contains("liquidity_score")) pos.liquidity_score = meta.value("liquidity_score", pos.liquidity_score);
                if (meta.contains("volatility")) pos.volatility = meta.value("volatility", pos.volatility);
                if (meta.contains("expected_value")) pos.expected_value = meta.value("expected_value", pos.expected_value);
                if (meta.contains("reward_risk_ratio")) pos.reward_risk_ratio = meta.value("reward_risk_ratio", pos.reward_risk_ratio);
                if (meta.contains("entry_archetype")) pos.entry_archetype = meta.value("entry_archetype", pos.entry_archetype);
                if (meta.contains("probabilistic_runtime_applied")) {
                    pos.probabilistic_runtime_applied = meta.value(
                        "probabilistic_runtime_applied",
                        pos.probabilistic_runtime_applied
                    );
                }
                if (meta.contains("probabilistic_h1_calibrated")) {
                    pos.probabilistic_h1_calibrated = meta.value(
                        "probabilistic_h1_calibrated",
                        pos.probabilistic_h1_calibrated
                    );
                }
                if (meta.contains("probabilistic_h5_calibrated")) {
                    pos.probabilistic_h5_calibrated = meta.value(
                        "probabilistic_h5_calibrated",
                        pos.probabilistic_h5_calibrated
                    );
                }
                if (meta.contains("probabilistic_h5_threshold")) {
                    pos.probabilistic_h5_threshold = meta.value(
                        "probabilistic_h5_threshold",
                        pos.probabilistic_h5_threshold
                    );
                }
                if (meta.contains("probabilistic_h5_margin")) {
                    pos.probabilistic_h5_margin = meta.value(
                        "probabilistic_h5_margin",
                        pos.probabilistic_h5_margin
                    );
                }
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
                        applyPersistedSignalMetadata(pos, payload);
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
                        applyPersistedSignalMetadata(pos, payload);
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
                                trade.entry_archetype = it->second.entry_archetype;
                                trade.probabilistic_runtime_applied = it->second.probabilistic_runtime_applied;
                                trade.probabilistic_h1_calibrated = it->second.probabilistic_h1_calibrated;
                                trade.probabilistic_h5_calibrated = it->second.probabilistic_h5_calibrated;
                                trade.probabilistic_h5_threshold = it->second.probabilistic_h5_threshold;
                                trade.probabilistic_h5_margin = it->second.probabilistic_h5_margin;
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
                                trade.entry_archetype = it->second.entry_archetype;
                                trade.probabilistic_runtime_applied = it->second.probabilistic_runtime_applied;
                                trade.probabilistic_h1_calibrated = it->second.probabilistic_h1_calibrated;
                                trade.probabilistic_h5_calibrated = it->second.probabilistic_h5_calibrated;
                                trade.probabilistic_h5_threshold = it->second.probabilistic_h5_threshold;
                                trade.probabilistic_h5_margin = it->second.probabilistic_h5_margin;
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
        LOG_ERROR("??????????????????좎럥踰▼뜝???좎뜫爰???????????좎뜾???????????????좎럥痢?? {}", e.what());
    }
}

// ===== [NEW] ???????????좎룞??????좎뜫爰?????獄쎼끏???????????????????????????좎럥荑??????????????????(?????????좎럡萸???????????????????????????? =====

std::string TradingEngine::exportPrometheusMetrics() const {
    // Prometheus ??????됰Ŧ????????????????좎럥利?????????좎럥큔????????????饔낅떽????????????????????????꾩룆梨띰쭕?뚢뵾???????
    // Grafana?? ???????????좎럩異?????????좎럩堉???????????????????좎럡?썲뜝??????????좎럥큔??????????????좎럥큔??????????????⑤벡??????????좎럥큔?????????
    
    auto metrics = risk_manager_->getRiskMetrics();
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    std::ostringstream oss;
    
    // ??????좎럥큔?????????????????(??? ???????饔낅떽???????????????????좎룞彛???)
    oss << "# HELP autolife_state AutoLife ??????좎럥큔??????????????????????????????????????됰Ŧ???????????n";
    oss << "# TYPE autolife_state gauge\n";

    // ???????????????좎럥諭???????????饔낅떽?????????
    oss << "# HELP autolife_capital_total ???????(KRW)\n";
    oss << "# TYPE autolife_capital_total gauge\n";
    oss << "# HELP autolife_capital_available ?????????????????????????좎럡?썽뇡癒?똿????????????????????????????????? KRW)\n";
    oss << "# TYPE autolife_capital_available gauge\n";
    oss << "# HELP autolife_capital_invested ??????????????좎럥??????????좎럡???????????????⑤즾????????????????????좎럩堉??????????????????????? KRW)\n";
    oss << "# TYPE autolife_capital_invested gauge\n";

    // ???????????????좎럥諭???????????饔낅떽?????????
    oss << "# HELP autolife_pnl_realized ????????????????????좎럡?썹땟戮녹???좎럩?????좎?留?? KRW)\n";
    oss << "# TYPE autolife_pnl_realized gauge\n";
    oss << "# HELP autolife_pnl_unrealized ????????좎럩??щ엠?????汝뷴뜝??좎룞????좎뜦維뽩뜝?????????????????좎럡?썹땟戮녹???좎럩?????좎?留??????????????, KRW)\n";
    oss << "# TYPE autolife_pnl_unrealized gauge\n";
    oss << "# HELP autolife_pnl_total ??????????????????????좎럩??щ엠?????汝뷴뜝??좎룞????좎뜦維뽩뜝???? KRW)\n";
    oss << "# TYPE autolife_pnl_total gauge\n";
    oss << "# HELP autolife_pnl_total_pct ????????좎럡?썹땟戮녹???좎럩?????좎?留????????????????????좎럥裕?????????%)\n";
    oss << "# TYPE autolife_pnl_total_pct gauge\n";

    // ?????????좎럥????????μ떜媛?걫??좎럩???????????????좎럥諭???????????饔낅떽?????????
    oss << "# HELP autolife_drawdown_max ??????좎럥큔?????????? ????????좎럡?썹땟戮녹???좎럩?????좎?留?????????????????????\n";
    oss << "# TYPE autolife_drawdown_max gauge\n";
    oss << "# HELP autolife_drawdown_current ????????좎럡?썹땟戮녹???좎럩?????좎?留?????????????????????\n";
    oss << "# TYPE autolife_drawdown_current gauge\n";

    // ???????????????좎럥諭???????????饔낅떽?????????
    oss << "# HELP autolife_positions_active ????????좎럡?썹땟戮녹???좎럩?????좎?留???????????좎럡萸????? ???????n";
    oss << "# TYPE autolife_positions_active gauge\n";
    oss << "# HELP autolife_positions_max ???????????????????좎?猷뤷뜝????????좎럥큔?????????? ???????n";
    oss << "# TYPE autolife_positions_max gauge\n";

    // ??????좎럥큔?????????????????????????饔낅떽?????????
    oss << "# HELP autolife_trades_total ????????좎럡?썹땟戮녹???좎럩?????좎?留????????좎럥큔???????????????n";
    oss << "# TYPE autolife_trades_total counter\n";
    oss << "# HELP autolife_trades_winning ????????좎럡?썹땟戮녹???좎럩?????좎?留???????????좎럥裕??????????????좎럥큔???????????????n";
    oss << "# TYPE autolife_trades_winning counter\n";
    oss << "# HELP autolife_trades_losing ????????좎럡?썹땟戮녹???좎럩?????좎?留?????????????좎럥큔???????????????n";
    oss << "# TYPE autolife_trades_losing counter\n";

    // ??????????????????좎럥痢?????좎룞??????????좎럥큔????????????????饔낅떽?????????
    oss << "# HELP autolife_winrate ????????좎럩猷놅㎖?곗춹??좎뜦逾???????????0~1)\n";
    oss << "# TYPE autolife_winrate gauge\n";
    oss << "# HELP autolife_profit_factor ?????????좎럥裕???????????????Profit Factor)\n";
    oss << "# TYPE autolife_profit_factor gauge\n";
    oss << "# HELP autolife_sharpe_ratio ?????????????좎럥큔???????????????????????????좎럥痢?????좎룞??????????좎럥큔??????????\n";
    oss << "# TYPE autolife_sharpe_ratio gauge\n";

    // ??????????????????????????饔낅떽?????????
    oss << "# HELP autolife_engine_running ???????????????? ???????????1=??????????0=???)\n";
    oss << "# TYPE autolife_engine_running gauge\n";
    oss << "# HELP autolife_engine_scans_total ???????????????遺얘턁???????????????????좎뜫?띶뜝?n";
    oss << "# TYPE autolife_engine_scans_total counter\n";
    oss << "# HELP autolife_engine_signals_total ????????꾩룆梨띰쭕?뚢뵾???????????????????⑥ъ８??????????좎럥?????????n";
    oss << "# TYPE autolife_engine_signals_total counter\n";

    // ???????????좎룞??????좎뜫爰?????獄쎼끏?????????????????????????????饔낅떽?????????

    // ??????????????좎럥큔???????????????????좎럥큔????????????饔낅떽??????????????饔낅떽?????????
    oss << "# HELP autolife_buy_orders_total ????????좎럡?썹땟戮녹???좎럩?????좎?留????????좎럥큔????????????????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝????n";
    oss << "# TYPE autolife_buy_orders_total counter\n";
    oss << "# HELP autolife_sell_orders_total ????????좎럡?썹땟戮녹???좎럩?????좎?留????????좎럥큔?????????????됰엨?????????????熬곣뫖利닷뜝?????좎럥????????????좎럩援▼뜝????n";
    oss << "# TYPE autolife_sell_orders_total counter\n";
    oss << "# HELP autolife_pnl_cumulative ????????좎럡?썹땟戮녹???좎럩?????좎?留???????????????????????????嚥싲갭큔??????????????좎럥梨룟뜝????? KRW)\n";
    oss << "# TYPE autolife_pnl_cumulative gauge\n";
    
    // 1. ???????????????좎럥諭??????????좎럥큔????????????饔낅떽???????
    oss << "autolife_capital_total{mode=\"" 
        << (config_.mode == TradingMode::LIVE ? "LIVE" : "PAPER") << "\"} "
        << metrics.total_capital << " " << timestamp_ms << "\n";
    
    oss << "autolife_capital_available{} " << metrics.available_capital << " " << timestamp_ms << "\n";
    oss << "autolife_capital_invested{} " << metrics.invested_capital << " " << timestamp_ms << "\n";
    
    // 2. ???????????????좎럥諭??????????좎럥큔????????????饔낅떽???????
    oss << "autolife_pnl_realized{} " << metrics.realized_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_unrealized{} " << metrics.unrealized_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_total{} " << metrics.total_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_total_pct{} " << metrics.total_pnl_pct << " " << timestamp_ms << "\n";
    
    // 3. ?????????좎럥????????μ떜媛?걫??좎럩???????????????좎럥諭??????????좎럥큔????????????饔낅떽???????
    oss << "autolife_drawdown_max{} " << metrics.max_drawdown << " " << timestamp_ms << "\n";
    oss << "autolife_drawdown_current{} " << metrics.current_drawdown << " " << timestamp_ms << "\n";
    
    // 4. ???????????????좎럥諭??????????좎럥큔????????????饔낅떽???????
    oss << "autolife_positions_active{} " << metrics.active_positions << " " << timestamp_ms << "\n";
    oss << "autolife_positions_max{} " << config_.max_positions << " " << timestamp_ms << "\n";
    
    // 5. ??????좎럥큔??????????????????
    oss << "autolife_trades_total{} " << metrics.total_trades << " " << timestamp_ms << "\n";
    oss << "autolife_trades_winning{} " << metrics.winning_trades << " " << timestamp_ms << "\n";
    oss << "autolife_trades_losing{} " << metrics.losing_trades << " " << timestamp_ms << "\n";
    
    // 6. ??????좎럥큔???????????????????????????????좎럥痢?????좎룞??????????좎럥큔?????????
    oss << "autolife_winrate{} " << metrics.win_rate << " " << timestamp_ms << "\n";
    oss << "autolife_profit_factor{} " << metrics.profit_factor << " " << timestamp_ms << "\n";
    oss << "autolife_sharpe_ratio{} " << metrics.sharpe_ratio << " " << timestamp_ms << "\n";
    
    // 7. ?????????????????????????좎럥큔????????????饔낅떽???????
    oss << "autolife_engine_running{} " << (running_ ? 1 : 0) << " " << timestamp_ms << "\n";
    oss << "autolife_engine_scans_total{} " << total_scans_ << " " << timestamp_ms << "\n";
    oss << "autolife_engine_signals_total{} " << total_signals_ << " " << timestamp_ms << "\n";
    
    // 8. [NEW] ???????????좎룞??????좎뜫爰?????獄쎼끏??????????????????????????? ??????좎럥큔????????????饔낅떽???????
    
    // 9. [NEW] ??????좎럥큔???????????????????????????좎럥큔????????????饔낅떽???????
    oss << "autolife_buy_orders_total{} " << prometheus_metrics_.total_buy_orders << " " << timestamp_ms << "\n";
    oss << "autolife_sell_orders_total{} " << prometheus_metrics_.total_sell_orders << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_cumulative{} " << prometheus_metrics_.cumulative_realized_pnl << " " << timestamp_ms << "\n";
    
    oss << "# End of AutoLife Metrics\n";
    
    return oss.str();
}

// [NEW] Prometheus HTTP ???????좎럩堉????????????????????꿔꺂???癰귙뢿沅?
void TradingEngine::runPrometheusHttpServer(int port) {
    prometheus_server_port_ = port;
    prometheus_server_running_ = true;
    
    LOG_INFO("Prometheus HTTP ????遺얘턁??????????????遺얘턁????????좎룞???????(???? {})", port);
    
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
    
    // ?????????????????????????(TIME_WAIT ?????????????????????????????????????
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
        LOG_ERROR("bind ??????????좎럥痢??(???? {})", port);
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
    
    LOG_INFO("Prometheus ?????좎럩堉???????????꿔꺂??????????遺얘턁?????????????????좎럡?썼린?????좎뜫爰??????????????????熬곣뫖利닷뜝??좎룞?쇿뜝??(http://localhost:{}/metrics)", port);
    
    // ???????좎럩堉??????????????????좎럥캇?????좎럡愿????
    while (prometheus_server_running_) {
        sockaddr_in client_addr = {};
        int client_addr_size = sizeof(client_addr);
        
        // 5????????????좎럡?썹땟戮녹???좎럩?????좎?留???????????????????
        timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_socket, &read_fds);
        
        int select_result = select(0, &read_fds, nullptr, nullptr, &timeout);
        if (select_result == 0) {
            // ??????????좎럡?썹땟戮녹???좎럩?????좎?留???- ??????????좎럡?썲뜝?????????좎럥큔????????????
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
        
        // HTTP ????????????????????좎듅?????
        char buffer[4096] = {0};
        int recv_result = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (recv_result > 0) {
            buffer[recv_result] = '\0';
            std::string request(buffer);
            
            // GET /metrics ??????됰Ŧ????????????
            if (request.find("GET /metrics") == 0) {
                // Prometheus ??????좎럥큔????????????饔낅떽???????????????꾩룆梨띰쭕?뚢뵾???????
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
                // ??????????좎럥큔????????????????????????
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
