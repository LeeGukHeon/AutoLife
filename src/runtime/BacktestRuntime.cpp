#include "runtime/BacktestRuntime.h"
#include "common/Logger.h"
#include "common/MarketDataWindowPolicy.h"
#include "common/PathUtils.h"
#include "common/RuntimeDiagnosticsShared.h"
#include "common/SignalPolicyShared.h"
#include "common/StrategyEdgeStatsShared.h"
#include "common/ExecutionGuardPolicyShared.h"
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
// Core execution plane should not be penalized versus legacy in backtest cost modeling.
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
using autolife::common::signal_policy::computeContextualEdgeGateFloor;
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
    double threshold_h5 = 0.6;
    double margin_h5 = 0.0;
    double online_margin_bias = 0.0;
    double online_strength_gain = 1.0;
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
    if (!model.hasMarket(market)) {
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
    out_snapshot.threshold_h5 = std::clamp(inference.selection_threshold_h5, 0.0, 1.0);
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

    if (cfg.probabilistic_runtime_scan_prefilter_enabled) {
        const double gate_margin = effectiveProbabilisticScanPrefilterMargin(cfg, regime);
        if (out_snapshot.margin_h5 < gate_margin) {
            if (reject_reason != nullptr) {
                *reject_reason = "probabilistic_market_prefilter";
            }
            return false;
        }
    }
    return true;
}

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

    const double score_weight = std::clamp(cfg.probabilistic_runtime_score_weight, 0.0, 1.0);
    signal.score += std::clamp(effective_margin * score_weight, -0.12, 0.12);
    signal.signal_filter = std::clamp(signal.signal_filter + (effective_margin * 0.10), 0.0, 1.0);
    signal.expected_value += effective_margin * std::clamp(
        cfg.probabilistic_runtime_expected_edge_weight,
        0.0,
        0.01
    );

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
    const autolife::strategy::Signal& signal
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
    return std::clamp(scale, 0.45, 1.45);
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
    const bool hostile_regime =
        regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == autolife::analytics::MarketRegime::TRENDING_DOWN;
    const double min_margin = hostile_regime ? 0.022 : -0.030;
    const double min_prob = hostile_regime ? 0.58 : 0.45;
    const double min_liquidity = hostile_regime ? 52.0 : 18.0;
    const double min_volume_surge = hostile_regime ? 0.80 : 0.20;
    const double max_spread = hostile_regime ? 0.0025 : 0.0060;

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

bool requiresTypedArchetype(const std::string& strategy_name) {
    return autolife::common::signal_policy::requiresTypedArchetype(strategy_name);
}

bool isAlphaHeadFallbackCandidate(const strategy::Signal& signal, bool alpha_head_mode) {
    return autolife::common::signal_policy::isAlphaHeadFallbackCandidate(signal, alpha_head_mode);
}

void normalizeSignalStopLossByRegime(strategy::Signal& signal, analytics::MarketRegime regime) {
    autolife::common::signal_policy::normalizeSignalStopLossByRegime(signal, regime);
}

struct EntryQualityGateSnapshot {
    enum class FailureKind {
        None,
        InvalidPriceLevels,
        RewardRiskBase,
        RewardRiskAdaptive,
        ExpectedEdgeBase,
        ExpectedEdgeAdaptive,
        RewardRiskAndExpectedEdgeBase,
        RewardRiskAndExpectedEdgeAdaptive,
    };
    enum class AdaptiveSource {
        Unknown,
        History,
        Regime,
        Mixed,
    };

    bool pass = false;
    FailureKind failure = FailureKind::None;
    AdaptiveSource adaptive_rr_source = AdaptiveSource::Unknown;
    AdaptiveSource adaptive_edge_source = AdaptiveSource::Unknown;
    double reward_risk_ratio = 0.0;
    double calibrated_expected_edge_pct = std::numeric_limits<double>::lowest();
    double reward_risk_gate_base = 0.0;
    double reward_risk_gate_effective = 0.0;
    double expected_edge_gate_base = 0.0;
    double expected_edge_gate_effective = 0.0;
    bool adaptive_relief_applied = false;
};

EntryQualityGateSnapshot evaluateEntryQualityGate(
    const strategy::Signal& signal,
    const engine::EngineConfig& cfg,
    double rr_gate_base,
    double edge_gate_base,
    double rr_adder_history,
    double rr_adder_regime,
    double edge_adder_history,
    double edge_adder_regime
) {
    EntryQualityGateSnapshot snapshot;
    snapshot.reward_risk_gate_base = rr_gate_base;
    snapshot.reward_risk_gate_effective = cfg.min_reward_risk;
    snapshot.expected_edge_gate_base = edge_gate_base;
    snapshot.expected_edge_gate_effective = cfg.min_expected_edge_pct;
    const double entry_price = signal.entry_price;
    const double take_profit_price =
        (signal.take_profit_2 > 0.0) ? signal.take_profit_2 : signal.take_profit_1;
    const double stop_loss_price = signal.stop_loss;

    if (entry_price <= 0.0 || take_profit_price <= 0.0 || stop_loss_price <= 0.0 ||
        take_profit_price <= entry_price || stop_loss_price >= entry_price) {
        snapshot.failure = EntryQualityGateSnapshot::FailureKind::InvalidPriceLevels;
        return snapshot;
    }

    const double gross_reward_pct = (take_profit_price - entry_price) / entry_price;
    const double gross_risk_pct = (entry_price - stop_loss_price) / entry_price;
    snapshot.reward_risk_ratio = (gross_risk_pct > 1e-8) ? (gross_reward_pct / gross_risk_pct) : 0.0;
    snapshot.calibrated_expected_edge_pct = computeCalibratedExpectedEdgePct(signal, cfg);

    const bool rr_ok = snapshot.reward_risk_ratio >= cfg.min_reward_risk;
    const bool rr_base_ok = snapshot.reward_risk_ratio >= rr_gate_base;
    const bool edge_ok = snapshot.calibrated_expected_edge_pct >= cfg.min_expected_edge_pct;
    const bool edge_base_ok = snapshot.calibrated_expected_edge_pct >= edge_gate_base;
    snapshot.pass = rr_ok && edge_ok;
    if (snapshot.pass) {
        snapshot.failure = EntryQualityGateSnapshot::FailureKind::None;
    } else if (!rr_ok && !edge_ok) {
        snapshot.failure = rr_base_ok
            ? EntryQualityGateSnapshot::FailureKind::RewardRiskAndExpectedEdgeAdaptive
            : EntryQualityGateSnapshot::FailureKind::RewardRiskAndExpectedEdgeBase;
    } else if (!rr_ok) {
        snapshot.failure = rr_base_ok
            ? EntryQualityGateSnapshot::FailureKind::RewardRiskAdaptive
            : EntryQualityGateSnapshot::FailureKind::RewardRiskBase;
    } else {
        snapshot.failure = edge_base_ok
            ? EntryQualityGateSnapshot::FailureKind::ExpectedEdgeAdaptive
            : EntryQualityGateSnapshot::FailureKind::ExpectedEdgeBase;
    }
    const auto classifyAdaptiveSource = [](double history_add, double regime_add) {
        const double history_positive = std::max(0.0, history_add);
        const double regime_positive = std::max(0.0, regime_add);
        if (history_positive <= 1e-9 && regime_positive <= 1e-9) {
            return EntryQualityGateSnapshot::AdaptiveSource::Unknown;
        }
        if (history_positive >= (regime_positive * 1.2)) {
            return EntryQualityGateSnapshot::AdaptiveSource::History;
        }
        if (regime_positive >= (history_positive * 1.2)) {
            return EntryQualityGateSnapshot::AdaptiveSource::Regime;
        }
        return EntryQualityGateSnapshot::AdaptiveSource::Mixed;
    };
    if (snapshot.failure == EntryQualityGateSnapshot::FailureKind::RewardRiskAdaptive ||
        snapshot.failure == EntryQualityGateSnapshot::FailureKind::RewardRiskAndExpectedEdgeAdaptive) {
        snapshot.adaptive_rr_source = classifyAdaptiveSource(rr_adder_history, rr_adder_regime);
    }
    if (snapshot.failure == EntryQualityGateSnapshot::FailureKind::ExpectedEdgeAdaptive ||
        snapshot.failure == EntryQualityGateSnapshot::FailureKind::RewardRiskAndExpectedEdgeAdaptive) {
        snapshot.adaptive_edge_source = classifyAdaptiveSource(edge_adder_history, edge_adder_regime);
    }

    if (!snapshot.pass && cfg.enable_entry_quality_adaptive_relief) {
        const bool adaptive_only_failure =
            snapshot.failure == EntryQualityGateSnapshot::FailureKind::RewardRiskAdaptive ||
            snapshot.failure == EntryQualityGateSnapshot::FailureKind::ExpectedEdgeAdaptive ||
            snapshot.failure == EntryQualityGateSnapshot::FailureKind::RewardRiskAndExpectedEdgeAdaptive;
        const bool high_stress_regime =
            signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
        const bool relief_regime_ok =
            !cfg.entry_quality_adaptive_relief_block_high_stress_regime || !high_stress_regime;
        const bool has_min_history =
            signal.strategy_trade_count >= std::max(0, cfg.entry_quality_adaptive_relief_min_strategy_trades);
        const bool history_win_ok =
            signal.strategy_win_rate >= cfg.entry_quality_adaptive_relief_min_strategy_win_rate;
        const bool history_pf_ok =
            signal.strategy_profit_factor <= 0.0 ||
            signal.strategy_profit_factor >= cfg.entry_quality_adaptive_relief_min_strategy_profit_factor;
        const bool quality_ok =
            signal.strength >= cfg.entry_quality_adaptive_relief_min_signal_strength &&
            signal.expected_value >= cfg.entry_quality_adaptive_relief_min_expected_value &&
            signal.liquidity_score >= cfg.entry_quality_adaptive_relief_min_liquidity_score &&
            has_min_history &&
            history_win_ok &&
            history_pf_ok &&
            relief_regime_ok;
        if (adaptive_only_failure && quality_ok) {
            const double rr_relax = std::clamp(
                cfg.entry_quality_adaptive_relief_rr_max_gap,
                0.0,
                0.25
            );
            const double edge_relax = std::clamp(
                cfg.entry_quality_adaptive_relief_edge_max_gap,
                0.0,
                0.0010
            );
            const double relieved_rr_gate = std::max(rr_gate_base, cfg.min_reward_risk - rr_relax);
            const double relieved_edge_gate = std::max(
                edge_gate_base,
                cfg.min_expected_edge_pct - edge_relax
            );
            const bool rr_relief_ok = snapshot.reward_risk_ratio >= relieved_rr_gate;
            const bool edge_relief_ok = snapshot.calibrated_expected_edge_pct >= relieved_edge_gate;
            if (rr_relief_ok && edge_relief_ok) {
                snapshot.pass = true;
                snapshot.failure = EntryQualityGateSnapshot::FailureKind::None;
                snapshot.reward_risk_gate_effective = relieved_rr_gate;
                snapshot.expected_edge_gate_effective = relieved_edge_gate;
                snapshot.adaptive_relief_applied = true;
            }
        }
    }

    // Bounded near-miss relief for thin-liquidity uptrend signals:
    // allow only tiny base-edge shortfalls when RR surplus and signal quality are strong.
    if (!snapshot.pass &&
        snapshot.failure == EntryQualityGateSnapshot::FailureKind::ExpectedEdgeBase) {
        const bool thin_uptrend =
            signal.market_regime == analytics::MarketRegime::TRENDING_UP &&
            signal.liquidity_score >= 40.0 &&
            signal.liquidity_score <= 52.0;
        if (thin_uptrend) {
            const double edge_gap = edge_gate_base - snapshot.calibrated_expected_edge_pct;
            const bool tiny_edge_gap = edge_gap > 0.0 && edge_gap <= 0.00008;
            const bool strong_quality =
                signal.strength >= 0.66 &&
                signal.expected_value >= std::max(0.00030, cfg.entry_quality_adaptive_relief_min_expected_value * 0.85);
            const bool rr_surplus = snapshot.reward_risk_ratio >= (rr_gate_base + 0.10);
            if (tiny_edge_gap && strong_quality && rr_surplus) {
                snapshot.pass = true;
                snapshot.failure = EntryQualityGateSnapshot::FailureKind::None;
                snapshot.expected_edge_gate_effective = snapshot.calibrated_expected_edge_pct;
                snapshot.adaptive_relief_applied = true;
            }
        }
    }

    // Probabilistic-primary near-miss relief:
    // in non-hostile regimes, allow a bounded base-edge shortfall when
    // probabilistic margin/calibrated confidence are supportive.
    if (!snapshot.pass &&
        snapshot.failure == EntryQualityGateSnapshot::FailureKind::ExpectedEdgeBase &&
        cfg.enable_probabilistic_runtime_model &&
        cfg.probabilistic_runtime_primary_mode &&
        signal.probabilistic_runtime_applied) {
        const bool hostile_regime =
            signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
        if (!hostile_regime) {
            const double edge_gap = edge_gate_base - snapshot.calibrated_expected_edge_pct;
            const double margin = signal.probabilistic_h5_margin;
            const double calibrated = signal.probabilistic_h5_calibrated;
            double max_edge_gap = std::clamp((margin + 0.01) / 0.12, 0.0, 1.0) * 0.00016;
            if (signal.market_regime == analytics::MarketRegime::RANGING) {
                max_edge_gap *= 1.15;
            }
            const bool quality_ok =
                signal.strength >= 0.62 &&
                signal.expected_value >= 0.00018 &&
                signal.liquidity_score >= 40.0 &&
                calibrated >= 0.50;
            const bool rr_prob_ok = snapshot.reward_risk_ratio >= (rr_gate_base + 0.02);
            if (quality_ok && rr_prob_ok && edge_gap > 0.0 && edge_gap <= max_edge_gap) {
                snapshot.pass = true;
                snapshot.failure = EntryQualityGateSnapshot::FailureKind::None;
                snapshot.expected_edge_gate_effective = snapshot.calibrated_expected_edge_pct;
                snapshot.adaptive_relief_applied = true;
            }
        }
    }
    return snapshot;
}

const char* entryQualityFailureReasonCode(const EntryQualityGateSnapshot& snapshot) {
    switch (snapshot.failure) {
        case EntryQualityGateSnapshot::FailureKind::InvalidPriceLevels:
            return "blocked_risk_gate_entry_quality_invalid_levels";
        case EntryQualityGateSnapshot::FailureKind::RewardRiskBase:
            return "blocked_risk_gate_entry_quality_rr_base";
        case EntryQualityGateSnapshot::FailureKind::RewardRiskAdaptive:
            switch (snapshot.adaptive_rr_source) {
                case EntryQualityGateSnapshot::AdaptiveSource::History:
                    return "blocked_risk_gate_entry_quality_rr_adaptive_history";
                case EntryQualityGateSnapshot::AdaptiveSource::Regime:
                    return "blocked_risk_gate_entry_quality_rr_adaptive_regime";
                case EntryQualityGateSnapshot::AdaptiveSource::Mixed:
                    return "blocked_risk_gate_entry_quality_rr_adaptive_mixed";
                case EntryQualityGateSnapshot::AdaptiveSource::Unknown:
                default:
                    return "blocked_risk_gate_entry_quality_rr_adaptive";
            }
        case EntryQualityGateSnapshot::FailureKind::ExpectedEdgeBase:
            return "blocked_risk_gate_entry_quality_edge_base";
        case EntryQualityGateSnapshot::FailureKind::ExpectedEdgeAdaptive:
            switch (snapshot.adaptive_edge_source) {
                case EntryQualityGateSnapshot::AdaptiveSource::History:
                    return "blocked_risk_gate_entry_quality_edge_adaptive_history";
                case EntryQualityGateSnapshot::AdaptiveSource::Regime:
                    return "blocked_risk_gate_entry_quality_edge_adaptive_regime";
                case EntryQualityGateSnapshot::AdaptiveSource::Mixed:
                    return "blocked_risk_gate_entry_quality_edge_adaptive_mixed";
                case EntryQualityGateSnapshot::AdaptiveSource::Unknown:
                default:
                    return "blocked_risk_gate_entry_quality_edge_adaptive";
            }
        case EntryQualityGateSnapshot::FailureKind::RewardRiskAndExpectedEdgeBase:
            return "blocked_risk_gate_entry_quality_rr_edge_base";
        case EntryQualityGateSnapshot::FailureKind::RewardRiskAndExpectedEdgeAdaptive:
            switch (snapshot.adaptive_rr_source) {
                case EntryQualityGateSnapshot::AdaptiveSource::History:
                    return "blocked_risk_gate_entry_quality_rr_edge_adaptive_history";
                case EntryQualityGateSnapshot::AdaptiveSource::Regime:
                    return "blocked_risk_gate_entry_quality_rr_edge_adaptive_regime";
                case EntryQualityGateSnapshot::AdaptiveSource::Mixed:
                    return "blocked_risk_gate_entry_quality_rr_edge_adaptive_mixed";
                case EntryQualityGateSnapshot::AdaptiveSource::Unknown:
                default:
                    return "blocked_risk_gate_entry_quality_rr_edge_adaptive";
            }
        case EntryQualityGateSnapshot::FailureKind::None:
        default:
            return "blocked_risk_gate_entry_quality";
    }
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
    const engine::EngineConfig& cfg,
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

double computeEntryQualityHeadScore(const EntryQualityGateSnapshot& snapshot) {
    const double rr_score = computeRatioScore(
        snapshot.reward_risk_ratio,
        snapshot.reward_risk_gate_effective
    );
    const double edge_score = computeRatioScore(
        snapshot.calibrated_expected_edge_pct,
        snapshot.expected_edge_gate_effective
    );
    return std::clamp(std::min(rr_score, edge_score), 0.0, 2.5);
}

SecondStageHistorySafetySeverity classifySecondStageHistorySafetySeverity(
    const strategy::Signal& signal
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

SecondStageConfirmationSnapshot evaluateSecondStageEntryConfirmation(
    const strategy::Signal& signal,
    const engine::EngineConfig& cfg,
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

    if (signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
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
                    signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                    signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
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
    if (signal.market_regime == analytics::MarketRegime::TRENDING_UP &&
        signal.strength >= 0.74 &&
        signal.liquidity_score >= 62.0) {
        min_rr_margin -= 0.02;
        min_edge_margin -= 0.00003;
    }
    if (signal.reason == "alpha_head_fallback_candidate" &&
        (signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
         signal.market_regime == analytics::MarketRegime::RANGING) &&
        signal.liquidity_score >= 58.0) {
        // Alpha-head fallback is an exploration path; keep margin checks
        // but avoid over-pruning in favorable liquid regimes.
        min_rr_margin -= 0.015;
        min_edge_margin -= 0.00002;
    }

    const bool hostile_regime =
        signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
    const bool favorable_regime =
        signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
        signal.market_regime == analytics::MarketRegime::RANGING;

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
    const strategy::Signal& signal,
    const engine::EngineConfig& cfg,
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
    const strategy::Signal& signal,
    const engine::EngineConfig& cfg,
    const EntryQualityGateSnapshot& entry_quality_eval,
    const SecondStageConfirmationSnapshot& second_stage_eval
) {
    TwoHeadEntryAggregationSnapshot snapshot;
    snapshot.enabled = cfg.enable_two_head_entry_second_stage_aggregation;
    snapshot.entry_head_pass = entry_quality_eval.pass;
    snapshot.second_stage_head_pass = second_stage_eval.pass;
    snapshot.entry_head_score = computeEntryQualityHeadScore(entry_quality_eval);
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
        signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
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

bool shouldAttributeTwoHeadFailureToSecondStage(
    const TwoHeadEntryAggregationSnapshot& snapshot
) {
    if (!snapshot.entry_head_pass && snapshot.second_stage_head_pass) {
        return false;
    }
    if (snapshot.entry_head_pass && !snapshot.second_stage_head_pass) {
        return true;
    }
    if (!snapshot.entry_head_pass && !snapshot.second_stage_head_pass) {
        return snapshot.second_stage_deficit >= snapshot.entry_deficit;
    }
    return false;
}

void applyArchetypeRiskAdjustments(
    const strategy::Signal& signal,
    double& required_signal_strength,
    double& regime_rr_add,
    double& regime_edge_add,
    bool& regime_pattern_block
) {
    autolife::common::signal_policy::applyArchetypeRiskAdjustments(
        signal,
        required_signal_strength,
        regime_rr_add,
        regime_edge_add,
        regime_pattern_block
    );
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
        const bool alpha_head_mode = engine_config_.use_strategy_alpha_head_mode;
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
        if (alpha_head_mode) {
            // Alpha-head mode mirrors live multi-candidate flow but still applies
            // a permissive quality prefilter to avoid extremely weak signals.
            primary_manager_min_strength = 0.34;
            primary_manager_min_expected_value = -0.00015;
            if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
                primary_manager_min_strength = 0.42;
                primary_manager_min_expected_value = 0.00005;
            } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
                primary_manager_min_strength = 0.45;
                primary_manager_min_expected_value = 0.00008;
            } else if (regime.regime == analytics::MarketRegime::RANGING) {
                primary_manager_min_strength = 0.36;
                primary_manager_min_expected_value = -0.00008;
            }
        }
        if (engine_config_.probabilistic_runtime_primary_mode && probabilistic_snapshot.applied) {
            const bool hostile_regime =
                regime.regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                regime.regime == analytics::MarketRegime::TRENDING_DOWN;
            const double margin = probabilistic_snapshot.margin_h5;
            const double relax_strength = hostile_regime
                ? std::clamp((margin + 0.02) / 0.20, 0.0, 1.0) * 0.05
                : std::clamp((margin + 0.06) / 0.20, 0.0, 1.0) * 0.14;
            const double relax_edge = hostile_regime
                ? std::clamp((margin + 0.03) / 0.22, 0.0, 1.0) * 0.00010
                : std::clamp((margin + 0.06) / 0.20, 0.0, 1.0) * 0.00045;
            const double strength_floor = hostile_regime ? 0.38 : 0.22;
            const double edge_floor = hostile_regime ? 0.0 : -0.00045;
            primary_manager_min_strength = std::max(
                strength_floor,
                primary_manager_min_strength - relax_strength
            );
            primary_manager_min_expected_value = std::max(
                edge_floor,
                primary_manager_min_expected_value - relax_edge
            );
        }
        std::vector<strategy::Signal> filtered_signals;
        strategy::StrategyManager::FilterDiagnostics manager_filter_diag;
        filtered_signals = strategy_manager_->filterSignalsWithDiagnostics(
            signals,
            primary_manager_min_strength,
            primary_manager_min_expected_value,
            regime.regime,
            &manager_filter_diag
        );
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
            candidate_signals = std::move(policy_output.selected_candidates);
        }
        if (core_policy_enabled && !filtered_signals.empty() && candidate_signals.empty()) {
            entry_funnel_.filtered_out_by_policy++;
            markEntryReject("filtered_out_by_policy");
        }

        strategy::Signal best_signal;
        strategy::StrategyManager::SelectionDiagnostics manager_select_diag;
        if (!candidate_signals.empty()) {
            if (core_bridge_enabled) {
                best_signal = strategy_manager_->selectRobustSignalWithDiagnostics(
                    candidate_signals,
                    regime.regime,
                    &manager_select_diag
                );
            } else {
                best_signal = strategy_manager_->selectBestSignal(candidate_signals);
            }
        }
        if (!candidate_signals.empty() && best_signal.type == strategy::SignalType::NONE) {
            entry_funnel_.no_best_signal++;
            markEntryReject("no_best_signal");
            mergeEntryRejectCounts(manager_select_diag.rejection_reason_counts);
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
            shadow_funnel_.primary_best_signal_available++;
        }
        if (!signals.empty()) {
            constexpr double kShadowStrengthRelax = 0.06;
            constexpr double kShadowExpectedValueRelax = 0.00012;
            const double shadow_min_strength = std::clamp(
                primary_manager_min_strength - kShadowStrengthRelax,
                0.25,
                0.95
            );
            const double shadow_min_expected_value = std::clamp(
                primary_manager_min_expected_value - kShadowExpectedValueRelax,
                -0.00035,
                0.0050
            );

            strategy::StrategyManager::FilterDiagnostics shadow_filter_diag;
            auto shadow_filtered_signals = strategy_manager_->filterSignalsWithDiagnostics(
                signals,
                shadow_min_strength,
                shadow_min_expected_value,
                regime.regime,
                &shadow_filter_diag
            );

            std::vector<strategy::Signal> shadow_candidate_signals = shadow_filtered_signals;
            if (core_policy_enabled && policy_controller_) {
                engine::PolicyInput shadow_policy_input;
                shadow_policy_input.candidates = shadow_filtered_signals;
                shadow_policy_input.small_seed_mode = small_seed_mode;
                shadow_policy_input.max_new_orders_per_scan = 1;
                shadow_policy_input.dominant_regime = regime.regime;
                if (performance_store_) {
                    shadow_policy_input.strategy_stats = &performance_store_->byStrategy();
                    shadow_policy_input.bucket_stats = &performance_store_->byBucket();
                }
                auto shadow_policy_output = policy_controller_->selectCandidates(shadow_policy_input);
                shadow_candidate_signals = std::move(shadow_policy_output.selected_candidates);
            }

            strategy::Signal shadow_best_signal;
            strategy::StrategyManager::SelectionDiagnostics shadow_select_diag;
            if (!shadow_candidate_signals.empty()) {
                if (core_bridge_enabled) {
                    shadow_best_signal = strategy_manager_->selectRobustSignalWithDiagnostics(
                        shadow_candidate_signals,
                        regime.regime,
                        &shadow_select_diag
                    );
                } else {
                    shadow_best_signal = strategy_manager_->selectBestSignal(shadow_candidate_signals);
                }
            }

            const int primary_manager_count = static_cast<int>(filtered_signals.size());
            const int primary_policy_count = static_cast<int>(candidate_signals.size());
            const int shadow_manager_count = static_cast<int>(shadow_filtered_signals.size());
            const int shadow_policy_count = static_cast<int>(shadow_candidate_signals.size());

            shadow_funnel_.shadow_after_manager_filter += shadow_manager_count;
            shadow_funnel_.shadow_after_policy_filter += shadow_policy_count;
            if (shadow_best_signal.type != strategy::SignalType::NONE) {
                shadow_funnel_.shadow_best_signal_available++;
            }
            if (shadow_policy_count > primary_policy_count) {
                shadow_funnel_.supply_improved_rounds++;
            }

            const double manager_base = std::max(1.0, static_cast<double>(signals.size()));
            const double policy_base = std::max(1.0, static_cast<double>(filtered_signals.size()));
            shadow_funnel_.manager_supply_lift_sum +=
                (static_cast<double>(shadow_manager_count - primary_manager_count) / manager_base);
            shadow_funnel_.policy_supply_lift_sum +=
                (static_cast<double>(shadow_policy_count - primary_policy_count) / policy_base);
        }

        const auto trade_history = risk_manager_->getTradeHistory();
        const int recent_strategy_window = std::max(16, engine_config_.min_strategy_trades_for_ev);
        const int recent_regime_window = std::max(8, engine_config_.min_strategy_trades_for_ev / 2);
        const int recent_market_regime_window = std::max(6, engine_config_.min_strategy_trades_for_ev / 2);
        const auto strategy_edge = buildStrategyEdgeStats(trade_history);
        const auto strategy_edge_recent = buildStrategyEdgeStats(trade_history, recent_strategy_window);
        const auto strategy_regime_edge = buildStrategyRegimeEdgeStats(trade_history);
        const auto strategy_regime_edge_recent = buildStrategyRegimeEdgeStats(
            trade_history,
            recent_regime_window
        );
        const auto market_strategy_regime_edge = buildMarketStrategyRegimeEdgeStats(trade_history);
        const auto market_strategy_regime_edge_recent = buildMarketStrategyRegimeEdgeStats(
            trade_history,
            recent_market_regime_window
        );

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
            const bool alpha_head_fallback_candidate =
                isAlphaHeadFallbackCandidate(best_signal, alpha_head_mode);
            const double history_penalty_scale = alpha_head_fallback_candidate ? 0.45 : 1.0;
            bool alpha_head_relief_eligible = true;
            const bool favorable_recovery_signal =
                no_entry_streak_candles_ >= 45 &&
                (best_signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
                 best_signal.market_regime == analytics::MarketRegime::RANGING) &&
                best_signal.strength >= 0.72 &&
                best_signal.liquidity_score >= 60.0;
            bool strategy_ev_ok = true;
            enum class StrategyEvFailKind {
                None = 0,
                PreCatastrophicNoRecovery,
                SevereThresholdNoRecovery,
                CatastrophicHistory,
                LossAsymmetryCollapse,
            };
            StrategyEvFailKind strategy_ev_fail_kind = StrategyEvFailKind::None;
            auto failStrategyEv = [&](StrategyEvFailKind kind) {
                if (strategy_ev_fail_kind == StrategyEvFailKind::None) {
                    strategy_ev_fail_kind = kind;
                }
                strategy_ev_ok = false;
            };
            double adaptive_rr_add_history = 0.0;
            double adaptive_rr_add_regime = 0.0;
            double adaptive_edge_add_history = 0.0;
            double adaptive_edge_add_regime = 0.0;
            auto addAdaptiveHistory = [&](double rr_add, double edge_add) {
                adaptive_rr_add_history += rr_add;
                adaptive_edge_add_history += edge_add;
            };
            auto addAdaptiveRegime = [&](double rr_add, double edge_add) {
                adaptive_rr_add_regime += rr_add;
                adaptive_edge_add_regime += edge_add;
            };
            double required_strength_floor = filter_threshold;
            if (alpha_head_fallback_candidate) {
                required_strength_floor = std::min(required_strength_floor, 0.58);
            }
            bool regime_pattern_block = false;
            auto markPatternBlockOrSoften = [&](int trades, double exp_krw, double wr, double pf) {
                if (!alpha_head_fallback_candidate) {
                    regime_pattern_block = true;
                    return;
                }
                // In alpha-head fallback mode, require stronger evidence before full hard block.
                if (trades >= 18 && exp_krw <= -28.0 && wr <= 0.18 && pf <= 0.70) {
                    regime_pattern_block = true;
                    return;
                }
                required_strength_floor = std::max(required_strength_floor, 0.60);
                addAdaptiveRegime(0.10, 0.00015);
            };
            auto stat_it = strategy_edge.find(best_signal.strategy_name);
            if (core_risk_enabled && stat_it != strategy_edge.end()) {
                const auto& stat = stat_it->second;
                const auto recent_stat_it = strategy_edge_recent.find(best_signal.strategy_name);
                const auto regime_ev_it = strategy_regime_edge.find(
                    makeStrategyRegimeKey(best_signal.strategy_name, best_signal.market_regime)
                );
                const bool has_recent_recovery =
                    recent_stat_it != strategy_edge_recent.end() &&
                    recent_stat_it->second.trades >= std::max(10, engine_config_.min_strategy_trades_for_ev / 2) &&
                    recent_stat_it->second.expectancy() >= (engine_config_.min_strategy_expectancy_krw + 1.0) &&
                    recent_stat_it->second.profitFactor() >=
                        std::max(1.00, engine_config_.min_strategy_profit_factor) &&
                    recent_stat_it->second.winRate() >= 0.52;
                const bool has_regime_recovery =
                    regime_ev_it != strategy_regime_edge.end() &&
                    regime_ev_it->second.trades >= 8 &&
                    regime_ev_it->second.expectancy() >= engine_config_.min_strategy_expectancy_krw &&
                    regime_ev_it->second.profitFactor() >=
                        std::max(0.98, engine_config_.min_strategy_profit_factor - 0.02) &&
                    regime_ev_it->second.winRate() >= 0.50;
                const bool has_recovery_evidence_any = has_recent_recovery || has_regime_recovery;
                const int relaxed_recovery_min_trades = std::max(
                    6,
                    engine_config_.strategy_ev_pre_cat_relaxed_recovery_min_trades
                );
                const double relaxed_recovery_expectancy_floor =
                    engine_config_.min_strategy_expectancy_krw -
                    std::max(0.0, engine_config_.strategy_ev_pre_cat_relaxed_recovery_expectancy_gap_krw);
                const double relaxed_recovery_profit_factor_floor = std::max(
                    0.90,
                    engine_config_.min_strategy_profit_factor -
                    std::max(0.0, engine_config_.strategy_ev_pre_cat_relaxed_recovery_profit_factor_gap)
                );
                const bool has_recent_recovery_relaxed =
                    recent_stat_it != strategy_edge_recent.end() &&
                    recent_stat_it->second.trades >= relaxed_recovery_min_trades &&
                    recent_stat_it->second.expectancy() >= relaxed_recovery_expectancy_floor &&
                    recent_stat_it->second.profitFactor() >= relaxed_recovery_profit_factor_floor &&
                    recent_stat_it->second.winRate() >= engine_config_.strategy_ev_pre_cat_relaxed_recovery_min_win_rate;
                const bool has_regime_recovery_relaxed =
                    regime_ev_it != strategy_regime_edge.end() &&
                    regime_ev_it->second.trades >= relaxed_recovery_min_trades &&
                    regime_ev_it->second.expectancy() >= relaxed_recovery_expectancy_floor &&
                    regime_ev_it->second.profitFactor() >= relaxed_recovery_profit_factor_floor &&
                    regime_ev_it->second.winRate() >= engine_config_.strategy_ev_pre_cat_relaxed_recovery_min_win_rate;
                const bool has_recovery_evidence_relaxed_recent_regime =
                    has_recent_recovery_relaxed || has_regime_recovery_relaxed;
                const bool has_recovery_evidence_relaxed_full_history =
                    stat.trades >= relaxed_recovery_min_trades &&
                    stat.expectancy() >= relaxed_recovery_expectancy_floor &&
                    stat.profitFactor() >= relaxed_recovery_profit_factor_floor &&
                    stat.winRate() >= engine_config_.strategy_ev_pre_cat_relaxed_recovery_min_win_rate;
                const bool has_recovery_evidence_relaxed_any =
                    has_recovery_evidence_relaxed_recent_regime ||
                    (engine_config_.enable_strategy_ev_pre_cat_relaxed_recovery_full_history_anchor &&
                     has_recovery_evidence_relaxed_full_history);
                const bool has_recovery_evidence_for_soften_raw =
                    has_recovery_evidence_any ||
                    (engine_config_.enable_strategy_ev_pre_cat_relaxed_recovery_evidence &&
                     has_recovery_evidence_relaxed_any);
                bool has_recovery_evidence_hysteresis_override = false;
                if (engine_config_.enable_strategy_ev_pre_cat_recovery_evidence_hysteresis) {
                    const int hysteresis_hold_steps = std::clamp(
                        engine_config_.strategy_ev_pre_cat_recovery_evidence_hysteresis_hold_steps,
                        1,
                        120
                    );
                    const int hysteresis_min_trades = std::max(
                        0,
                        engine_config_.strategy_ev_pre_cat_recovery_evidence_hysteresis_min_trades
                    );
                    const std::string hysteresis_key = makeStrategyRegimeKey(
                        best_signal.strategy_name,
                        best_signal.market_regime
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
                const bool recovery_quality_regime_ok =
                    best_signal.market_regime == analytics::MarketRegime::TRENDING_UP ||
                    best_signal.market_regime == analytics::MarketRegime::RANGING;
                const bool recovery_quality_strength_ok = best_signal.strength >= 0.70;
                const bool recovery_quality_expected_value_ok = best_signal.expected_value >= 0.00075;
                const bool recovery_quality_liquidity_ok = best_signal.liquidity_score >= 58.0;
                const bool recovery_quality_context =
                    recovery_quality_regime_ok &&
                    recovery_quality_strength_ok &&
                    recovery_quality_expected_value_ok &&
                    recovery_quality_liquidity_ok;
                const bool recovery_quality_hysteresis_strength_ok =
                    best_signal.strength >= engine_config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_strength;
                const bool recovery_quality_hysteresis_expected_value_ok =
                    best_signal.expected_value >= engine_config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_expected_value;
                const bool recovery_quality_hysteresis_liquidity_ok =
                    best_signal.liquidity_score >= engine_config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_liquidity;
                const bool recovery_quality_context_hysteresis_relaxed =
                    recovery_quality_regime_ok &&
                    recovery_quality_hysteresis_strength_ok &&
                    recovery_quality_hysteresis_expected_value_ok &&
                    recovery_quality_hysteresis_liquidity_ok;
                const bool recovery_quality_hysteresis_override =
                    engine_config_.enable_strategy_ev_pre_cat_recovery_quality_hysteresis_relief &&
                    has_recovery_evidence_hysteresis_override &&
                    !recovery_quality_context &&
                    recovery_quality_context_hysteresis_relaxed;
                const bool recovery_quality_context_for_soften =
                    recovery_quality_context || recovery_quality_hysteresis_override;
                const StrategyEdgeStats* recent_stat =
                    recent_stat_it != strategy_edge_recent.end() ? &recent_stat_it->second : nullptr;
                const StrategyEdgeStats* regime_stat =
                    regime_ev_it != strategy_regime_edge.end() ? &regime_ev_it->second : nullptr;
                const int pre_cat_local_recent_trades = recent_stat != nullptr ? recent_stat->trades : -1;
                const int pre_cat_local_regime_trades = regime_stat != nullptr ? regime_stat->trades : -1;
                const int pre_cat_local_trade_scope = std::max(
                    pre_cat_local_recent_trades,
                    pre_cat_local_regime_trades
                );
                // Relief paths should be bounded by local regime/recent evidence scope first.
                // Using only full-history trades causes permanent disablement after warm-up.
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
                    const double exp_floor = engine_config_.min_strategy_expectancy_krw - (relaxed_scope ? 7.0 : 5.0);
                    const double pf_floor = std::max(
                        0.80,
                        engine_config_.min_strategy_profit_factor - (relaxed_scope ? 0.24 : 0.18)
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
                    if (exp_krw < (engine_config_.min_strategy_expectancy_krw - 18.0) &&
                        pf < std::max(0.72, engine_config_.min_strategy_profit_factor - 0.32) &&
                        loss_to_win_ratio >= 1.70) {
                        pressure += 0.28;
                    }
                    return std::clamp(pressure, 0.0, 1.0);
                };
                const double full_history_pressure = computeHistoryNegativePressure(stat, false);
                const double recent_history_pressure =
                    (recent_stat != nullptr &&
                     recent_stat->trades >= std::max(8, engine_config_.min_strategy_trades_for_ev / 5))
                    ? computeHistoryNegativePressure(*recent_stat, true)
                    : 0.0;
                const double regime_history_pressure =
                    (regime_stat != nullptr && regime_stat->trades >= 8)
                    ? computeHistoryNegativePressure(*regime_stat, true)
                    : 0.0;
                const bool severe_full_negative =
                    stat.trades >= std::max(18, engine_config_.min_strategy_trades_for_ev / 3) &&
                    full_history_pressure >= 0.70;
                const int severe_full_min_trades_pre_cat = engine_config_.enable_strategy_ev_pre_cat_relaxed_severe_gate
                    ? std::max(10, engine_config_.strategy_ev_pre_cat_relaxed_severe_min_trades)
                    : std::max(18, engine_config_.min_strategy_trades_for_ev / 3);
                const double severe_full_pressure_threshold_pre_cat =
                    engine_config_.enable_strategy_ev_pre_cat_relaxed_severe_gate
                    ? std::clamp(engine_config_.strategy_ev_pre_cat_relaxed_severe_pressure_threshold, 0.70, 0.95)
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
                    stat_exp_krw < (engine_config_.min_strategy_expectancy_krw - 10.0);
                const bool severe_axis_pf =
                    stat_pf < std::max(0.78, engine_config_.min_strategy_profit_factor - 0.24);
                const bool severe_axis_wr = stat_wr < 0.40;
                const bool severe_axis_asym = stat_loss_to_win_ratio >= 1.55;
                const int severe_axis_count =
                    static_cast<int>(severe_axis_exp) +
                    static_cast<int>(severe_axis_pf) +
                    static_cast<int>(severe_axis_wr) +
                    static_cast<int>(severe_axis_asym);
                const bool severe_combo_catastrophic =
                    stat_exp_krw < (engine_config_.min_strategy_expectancy_krw - 16.0) &&
                    stat_pf < std::max(0.75, engine_config_.min_strategy_profit_factor - 0.28) &&
                    (stat_wr < 0.36 || stat_loss_to_win_ratio >= 1.65);
                const double severe_composite_pressure_threshold = std::clamp(
                    engine_config_.strategy_ev_pre_cat_composite_pressure_threshold,
                    0.70,
                    0.98
                );
                const int severe_composite_min_critical_signals = std::clamp(
                    engine_config_.strategy_ev_pre_cat_composite_min_critical_signals,
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
                const bool severe_composite_pressure_only =
                    stat.trades >= severe_full_min_trades_pre_cat &&
                    full_history_pressure >= severe_composite_pressure_threshold &&
                    severe_axis_count < severe_composite_min_critical_signals;
                const bool severe_full_negative_pre_cat_composite =
                    severe_composite_catastrophic_path || severe_composite_pressure_axis;
                const bool severe_full_negative_pre_cat =
                    engine_config_.enable_strategy_ev_pre_cat_composite_severe_model
                    ? severe_full_negative_pre_cat_composite
                    : severe_full_negative_pre_cat_threshold;
                const bool severe_recent_negative =
                    recent_stat != nullptr &&
                    recent_stat->trades >= std::max(8, engine_config_.min_strategy_trades_for_ev / 5) &&
                    recent_history_pressure >= 0.62;
                const bool severe_regime_negative =
                    regime_stat != nullptr &&
                    regime_stat->trades >= 8 &&
                    regime_history_pressure >= 0.64;
                const bool enable_strategy_ev_sync_guard =
                    engine_config_.enable_strategy_ev_pre_cat_sync_guard;
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
                    best_signal.market_regime != analytics::MarketRegime::HIGH_VOLATILITY &&
                    best_signal.market_regime != analytics::MarketRegime::TRENDING_DOWN;
                const double contextual_severe_max_pressure = std::clamp(
                    engine_config_.strategy_ev_pre_cat_contextual_severe_max_pressure,
                    0.75,
                    0.98
                );
                const int contextual_severe_max_axis_count = std::clamp(
                    engine_config_.strategy_ev_pre_cat_contextual_severe_max_axis_count,
                    1,
                    4
                );
                const bool contextual_severe_downgrade_ready =
                    engine_config_.enable_strategy_ev_pre_cat_contextual_severe_downgrade &&
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
                    (best_signal.expected_risk_pct > 1e-9)
                    ? (best_signal.expected_return_pct / best_signal.expected_risk_pct)
                    : 0.0;
                const double pre_cat_unsynced_override_min_strength = std::clamp(
                    engine_config_.strategy_ev_pre_cat_unsynced_override_min_strength,
                    0.60,
                    0.92
                );
                const double pre_cat_unsynced_override_min_expected_value = std::clamp(
                    engine_config_.strategy_ev_pre_cat_unsynced_override_min_expected_value,
                    0.00040,
                    0.00300
                );
                const double pre_cat_unsynced_override_min_liquidity = std::clamp(
                    engine_config_.strategy_ev_pre_cat_unsynced_override_min_liquidity,
                    45.0,
                    95.0
                );
                const double pre_cat_unsynced_override_min_reward_risk = std::clamp(
                    engine_config_.strategy_ev_pre_cat_unsynced_override_min_reward_risk,
                    1.05,
                    2.60
                );
                const double pre_cat_unsynced_override_max_full_history_pressure = std::clamp(
                    engine_config_.strategy_ev_pre_cat_unsynced_override_max_full_history_pressure,
                    0.70,
                    1.00
                );
                const int pre_cat_unsynced_override_max_severe_axis_count = std::clamp(
                    engine_config_.strategy_ev_pre_cat_unsynced_override_max_severe_axis_count,
                    1,
                    4
                );
                const bool unsynced_soft_override_ready =
                    enable_strategy_ev_sync_guard &&
                    non_hostile_regime &&
                    best_signal.strength >= pre_cat_unsynced_override_min_strength &&
                    best_signal.expected_value >= pre_cat_unsynced_override_min_expected_value &&
                    best_signal.liquidity_score >= pre_cat_unsynced_override_min_liquidity &&
                    (raw_reward_risk_ratio <= 0.0 ||
                     raw_reward_risk_ratio >= pre_cat_unsynced_override_min_reward_risk) &&
                    full_history_pressure <= pre_cat_unsynced_override_max_full_history_pressure &&
                    severe_axis_count <= pre_cat_unsynced_override_max_severe_axis_count;
                const double pre_cat_soften_strength_floor =
                    recovery_quality_hysteresis_override
                    ? std::min(0.70, engine_config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_strength)
                    : 0.70;
                const double pre_cat_soften_expected_value_floor =
                    recovery_quality_hysteresis_override
                    ? std::min(0.00070, engine_config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_expected_value)
                    : 0.00070;
                const double pre_cat_soften_liquidity_floor =
                    recovery_quality_hysteresis_override
                    ? std::min(58.0, engine_config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_liquidity)
                    : 58.0;
                const int pre_cat_recovery_bridge_max_trades = std::max(
                    8,
                    engine_config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_strategy_trades
                );
                const double pre_cat_recovery_bridge_max_full_history_pressure = std::clamp(
                    engine_config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_full_history_pressure,
                    0.70,
                    1.00
                );
                const int pre_cat_recovery_bridge_max_severe_axis_count = std::clamp(
                    engine_config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_severe_axis_count,
                    1,
                    4
                );
                const int pre_cat_recovery_bridge_max_activations_per_key = std::max(
                    1,
                    engine_config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_activations_per_key
                );
                const std::string pre_cat_recovery_bridge_key = makeStrategyRegimeKey(
                    best_signal.strategy_name,
                    best_signal.market_regime
                );
                const int pre_cat_negative_history_quarantine_hold_steps = std::clamp(
                    engine_config_.strategy_ev_pre_cat_negative_history_quarantine_hold_steps,
                    1,
                    60
                );
                const double pre_cat_negative_history_quarantine_min_full_history_pressure = std::clamp(
                    engine_config_.strategy_ev_pre_cat_negative_history_quarantine_min_full_history_pressure,
                    0.70,
                    1.00
                );
                const double pre_cat_negative_history_quarantine_max_history_pf = std::clamp(
                    engine_config_.strategy_ev_pre_cat_negative_history_quarantine_max_history_pf,
                    0.20,
                    1.20
                );
                const double pre_cat_negative_history_quarantine_max_history_expectancy_krw =
                    engine_config_.strategy_ev_pre_cat_negative_history_quarantine_max_history_expectancy_krw;
                int& pre_cat_negative_history_quarantine_hold_left =
                    pre_cat_negative_history_quarantine_hold_by_key_[pre_cat_recovery_bridge_key];
                bool pre_cat_negative_history_quarantine_active = false;
                if (engine_config_.enable_strategy_ev_pre_cat_negative_history_quarantine &&
                    pre_cat_negative_history_quarantine_hold_left > 0) {
                    pre_cat_negative_history_quarantine_active = true;
                    entry_funnel_.strategy_ev_pre_cat_negative_history_quarantine_active++;
                    pre_cat_negative_history_quarantine_hold_left =
                        std::max(0, pre_cat_negative_history_quarantine_hold_left - 1);
                }
                const int pre_cat_recovery_bridge_activations_used =
                    pre_cat_recovery_bridge_activation_by_key_.count(pre_cat_recovery_bridge_key) > 0
                    ? pre_cat_recovery_bridge_activation_by_key_.at(pre_cat_recovery_bridge_key)
                    : 0;
                const bool pre_cat_recovery_bridge_surrogate_ready =
                    engine_config_.enable_strategy_ev_pre_cat_recovery_evidence_bridge_surrogate &&
                    !has_recovery_evidence_relaxed_any &&
                    pre_cat_relief_trade_scope <= std::max(12, pre_cat_recovery_bridge_max_trades / 2) &&
                    best_signal.strength >= std::max(0.72, pre_cat_soften_strength_floor + 0.02) &&
                    best_signal.expected_value >= std::max(0.00100, pre_cat_soften_expected_value_floor + 0.00020) &&
                    best_signal.liquidity_score >= std::max(60.0, pre_cat_soften_liquidity_floor + 2.0) &&
                    full_history_pressure <= std::min(pre_cat_recovery_bridge_max_full_history_pressure, 0.92) &&
                    recent_history_pressure <= 0.78 &&
                    regime_history_pressure <= 0.80 &&
                    severe_axis_count <= std::min(pre_cat_recovery_bridge_max_severe_axis_count, 2);
                const bool pre_cat_recovery_bridge_ready =
                    engine_config_.enable_strategy_ev_pre_cat_recovery_evidence_bridge &&
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
                    engine_config_.enable_strategy_ev_pre_cat_soften_non_severe &&
                    pre_cat_soften_candidate_rr_ok &&
                    best_signal.strength >= pre_cat_soften_strength_floor &&
                    best_signal.expected_value >= pre_cat_soften_expected_value_floor &&
                    best_signal.liquidity_score >= pre_cat_soften_liquidity_floor;
                const int pre_cat_no_soft_quality_relief_max_trades = std::max(
                    8,
                    engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_max_strategy_trades
                );
                const double pre_cat_no_soft_quality_relief_min_strength = std::clamp(
                    engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_min_strength,
                    0.60,
                    0.90
                );
                const double pre_cat_no_soft_quality_relief_min_expected_value = std::clamp(
                    engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_min_expected_value,
                    0.00040,
                    0.00250
                );
                const double pre_cat_no_soft_quality_relief_min_liquidity = std::clamp(
                    engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_min_liquidity,
                    48.0,
                    90.0
                );
                const double pre_cat_no_soft_quality_relief_min_reward_risk = std::clamp(
                    engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_min_reward_risk,
                    1.10,
                    2.40
                );
                const double pre_cat_no_soft_quality_relief_max_full_history_pressure = std::clamp(
                    engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_max_full_history_pressure,
                    0.70,
                    1.00
                );
                const int pre_cat_no_soft_quality_relief_max_severe_axis_count = std::clamp(
                    engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_max_severe_axis_count,
                    1,
                    4
                );
                const int pre_cat_no_soft_quality_relief_max_activations_per_key = std::max(
                    1,
                    engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_max_activations_per_key
                );
                const std::string pre_cat_no_soft_quality_relief_key = makeStrategyRegimeKey(
                    best_signal.strategy_name,
                    best_signal.market_regime
                );
                const int pre_cat_no_soft_quality_relief_activations_used =
                    pre_cat_no_soft_quality_relief_activation_by_key_.count(pre_cat_no_soft_quality_relief_key) > 0
                    ? pre_cat_no_soft_quality_relief_activation_by_key_.at(pre_cat_no_soft_quality_relief_key)
                    : 0;
                const bool pre_cat_no_soft_quality_relief_ready =
                    engine_config_.enable_strategy_ev_pre_cat_no_soft_quality_relief &&
                    !has_recovery_evidence_for_soften_effective &&
                    pre_cat_no_soft_quality_relief_activations_used <
                        pre_cat_no_soft_quality_relief_max_activations_per_key &&
                    pre_cat_relief_trade_scope <= pre_cat_no_soft_quality_relief_max_trades &&
                    recovery_quality_context_for_soften &&
                    !severe_full_negative_pre_cat_effective &&
                    best_signal.strength >= pre_cat_no_soft_quality_relief_min_strength &&
                    best_signal.expected_value >= pre_cat_no_soft_quality_relief_min_expected_value &&
                    best_signal.liquidity_score >= pre_cat_no_soft_quality_relief_min_liquidity &&
                    (raw_reward_risk_ratio <= 0.0 ||
                     raw_reward_risk_ratio >= pre_cat_no_soft_quality_relief_min_reward_risk) &&
                    full_history_pressure <= pre_cat_no_soft_quality_relief_max_full_history_pressure &&
                    severe_axis_count <= pre_cat_no_soft_quality_relief_max_severe_axis_count;
                const int pre_cat_pressure_rebound_relief_max_trades = std::max(
                    8,
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_strategy_trades
                );
                const double pre_cat_pressure_rebound_relief_min_strength = std::clamp(
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_strength,
                    0.60,
                    0.92
                );
                const double pre_cat_pressure_rebound_relief_min_expected_value = std::clamp(
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_expected_value,
                    0.00040,
                    0.00300
                );
                const double pre_cat_pressure_rebound_relief_min_liquidity = std::clamp(
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_liquidity,
                    45.0,
                    95.0
                );
                const double pre_cat_pressure_rebound_relief_min_reward_risk = std::clamp(
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_reward_risk,
                    1.05,
                    2.60
                );
                const double pre_cat_pressure_rebound_relief_min_full_history_pressure = std::clamp(
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_full_history_pressure,
                    0.70,
                    1.00
                );
                const double pre_cat_pressure_rebound_relief_max_recent_history_pressure = std::clamp(
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_recent_history_pressure,
                    0.50,
                    0.98
                );
                const double pre_cat_pressure_rebound_relief_max_regime_history_pressure = std::clamp(
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_regime_history_pressure,
                    0.50,
                    0.98
                );
                const int pre_cat_pressure_rebound_relief_max_severe_axis_count = std::clamp(
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_severe_axis_count,
                    1,
                    4
                );
                const int pre_cat_pressure_rebound_relief_max_activations_per_key = std::max(
                    1,
                    engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_activations_per_key
                );
                const int pre_cat_pressure_rebound_relief_activations_used =
                    pre_cat_pressure_rebound_relief_activation_by_key_.count(pre_cat_no_soft_quality_relief_key) > 0
                    ? pre_cat_pressure_rebound_relief_activation_by_key_.at(pre_cat_no_soft_quality_relief_key)
                    : 0;
                const bool pre_cat_pressure_rebound_relief_ready =
                    engine_config_.enable_strategy_ev_pre_cat_pressure_rebound_relief &&
                    !has_recovery_evidence_for_soften_effective &&
                    pre_cat_pressure_rebound_relief_activations_used <
                        pre_cat_pressure_rebound_relief_max_activations_per_key &&
                    pre_cat_relief_trade_scope <= pre_cat_pressure_rebound_relief_max_trades &&
                    recovery_quality_context_for_soften &&
                    non_hostile_regime &&
                    !severe_history_sync_pre_cat &&
                    best_signal.strength >= pre_cat_pressure_rebound_relief_min_strength &&
                    best_signal.expected_value >= pre_cat_pressure_rebound_relief_min_expected_value &&
                    best_signal.liquidity_score >= pre_cat_pressure_rebound_relief_min_liquidity &&
                    (raw_reward_risk_ratio <= 0.0 ||
                     raw_reward_risk_ratio >= pre_cat_pressure_rebound_relief_min_reward_risk) &&
                    full_history_pressure >= pre_cat_pressure_rebound_relief_min_full_history_pressure &&
                    recent_history_pressure <= pre_cat_pressure_rebound_relief_max_recent_history_pressure &&
                    regime_history_pressure <= pre_cat_pressure_rebound_relief_max_regime_history_pressure &&
                    severe_axis_count <= pre_cat_pressure_rebound_relief_max_severe_axis_count;
                const bool pre_cat_candidate_rr_failsafe_enabled =
                    engine_config_.enable_strategy_ev_pre_cat_candidate_rr_failsafe;
                const int pre_cat_candidate_rr_failsafe_max_trades = std::max(
                    8,
                    engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_strategy_trades
                );
                const double pre_cat_candidate_rr_failsafe_min_strength = std::clamp(
                    engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_strength,
                    0.60,
                    0.92
                );
                const double pre_cat_candidate_rr_failsafe_min_expected_value = std::clamp(
                    engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_expected_value,
                    0.00040,
                    0.00300
                );
                const double pre_cat_candidate_rr_failsafe_min_liquidity = std::clamp(
                    engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_liquidity,
                    45.0,
                    95.0
                );
                const double pre_cat_candidate_rr_failsafe_min_reward_risk = std::clamp(
                    engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_reward_risk,
                    1.05,
                    2.60
                );
                const double pre_cat_candidate_rr_failsafe_max_full_history_pressure = std::clamp(
                    engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_full_history_pressure,
                    0.70,
                    1.00
                );
                const int pre_cat_candidate_rr_failsafe_max_severe_axis_count = std::clamp(
                    engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_severe_axis_count,
                    1,
                    4
                );
                const int pre_cat_candidate_rr_failsafe_max_activations_per_key = std::max(
                    1,
                    engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_activations_per_key
                );
                const int pre_cat_candidate_rr_failsafe_activations_used =
                    pre_cat_candidate_rr_failsafe_activation_by_key_.count(pre_cat_no_soft_quality_relief_key) > 0
                    ? pre_cat_candidate_rr_failsafe_activation_by_key_.at(pre_cat_no_soft_quality_relief_key)
                    : 0;
                const bool pre_cat_candidate_rr_failsafe_ready =
                    pre_cat_candidate_rr_failsafe_enabled &&
                    !engine_config_.enable_strategy_ev_pre_cat_soften_non_severe &&
                    pre_cat_soften_candidate_rr_ok &&
                    pre_cat_candidate_rr_failsafe_activations_used <
                        pre_cat_candidate_rr_failsafe_max_activations_per_key &&
                    pre_cat_relief_trade_scope <= pre_cat_candidate_rr_failsafe_max_trades &&
                    best_signal.strength >= pre_cat_candidate_rr_failsafe_min_strength &&
                    best_signal.expected_value >= pre_cat_candidate_rr_failsafe_min_expected_value &&
                    best_signal.liquidity_score >= pre_cat_candidate_rr_failsafe_min_liquidity &&
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
                    best_signal.strength >= pre_cat_soften_strength_floor &&
                    best_signal.expected_value >= pre_cat_soften_expected_value_floor &&
                    best_signal.liquidity_score >= pre_cat_soften_liquidity_floor &&
                    (raw_reward_risk_ratio <= 0.0 ||
                     raw_reward_risk_ratio >= 1.18) &&
                    full_history_pressure <= pre_cat_sync_override_max_full_history_pressure &&
                    severe_axis_count <= pre_cat_sync_override_max_severe_axis_count;
                auto capturePreCatFeatureSnapshot = [&](Result::PreCatFeatureSnapshotBranch& branch) {
                    const int sample_index = ++branch.samples;
                    auto updateAvg = [&](double& avg, double value) {
                        if (!std::isfinite(value)) {
                            return;
                        }
                        avg += (value - avg) / static_cast<double>(sample_index);
                    };

                    if (recovery_quality_context) {
                        branch.recovery_quality_context_hits++;
                    }
                    if (has_recovery_evidence_for_soften_effective) {
                        branch.recovery_evidence_hits++;
                    }
                    if (has_recovery_evidence_relaxed_any) {
                        branch.recovery_evidence_relaxed_hits++;
                    }
                    if (has_recovery_evidence_hysteresis_override) {
                        branch.recovery_evidence_hysteresis_hits++;
                    }
                    if (non_hostile_regime) {
                        branch.non_hostile_regime_hits++;
                    }
                    if (severe_history_sync_pre_cat) {
                        branch.severe_active_hits++;
                    }
                    if (contextual_severe_downgrade_ready) {
                        branch.contextual_downgrade_hits++;
                    }
                    if (pre_cat_soften_ready) {
                        branch.soften_ready_hits++;
                    }
                    if (pre_cat_soften_candidate_rr_ok) {
                        branch.soften_candidate_rr_ok_hits++;
                    }
                    if (severe_axis_count >= severe_composite_min_critical_signals) {
                        branch.severe_axis_ge_threshold_hits++;
                    }

                    updateAvg(branch.avg_signal_strength, best_signal.strength);
                    updateAvg(branch.avg_signal_expected_value, best_signal.expected_value);
                    updateAvg(branch.avg_signal_liquidity, best_signal.liquidity_score);
                    updateAvg(branch.avg_signal_reward_risk, raw_reward_risk_ratio);
                    updateAvg(branch.avg_history_expectancy_krw, stat_exp_krw);
                    updateAvg(branch.avg_history_profit_factor, stat_pf);
                    updateAvg(branch.avg_history_win_rate, stat_wr);
                    updateAvg(branch.avg_history_trades, static_cast<double>(stat.trades));
                    updateAvg(branch.avg_history_loss_to_win, stat_loss_to_win_ratio);
                    updateAvg(branch.avg_full_history_pressure, full_history_pressure);
                    updateAvg(branch.avg_recent_history_pressure, recent_history_pressure);
                    updateAvg(branch.avg_regime_history_pressure, regime_history_pressure);
                    updateAvg(branch.avg_severe_axis_count, static_cast<double>(severe_axis_count));
                };
                if (stat.trades >= std::max(12, engine_config_.min_strategy_trades_for_ev / 4)) {
                    const double wr = stat.winRate();
                    const double pf = stat.profitFactor();
                    const double exp_krw = stat.expectancy();
                    const bool pre_catastrophic =
                        exp_krw < (engine_config_.min_strategy_expectancy_krw - 5.0) &&
                        pf < std::max(0.88, engine_config_.min_strategy_profit_factor - 0.18) &&
                        wr < 0.60;
                    if (pre_catastrophic) {
                        capturePreCatFeatureSnapshot(pre_cat_feature_snapshot_.observed);
                        entry_funnel_.strategy_ev_pre_cat_observed++;
                        if (recovery_quality_context) {
                            entry_funnel_.strategy_ev_pre_cat_recovery_quality_context++;
                        }
                        if (has_recent_recovery || has_regime_recovery) {
                            entry_funnel_.strategy_ev_pre_cat_recovery_evidence_any++;
                        }
                        if (has_recovery_evidence_relaxed_any) {
                            entry_funnel_.strategy_ev_pre_cat_recovery_evidence_relaxed_any++;
                        }
                        if (has_recovery_evidence_relaxed_recent_regime) {
                            entry_funnel_.strategy_ev_pre_cat_recovery_evidence_relaxed_recent_regime++;
                        }
                        if (has_recovery_evidence_relaxed_full_history) {
                            entry_funnel_.strategy_ev_pre_cat_recovery_evidence_relaxed_full_history++;
                        }
                        if (has_recovery_evidence_for_soften) {
                            entry_funnel_.strategy_ev_pre_cat_recovery_evidence_for_soften++;
                        }
                        if (pre_cat_recovery_bridge_ready) {
                            entry_funnel_.strategy_ev_pre_cat_recovery_evidence_bridge++;
                            if (pre_cat_recovery_bridge_surrogate_ready) {
                                entry_funnel_.strategy_ev_pre_cat_recovery_evidence_bridge_surrogate++;
                            }
                        }
                        if (has_recovery_evidence_hysteresis_override) {
                            entry_funnel_.strategy_ev_pre_cat_recovery_evidence_hysteresis_override++;
                        }
                        if (recovery_quality_hysteresis_override) {
                            entry_funnel_.strategy_ev_pre_cat_quality_hysteresis_override++;
                        }
                        if (recovery_quality_context && has_recovery_evidence_relaxed_any) {
                            entry_funnel_.strategy_ev_pre_cat_quality_context_relaxed_overlap++;
                        }
                        if (!recovery_quality_regime_ok) {
                            entry_funnel_.strategy_ev_pre_cat_quality_fail_regime++;
                        }
                        if (!recovery_quality_strength_ok) {
                            entry_funnel_.strategy_ev_pre_cat_quality_fail_strength++;
                        }
                        if (!recovery_quality_expected_value_ok) {
                            entry_funnel_.strategy_ev_pre_cat_quality_fail_expected_value++;
                        }
                        if (!recovery_quality_liquidity_ok) {
                            entry_funnel_.strategy_ev_pre_cat_quality_fail_liquidity++;
                        }
                        if (severe_full_negative_pre_cat_threshold) {
                            entry_funnel_.strategy_ev_pre_cat_severe_legacy_hits++;
                        }
                        if (severe_full_negative_pre_cat_composite) {
                            entry_funnel_.strategy_ev_pre_cat_severe_composite_hits++;
                        }
                        if (severe_composite_catastrophic_path) {
                            entry_funnel_.strategy_ev_pre_cat_severe_composite_catastrophic_hits++;
                        }
                        if (severe_composite_pressure_axis) {
                            entry_funnel_.strategy_ev_pre_cat_severe_composite_pressure_axis_hits++;
                        }
                        if (severe_composite_pressure_only) {
                            entry_funnel_.strategy_ev_pre_cat_severe_composite_pressure_only_hits++;
                        }
                        if (contextual_severe_downgrade_ready) {
                            entry_funnel_.strategy_ev_pre_cat_contextual_severe_downgrade_hits++;
                        }
                        if (severe_history_sync_pre_cat) {
                            entry_funnel_.strategy_ev_pre_cat_severe_active_hits++;
                        }
                        if (pre_cat_soften_candidate_quality_and_evidence) {
                            entry_funnel_.strategy_ev_pre_cat_soften_candidate_quality_and_evidence++;
                        }
                        if (pre_cat_soften_candidate_non_severe) {
                            entry_funnel_.strategy_ev_pre_cat_soften_candidate_non_severe++;
                        }
                        if (pre_cat_soften_candidate_non_hostile) {
                            entry_funnel_.strategy_ev_pre_cat_soften_candidate_non_hostile++;
                        }
                        if (pre_cat_soften_candidate_rr_ok) {
                            entry_funnel_.strategy_ev_pre_cat_soften_candidate_rr_ok++;
                        }
                        if (pre_cat_soften_ready) {
                            entry_funnel_.strategy_ev_pre_cat_soften_ready++;
                        }
                        if (pre_cat_negative_history_quarantine_active) {
                            capturePreCatFeatureSnapshot(pre_cat_feature_snapshot_.blocked_no_soft_path);
                            entry_funnel_.strategy_ev_pre_cat_blocked_no_soft_path++;
                            failStrategyEv(StrategyEvFailKind::PreCatastrophicNoRecovery);
                        } else if (contextual_recovery_ready) {
                            capturePreCatFeatureSnapshot(pre_cat_feature_snapshot_.softened_contextual);
                            entry_funnel_.strategy_ev_pre_cat_softened_contextual++;
                            required_strength_floor = std::max(
                                required_strength_floor,
                                alpha_head_fallback_candidate ? 0.58 : 0.62
                            );
                            addAdaptiveHistory(
                                0.06 * history_penalty_scale,
                                0.00010 * history_penalty_scale
                            );
                        } else if (pre_cat_sync_override_ready) {
                            capturePreCatFeatureSnapshot(pre_cat_feature_snapshot_.softened_override);
                            entry_funnel_.strategy_ev_pre_cat_softened_override++;
                            required_strength_floor = std::max(
                                required_strength_floor,
                                alpha_head_fallback_candidate ? 0.66 : 0.70
                            );
                            addAdaptiveHistory(
                                0.11 * history_penalty_scale,
                                0.00016 * history_penalty_scale
                            );
                        } else if (severe_history_sync_pre_cat) {
                            capturePreCatFeatureSnapshot(pre_cat_feature_snapshot_.blocked_severe_sync);
                            entry_funnel_.strategy_ev_pre_cat_blocked_severe_sync++;
                            failStrategyEv(StrategyEvFailKind::PreCatastrophicNoRecovery);
                        } else if (unsynced_soft_override_ready ||
                                   pre_cat_soften_ready ||
                                   pre_cat_no_soft_quality_relief_ready) {
                            capturePreCatFeatureSnapshot(pre_cat_feature_snapshot_.softened_override);
                            entry_funnel_.strategy_ev_pre_cat_softened_override++;
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
                                entry_funnel_.strategy_ev_pre_cat_softened_no_soft_quality_relief++;
                                required_strength_floor = std::max(
                                    required_strength_floor,
                                    alpha_head_fallback_candidate ? 0.63 : 0.67
                                );
                                addAdaptiveHistory(
                                    0.09 * history_penalty_scale,
                                    0.00014 * history_penalty_scale
                                );
                            } else if (used_candidate_rr_failsafe) {
                                pre_cat_candidate_rr_failsafe_activation_by_key_[pre_cat_no_soft_quality_relief_key]++;
                                entry_funnel_.strategy_ev_pre_cat_softened_candidate_rr_failsafe++;
                                required_strength_floor = std::max(
                                    required_strength_floor,
                                    alpha_head_fallback_candidate ? 0.65 : 0.69
                                );
                                addAdaptiveHistory(
                                    0.10 * history_penalty_scale,
                                    0.00015 * history_penalty_scale
                                );
                            } else if (used_pressure_rebound_relief) {
                                pre_cat_pressure_rebound_relief_activation_by_key_[pre_cat_no_soft_quality_relief_key]++;
                                entry_funnel_.strategy_ev_pre_cat_softened_pressure_rebound_relief++;
                                required_strength_floor = std::max(
                                    required_strength_floor,
                                    alpha_head_fallback_candidate ? 0.66 : 0.70
                                );
                                addAdaptiveHistory(
                                    0.12 * history_penalty_scale,
                                    0.00018 * history_penalty_scale
                                );
                            } else {
                                required_strength_floor = std::max(
                                    required_strength_floor,
                                    alpha_head_fallback_candidate ? 0.60 : 0.64
                                );
                                addAdaptiveHistory(
                                    0.07 * history_penalty_scale,
                                    0.00012 * history_penalty_scale
                                );
                            }
                            if (used_recovery_evidence_bridge) {
                                pre_cat_recovery_bridge_activation_by_key_[pre_cat_recovery_bridge_key]++;
                            }
                        } else {
                            capturePreCatFeatureSnapshot(pre_cat_feature_snapshot_.blocked_no_soft_path);
                            entry_funnel_.strategy_ev_pre_cat_blocked_no_soft_path++;
                            const bool pre_cat_negative_history_quarantine_set_ready =
                                engine_config_.enable_strategy_ev_pre_cat_negative_history_quarantine &&
                                full_history_pressure >=
                                    pre_cat_negative_history_quarantine_min_full_history_pressure &&
                                stat_pf <= pre_cat_negative_history_quarantine_max_history_pf &&
                                stat_exp_krw <=
                                    pre_cat_negative_history_quarantine_max_history_expectancy_krw;
                            if (pre_cat_negative_history_quarantine_set_ready) {
                                pre_cat_negative_history_quarantine_hold_by_key_[pre_cat_recovery_bridge_key] =
                                    pre_cat_negative_history_quarantine_hold_steps;
                                entry_funnel_.strategy_ev_pre_cat_negative_history_quarantine_set++;
                            }
                            failStrategyEv(StrategyEvFailKind::PreCatastrophicNoRecovery);
                        }
                    }
                }
                if (stat.trades >= engine_config_.min_strategy_trades_for_ev) {
                    const double exp_krw = stat.expectancy();
                    const double pf = stat.profitFactor();
                    if (exp_krw < engine_config_.min_strategy_expectancy_krw ||
                        pf < engine_config_.min_strategy_profit_factor) {
                        required_strength_floor += (0.04 * history_penalty_scale);
                        addAdaptiveHistory(
                            (0.12 * history_penalty_scale),
                            (0.00025 * history_penalty_scale)
                        );
                        if (exp_krw < (engine_config_.min_strategy_expectancy_krw - 15.0) &&
                            pf < std::max(0.75, engine_config_.min_strategy_profit_factor - 0.20)) {
                            if (alpha_head_fallback_candidate) {
                                const bool catastrophic_history =
                                    stat.trades >= std::max(18, engine_config_.min_strategy_trades_for_ev / 2) &&
                                    (exp_krw < (engine_config_.min_strategy_expectancy_krw - 28.0) ||
                                     pf < std::max(0.65, engine_config_.min_strategy_profit_factor - 0.45));
                                if (catastrophic_history) {
                                    failStrategyEv(StrategyEvFailKind::CatastrophicHistory);
                                } else {
                                    required_strength_floor = std::max(required_strength_floor, 0.62);
                                    addAdaptiveHistory(0.08, 0.00012);
                                }
                            } else {
                                if (severe_history_sync) {
                                    failStrategyEv(StrategyEvFailKind::SevereThresholdNoRecovery);
                                } else if (unsynced_soft_override_ready) {
                                    required_strength_floor = std::max(required_strength_floor, 0.63);
                                    addAdaptiveHistory(
                                        0.10 * history_penalty_scale,
                                        0.00016 * history_penalty_scale
                                    );
                                } else {
                                    failStrategyEv(StrategyEvFailKind::SevereThresholdNoRecovery);
                                }
                            }
                        }
                    }
                }
                if (stat.trades >= 8) {
                    const double wr = stat.winRate();
                    const double pf = stat.profitFactor();
                    const double exp_krw = stat.expectancy();
                    const double avg_win_krw = stat.avgWinKrw();
                    const double avg_loss_abs_krw = stat.avgLossAbsKrw();
                    const double loss_to_win_ratio = (avg_win_krw > 1e-9)
                        ? (avg_loss_abs_krw / avg_win_krw)
                        : 0.0;
                    if (stat.trades >= 10) {
                        const bool weak_history = exp_krw < -2.0 || pf < 0.95;
                        const bool asymmetry_history = wr >= 0.50 && loss_to_win_ratio >= 1.35;
                        if (weak_history || asymmetry_history) {
                            alpha_head_relief_eligible = false;
                        }
                    }
                    // Generic reward-risk asymmetry guard:
                    // if win-rate looks acceptable but expectancy is still negative,
                    // enforce tighter gates because loss magnitude is dominating.
                    if (stat.trades >= 10 &&
                        exp_krw < 0.0 &&
                        pf < 1.00 &&
                        wr >= 0.50 &&
                        loss_to_win_ratio >= 1.30) {
                        const double asymmetry_pressure = std::clamp(
                            (loss_to_win_ratio - 1.20) / 1.20,
                            0.0,
                            1.0
                        );
                        required_strength_floor = std::max(
                            required_strength_floor,
                            alpha_head_fallback_candidate ? 0.58 : 0.62
                        );
                        addAdaptiveHistory(
                            (0.08 + (0.10 * asymmetry_pressure)) * history_penalty_scale,
                            (0.00014 + (0.00018 * asymmetry_pressure)) * history_penalty_scale
                        );
                        if (stat.trades >= 14 &&
                            exp_krw < -6.0 &&
                            pf < 0.85 &&
                            loss_to_win_ratio >= 1.65) {
                            if (alpha_head_fallback_candidate) {
                                required_strength_floor = std::max(required_strength_floor, 0.70);
                                addAdaptiveHistory(0.16, 0.00024);
                            } else {
                                if (severe_history_sync) {
                                    failStrategyEv(StrategyEvFailKind::LossAsymmetryCollapse);
                                } else if (unsynced_soft_override_ready) {
                                    required_strength_floor = std::max(required_strength_floor, 0.64);
                                    addAdaptiveHistory(
                                        0.11 * history_penalty_scale,
                                        0.00017 * history_penalty_scale
                                    );
                                } else {
                                    failStrategyEv(StrategyEvFailKind::LossAsymmetryCollapse);
                                }
                            }
                        }
                    }
                    const double history_confidence = std::clamp(
                        (static_cast<double>(stat.trades) - 8.0) / 24.0,
                        0.35,
                        1.0
                    );
                    double rr_band_add = 0.0;
                    double edge_band_add = 0.0;

                    if (wr < 0.36) {
                        rr_band_add += 0.24;
                        edge_band_add += 0.00042;
                    } else if (wr < 0.42) {
                        rr_band_add += 0.16;
                        edge_band_add += 0.00028;
                    } else if (wr < 0.48) {
                        rr_band_add += 0.09;
                        edge_band_add += 0.00016;
                    } else if (wr < 0.52) {
                        rr_band_add += 0.04;
                        edge_band_add += 0.00007;
                    }

                    if (pf < 0.82) {
                        rr_band_add += 0.20;
                        edge_band_add += 0.00036;
                    } else if (pf < 0.92) {
                        rr_band_add += 0.13;
                        edge_band_add += 0.00023;
                    } else if (pf < 1.00) {
                        rr_band_add += 0.07;
                        edge_band_add += 0.00012;
                    } else if (pf < 1.05) {
                        rr_band_add += 0.03;
                        edge_band_add += 0.00005;
                    }

                    if (exp_krw < -30.0) {
                        rr_band_add += 0.16;
                        edge_band_add += 0.00028;
                    } else if (exp_krw < -15.0) {
                        rr_band_add += 0.10;
                        edge_band_add += 0.00017;
                    } else if (exp_krw < 0.0) {
                        rr_band_add += 0.05;
                        edge_band_add += 0.00008;
                    }

                    rr_band_add *= (history_penalty_scale * history_confidence);
                    edge_band_add *= (history_penalty_scale * history_confidence);

                    if (favorable_recovery_signal) {
                        rr_band_add *= 0.88;
                        edge_band_add *= 0.88;
                    }

                    const double rr_band_cap = alpha_head_fallback_candidate ? 0.24 : 0.32;
                    const double edge_band_cap = alpha_head_fallback_candidate ? 0.00042 : 0.00062;
                    rr_band_add = std::clamp(rr_band_add, 0.0, rr_band_cap);
                    edge_band_add = std::clamp(edge_band_add, 0.0, edge_band_cap);
                    addAdaptiveHistory(rr_band_add, edge_band_add);
                }
            }
            auto regime_it = strategy_regime_edge.find(
                makeStrategyRegimeKey(best_signal.strategy_name, best_signal.market_regime)
            );
            auto regime_recent_it = strategy_regime_edge_recent.find(
                makeStrategyRegimeKey(best_signal.strategy_name, best_signal.market_regime)
            );
            auto market_regime_it = market_strategy_regime_edge.find(
                makeMarketStrategyRegimeKey(
                    best_signal.market,
                    best_signal.strategy_name,
                    best_signal.market_regime
                )
            );
            auto market_regime_recent_it = market_strategy_regime_edge_recent.find(
                makeMarketStrategyRegimeKey(
                    best_signal.market,
                    best_signal.strategy_name,
                    best_signal.market_regime
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
                (regime_it != strategy_regime_edge.end()) ? &regime_it->second : nullptr;
            const StrategyEdgeStats* regime_recent_stat =
                (regime_recent_it != strategy_regime_edge_recent.end()) ? &regime_recent_it->second : nullptr;
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
            const bool strategy_history_deeply_negative =
                stat_it != strategy_edge.end() &&
                stat_it->second.trades >= std::max(12, engine_config_.min_strategy_trades_for_ev / 2) &&
                stat_it->second.expectancy() <= (engine_config_.min_strategy_expectancy_krw - 12.0) &&
                stat_it->second.profitFactor() <=
                    std::max(0.86, engine_config_.min_strategy_profit_factor - 0.12);
            if (enable_loser_context_quarantine &&
                core_risk_enabled &&
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
                       core_risk_enabled &&
                       loser_context_severity >= 0.48 &&
                       severe_context_overlap) {
                required_strength_floor = std::max(
                    required_strength_floor,
                    0.59 + (0.04 * loser_context_severity)
                );
                addAdaptiveRegime(
                    (0.05 + (0.08 * loser_context_severity)) * history_penalty_scale,
                    (0.00008 + (0.00012 * loser_context_severity)) * history_penalty_scale
                );
                if (loser_context_severity >= 0.62 && strategy_history_deeply_negative) {
                    alpha_head_relief_eligible = false;
                }
            }
            if (core_risk_enabled && regime_it != strategy_regime_edge.end()) {
                const auto& stat = regime_it->second;
                if (stat.trades >= 6) {
                    const double wr = stat.winRate();
                    const double pf = stat.profitFactor();
                    const double exp_krw = stat.expectancy();
                    if (exp_krw < -20.0 || (wr < 0.30 && pf < 0.78)) {
                        markPatternBlockOrSoften(stat.trades, exp_krw, wr, pf);
                    }
                    if (stat.trades >= 18 &&
                        exp_krw < -22.0 &&
                        wr < 0.20) {
                        markPatternBlockOrSoften(stat.trades, exp_krw, wr, pf);
                    }
                    const double regime_confidence = std::clamp(
                        (static_cast<double>(stat.trades) - 6.0) / 22.0,
                        0.30,
                        1.0
                    );
                    const double regime_scale = history_penalty_scale * regime_confidence;
                    double rr_band_add = 0.0;
                    double edge_band_add = 0.0;

                    if (wr < 0.34) {
                        rr_band_add += 0.17;
                        edge_band_add += 0.00030;
                    } else if (wr < 0.42) {
                        rr_band_add += 0.11;
                        edge_band_add += 0.00020;
                    } else if (wr < 0.48) {
                        rr_band_add += 0.06;
                        edge_band_add += 0.00011;
                    } else if (wr < 0.52) {
                        rr_band_add += 0.03;
                        edge_band_add += 0.00005;
                    }

                    if (pf < 0.82) {
                        rr_band_add += 0.15;
                        edge_band_add += 0.00028;
                    } else if (pf < 0.92) {
                        rr_band_add += 0.10;
                        edge_band_add += 0.00018;
                    } else if (pf < 1.00) {
                        rr_band_add += 0.05;
                        edge_band_add += 0.00009;
                    } else if (pf < 1.05) {
                        rr_band_add += 0.02;
                        edge_band_add += 0.00004;
                    }

                    if (exp_krw < -25.0) {
                        rr_band_add += 0.14;
                        edge_band_add += 0.00024;
                    } else if (exp_krw < -12.0) {
                        rr_band_add += 0.09;
                        edge_band_add += 0.00015;
                    } else if (exp_krw < 0.0) {
                        rr_band_add += 0.04;
                        edge_band_add += 0.00007;
                    }

                    if (stat.trades >= 12 &&
                        exp_krw < -15.0 &&
                        (wr < 0.40 || pf < 0.90)) {
                        required_strength_floor = std::max(required_strength_floor, 0.62);
                    }

                    if (exp_krw < 0.0) {
                        required_strength_floor += std::clamp((-exp_krw) / 3200.0, 0.0, 0.06) * regime_scale;
                    } else if (exp_krw > 8.0 && wr >= 0.58 && pf >= 1.15) {
                        required_strength_floor -= 0.015;
                        rr_band_add -= 0.05;
                        edge_band_add -= 0.00009;
                    }
                    if (wr < 0.42) {
                        required_strength_floor += (0.018 * regime_scale);
                    }
                    if (pf < 0.95) {
                        required_strength_floor += (0.014 * regime_scale);
                    }

                    rr_band_add *= regime_scale;
                    edge_band_add *= regime_scale;
                    if (favorable_recovery_signal && rr_band_add > 0.0) {
                        rr_band_add *= 0.92;
                        edge_band_add *= 0.92;
                    }

                    const double rr_band_cap = alpha_head_fallback_candidate ? 0.22 : 0.30;
                    const double edge_band_cap = alpha_head_fallback_candidate ? 0.00040 : 0.00056;
                    rr_band_add = std::clamp(rr_band_add, -0.08, rr_band_cap);
                    edge_band_add = std::clamp(edge_band_add, -0.00014, edge_band_cap);
                    addAdaptiveRegime(rr_band_add, edge_band_add);
                }
            }
            if (core_risk_enabled && market_regime_it != market_strategy_regime_edge.end()) {
                const auto& stat = market_regime_it->second;
                if (stat.trades >= 4) {
                    const double wr = stat.winRate();
                    const double pf = stat.profitFactor();
                    const double exp_krw = stat.expectancy();

                    if (exp_krw < -40.0 && wr < 0.25 && pf < 0.75) {
                        markPatternBlockOrSoften(stat.trades, exp_krw, wr, pf);
                    }
                    const double market_confidence = std::clamp(
                        (static_cast<double>(stat.trades) - 4.0) / 18.0,
                        0.30,
                        1.0
                    );
                    const double market_scale = history_penalty_scale * market_confidence;
                    double rr_band_add = 0.0;
                    double edge_band_add = 0.0;

                    if (wr < 0.30) {
                        rr_band_add += 0.10;
                        edge_band_add += 0.00018;
                    } else if (wr < 0.38) {
                        rr_band_add += 0.06;
                        edge_band_add += 0.00010;
                    }

                    if (pf < 0.80) {
                        rr_band_add += 0.10;
                        edge_band_add += 0.00017;
                    } else if (pf < 0.90) {
                        rr_band_add += 0.06;
                        edge_band_add += 0.00010;
                    }

                    if (exp_krw < -30.0) {
                        rr_band_add += 0.12;
                        edge_band_add += 0.00020;
                    } else if (exp_krw < -15.0) {
                        rr_band_add += 0.07;
                        edge_band_add += 0.00012;
                    } else if (exp_krw < -5.0) {
                        rr_band_add += 0.03;
                        edge_band_add += 0.00005;
                    }

                    if (exp_krw < -20.0 && wr < 0.35 && pf < 0.85) {
                        required_strength_floor += (0.04 * market_scale);
                    } else if (exp_krw < -10.0) {
                        required_strength_floor += (0.015 * market_scale);
                    } else if (exp_krw > 10.0 && wr >= 0.60 && pf >= 1.20) {
                        required_strength_floor -= 0.015;
                        rr_band_add -= 0.04;
                        edge_band_add -= 0.00007;
                    }

                    rr_band_add *= market_scale;
                    edge_band_add *= market_scale;
                    if (favorable_recovery_signal && rr_band_add > 0.0) {
                        rr_band_add *= 0.94;
                        edge_band_add *= 0.94;
                    }
                    const double rr_band_cap = alpha_head_fallback_candidate ? 0.14 : 0.18;
                    const double edge_band_cap = alpha_head_fallback_candidate ? 0.00025 : 0.00034;
                    rr_band_add = std::clamp(rr_band_add, -0.06, rr_band_cap);
                    edge_band_add = std::clamp(edge_band_add, -0.00010, edge_band_cap);
                    addAdaptiveRegime(rr_band_add, edge_band_add);
                }
            }
            applyArchetypeRiskAdjustments(
                best_signal,
                required_strength_floor,
                adaptive_rr_add_regime,
                adaptive_edge_add_regime,
                regime_pattern_block
            );
            if (favorable_recovery_signal && adaptive_rr_add_regime > 0.0) {
                adaptive_rr_add_regime *= 0.94;
                adaptive_edge_add_regime *= 0.94;
            }
            if (adaptive_rr_add_regime > 0.0 && adaptive_edge_add_regime > 0.0) {
                const bool hostile_regime =
                    best_signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                    best_signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
                const bool quality_signal =
                    best_signal.strength >= 0.72 &&
                    best_signal.liquidity_score >= 58.0 &&
                    best_signal.expected_value >= 0.00075;
                if (!hostile_regime && quality_signal) {
                    // Joint RR/edge regime softening:
                    // avoid double-penalizing both gates on favorable, high-quality contexts.
                    const double strength_conf = std::clamp((best_signal.strength - 0.70) / 0.18, 0.0, 1.0);
                    const double liquidity_conf = std::clamp((best_signal.liquidity_score - 58.0) / 18.0, 0.0, 1.0);
                    const double ev_conf = std::clamp((best_signal.expected_value - 0.00075) / 0.00120, 0.0, 1.0);
                    double evidence_conf = 0.40 +
                        (0.60 * ((0.45 * strength_conf) + (0.35 * liquidity_conf) + (0.20 * ev_conf)));
                    if (regime_it != strategy_regime_edge.end()) {
                        const auto& regime_stat = regime_it->second;
                        if (regime_stat.trades >= 10 &&
                            regime_stat.expectancy() >= 4.0 &&
                            regime_stat.winRate() >= 0.52 &&
                            regime_stat.profitFactor() >= 1.05) {
                            evidence_conf = std::min(1.0, evidence_conf + 0.10);
                        }
                    }

                    const double rr_cap_ref = alpha_head_fallback_candidate ? 0.30 : 0.38;
                    const double edge_cap_ref = alpha_head_fallback_candidate ? 0.00055 : 0.00082;
                    const double rr_pressure = std::clamp(adaptive_rr_add_regime / rr_cap_ref, 0.0, 1.0);
                    const double edge_pressure = std::clamp(adaptive_edge_add_regime / edge_cap_ref, 0.0, 1.0);
                    const double joint_pressure = std::clamp((rr_pressure + edge_pressure) * 0.5, 0.0, 1.0);

                    double rr_relax = std::min(adaptive_rr_add_regime * 0.30, 0.08 * evidence_conf * joint_pressure);
                    double edge_relax = std::min(adaptive_edge_add_regime * 0.34, 0.00014 * evidence_conf * joint_pressure);
                    if (favorable_recovery_signal) {
                        rr_relax *= 1.10;
                        edge_relax *= 1.10;
                    }

                    adaptive_rr_add_regime = std::max(0.0, adaptive_rr_add_regime - rr_relax);
                    adaptive_edge_add_regime = std::max(0.0, adaptive_edge_add_regime - edge_relax);
                }
            }
            const double regime_rr_global_cap = alpha_head_fallback_candidate ? 0.30 : 0.38;
            const double regime_edge_global_cap = alpha_head_fallback_candidate ? 0.00055 : 0.00082;
            adaptive_rr_add_regime = std::clamp(adaptive_rr_add_regime, -0.12, regime_rr_global_cap);
            adaptive_edge_add_regime = std::clamp(adaptive_edge_add_regime, -0.00020, regime_edge_global_cap);
            if (alpha_head_fallback_candidate) {
                required_strength_floor = std::max(0.46, required_strength_floor - 0.05);
            }
            required_strength_floor = std::clamp(
                required_strength_floor,
                alpha_head_fallback_candidate ? 0.34 : 0.35,
                alpha_head_fallback_candidate ? 0.78 : 0.92
            );
            const bool alpha_head_quality_override =
                alpha_head_fallback_candidate &&
                !regime_pattern_block &&
                archetype_ready &&
                best_signal.expected_value >= 0.0007 &&
                best_signal.liquidity_score >= 58.0 &&
                best_signal.strength >= (required_strength_floor - 0.03);
            const bool pattern_strength_ok =
                !regime_pattern_block &&
                archetype_ready &&
                (best_signal.strength >= required_strength_floor || alpha_head_quality_override);
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
                // Conservative baseline edge-floor calibration:
                // adjust only in small bounded ranges using broad quality/context signals.
                const double edge_floor_nominal = computeContextualEdgeGateFloor(
                    best_signal,
                    tuned_cfg,
                    tuned_cfg.min_expected_edge_pct
                );
                double edge_base_shift = 0.0;

                if (best_signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                    best_signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
                    edge_base_shift += 0.00006;
                } else if (best_signal.market_regime == analytics::MarketRegime::TRENDING_UP &&
                           best_signal.strength >= 0.74 &&
                           best_signal.liquidity_score >= 62.0) {
                    edge_base_shift -= 0.00004;
                }

                if (best_signal.liquidity_score > 0.0 && best_signal.liquidity_score < 50.0) {
                    edge_base_shift += 0.00005;
                } else if (best_signal.liquidity_score >= 65.0 &&
                           best_signal.expected_value >= 0.0009) {
                    edge_base_shift -= 0.00003;
                }

                auto edge_base_stat_it = strategy_edge.find(best_signal.strategy_name);
                if (edge_base_stat_it != strategy_edge.end()) {
                    const auto& stat = edge_base_stat_it->second;
                    if (stat.trades >= std::max(8, engine_config_.min_strategy_trades_for_ev / 2)) {
                        const double stat_conf = std::clamp(
                            (static_cast<double>(stat.trades) - 8.0) / 24.0,
                            0.35,
                            1.0
                        );
                        const double wr = stat.winRate();
                        const double pf = stat.profitFactor();
                        const double exp_krw = stat.expectancy();

                        if (exp_krw >= 6.0 && pf >= 1.10 && wr >= 0.54) {
                            edge_base_shift -= (0.00007 * stat_conf);
                        } else if (exp_krw <= -8.0 || (pf < 0.90 && wr < 0.45)) {
                            edge_base_shift += (0.00008 * stat_conf);
                        } else if (exp_krw < 0.0 || pf < 1.00) {
                            edge_base_shift += (0.00004 * stat_conf);
                        }
                    }
                }

                if (favorable_recovery_signal) {
                    edge_base_shift -= 0.00005;
                }
                if (alpha_head_fallback_candidate) {
                    edge_base_shift = std::max(edge_base_shift, -0.00003);
                }

                // Holdout guardrails for edge baseline:
                // relax only when edge pressure dominates under favorable quality contexts,
                // and avoid extra relaxation when RR pressure is already elevated.
                const double rr_history_cap_ref = alpha_head_fallback_candidate ? 0.24 : 0.32;
                const double edge_history_cap_ref = alpha_head_fallback_candidate ? 0.00042 : 0.00062;
                const double rr_regime_pressure = std::clamp(
                    std::max(0.0, adaptive_rr_add_regime) / std::max(0.12, regime_rr_global_cap),
                    0.0,
                    1.0
                );
                const double rr_history_pressure = std::clamp(
                    std::max(0.0, adaptive_rr_add_history) / rr_history_cap_ref,
                    0.0,
                    1.0
                );
                const double edge_regime_pressure = std::clamp(
                    std::max(0.0, adaptive_edge_add_regime) / std::max(0.00020, regime_edge_global_cap),
                    0.0,
                    1.0
                );
                const double edge_history_pressure = std::clamp(
                    std::max(0.0, adaptive_edge_add_history) / edge_history_cap_ref,
                    0.0,
                    1.0
                );
                const double rr_pressure = std::clamp(
                    (0.72 * rr_regime_pressure) + (0.28 * rr_history_pressure),
                    0.0,
                    1.0
                );
                const double edge_pressure = std::clamp(
                    (0.72 * edge_regime_pressure) + (0.28 * edge_history_pressure),
                    0.0,
                    1.0
                );
                const bool hostile_regime_for_base =
                    best_signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                    best_signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
                const bool favorable_edge_context =
                    !hostile_regime_for_base &&
                    best_signal.strength >= 0.72 &&
                    best_signal.liquidity_score >= 60.0 &&
                    best_signal.expected_value >= 0.00080;
                const bool supportive_edge_context =
                    !hostile_regime_for_base &&
                    best_signal.strength >= 0.66 &&
                    best_signal.liquidity_score >= 55.0 &&
                    best_signal.expected_value >= 0.00065;
                if (rr_pressure >= 0.62 && edge_pressure <= rr_pressure) {
                    edge_base_shift += (0.00003 * rr_pressure);
                    edge_base_shift = std::max(edge_base_shift, -0.00002);
                } else if (favorable_edge_context &&
                           edge_pressure >= 0.48 &&
                           rr_pressure <= 0.46) {
                    const double relax_conf = std::clamp(
                        (edge_pressure - rr_pressure + 0.05) / 0.55,
                        0.0,
                        1.0
                    );
                    edge_base_shift -= (0.00006 * relax_conf);
                } else if (favorable_edge_context &&
                           rr_pressure <= 0.46 &&
                           edge_pressure < 0.48) {
                    const double low_pressure_relax = std::clamp(
                        (0.48 - edge_pressure) / 0.48,
                        0.0,
                        1.0
                    );
                    edge_base_shift -= (0.00003 + (0.00002 * low_pressure_relax));
                } else if (supportive_edge_context &&
                           rr_pressure <= 0.42) {
                    edge_base_shift -= 0.00003;
                } else if (!hostile_regime_for_base &&
                           favorable_recovery_signal &&
                           rr_pressure <= 0.40 &&
                           edge_pressure >= 0.40) {
                    edge_base_shift -= 0.00002;
                }

                double edge_shift_min = hostile_regime_for_base ? -0.00010 : -0.00015;
                double edge_floor_min_ratio = hostile_regime_for_base ? 0.72 : 0.66;
                if (rr_pressure >= 0.62) {
                    edge_shift_min = -0.00006;
                    edge_floor_min_ratio = 0.74;
                } else if (favorable_edge_context && rr_pressure <= 0.40) {
                    edge_shift_min = -0.00020;
                    edge_floor_min_ratio = 0.60;
                } else if (favorable_edge_context &&
                           rr_pressure <= 0.48 &&
                           edge_pressure >= 0.35) {
                    edge_shift_min = -0.00015;
                    edge_floor_min_ratio = 0.66;
                }

                edge_base_shift = std::clamp(edge_base_shift, edge_shift_min, 0.00014);
                const double edge_floor_min = std::max(0.00030, edge_floor_nominal * edge_floor_min_ratio);
                const double edge_floor_max = edge_floor_nominal + 0.00018;
                tuned_cfg.min_expected_edge_pct = std::clamp(
                    edge_floor_nominal + edge_base_shift,
                    edge_floor_min,
                    edge_floor_max
                );
            }
            const double rr_gate_base = tuned_cfg.min_reward_risk;
            const double edge_gate_base = tuned_cfg.min_expected_edge_pct;
            if (core_risk_enabled) {
                const double adaptive_rr_add = adaptive_rr_add_history + adaptive_rr_add_regime;
                const double adaptive_edge_add = adaptive_edge_add_history + adaptive_edge_add_regime;
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
            if (core_risk_enabled && alpha_head_fallback_candidate) {
                const bool hostile_or_thin =
                    best_signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                    best_signal.market_regime == analytics::MarketRegime::TRENDING_DOWN ||
                    (best_signal.liquidity_score > 0.0 && best_signal.liquidity_score < 50.0);
                double rr_relax = hostile_or_thin ? 0.06 : 0.16;
                double edge_relax = hostile_or_thin ? 0.00035 : 0.00100;
                if (!alpha_head_relief_eligible) {
                    rr_relax *= 0.35;
                    edge_relax *= 0.30;
                }
                tuned_cfg.min_reward_risk = std::max(1.03, tuned_cfg.min_reward_risk - rr_relax);
                tuned_cfg.min_expected_edge_pct = std::max(
                    std::max(0.00035, engine_config_.min_expected_edge_pct * 0.35),
                    tuned_cfg.min_expected_edge_pct - edge_relax
                );
            }
            if (core_risk_enabled && favorable_recovery_signal) {
                // Entry-frequency recovery path: in favorable regimes, ease gates slightly
                // only after prolonged no-entry streak and only for high-quality signals.
                tuned_cfg.min_reward_risk = std::max(1.05, tuned_cfg.min_reward_risk - 0.10);
                tuned_cfg.min_expected_edge_pct = std::max(
                    engine_config_.min_expected_edge_pct * 0.80,
                    tuned_cfg.min_expected_edge_pct * 0.82
                );
            }
            double effective_rr_adder_history = 0.0;
            double effective_rr_adder_regime = 0.0;
            double effective_edge_adder_history = 0.0;
            double effective_edge_adder_regime = 0.0;
            if (core_risk_enabled) {
                const double effective_rr_raise = std::max(0.0, tuned_cfg.min_reward_risk - rr_gate_base);
                const double history_positive = std::max(0.0, adaptive_rr_add_history);
                const double regime_positive = std::max(0.0, adaptive_rr_add_regime);
                const double positive_sum = history_positive + regime_positive;
                if (effective_rr_raise > 1e-9 && positive_sum > 1e-9) {
                    effective_rr_adder_history = effective_rr_raise * (history_positive / positive_sum);
                    effective_rr_adder_regime = effective_rr_raise * (regime_positive / positive_sum);
                }
                const double effective_edge_raise = std::max(0.0, tuned_cfg.min_expected_edge_pct - edge_gate_base);
                const double history_edge_positive = std::max(0.0, adaptive_edge_add_history);
                const double regime_edge_positive = std::max(0.0, adaptive_edge_add_regime);
                const double edge_positive_sum = history_edge_positive + regime_edge_positive;
                if (effective_edge_raise > 1e-12 && edge_positive_sum > 1e-12) {
                    effective_edge_adder_history = effective_edge_raise * (history_edge_positive / edge_positive_sum);
                    effective_edge_adder_regime = effective_edge_raise * (regime_edge_positive / edge_positive_sum);
                }
            }

            normalizeSignalStopLossByRegime(best_signal, best_signal.market_regime);
            const bool rr_rebalance_ok = rebalanceSignalRiskReward(best_signal, tuned_cfg);
            const bool regime_gate_ok =
                !core_risk_enabled ||
                passesRegimeGate(best_signal.market_regime, engine_config_);
            const EntryQualityGateSnapshot entry_quality_eval =
                core_risk_enabled
                ? evaluateEntryQualityGate(
                    best_signal,
                    tuned_cfg,
                    rr_gate_base,
                    edge_gate_base,
                    effective_rr_adder_history,
                    effective_rr_adder_regime,
                    effective_edge_adder_history,
                    effective_edge_adder_regime
                )
                : EntryQualityGateSnapshot{};
            const bool entry_quality_gate_ok =
                !core_risk_enabled ||
                entry_quality_eval.pass;
            const bool risk_gate_prereq_ok =
                !core_risk_enabled ||
                (strategy_ev_ok && regime_gate_ok);
            const double reward_risk_ratio = entry_quality_eval.reward_risk_ratio;
            const double calibrated_expected_edge_pct = entry_quality_eval.calibrated_expected_edge_pct;
            const SecondStageConfirmationSnapshot second_stage_eval =
                !core_risk_enabled ||
                !rr_rebalance_ok
                ? SecondStageConfirmationSnapshot{}
                : evaluateSecondStageEntryConfirmation(
                    best_signal,
                    tuned_cfg,
                    reward_risk_ratio,
                    entry_quality_eval.reward_risk_gate_effective,
                    calibrated_expected_edge_pct,
                    entry_quality_eval.expected_edge_gate_effective
                );
            const bool second_stage_ok =
                !core_risk_enabled ||
                (rr_rebalance_ok && second_stage_eval.pass);
            const TwoHeadEntryAggregationSnapshot two_head_eval =
                !core_risk_enabled ||
                !rr_rebalance_ok
                ? TwoHeadEntryAggregationSnapshot{}
                : evaluateTwoHeadEntryAggregation(
                    best_signal,
                    tuned_cfg,
                    entry_quality_eval,
                    second_stage_eval
                );
            if (core_risk_enabled && rr_rebalance_ok && second_stage_eval.rr_margin_near_miss) {
                entry_funnel_.second_stage_rr_margin_near_miss_observed++;
            }
            if (core_risk_enabled && rr_rebalance_ok && second_stage_eval.rr_margin_soft_score_applied) {
                entry_funnel_.second_stage_rr_margin_soft_score_applied++;
            }
            if (core_risk_enabled && rr_rebalance_ok && two_head_eval.rr_margin_near_miss_relief_applied) {
                entry_funnel_.second_stage_rr_margin_near_miss_relief_applied++;
            }
            if (core_risk_enabled && rr_rebalance_ok && two_head_eval.rr_margin_near_miss_head_score_floor_applied) {
                entry_funnel_.two_head_aggregation_rr_margin_near_miss_head_score_floor_applied++;
            }
            if (core_risk_enabled && rr_rebalance_ok && two_head_eval.rr_margin_near_miss_floor_relax_applied) {
                entry_funnel_.two_head_aggregation_rr_margin_near_miss_floor_relax_applied++;
            }
            if (core_risk_enabled && rr_rebalance_ok && two_head_eval.rr_margin_near_miss_adaptive_floor_relax_applied) {
                entry_funnel_.two_head_aggregation_rr_margin_near_miss_adaptive_floor_relax_applied++;
            }
            if (core_risk_enabled && rr_rebalance_ok && two_head_eval.rr_margin_near_miss_surplus_compensation_applied) {
                entry_funnel_.two_head_aggregation_rr_margin_near_miss_surplus_compensation_applied++;
            }
            if (core_risk_enabled && rr_rebalance_ok && two_head_eval.rr_margin_near_miss_relief_blocked) {
                entry_funnel_.two_head_aggregation_rr_margin_near_miss_relief_blocked++;
                if (two_head_eval.rr_margin_near_miss_relief_blocked_override_disallowed) {
                    entry_funnel_.two_head_aggregation_rr_margin_near_miss_relief_blocked_override_disallowed++;
                }
                if (two_head_eval.rr_margin_near_miss_relief_blocked_entry_floor) {
                    entry_funnel_.two_head_aggregation_rr_margin_near_miss_relief_blocked_entry_floor++;
                }
                if (two_head_eval.rr_margin_near_miss_relief_blocked_second_stage_floor) {
                    entry_funnel_.two_head_aggregation_rr_margin_near_miss_relief_blocked_second_stage_floor++;
                }
                if (two_head_eval.rr_margin_near_miss_relief_blocked_aggregate_score) {
                    entry_funnel_.two_head_aggregation_rr_margin_near_miss_relief_blocked_aggregate_score++;
                }
            }
            const bool two_head_gate_ok =
                !core_risk_enabled ||
                ((tuned_cfg.enable_two_head_entry_second_stage_aggregation
                    ? two_head_eval.aggregate_pass
                    : (entry_quality_gate_ok && second_stage_ok)));
            const bool risk_gate_ok =
                !core_risk_enabled ||
                (risk_gate_prereq_ok && two_head_gate_ok);
            if (!pattern_strength_ok) {
                entry_funnel_.blocked_pattern_gate++;
                if (!archetype_ready) {
                    markEntryReject("blocked_pattern_missing_archetype");
                } else {
                    markEntryReject("blocked_pattern_strength_or_regime");
                }
            } else if (!rr_rebalance_ok) {
                entry_funnel_.blocked_rr_rebalance++;
                markEntryReject("blocked_rr_rebalance");
            } else if (!risk_gate_ok) {
                entry_funnel_.blocked_risk_gate++;
                if (!risk_gate_prereq_ok) {
                    if (!strategy_ev_ok) {
                        entry_funnel_.blocked_risk_gate_strategy_ev++;
                        switch (strategy_ev_fail_kind) {
                            case StrategyEvFailKind::PreCatastrophicNoRecovery:
                                entry_funnel_.blocked_risk_gate_strategy_ev_pre_catastrophic++;
                                break;
                            case StrategyEvFailKind::SevereThresholdNoRecovery:
                                entry_funnel_.blocked_risk_gate_strategy_ev_severe_threshold++;
                                break;
                            case StrategyEvFailKind::CatastrophicHistory:
                                entry_funnel_.blocked_risk_gate_strategy_ev_catastrophic_history++;
                                break;
                            case StrategyEvFailKind::LossAsymmetryCollapse:
                                entry_funnel_.blocked_risk_gate_strategy_ev_loss_asymmetry++;
                                break;
                            case StrategyEvFailKind::None:
                            default:
                                entry_funnel_.blocked_risk_gate_strategy_ev_unknown++;
                                break;
                        }
                        markEntryReject("blocked_risk_gate_strategy_ev");
                    } else if (!regime_gate_ok) {
                        entry_funnel_.blocked_risk_gate_regime++;
                        markEntryReject("blocked_risk_gate_regime");
                    } else {
                        entry_funnel_.blocked_risk_gate_other++;
                        markEntryReject("blocked_risk_gate_other");
                    }
                } else {
                    if (core_risk_enabled &&
                        tuned_cfg.enable_two_head_entry_second_stage_aggregation &&
                        !two_head_eval.aggregate_pass) {
                        entry_funnel_.two_head_aggregation_blocked++;
                        if (two_head_eval.rr_margin_near_miss_relief_blocked) {
                            LOG_INFO(
                                "{} near-miss relief blocked [{}]: override_disallowed={}, entry_floor={}, second_floor={}, aggregate_score={}, head_floor_applied={}, head_floor_value={:.4f}, floor_relax={}, adaptive_floor_relax={}, adaptive_relax_strength={:.4f}, surplus_comp={}, surplus_second={}, surplus_agg={}, entry_surplus={:.4f}, second_deficit={:.4f}, aggregate_deficit={:.4f}, edge_score={:.4f}, surplus_bonus={:.4f}",
                                market_name_,
                                best_signal.strategy_name,
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
                                two_head_eval.rr_margin_near_miss_surplus_bonus
                            );
                        }
                    }
                    const bool attribute_second_stage =
                        tuned_cfg.enable_two_head_entry_second_stage_aggregation
                        ? shouldAttributeTwoHeadFailureToSecondStage(two_head_eval)
                        : (entry_quality_gate_ok && !second_stage_ok);
                    if (!attribute_second_stage && !entry_quality_gate_ok) {
                        entry_funnel_.blocked_risk_gate_entry_quality++;
                        const double edge_gap_effective =
                            entry_quality_eval.expected_edge_gate_effective -
                            entry_quality_eval.calibrated_expected_edge_pct;
                        auto markEdgeGapBucket = [&]() {
                            entry_quality_edge_gap_buckets_[edgeGapBucket(edge_gap_effective)]++;
                        };
                        switch (entry_quality_eval.failure) {
                            case EntryQualityGateSnapshot::FailureKind::InvalidPriceLevels:
                                entry_funnel_.blocked_risk_gate_entry_quality_invalid_levels++;
                                break;
                            case EntryQualityGateSnapshot::FailureKind::RewardRiskBase:
                                entry_funnel_.blocked_risk_gate_entry_quality_rr++;
                                entry_funnel_.blocked_risk_gate_entry_quality_rr_base++;
                                break;
                            case EntryQualityGateSnapshot::FailureKind::RewardRiskAdaptive:
                                entry_funnel_.blocked_risk_gate_entry_quality_rr++;
                                entry_funnel_.blocked_risk_gate_entry_quality_rr_adaptive++;
                                switch (entry_quality_eval.adaptive_rr_source) {
                                    case EntryQualityGateSnapshot::AdaptiveSource::History:
                                        entry_funnel_.blocked_risk_gate_entry_quality_rr_adaptive_history++;
                                        break;
                                    case EntryQualityGateSnapshot::AdaptiveSource::Regime:
                                        entry_funnel_.blocked_risk_gate_entry_quality_rr_adaptive_regime++;
                                        break;
                                    case EntryQualityGateSnapshot::AdaptiveSource::Mixed:
                                        entry_funnel_.blocked_risk_gate_entry_quality_rr_adaptive_mixed++;
                                        break;
                                    case EntryQualityGateSnapshot::AdaptiveSource::Unknown:
                                    default:
                                        break;
                                }
                                break;
                            case EntryQualityGateSnapshot::FailureKind::ExpectedEdgeBase:
                                entry_funnel_.blocked_risk_gate_entry_quality_edge++;
                                entry_funnel_.blocked_risk_gate_entry_quality_edge_base++;
                                markEdgeGapBucket();
                                break;
                            case EntryQualityGateSnapshot::FailureKind::ExpectedEdgeAdaptive:
                                entry_funnel_.blocked_risk_gate_entry_quality_edge++;
                                entry_funnel_.blocked_risk_gate_entry_quality_edge_adaptive++;
                                markEdgeGapBucket();
                                switch (entry_quality_eval.adaptive_edge_source) {
                                    case EntryQualityGateSnapshot::AdaptiveSource::History:
                                        entry_funnel_.blocked_risk_gate_entry_quality_edge_adaptive_history++;
                                        break;
                                    case EntryQualityGateSnapshot::AdaptiveSource::Regime:
                                        entry_funnel_.blocked_risk_gate_entry_quality_edge_adaptive_regime++;
                                        break;
                                    case EntryQualityGateSnapshot::AdaptiveSource::Mixed:
                                        entry_funnel_.blocked_risk_gate_entry_quality_edge_adaptive_mixed++;
                                        break;
                                    case EntryQualityGateSnapshot::AdaptiveSource::Unknown:
                                    default:
                                        break;
                                }
                                break;
                            case EntryQualityGateSnapshot::FailureKind::RewardRiskAndExpectedEdgeBase:
                                entry_funnel_.blocked_risk_gate_entry_quality_rr_edge++;
                                entry_funnel_.blocked_risk_gate_entry_quality_rr_edge_base++;
                                markEdgeGapBucket();
                                break;
                            case EntryQualityGateSnapshot::FailureKind::RewardRiskAndExpectedEdgeAdaptive:
                                entry_funnel_.blocked_risk_gate_entry_quality_rr_edge++;
                                entry_funnel_.blocked_risk_gate_entry_quality_rr_edge_adaptive++;
                                markEdgeGapBucket();
                                switch (entry_quality_eval.adaptive_rr_source) {
                                    case EntryQualityGateSnapshot::AdaptiveSource::History:
                                        entry_funnel_.blocked_risk_gate_entry_quality_rr_edge_adaptive_history++;
                                        break;
                                    case EntryQualityGateSnapshot::AdaptiveSource::Regime:
                                        entry_funnel_.blocked_risk_gate_entry_quality_rr_edge_adaptive_regime++;
                                        break;
                                    case EntryQualityGateSnapshot::AdaptiveSource::Mixed:
                                        entry_funnel_.blocked_risk_gate_entry_quality_rr_edge_adaptive_mixed++;
                                        break;
                                    case EntryQualityGateSnapshot::AdaptiveSource::Unknown:
                                    default:
                                        break;
                                }
                                break;
                            case EntryQualityGateSnapshot::FailureKind::None:
                            default:
                                break;
                        }
                        markEntryReject(entryQualityFailureReasonCode(entry_quality_eval));
                    } else if (attribute_second_stage && !second_stage_ok) {
                        entry_funnel_.blocked_second_stage_confirmation++;
                        switch (second_stage_eval.failure) {
                            case SecondStageConfirmationFailureKind::RRMarginShortfall:
                                entry_funnel_.blocked_second_stage_confirmation_rr_margin++;
                                if (second_stage_eval.rr_margin_near_miss) {
                                    entry_funnel_.blocked_second_stage_confirmation_rr_margin_near_miss++;
                                }
                                break;
                            case SecondStageConfirmationFailureKind::EdgeMarginShortfall:
                                entry_funnel_.blocked_second_stage_confirmation_edge_margin++;
                                break;
                            case SecondStageConfirmationFailureKind::HostileSafetyAdders:
                                entry_funnel_.blocked_second_stage_confirmation_hostile_safety_adders++;
                                switch (second_stage_eval.safety_source) {
                                    case SecondStageConfirmationSafetySource::Regime:
                                        entry_funnel_.blocked_second_stage_confirmation_hostile_regime_safety_adders++;
                                        break;
                                    case SecondStageConfirmationSafetySource::Liquidity:
                                        entry_funnel_.blocked_second_stage_confirmation_hostile_liquidity_safety_adders++;
                                        break;
                                    case SecondStageConfirmationSafetySource::StrategyHistory:
                                        entry_funnel_.blocked_second_stage_confirmation_hostile_history_safety_adders++;
                                        switch (classifySecondStageHistorySafetySeverity(best_signal)) {
                                            case SecondStageHistorySafetySeverity::Mild:
                                                entry_funnel_.blocked_second_stage_confirmation_hostile_history_mild_safety_adders++;
                                                break;
                                            case SecondStageHistorySafetySeverity::Moderate:
                                                entry_funnel_.blocked_second_stage_confirmation_hostile_history_moderate_safety_adders++;
                                                break;
                                            case SecondStageHistorySafetySeverity::Severe:
                                                entry_funnel_.blocked_second_stage_confirmation_hostile_history_severe_safety_adders++;
                                                break;
                                        }
                                        break;
                                    case SecondStageConfirmationSafetySource::DynamicTighten:
                                        entry_funnel_.blocked_second_stage_confirmation_hostile_dynamic_tighten_safety_adders++;
                                        break;
                                    case SecondStageConfirmationSafetySource::Unknown:
                                    default:
                                        break;
                                }
                                break;
                            case SecondStageConfirmationFailureKind::None:
                            default:
                                break;
                        }
                        markEntryReject("blocked_second_stage_confirmation");
                    } else if (!entry_quality_gate_ok) {
                        entry_funnel_.blocked_risk_gate_entry_quality++;
                        markEntryReject(entryQualityFailureReasonCode(entry_quality_eval));
                    } else if (!second_stage_ok) {
                        entry_funnel_.blocked_second_stage_confirmation++;
                        markEntryReject("blocked_second_stage_confirmation");
                    } else {
                        entry_funnel_.blocked_risk_gate_other++;
                        markEntryReject("blocked_risk_gate_other");
                    }
                }
            } else if (pattern_strength_ok && rr_rebalance_ok && risk_gate_ok) {
                if (core_risk_enabled &&
                    tuned_cfg.enable_two_head_entry_second_stage_aggregation &&
                    two_head_eval.override_applied) {
                    entry_funnel_.two_head_aggregation_override_accept++;
                    if (two_head_eval.rr_margin_near_miss_relief_applied) {
                        entry_funnel_.two_head_aggregation_override_accept_rr_margin_near_miss++;
                    }
                    LOG_INFO(
                        "{} two-head aggregation override accepted [{}]: entry_score {:.3f}, second_score {:.3f}, agg {:.3f}",
                        market_name_,
                        best_signal.strategy_name,
                        two_head_eval.entry_head_score,
                        two_head_eval.second_stage_head_score,
                        two_head_eval.aggregate_score
                    );
                    if (two_head_eval.rr_margin_near_miss_relief_applied) {
                        LOG_INFO(
                            "{} two-head near-miss relief [{}]: rr_gap {:.4f}, min_rr_margin {:.4f}, head_floor_applied={}, head_floor_value={:.4f}, floor_relax={}, adaptive_floor_relax={}, adaptive_relax_strength={:.4f}, surplus_comp={}, surplus_second={}, surplus_agg={}, surplus_bonus {:.4f}, eff_second_floor {:.3f}, eff_agg_floor {:.3f}",
                            market_name_,
                            best_signal.strategy_name,
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
                            two_head_eval.effective_min_aggregate_score
                        );
                    }
                }
                if (core_risk_enabled && entry_quality_eval.adaptive_relief_applied) {
                    const double relief_scale = std::clamp(
                        tuned_cfg.entry_quality_adaptive_relief_position_scale,
                        0.20,
                        1.0
                    );
                    LOG_INFO("{} entry-quality adaptive relief applied [{}]: rr {:.2f}>=gate {:.2f}, edge {:.3f}%>=gate {:.3f}%, size_scale {:.2f}x",
                             market_name_,
                             best_signal.strategy_name,
                             reward_risk_ratio,
                             entry_quality_eval.reward_risk_gate_effective,
                             calibrated_expected_edge_pct * 100.0,
                             entry_quality_eval.expected_edge_gate_effective * 100.0,
                             relief_scale);
                    best_signal.position_size *= relief_scale;
                }
                const double probabilistic_position_scale =
                    probabilisticPositionScaleForEntry(engine_config_, best_signal);
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

        if (entry_executed) {
            no_entry_streak_candles_ = 0;
        } else {
            no_entry_streak_candles_++;
        }
    } else {
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










