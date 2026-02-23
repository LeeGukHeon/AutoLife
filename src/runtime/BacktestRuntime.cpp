#include "runtime/BacktestRuntime.h"
#include "common/Logger.h"
#include "common/MarketDataWindowPolicy.h"
#include "common/PathUtils.h"
#include "common/RuntimeDiagnosticsShared.h"
#include "common/SignalPolicyShared.h"
#include "common/StrategyEdgeStatsShared.h"
#include "common/ExecutionGuardPolicyShared.h"
#include "common/ProbabilisticRegimeSpec.h"
#include "analytics/OrderbookAnalyzer.h"
#include "analytics/ProbabilisticRuntimeFeatures.h"
#include "execution/OrderLifecycleStateMachine.h"
#include "execution/ExecutionUpdateSchema.h"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string_view>

#include "strategy/FoundationAdaptiveStrategy.h"

namespace autolife {
namespace backtest {

namespace {
// Keep core execution plane slippage constants explicit in backtest cost modeling.
constexpr double CORE_ENTRY_SLIPPAGE_PCT = 0.00018;  // 0.018%
constexpr double CORE_EXIT_SLIPPAGE_PCT = 0.00026;   // 0.026%
constexpr double CORE_STOP_SLIPPAGE_PCT = 0.00095;   // 0.095%
constexpr double LEGACY_ENTRY_SLIPPAGE_PCT = 0.0002; // 0.02%
constexpr double LEGACY_EXIT_SLIPPAGE_PCT = 0.0003;  // 0.03%
constexpr double LEGACY_STOP_SLIPPAGE_PCT = 0.0010;  // 0.10%
// Live scanner and backtest must share a single window policy.
const size_t BACKTEST_CANDLE_WINDOW = common::targetBarsForTimeframe("1m", 200);
const size_t TF_5M_MAX_BARS = common::targetBarsForTimeframe("5m", 120);
const size_t TF_15M_MAX_BARS = common::targetBarsForTimeframe("15m", 120);
const size_t TF_1H_MAX_BARS = common::targetBarsForTimeframe("1h", 120);
const size_t TF_4H_MAX_BARS = common::targetBarsForTimeframe("4h", 90);
const size_t TF_1D_MAX_BARS = common::targetBarsForTimeframe("1d", 60);

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

std::string edgeGapBucket(double edge_gap) {
    if (!std::isfinite(edge_gap)) {
        return "edge_gap_invalid";
    }
    if (edge_gap <= 0.0) {
        return "edge_gap_le_0";
    }
    if (edge_gap <= 0.00002) {
        return "edge_gap_0_0p2bp";
    }
    if (edge_gap <= 0.00005) {
        return "edge_gap_0p2_0p5bp";
    }
    if (edge_gap <= 0.00010) {
        return "edge_gap_0p5_1p0bp";
    }
    if (edge_gap <= 0.00020) {
        return "edge_gap_1p0_2p0bp";
    }
    return "edge_gap_gt_2p0bp";
}

int adaptivePartialRatioHistogramIndex(double ratio) {
    if (ratio < 0.45) {
        return 0; // 0.35~0.44
    }
    if (ratio < 0.55) {
        return 1; // 0.45~0.54
    }
    if (ratio < 0.65) {
        return 2; // 0.55~0.64
    }
    if (ratio < 0.75) {
        return 3; // 0.65~0.74
    }
    return 4; // 0.75~0.80
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

std::filesystem::path policyDecisionArtifactPath() {
    return autolife::utils::PathUtils::resolveRelativePath("logs/policy_decisions_backtest.jsonl");
}

void resetPolicyDecisionArtifact() {
    const auto path = policyDecisionArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
}

void appendPolicyDecisionAudit(
    const std::vector<autolife::engine::PolicyDecisionRecord>& decisions,
    autolife::analytics::MarketRegime dominant_regime,
    bool small_seed_mode,
    int max_new_orders_per_scan,
    long long timestamp_ms
) {
    if (decisions.empty()) {
        return;
    }

    nlohmann::json line;
    line["ts"] = normalizeTimestampMs(timestamp_ms);
    line["source"] = "backtest";
    line["dominant_regime"] = autolife::common::runtime_diag::marketRegimeLabel(dominant_regime);
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

    const auto path = policyDecisionArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::app);
    if (!out.is_open()) {
        LOG_WARN("Policy decision artifact open failed: {}", path.string());
        return;
    }
    out << line.dump() << "\n";
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
using autolife::common::execution_guard::computeRealtimeEntryVetoThresholds;
using autolife::common::execution_guard::computeDynamicSlippageThresholds;
using autolife::common::strategy_edge::StrategyEdgeStats;
using autolife::common::strategy_edge::buildStrategyEdgeStats;
using autolife::common::strategy_edge::makeStrategyRegimeKey;
using autolife::common::strategy_edge::makeMarketStrategyRegimeKey;
using autolife::common::strategy_edge::buildStrategyRegimeEdgeStats;
using autolife::common::strategy_edge::buildMarketStrategyRegimeEdgeStats;
using autolife::common::runtime_diag::marketRegimeLabel;
using autolife::common::runtime_diag::strengthBucket;
using autolife::common::runtime_diag::expectedValueBucket;
using autolife::common::runtime_diag::rewardRiskBucket;
using autolife::common::runtime_diag::volatilityBucket;
using autolife::common::runtime_diag::liquidityBucket;

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
    double expected_edge_pct = 0.0;
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
    out_snapshot.expected_edge_pct = std::clamp(inference.expected_edge_pct, -0.05, 0.05);
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

    const double score_weight = std::clamp(cfg.probabilistic_runtime_score_weight, 0.0, 1.0);
    signal.score += std::clamp(effective_margin * score_weight, -0.12, 0.12);
    signal.signal_filter = std::clamp(signal.signal_filter + (effective_margin * 0.10), 0.0, 1.0);
    signal.expected_value += effective_margin * std::clamp(
        cfg.probabilistic_runtime_expected_edge_weight,
        0.0,
        0.01
    );
    if (std::isfinite(snapshot.expected_edge_pct) && std::abs(snapshot.expected_edge_pct) > 1e-9) {
        const double ev_blend = 0.20;
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
    const bool rescue_archetype =
        signal.entry_archetype.find("CORE_RESCUE") != std::string::npos;
    const bool range_pullback_archetype =
        signal.entry_archetype.find("FOUNDATION_RANGE_PULLBACK") != std::string::npos;
    const bool fragility_archetype = rescue_archetype || range_pullback_archetype;
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
            signal.expected_value = (signal.expected_value * 0.20) + (model_ev * 0.80);
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
        const double breakeven_mult = (!hostile_regime && fragility_archetype) ? 0.48 : 0.70;
        const double trailing_mult = (!hostile_regime && fragility_archetype) ? 0.82 : 1.10;
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
    const bool hostile_regime =
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    if (!snapshot.applied) {
        *min_strength = std::min(*min_strength, hostile_regime ? 0.36 : 0.28);
        *min_expected_value = std::min(*min_expected_value, hostile_regime ? 0.00002 : -0.00030);
        return;
    }

    const double prob = std::clamp(snapshot.prob_h5_calibrated, 0.0, 1.0);
    const double margin = std::clamp(snapshot.margin_h5, -1.0, 1.0);
    const double confidence = std::clamp(
        (std::clamp((prob - 0.50) / 0.20, 0.0, 1.0) * 0.65) +
        (std::clamp((margin + 0.01) / 0.08, 0.0, 1.0) * 0.35),
        0.0,
        1.0
    );

    double target_strength = hostile_regime
        ? (0.34 - (confidence * 0.08))
        : (0.24 - (confidence * 0.14));
    double target_edge = hostile_regime
        ? (0.00002 - (confidence * 0.00008))
        : (-0.00035 - (confidence * 0.00035));
    if (margin < 0.0) {
        target_strength += hostile_regime ? 0.03 : 0.02;
        target_edge += hostile_regime ? 0.00005 : 0.00010;
    }

    target_strength = std::clamp(target_strength, hostile_regime ? 0.26 : 0.12, hostile_regime ? 0.38 : 0.24);
    target_edge = std::clamp(target_edge, hostile_regime ? -0.00010 : -0.00080, hostile_regime ? 0.00008 : -0.00020);
    *min_strength = std::min(*min_strength, target_strength);
    *min_expected_value = std::min(*min_expected_value, target_edge);
}

bool shouldUseProbabilisticPrimaryFallback(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot,
    const autolife::analytics::CoinMetrics& metrics,
    autolife::analytics::MarketRegime regime
) {
    if (!cfg.enable_probabilistic_runtime_model ||
        !cfg.probabilistic_runtime_primary_mode ||
        !snapshot.applied) {
        return false;
    }
    if (probabilisticUncertaintyBlocksEntry(cfg, snapshot)) {
        return false;
    }
    if (cfg.probabilistic_regime_spec_enabled && snapshot.regime_block_new_entries) {
        return false;
    }
    const bool hostile_regime =
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    const double regime_threshold_add = cfg.probabilistic_regime_spec_enabled
        ? std::max(0.0, snapshot.regime_threshold_add)
        : 0.0;
    const double min_margin = (hostile_regime ? 0.020 : -0.040) + regime_threshold_add;
    const double min_prob = std::clamp(
        (hostile_regime ? 0.56 : 0.42) + (regime_threshold_add * 1.4),
        0.0,
        1.0
    );
    const double min_liquidity = hostile_regime ? 50.0 : 14.0;
    const double min_volume_surge = hostile_regime ? 0.72 : 0.14;
    const double max_spread = hostile_regime ? 0.0025 : 0.0065;

    if (snapshot.margin_h5 < min_margin || snapshot.prob_h5_calibrated < min_prob) {
        return false;
    }
    if (metrics.liquidity_score < min_liquidity || metrics.volume_surge_ratio < min_volume_surge) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid && metrics.orderbook_snapshot.spread_pct > max_spread) {
        return false;
    }
    return true;
}

autolife::strategy::Signal buildProbabilisticPrimaryFallbackSignal(
    const std::string& market,
    const autolife::analytics::CoinMetrics& metrics,
    double current_price,
    double available_capital,
    autolife::analytics::MarketRegime regime,
    const ProbabilisticRuntimeSnapshot& snapshot
) {
    autolife::strategy::Signal signal;
    signal.market = market;
    signal.strategy_name = "Probabilistic Primary Runtime";
    signal.reason = "probabilistic_primary_direct_fallback";
    signal.market_regime = regime;
    signal.entry_archetype = "PROBABILISTIC_PRIMARY_RUNTIME";
    signal.entry_price = current_price;
    signal.timestamp = getCurrentTimeMs();

    const bool hostile_regime =
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    const double margin = snapshot.margin_h5;
    const double prob = std::clamp(snapshot.prob_h5_calibrated, 0.0, 1.0);
    const double volatility = std::max(0.0, metrics.volatility);
    const double volatility_scale = std::clamp(
        (volatility > 0.0 ? volatility / 4.0 : 1.0),
        0.50,
        1.80
    );

    double risk_pct = hostile_regime ? 0.0032 : 0.0045;
    risk_pct = std::clamp(
        risk_pct * volatility_scale,
        hostile_regime ? 0.0025 : 0.0030,
        hostile_regime ? 0.0080 : 0.0120
    );
    double reward_risk = hostile_regime ? 1.20 : 1.45;
    reward_risk += std::clamp(margin * 8.0, 0.0, 0.90);
    reward_risk = std::clamp(reward_risk, hostile_regime ? 1.10 : 1.30, 2.60);

    const double margin_norm = std::clamp((margin + 0.01) / 0.12, 0.0, 1.0);
    double position_ratio = 0.04 + (margin_norm * 0.12);
    if (hostile_regime) {
        position_ratio *= 0.55;
    }
    if (metrics.liquidity_score < 35.0) {
        position_ratio *= 0.75;
    }
    if (volatility > 4.0) {
        position_ratio *= 0.80;
    }
    const double max_ratio = hostile_regime ? 0.10 : 0.18;
    signal.position_size = std::clamp(position_ratio, 0.02, max_ratio);

    const double notional = available_capital * signal.position_size;
    if (notional < 1.0) {
        signal.type = autolife::strategy::SignalType::NONE;
        signal.reason = "probabilistic_primary_direct_fallback_invalid_notional";
        return signal;
    }

    signal.stop_loss = current_price * (1.0 - risk_pct);
    signal.take_profit_1 = current_price * (1.0 + risk_pct * std::max(1.0, reward_risk * 0.55));
    signal.take_profit_2 = current_price * (1.0 + risk_pct * reward_risk);
    signal.breakeven_trigger = current_price * (1.0 + risk_pct * 0.75);
    signal.trailing_start = current_price * (1.0 + risk_pct * 1.15);

    signal.liquidity_score = metrics.liquidity_score;
    signal.volatility = metrics.volatility;
    signal.signal_filter = std::clamp(0.54 + (margin * 0.45), 0.45, 0.88);
    signal.strength = std::clamp(0.45 + (prob * 0.45) + (margin * 2.0), 0.35, 0.94);
    signal.type = (prob >= 0.62 && margin >= 0.03)
        ? autolife::strategy::SignalType::STRONG_BUY
        : autolife::strategy::SignalType::BUY;

    signal.expected_return_pct = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
    signal.expected_risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
    const double implied_win = std::clamp(prob, 0.45, 0.75);
    signal.expected_value =
        (implied_win * signal.expected_return_pct) -
        ((1.0 - implied_win) * signal.expected_risk_pct);

    signal.buy_order_type = autolife::strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = autolife::strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 2;
    signal.retry_wait_ms = 700;
    return signal;
}

struct ProbabilisticPrimaryMinimums {
    double min_h5_calibrated = 0.5;
    double min_h5_margin = 0.0;
    double min_liquidity_score = 0.0;
    double min_signal_strength = 0.0;
};

ProbabilisticPrimaryMinimums effectiveProbabilisticPrimaryMinimums(
    const autolife::engine::EngineConfig& cfg,
    autolife::analytics::MarketRegime regime,
    const ProbabilisticRuntimeSnapshot* snapshot
) {
    ProbabilisticPrimaryMinimums out;
    out.min_h5_calibrated = cfg.probabilistic_primary_min_h5_calibrated;
    out.min_h5_margin = cfg.probabilistic_primary_min_h5_margin;
    out.min_liquidity_score = cfg.probabilistic_primary_min_liquidity_score;
    out.min_signal_strength = cfg.probabilistic_primary_min_signal_strength;

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
    const double prob = signal.probabilistic_runtime_applied
        ? std::clamp(signal.probabilistic_h5_calibrated, 0.0, 1.0)
        : 0.5;
    const double margin = signal.probabilistic_runtime_applied
        ? std::clamp(signal.probabilistic_h5_margin, -1.0, 1.0)
        : 0.0;
    const double confidence = std::clamp(
        (std::clamp((prob - 0.50) / 0.25, 0.0, 1.0) * 0.65) +
        (std::clamp((margin + 0.02) / 0.12, 0.0, 1.0) * 0.35),
        0.0,
        1.0
    );
    const double margin_score = std::clamp((margin + 0.10) / 0.20, 0.0, 1.0);
    const double liquidity_score = std::clamp(signal.liquidity_score / 100.0, 0.0, 1.0);
    const double strength_score = std::clamp(signal.strength, 0.0, 1.0);
    const double expected_edge_score = std::clamp((signal.expected_value + 0.0005) / 0.0025, 0.0, 1.0);

    double prob_weight = 0.50;
    double margin_weight = 0.22;
    double liquidity_weight = 0.10;
    double strength_weight = 0.10;
    double edge_weight = 0.08;
    const bool hostile =
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    if (hostile) {
        prob_weight = 0.56;
        margin_weight = 0.24;
        liquidity_weight = 0.12;
        strength_weight = 0.08;
        edge_weight = 0.00;
    }
    double score =
        (prob * prob_weight) +
        (margin_score * margin_weight) +
        (liquidity_score * liquidity_weight) +
        (strength_score * strength_weight) +
        (expected_edge_score * edge_weight);

    if (signal.type == autolife::strategy::SignalType::STRONG_BUY) {
        score += 0.02;
    }
    if (cfg.probabilistic_runtime_primary_mode && signal.probabilistic_runtime_applied) {
        score += std::clamp(signal.probabilistic_h5_margin * 0.08, -0.03, 0.03);
    }

    const std::string& archetype = signal.entry_archetype;
    if (archetype.find("CORE_RESCUE") != std::string::npos) {
        if (confidence < 0.72 || signal.strength < 0.46 || margin < 0.002) {
            score -= 0.16;
        } else {
            score += 0.02;
        }
    } else if (archetype.find("FOUNDATION_RANGE_PULLBACK") != std::string::npos) {
        if (signal.strength < 0.50 && (margin < 0.008 || prob < 0.54)) {
            score -= 0.11;
        } else if (margin >= 0.012 && prob >= 0.57) {
            score += 0.03;
        }
    } else if (archetype.find("FOUNDATION_UPTREND_CONTINUATION") != std::string::npos) {
        if (margin >= 0.0 && prob >= 0.52) {
            score += 0.03;
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
        effectiveProbabilisticPrimaryMinimums(cfg, regime, snapshot);
    const bool hostile_regime =
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    const bool supply_probe_reason =
        signal.reason.find("signal_supply_fallback") != std::string::npos ||
        signal.reason.find("ranging_low_flow") != std::string::npos ||
        signal.reason.find("uptrend_low_flow_probe") != std::string::npos ||
        signal.reason.find("manager_soft_queue_promoted") != std::string::npos ||
        signal.reason.find("manager_probabilistic_primary_promoted") != std::string::npos ||
        signal.reason.find("manager_probabilistic_primary_fastpass") != std::string::npos;
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
    const bool rescue_archetype =
        signal.entry_archetype.find("CORE_RESCUE") != std::string::npos;
    const bool range_pullback_archetype =
        signal.entry_archetype.find("FOUNDATION_RANGE_PULLBACK") != std::string::npos;
    if (!hostile_regime && rescue_archetype) {
        if (signal.probabilistic_h5_calibrated < 0.55 ||
            signal.probabilistic_h5_margin < 0.002 ||
            signal.liquidity_score < 18.0 ||
            signal.strength < 0.24) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_rescue_quality";
            }
            return false;
        }
    }
    if (!hostile_regime && range_pullback_archetype) {
        if (signal.strength < 0.24 &&
            (signal.probabilistic_h5_margin < 0.008 ||
             signal.probabilistic_h5_calibrated < 0.54)) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_rangepullback_quality";
            }
            return false;
        }
    }
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
        if (signal.probabilistic_h5_margin < -0.035 &&
            signal.probabilistic_h5_calibrated < 0.42) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_margin";
            }
            return false;
        }
        if (signal.probabilistic_h5_calibrated < 0.38 &&
            signal.probabilistic_h5_margin < -0.025) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_calibrated";
            }
            return false;
        }
    }
    if (supply_probe_reason && !hostile_regime) {
        mins.min_h5_calibrated = std::max(0.0, mins.min_h5_calibrated - 0.03);
        mins.min_h5_margin = std::max(-0.50, mins.min_h5_margin - 0.015);
        mins.min_liquidity_score = std::max(0.0, mins.min_liquidity_score - 12.0);
        mins.min_signal_strength = std::max(0.0, mins.min_signal_strength - 0.08);
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
        if (rescue_archetype &&
            !hostile_regime &&
            (signal.probabilistic_h5_calibrated < 0.56 ||
             signal.probabilistic_h5_margin < 0.004 ||
             signal.strength < 0.26)) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_rescue_quality";
            }
            return false;
        }
        if (range_pullback_archetype &&
            !hostile_regime &&
            signal.strength < 0.22 &&
            (signal.probabilistic_h5_margin < 0.010 ||
             signal.probabilistic_h5_calibrated < 0.53)) {
            if (reject_reason != nullptr) {
                *reject_reason = "blocked_probabilistic_primary_rangepullback_quality";
            }
            return false;
        }
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
        if (composite_score >= 0.48 &&
            signal.probabilistic_h5_margin >= (mins.min_h5_margin - 0.028)) {
            return true;
        }
        if (signal.probabilistic_h5_calibrated >= 0.44 &&
            signal.probabilistic_h5_margin >= -0.018 &&
            signal.liquidity_score >= 16.0 &&
            signal.strength >= 0.10) {
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

bool requiresTypedArchetype(const std::string& strategy_name) {
    return autolife::common::signal_policy::requiresTypedArchetype(strategy_name);
}

void normalizeSignalStopLossByRegime(strategy::Signal& signal, analytics::MarketRegime regime) {
    autolife::common::signal_policy::normalizeSignalStopLossByRegime(signal, regime);
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
    resetPolicyDecisionArtifact();
    balance_krw_ = config.getInitialCapital();
    balance_asset_ = 0.0;
    max_balance_ = balance_krw_;
    loaded_tf_cursors_.clear();
    entry_funnel_ = Result::EntryFunnelSummary{};
    shadow_funnel_ = Result::ShadowFunnelSummary{};
    pre_cat_feature_snapshot_ = Result::PreCatFeatureSnapshot{};
    strategy_generated_counts_.clear();
    strategy_selected_best_counts_.clear();
    strategy_blocked_by_risk_manager_counts_.clear();
    strategy_entries_executed_counts_.clear();
    strategy_skipped_disabled_counts_.clear();
    strategy_no_signal_counts_.clear();
    strategy_collect_exception_count_ = 0;
    entry_rejection_reason_counts_.clear();
    no_signal_pattern_counts_.clear();
    entry_quality_edge_gap_buckets_.clear();
    intrabar_stop_tp_collision_count_ = 0;
    intrabar_collision_by_strategy_.clear();
    pre_cat_recovery_hysteresis_hold_by_key_.clear();
    pre_cat_recovery_bridge_activation_by_key_.clear();
    pre_cat_no_soft_quality_relief_activation_by_key_.clear();
    pre_cat_candidate_rr_failsafe_activation_by_key_.clear();
    pre_cat_pressure_rebound_relief_activation_by_key_.clear();
    pre_cat_negative_history_quarantine_hold_by_key_.clear();
    adaptive_stop_update_count_ = 0;
    adaptive_tp_recalibration_count_ = 0;
    adaptive_partial_ratio_samples_ = 0;
    adaptive_partial_ratio_sum_ = 0.0;
    adaptive_partial_ratio_histogram_ = {0, 0, 0, 0, 0};
    recent_best_ask_price_ = 0.0;
    recent_best_ask_timestamp_ms_ = 0;
    
    // Reset Risk Manager with initial capital
    risk_manager_ = std::make_unique<risk::RiskManager>(balance_krw_);
    // Backtest uses deterministic replay; disable re-entry cooldown.
    risk_manager_->setMinReentryInterval(0);
    risk_manager_->setMaxDailyTrades(1000);
    
    auto foundation = std::make_shared<strategy::FoundationAdaptiveStrategy>(http_client_);
    const bool foundation_enabled = isStrategyEnabledByConfig(engine_config_, foundation->getInfo().name);
    foundation->setEnabled(foundation_enabled);
    strategy_manager_->registerStrategy(foundation);
    LOG_INFO("Registered strategy: foundation_adaptive (enabled={})",
             foundation_enabled ? "Y" : "N");

    probabilistic_runtime_model_loaded_ = false;
    if (engine_config_.enable_probabilistic_runtime_model) {
        const auto bundle_path = autolife::utils::PathUtils::resolveRelativePath(
            engine_config_.probabilistic_runtime_bundle_path
        );
        std::string error_message;
        probabilistic_runtime_model_loaded_ =
            probabilistic_runtime_model_.loadFromFile(bundle_path.string(), &error_message);
        if (probabilistic_runtime_model_loaded_) {
            LOG_INFO(
                "Backtest probabilistic runtime bundle loaded: path={}, features={}",
                bundle_path.string(),
                probabilistic_runtime_model_.featureColumns().size()
            );
        } else {
            LOG_WARN(
                "Backtest probabilistic runtime bundle load skipped: path={}, reason={}",
                bundle_path.string(),
                error_message.empty() ? std::string("unknown") : error_message
            );
        }
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
        {"15m", {"15m"}},
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
    strict_live_equivalent_data_parity_ =
        common::hasLiveEquivalentCompanionSet(loaded_tf_candles_);
    strict_data_parity_skip_count_ = 0;

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
    LOG_INFO("Backtest live-equivalent data parity mode: {}",
             strict_live_equivalent_data_parity_ ? "enabled" : "disabled");
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
    
    // Keep warm-up policy explicit:
    // - relaxed mode: lightweight warm-up for synthetic/small fixtures
    // - strict mode: live-equivalent minimum bars on 1m
    const size_t min_1m_warmup = strict_live_equivalent_data_parity_
        ? common::minRequiredBarsForTimeframe("1m", 120)
        : static_cast<size_t>(30);
    if (current_candles_.size() < min_1m_warmup) {
        return;
    }

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
    metrics.candles_by_tf["15m"] = getTimeframeCandles("15m", candle.timestamp, 15, TF_15M_MAX_BARS);
    metrics.candles_by_tf["1h"] = getTimeframeCandles("1h", candle.timestamp, 60, TF_1H_MAX_BARS);
    metrics.candles_by_tf["4h"] = getTimeframeCandles("4h", candle.timestamp, 240, TF_4H_MAX_BARS);
    metrics.candles_by_tf["1d"] = getTimeframeCandles("1d", candle.timestamp, 1440, TF_1D_MAX_BARS);
    common::trimCandlesByPolicy(metrics.candles_by_tf);
    auto tf_1m_it = metrics.candles_by_tf.find("1m");
    if (tf_1m_it != metrics.candles_by_tf.end()) {
        metrics.candles = tf_1m_it->second;
    }
    if (strict_live_equivalent_data_parity_) {
        const auto data_window_check = common::checkLiveEquivalentWindow(metrics.candles_by_tf);
        if (!data_window_check.pass) {
            strict_data_parity_skip_count_++;
            if (strict_data_parity_skip_count_ == 1 || (strict_data_parity_skip_count_ % 200) == 0) {
                LOG_INFO(
                    "Backtest strict parity skip: market={}, ts={}, reason={}",
                    market_name_,
                    candle.timestamp,
                    common::buildWindowCheckSummary(data_window_check)
                );
            }
            return;
        }
    }
    metrics.current_price = current_price;
    metrics.volatility = regime.atr_pct;
    
    // Calculate price_change_rate from previous candle
    if (current_candles_.size() >= 2) {
        double prev_close = current_candles_[current_candles_.size() - 2].close;
        if (prev_close > 0) {
            metrics.price_change_rate = ((current_price - prev_close) / prev_close) * 100.0;
        }
    }
    
    // Estimate volume surge from rolling notional (price * volume) to reduce
    // asset-price-scale bias in backtest synthetic market metrics.
    double avg_vol = 0.0;
    double avg_notional = 0.0;
    if (current_candles_.size() >= 20) {
        for (size_t vi = current_candles_.size() - 20; vi < current_candles_.size() - 1; ++vi) {
            avg_vol += current_candles_[vi].volume;
            avg_notional += (current_candles_[vi].close * current_candles_[vi].volume);
        }
        avg_vol /= 19.0;
        avg_notional /= 19.0;
        const double current_notional = candle.close * candle.volume;
        if (avg_notional > 0.0) {
            metrics.volume_surge_ratio = current_notional / avg_notional;
        } else {
            metrics.volume_surge_ratio = (avg_vol > 0.0) ? (candle.volume / avg_vol) : 1.0;
        }
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

    const double vol_baseline_t = std::clamp((avg_vol - 8.0) / 90.0, 0.0, 1.0);
    const double notional_log = std::log10(std::max(avg_notional, 1.0));
    const double notional_t = std::clamp((notional_log - 7.5) / 2.6, 0.0, 1.0);
    const double spread_stress_t = std::clamp((candle_range_pct - 0.25) / 2.2, 0.0, 1.0);
    const double surge_bonus = std::clamp((metrics.volume_surge_ratio - 1.0) * 6.0, -7.0, 12.0);
    double regime_adjust = 0.0;
    if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        regime_adjust += 2.0;
    } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        regime_adjust -= 4.0;
    } else if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        regime_adjust -= 8.0;
    } else if (regime.regime == analytics::MarketRegime::RANGING) {
        regime_adjust -= 1.5;
    }
    metrics.liquidity_score = std::clamp(
        38.0 + (vol_baseline_t * 22.0) + (notional_t * 32.0) - (spread_stress_t * 18.0) + surge_bonus + regime_adjust,
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
        metrics.orderbook_snapshot = analytics::OrderbookAnalyzer::analyze(units, 1000000.0);
        if (metrics.orderbook_snapshot.valid) {
            // Keep synthetic flow fields aligned with live orderbook-derived values.
            metrics.order_book_imbalance = std::clamp(metrics.orderbook_snapshot.imbalance, -0.7, 0.7);
            metrics.buy_pressure = std::clamp(50.0 + (metrics.order_book_imbalance * 35.0), 10.0, 90.0);
            metrics.sell_pressure = std::clamp(100.0 - metrics.buy_pressure, 10.0, 90.0);
        }
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
            const auto drawdown_slippage = computeDynamicSlippageThresholds(
                engine_config_,
                market_hostility_ewma_,
                false,
                position->market_regime,
                position->signal_strength,
                position->liquidity_score,
                position->expected_value,
                "max_drawdown"
            );
            const double forced_exit_slippage = std::min(
                exitSlippagePct(engine_config_),
                drawdown_slippage.max_slippage_pct
            );
            const double forced_exit = current_price * (1.0 - forced_exit_slippage);
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
                const auto stop_slippage = computeDynamicSlippageThresholds(
                    engine_config_,
                    market_hostility_ewma_,
                    false,
                    position->market_regime,
                    position->signal_strength,
                    position->liquidity_score,
                    position->expected_value,
                    "stop_loss"
                );
                const double stop_fill_slippage = std::min(
                    stopSlippagePct(engine_config_),
                    stop_slippage.max_slippage_pct
                );
                const double stop_fill = position->stop_loss * (1.0 - stop_fill_slippage);
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
                    const auto tp1_slippage = computeDynamicSlippageThresholds(
                        engine_config_,
                        market_hostility_ewma_,
                        false,
                        position->market_regime,
                        position->signal_strength,
                        position->liquidity_score,
                        position->expected_value,
                        "take_profit_1"
                    );
                    const double tp1_fill_slippage = std::min(
                        exitSlippagePct(engine_config_),
                        tp1_slippage.max_slippage_pct
                    );
                    const double tp1_fill = position->take_profit_1 * (1.0 - tp1_fill_slippage);
                    const double partial_ratio = std::clamp(
                        risk_manager_->getAdaptivePartialExitRatio(market_name_),
                        0.35,
                        0.80
                    );
                    adaptive_partial_ratio_samples_++;
                    adaptive_partial_ratio_sum_ += partial_ratio;
                    const int partial_ratio_hist_idx = adaptivePartialRatioHistogramIndex(partial_ratio);
                    if (partial_ratio_hist_idx >= 0 &&
                        partial_ratio_hist_idx < static_cast<int>(adaptive_partial_ratio_histogram_.size())) {
                        adaptive_partial_ratio_histogram_[static_cast<size_t>(partial_ratio_hist_idx)]++;
                    }
                    const double partial_qty = position->quantity * partial_ratio;
                    const double partial_notional = partial_qty * tp1_fill;

                    if (partial_qty <= 0.0 || partial_notional < engine_config_.min_order_krw) {
                        const double full_notional = position->quantity * tp1_fill;
                        if (full_notional >= engine_config_.min_order_krw) {
                            const risk::Position closed_position = *position;
                            Order tp_full_order;
                            tp_full_order.market = market_name_;
                            tp_full_order.side = OrderSide::SELL;
                            tp_full_order.volume = position->quantity;
                            tp_full_order.price = tp1_fill;
                            tp_full_order.strategy_name = position->strategy_name;
                            executeOrder(tp_full_order, tp1_fill);
                            risk_manager_->exitPosition(market_name_, tp1_fill, "TakeProfitFullDueToMinOrder");
                            notifyStrategyClosed(closed_position, tp1_fill);
                            position = nullptr;
                        } else {
                            risk_manager_->setHalfClosed(market_name_, true);
                            risk_manager_->moveStopToBreakeven(market_name_);
                            position = risk_manager_->getPosition(market_name_);
                        }
                    } else {
                        Order tp1_order;
                        tp1_order.market = market_name_;
                        tp1_order.side = OrderSide::SELL;
                        tp1_order.volume = partial_qty;
                        tp1_order.price = tp1_fill;
                        tp1_order.strategy_name = position->strategy_name;
                        executeOrder(tp1_order, tp1_fill);

                        if (risk_manager_->applyPartialSellFill(market_name_, tp1_fill, partial_qty, "TakeProfit1")) {
                            risk_manager_->setHalfClosed(market_name_, true);
                            risk_manager_->moveStopToBreakeven(market_name_);
                        } else {
                            LOG_WARN("Backtest TP1 accounting failed: {} qty {:.8f} @ {:.0f}",
                                     market_name_, partial_qty, tp1_fill);
                        }
                        position = risk_manager_->getPosition(market_name_);
                    }
                }

                if (position) {
                    const bool tp2_touched_after_partial = tp2_touched || (candle.high >= position->take_profit_2);
                    if (tp2_touched_after_partial) {
                        const risk::Position closed_position = *position;
                        const auto tp2_slippage = computeDynamicSlippageThresholds(
                            engine_config_,
                            market_hostility_ewma_,
                            false,
                            position->market_regime,
                            position->signal_strength,
                            position->liquidity_score,
                            position->expected_value,
                            "take_profit_2"
                        );
                        const double tp2_fill_slippage = std::min(
                            exitSlippagePct(engine_config_),
                            tp2_slippage.max_slippage_pct
                        );
                        const double tp2_fill = position->take_profit_2 * (1.0 - tp2_fill_slippage);
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
                        const auto strategy_exit_slippage = computeDynamicSlippageThresholds(
                            engine_config_,
                            market_hostility_ewma_,
                            false,
                            position->market_regime,
                            position->signal_strength,
                            position->liquidity_score,
                            position->expected_value,
                            "strategy_exit"
                        );
                        const double strategy_exit_fill_slippage = std::min(
                            exitSlippagePct(engine_config_),
                            strategy_exit_slippage.max_slippage_pct
                        );
                        // Execute Sell
                        Order order;
                        order.market = market_name_;
                        order.side = OrderSide::SELL;
                        order.volume = position->quantity;
                        order.price = current_price * (1.0 - strategy_exit_fill_slippage);
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

    // Apply post-entry adaptive stop logic after current-candle exit checks.
    // This avoids same-candle stop tightening lookahead in backtest.
    if (position) {
        const double stop_before = position->stop_loss;
        const double tp1_before = position->take_profit_1;
        const double tp2_before = position->take_profit_2;
        risk_manager_->applyAdaptiveRiskControls(market_name_);
        position = risk_manager_->getPosition(market_name_);
        if (position) {
            if (position->stop_loss > stop_before + 1e-9) {
                adaptive_stop_update_count_++;
            }
            const bool tp1_changed = std::fabs(position->take_profit_1 - tp1_before) > 1e-9;
            const bool tp2_changed = std::fabs(position->take_profit_2 - tp2_before) > 1e-9;
            if (tp1_changed || tp2_changed) {
                adaptive_tp_recalibration_count_++;
            }
        }
    }
    
    // 4. Generate Entry Signals (only if no position)
    if (!position) {
        entry_funnel_.entry_rounds++;
        bool entry_executed = false;
        auto markEntryReject = [&](const char* reason) {
            entry_rejection_reason_counts_[reason]++;
        };
        auto mergeEntryRejectCounts = [&](const std::map<std::string, int>& counts) {
            for (const auto& kv : counts) {
                if (kv.first.empty() || kv.second <= 0) {
                    continue;
                }
                entry_rejection_reason_counts_[kv.first] += kv.second;
            }
        };
        strategy::StrategyManager::CollectDiagnostics collect_diag;
        ProbabilisticRuntimeSnapshot probabilistic_snapshot;
        const auto probabilistic_online_state = computeProbabilisticOnlineLearningState(
            risk_manager_->getTradeHistory(),
            engine_config_,
            market_name_,
            regime.regime
        );
        std::string probabilistic_prefilter_reason;
        std::vector<strategy::Signal> signals;
        if (!inferProbabilisticRuntimeSnapshot(
                probabilistic_runtime_model_,
                engine_config_,
                metrics,
                market_name_,
                regime.regime,
                &probabilistic_online_state,
                probabilistic_snapshot,
                &probabilistic_prefilter_reason)) {
            entry_funnel_.no_signal_generated++;
            markEntryReject(
                probabilistic_prefilter_reason.empty()
                    ? "probabilistic_market_prefilter"
                    : probabilistic_prefilter_reason.c_str()
            );
            const std::string no_signal_pattern_key =
                std::string("regime=") + marketRegimeLabel(regime.regime) +
                "|vol=" + volatilityBucket(metrics.volatility) +
                "|liq=" + liquidityBucket(metrics.liquidity_score);
            no_signal_pattern_counts_[no_signal_pattern_key]++;
        } else {
            signals = strategy_manager_->collectSignals(
                market_name_,
                metrics,
                current_candles_,
                current_price,
                risk_manager_->getRiskMetrics().available_capital,
                regime,
                &collect_diag
            );
        }
        if (!signals.empty()) {
            std::vector<strategy::Signal> adjusted_signals;
            adjusted_signals.reserve(signals.size());
            for (auto& signal : signals) {
                std::string reject_reason;
                if (!applyProbabilisticRuntimeAdjustment(
                        engine_config_,
                        probabilistic_snapshot,
                        signal,
                        &reject_reason)) {
                    markEntryReject(
                        reject_reason.empty()
                            ? "probabilistic_runtime_hard_gate"
                            : reject_reason.c_str()
                    );
                    continue;
                }
                adjusted_signals.push_back(std::move(signal));
            }
            signals = std::move(adjusted_signals);
        }
        for (const auto& kv : collect_diag.skipped_disabled_by_strategy) {
            if (!kv.first.empty() && kv.second > 0) {
                strategy_skipped_disabled_counts_[kv.first] += kv.second;
            }
        }
        for (const auto& kv : collect_diag.no_signal_by_strategy) {
            if (!kv.first.empty() && kv.second > 0) {
                strategy_no_signal_counts_[kv.first] += kv.second;
            }
        }
        if (collect_diag.exception_count > 0) {
            strategy_collect_exception_count_ += collect_diag.exception_count;
        }
        if (signals.empty() &&
            shouldUseProbabilisticPrimaryFallback(
                engine_config_,
                probabilistic_snapshot,
                metrics,
                regime.regime)) {
            auto fallback_signal = buildProbabilisticPrimaryFallbackSignal(
                market_name_,
                metrics,
                current_price,
                risk_manager_->getRiskMetrics().available_capital,
                regime.regime,
                probabilistic_snapshot
            );
            if (fallback_signal.type != strategy::SignalType::NONE) {
                std::string fallback_reject_reason;
                if (applyProbabilisticRuntimeAdjustment(
                        engine_config_,
                        probabilistic_snapshot,
                        fallback_signal,
                        &fallback_reject_reason)) {
                    signals.push_back(std::move(fallback_signal));
                    markEntryReject("probabilistic_primary_direct_fallback");
                } else if (!fallback_reject_reason.empty()) {
                    markEntryReject(fallback_reject_reason.c_str());
                }
            }
        }
        if (signals.empty()) {
            entry_funnel_.no_signal_generated++;
            markEntryReject("no_signal_generated");
            const std::string no_signal_pattern_key =
                std::string("regime=") + marketRegimeLabel(regime.regime) +
                "|vol=" + volatilityBucket(metrics.volatility) +
                "|liq=" + liquidityBucket(metrics.liquidity_score);
            no_signal_pattern_counts_[no_signal_pattern_key]++;
            for (const auto& kv : collect_diag.no_signal_reason_counts) {
                if (kv.first.empty() || kv.second <= 0) {
                    continue;
                }
                entry_rejection_reason_counts_[kv.first] += kv.second;
            }
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

        double primary_manager_min_strength = filter_threshold;
        double primary_manager_min_expected_value = min_expected_value;
        applyProbabilisticManagerFloors(
            engine_config_,
            probabilistic_snapshot,
            regime.regime,
            &primary_manager_min_strength,
            &primary_manager_min_expected_value
        );
        std::vector<strategy::Signal> filtered_signals;
        strategy::StrategyManager::FilterDiagnostics manager_filter_diag;
        filtered_signals = strategy_manager_->filterSignalsWithDiagnostics(
            signals,
            primary_manager_min_strength,
            primary_manager_min_expected_value,
            regime.regime,
            &manager_filter_diag
        );
        std::vector<strategy::Signal> ranked_filtered;
        ranked_filtered.reserve(filtered_signals.size());
        for (auto& signal : filtered_signals) {
            std::string primary_reject_reason;
            if (!passesProbabilisticPrimaryMinimums(
                    engine_config_,
                    signal,
                    regime.regime,
                    &probabilistic_snapshot,
                    &primary_reject_reason)) {
                markEntryReject(
                    primary_reject_reason.empty()
                        ? "filtered_out_by_probabilistic_primary_minimum"
                        : primary_reject_reason.c_str()
                );
                continue;
            }
            ranked_filtered.push_back(std::move(signal));
        }
        std::stable_sort(
            ranked_filtered.begin(),
            ranked_filtered.end(),
            [&](const strategy::Signal& lhs, const strategy::Signal& rhs) {
                return probabilisticPrimaryPriorityScore(engine_config_, lhs, regime.regime) >
                       probabilisticPrimaryPriorityScore(engine_config_, rhs, regime.regime);
            }
        );
        filtered_signals = std::move(ranked_filtered);
        if (!signals.empty() && filtered_signals.empty()) {
            entry_funnel_.filtered_out_by_manager++;
            markEntryReject("filtered_out_by_manager");
            mergeEntryRejectCounts(manager_filter_diag.rejection_reason_counts);
        }
        std::vector<strategy::Signal> candidate_signals = filtered_signals;
        std::vector<engine::PolicyDecisionRecord> core_policy_decisions;
        int core_dropped_by_policy = 0;
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
            core_policy_decisions = policy_output.decisions;
            core_dropped_by_policy = policy_output.dropped_by_policy;
            appendPolicyDecisionAudit(
                core_policy_decisions,
                regime.regime,
                small_seed_mode,
                policy_input.max_new_orders_per_scan,
                candle.timestamp
            );
            candidate_signals = std::move(policy_output.selected_candidates);
        }
        if (core_policy_enabled && !filtered_signals.empty() && candidate_signals.empty()) {
            entry_funnel_.filtered_out_by_policy++;
            markEntryReject("filtered_out_by_policy");
        }

        strategy::Signal best_signal;
        if (!candidate_signals.empty()) {
            std::stable_sort(
                candidate_signals.begin(),
                candidate_signals.end(),
                [&](const strategy::Signal& lhs, const strategy::Signal& rhs) {
                    return probabilisticPrimaryPriorityScore(engine_config_, lhs, regime.regime) >
                           probabilisticPrimaryPriorityScore(engine_config_, rhs, regime.regime);
                }
            );
            best_signal = candidate_signals.front();
            LOG_INFO(
                "{} probabilistic-primary best selected [{}]: p_h5={:.4f}, margin={:+.4f}, liq={:.1f}, strength={:.3f}, candidates={}, regime_state={}, vol_z={:+.3f}, dd_speed_bps={:.2f}, ens_n={}, u_std={:.4f}",
                market_name_,
                best_signal.strategy_name,
                best_signal.probabilistic_h5_calibrated,
                best_signal.probabilistic_h5_margin,
                best_signal.liquidity_score,
                best_signal.strength,
                static_cast<int>(candidate_signals.size()),
                autolife::common::probabilistic_regime::stateLabel(probabilistic_snapshot.regime_state),
                probabilistic_snapshot.regime_volatility_zscore,
                probabilistic_snapshot.regime_drawdown_speed_bps,
                probabilistic_snapshot.ensemble_member_count,
                probabilistic_snapshot.prob_h5_std
            );
        }
        if (!candidate_signals.empty() && best_signal.type == strategy::SignalType::NONE) {
            entry_funnel_.no_best_signal++;
            markEntryReject("no_best_signal");
        }
        if (best_signal.type != strategy::SignalType::NONE && !best_signal.strategy_name.empty()) {
            strategy_selected_best_counts_[best_signal.strategy_name]++;
        }

        // Shadow-only candidate probe:
        // evaluate relaxed candidate supply without touching actual order flow.
        shadow_funnel_.rounds++;
        shadow_funnel_.primary_generated_signals += static_cast<int>(signals.size());
        shadow_funnel_.primary_after_manager_filter += static_cast<int>(filtered_signals.size());
        shadow_funnel_.primary_after_policy_filter += static_cast<int>(candidate_signals.size());
                if (best_signal.type != strategy::SignalType::NONE) {
            best_signal.market_regime = regime.regime;

            normalizeSignalStopLossByRegime(best_signal, best_signal.market_regime);
            const bool rr_rebalance_ok = rebalanceSignalRiskReward(best_signal, engine_config_);
            if (!rr_rebalance_ok) {
                entry_funnel_.blocked_rr_rebalance++;
                markEntryReject("blocked_rr_rebalance");
            } else {
                const bool regime_gate_ok =
                    !core_risk_enabled || passesRegimeGate(best_signal.market_regime, engine_config_);
                if (!regime_gate_ok) {
                    entry_funnel_.blocked_risk_gate_regime++;
                    markEntryReject("blocked_risk_gate_regime");
                } else {
                    const bool archetype_required = requiresTypedArchetype(best_signal.strategy_name);
                    const bool archetype_ready =
                        !archetype_required ||
                        (!best_signal.entry_archetype.empty() && best_signal.entry_archetype != "UNSPECIFIED");
                    if (!archetype_ready) {
                        entry_funnel_.blocked_pattern_gate++;
                        markEntryReject("blocked_pattern_missing_archetype");
                    } else {
                        const double entry_price = best_signal.entry_price;
                        const double take_profit_price =
                            (best_signal.take_profit_2 > 0.0) ? best_signal.take_profit_2 : best_signal.take_profit_1;
                        const double stop_loss_price = best_signal.stop_loss;
                        if (entry_price <= 0.0 || take_profit_price <= 0.0 || stop_loss_price <= 0.0 ||
                            take_profit_price <= entry_price || stop_loss_price >= entry_price) {
                            entry_funnel_.blocked_risk_gate_entry_quality_invalid_levels++;
                            markEntryReject("blocked_risk_gate_entry_quality_invalid_levels");
                        } else {
                            const double probabilistic_position_scale =
                                probabilisticPositionScaleForEntry(
                                    engine_config_,
                                    best_signal,
                                    &probabilistic_snapshot
                                );
                            if (std::fabs(probabilistic_position_scale - 1.0) > 1e-6) {
                                best_signal.position_size *= probabilistic_position_scale;
                                LOG_INFO(
                                    "{} probabilistic entry size scale [{}]: margin={:+.4f}, scale={:.3f}, pos={:.4f}",
                                    market_name_,
                                    best_signal.strategy_name,
                                    best_signal.probabilistic_h5_margin,
                                    probabilistic_position_scale,
                                    best_signal.position_size
                                );
                            }
                            const auto dynamic_buy_slippage = computeDynamicSlippageThresholds(
                                engine_config_,
                                market_hostility_ewma_,
                                true,
                                best_signal.market_regime,
                                best_signal.strength,
                                best_signal.liquidity_score,
                                best_signal.expected_value
                            );
                            bool realtime_entry_veto_ok = true;
                            std::string realtime_entry_veto_reason;
                            if (engine_config_.enable_realtime_entry_veto) {
                                const auto veto_thresholds = computeRealtimeEntryVetoThresholds(
                                    engine_config_,
                                    best_signal.market_regime,
                                    best_signal.strength,
                                    best_signal.liquidity_score,
                                    market_hostility_ewma_
                                );
                    const auto& snapshot = metrics.orderbook_snapshot;
                    const long long now_ms = toMsTimestamp(candle.timestamp);
                    if (!snapshot.valid || snapshot.best_ask <= 0.0) {
                        realtime_entry_veto_ok = false;
                        realtime_entry_veto_reason = "blocked_realtime_entry_veto_invalid_orderbook";
                    } else if (snapshot.spread_pct > veto_thresholds.max_spread_pct) {
                        realtime_entry_veto_ok = false;
                        realtime_entry_veto_reason = "blocked_realtime_entry_veto_spread";
                    } else if (snapshot.imbalance < veto_thresholds.min_orderbook_imbalance) {
                        realtime_entry_veto_ok = false;
                        realtime_entry_veto_reason = "blocked_realtime_entry_veto_imbalance";
                    } else {
                        const double max_drop_pct = std::max(0.0001, veto_thresholds.max_drop_pct);
                        const double drop_vs_signal = (best_signal.entry_price > 0.0)
                            ? ((best_signal.entry_price - snapshot.best_ask) / best_signal.entry_price)
                            : 0.0;
                        if (drop_vs_signal > max_drop_pct) {
                            realtime_entry_veto_ok = false;
                            realtime_entry_veto_reason = "blocked_realtime_entry_veto_drop_vs_signal";
                        } else if (recent_best_ask_price_ > 0.0 &&
                                   recent_best_ask_timestamp_ms_ > 0) {
                            const long long age_ms = now_ms - recent_best_ask_timestamp_ms_;
                            const int tracking_window_seconds = std::max(
                                10,
                                engine_config_.realtime_entry_veto_tracking_window_seconds
                            );
                            if (age_ms >= 0 &&
                                age_ms <= static_cast<long long>(tracking_window_seconds) * 1000LL) {
                                const double drop_vs_recent =
                                    (recent_best_ask_price_ - snapshot.best_ask) / recent_best_ask_price_;
                                if (drop_vs_recent > max_drop_pct) {
                                    realtime_entry_veto_ok = false;
                                    realtime_entry_veto_reason = "blocked_realtime_entry_veto_drop_vs_recent";
                                }
                            }
                        }
                    }
                    if (snapshot.valid && snapshot.best_ask > 0.0) {
                        recent_best_ask_price_ = snapshot.best_ask;
                        recent_best_ask_timestamp_ms_ = now_ms;
                    }
                }
                if (!realtime_entry_veto_ok) {
                    entry_funnel_.blocked_risk_gate_other++;
                    const char* veto_reason_code = realtime_entry_veto_reason.empty()
                        ? "blocked_realtime_entry_veto"
                        : realtime_entry_veto_reason.c_str();
                    markEntryReject(veto_reason_code);
                } else {
                const double min_order_krw = std::max(5000.0, engine_config_.min_order_krw);
                const double fee_rate = Config::getInstance().getFeeRate();
                const double stop_guard_pct = [&]() {
                    if (best_signal.entry_price > 0.0 &&
                        best_signal.stop_loss > 0.0 &&
                        best_signal.stop_loss < best_signal.entry_price) {
                        const double rp = (best_signal.entry_price - best_signal.stop_loss) / best_signal.entry_price;
                        return std::clamp(rp, 0.01, 0.20);
                    }
                    return 0.03;
                }();
                const double slip_guard_pct = dynamic_buy_slippage.guard_slippage_pct;
                const double exit_retention = std::max(0.50, 1.0 - stop_guard_pct - slip_guard_pct - fee_rate);
                const double min_safe_order_krw = std::max(min_order_krw, min_order_krw / exit_retention);
                const double min_executable_order_krw =
                    std::ceil(min_safe_order_krw / min_order_krw) * min_order_krw;
                const double available_cash_for_gate = risk_manager_->getRiskMetrics().available_capital;
                const bool has_min_order_capital = available_cash_for_gate >= min_executable_order_krw;
                if (!has_min_order_capital) {
                    entry_funnel_.blocked_min_order_or_capital++;
                    markEntryReject("blocked_min_order_or_capital");
                } else {
                    const double min_position_size_for_order = min_executable_order_krw / available_cash_for_gate;
                    const double effective_position_size = std::clamp(
                        std::max(best_signal.position_size, min_position_size_for_order),
                        0.0,
                        1.0
                    );
                    const double modeled_entry_slippage_pct = entrySlippagePct(engine_config_);
                    if (modeled_entry_slippage_pct > dynamic_buy_slippage.max_slippage_pct) {
                        entry_funnel_.blocked_risk_gate_other++;
                        markEntryReject("blocked_dynamic_slippage_cap");
                    } else {
                        // Validate Risk
                        const bool can_enter_position = risk_manager_->canEnterPosition(
                            market_name_,
                            current_price * (1.0 + modeled_entry_slippage_pct),
                            effective_position_size,
                            best_signal.strategy_name
                        );
                        if (!can_enter_position) {
                            entry_funnel_.blocked_risk_manager++;
                            markEntryReject("blocked_risk_manager");
                            if (!best_signal.strategy_name.empty()) {
                                strategy_blocked_by_risk_manager_counts_[best_signal.strategy_name]++;
                            }
                        } else {
                    // Execute Buy
                    double available_cash = risk_manager_->getRiskMetrics().available_capital;
                    double fill_price = current_price * (1.0 + modeled_entry_slippage_pct);
                    const double fee_rate_exec = Config::getInstance().getFeeRate();
                    const double fee_reserve = std::clamp(engine_config_.order_fee_reserve_pct, 0.0, 0.02);
                    const double spendable_capital = available_cash / (1.0 + fee_reserve);
                    const bool capital_gate_ok = spendable_capital >= min_executable_order_krw;

                    if (capital_gate_ok) {
                        double desired_order_krw = available_cash * effective_position_size;
                        double max_order_krw = std::min(engine_config_.max_order_krw, spendable_capital);
                        if (available_cash <= engine_config_.small_account_tier1_capital_krw) {
                            const double tier_cap = std::clamp(engine_config_.small_account_tier1_max_order_pct, 0.01, 1.0);
                            max_order_krw = std::min(max_order_krw, std::max(min_executable_order_krw, available_cash * tier_cap));
                        } else if (available_cash <= engine_config_.small_account_tier2_capital_krw) {
                            const double tier_cap = std::clamp(engine_config_.small_account_tier2_max_order_pct, 0.01, 1.0);
                            max_order_krw = std::min(max_order_krw, std::max(min_executable_order_krw, available_cash * tier_cap));
                        }

                        const double order_amount = std::clamp(desired_order_krw, min_executable_order_krw, max_order_krw);
                        const double quantity = order_amount / (fill_price * (1.0 + fee_rate_exec));
                        const double fee = quantity * fill_price * fee_rate_exec;
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
                            const double round_trip_cost = computeEffectiveRoundTripCostPct(best_signal, engine_config_);
                            const double net_edge = reward_pct - round_trip_cost;
                            const double calibrated_edge = computeCalibratedExpectedEdgePct(best_signal, engine_config_);
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
                                best_signal.entry_archetype,
                                best_signal.probabilistic_runtime_applied,
                                best_signal.probabilistic_h1_calibrated,
                                best_signal.probabilistic_h5_calibrated,
                                best_signal.probabilistic_h5_threshold,
                                best_signal.probabilistic_h5_margin
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
                            markEntryReject("blocked_order_sizing");
                        }
                    } else {
                        entry_funnel_.blocked_min_order_or_capital++;
                        markEntryReject("blocked_min_order_or_capital");
                    }
                        }
                    }
                }
                }
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
    }
    else {
        if (!was_open_position_prev_candle_) {
            entry_funnel_.skipped_due_to_open_position++;
            entry_rejection_reason_counts_["skipped_due_to_open_position"]++;
        }
    }
    was_open_position_prev_candle_ = (risk_manager_->getPosition(market_name_) != nullptr);

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

            const std::string regime_label = marketRegimeLabel(trade.market_regime);
            const std::string archetype_label =
                trade.entry_archetype.empty() ? "UNSPECIFIED" : trade.entry_archetype;
            const std::string vol_bucket = volatilityBucket(trade.volatility);
            const std::string liq_bucket = liquidityBucket(trade.liquidity_score);
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
                vol_bucket + "|" + liq_bucket + "|" +
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
            const size_t p6 = (p5 == std::string::npos) ? std::string::npos : key.find('|', p5 + 1);
            const size_t p7 = (p6 == std::string::npos) ? std::string::npos : key.find('|', p6 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos ||
                p4 == std::string::npos || p5 == std::string::npos ||
                p6 == std::string::npos || p7 == std::string::npos) {
                continue;
            }
            Result::PatternSummary ps;
            ps.strategy_name = key.substr(0, p1);
            ps.entry_archetype = key.substr(p1 + 1, p2 - p1 - 1);
            ps.regime = key.substr(p2 + 1, p3 - p2 - 1);
            ps.volatility_bucket = key.substr(p3 + 1, p4 - p3 - 1);
            ps.liquidity_bucket = key.substr(p4 + 1, p5 - p4 - 1);
            ps.strength_bucket = key.substr(p5 + 1, p6 - p5 - 1);
            ps.expected_value_bucket = key.substr(p6 + 1, p7 - p6 - 1);
            ps.reward_risk_bucket = key.substr(p7 + 1);
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
                if (a.volatility_bucket != b.volatility_bucket) return a.volatility_bucket < b.volatility_bucket;
                if (a.liquidity_bucket != b.liquidity_bucket) return a.liquidity_bucket < b.liquidity_bucket;
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
    result.entry_rejection_reason_counts = entry_rejection_reason_counts_;
    result.no_signal_pattern_counts = no_signal_pattern_counts_;
    result.entry_quality_edge_gap_buckets = entry_quality_edge_gap_buckets_;
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
    for (const auto& [name, _] : strategy_skipped_disabled_counts_) {
        strategy_names.insert(name);
    }
    for (const auto& [name, _] : strategy_no_signal_counts_) {
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

        Result::StrategyCollectionSummary collect_item;
        collect_item.strategy_name = strategy_name;
        collect_item.skipped_disabled = strategy_skipped_disabled_counts_.count(strategy_name)
            ? strategy_skipped_disabled_counts_.at(strategy_name) : 0;
        collect_item.no_signal = strategy_no_signal_counts_.count(strategy_name)
            ? strategy_no_signal_counts_.at(strategy_name) : 0;
        collect_item.generated = strategy_generated_counts_.count(strategy_name)
            ? strategy_generated_counts_.at(strategy_name) : 0;
        result.strategy_collection_summaries.push_back(std::move(collect_item));
    }
    std::sort(result.strategy_signal_funnel.begin(), result.strategy_signal_funnel.end(),
        [](const Result::StrategySignalFunnel& a, const Result::StrategySignalFunnel& b) {
            if (a.generated_signals != b.generated_signals) {
                return a.generated_signals > b.generated_signals;
            }
            return a.strategy_name < b.strategy_name;
        });
    std::sort(result.strategy_collection_summaries.begin(), result.strategy_collection_summaries.end(),
        [](const Result::StrategyCollectionSummary& a, const Result::StrategyCollectionSummary& b) {
            if (a.no_signal != b.no_signal) {
                return a.no_signal > b.no_signal;
            }
            if (a.generated != b.generated) {
                return a.generated > b.generated;
            }
            return a.strategy_name < b.strategy_name;
        });
    result.strategy_collect_exception_count = strategy_collect_exception_count_;
    result.entry_funnel = entry_funnel_;
    result.shadow_funnel = shadow_funnel_;
    if (result.shadow_funnel.rounds > 0) {
        const double rounds = static_cast<double>(result.shadow_funnel.rounds);
        result.shadow_funnel.avg_manager_supply_lift =
            result.shadow_funnel.manager_supply_lift_sum / rounds;
        result.shadow_funnel.avg_policy_supply_lift =
            result.shadow_funnel.policy_supply_lift_sum / rounds;
    } else {
        result.shadow_funnel.avg_manager_supply_lift = 0.0;
        result.shadow_funnel.avg_policy_supply_lift = 0.0;
    }
    result.pre_cat_feature_snapshot = pre_cat_feature_snapshot_;
    result.post_entry_risk_telemetry.adaptive_stop_updates = adaptive_stop_update_count_;
    result.post_entry_risk_telemetry.adaptive_tp_recalibration_updates = adaptive_tp_recalibration_count_;
    result.post_entry_risk_telemetry.adaptive_partial_ratio_samples = adaptive_partial_ratio_samples_;
    result.post_entry_risk_telemetry.adaptive_partial_ratio_sum = adaptive_partial_ratio_sum_;
    result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram = adaptive_partial_ratio_histogram_;
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










