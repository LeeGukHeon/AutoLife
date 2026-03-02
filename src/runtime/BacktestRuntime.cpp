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
#include "gate_vnext/GateVNext.h"
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
#include <utility>

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

struct GateVNextBundleSettings {
    int quality_topk = 5;
    double ev_scale_bps = 10.0;
};

GateVNextBundleSettings loadGateVNextBundleSettings(
    const std::filesystem::path& bundle_path
) {
    GateVNextBundleSettings out{};
    try {
        std::ifstream in(bundle_path, std::ios::binary);
        if (!in.is_open()) {
            return out;
        }
        nlohmann::json root;
        in >> root;
        if (!root.is_object()) {
            return out;
        }
        nlohmann::json vnext = nlohmann::json::object();
        if (root.contains("gate_vnext") && root["gate_vnext"].is_object()) {
            vnext = root["gate_vnext"];
        } else if (root.contains("vnext") && root["vnext"].is_object()) {
            vnext = root["vnext"];
        }
        if (vnext.is_object()) {
            out.quality_topk = std::max(1, vnext.value("quality_topk", out.quality_topk));
            out.ev_scale_bps = std::max(1e-9, vnext.value("ev_scale_bps", out.ev_scale_bps));
        }
        if (root.contains("quality_topk")) {
            out.quality_topk = std::max(1, root.value("quality_topk", out.quality_topk));
        }
        if (root.contains("ev_scale_bps")) {
            out.ev_scale_bps = std::max(1e-9, root.value("ev_scale_bps", out.ev_scale_bps));
        }
    } catch (...) {
        return GateVNextBundleSettings{};
    }
    return out;
}

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
        item["expected_edge_calibrated_bps"] = d.expected_edge_calibrated_bps;
        item["expected_edge_used_for_gate_bps"] = d.expected_edge_used_for_gate_bps;
        item["cost_bps_estimate_used"] = d.cost_bps_estimate_used;
        item["edge_semantics"] = d.edge_semantics;
        item["root_cost_model_enabled_configured"] = d.root_cost_model_enabled_configured;
        item["phase3_cost_model_enabled_configured"] = d.phase3_cost_model_enabled_configured;
        item["root_cost_model_enabled_effective"] = d.root_cost_model_enabled_effective;
        item["phase3_cost_model_enabled_effective"] = d.phase3_cost_model_enabled_effective;
        item["edge_semantics_guard_violation"] = d.edge_semantics_guard_violation;
        item["edge_semantics_guard_forced_off"] = d.edge_semantics_guard_forced_off;
        item["edge_semantics_guard_action"] = d.edge_semantics_guard_action;
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

std::string normalizeRegimeEntryDisableKey(const std::string& raw_key) {
    const std::string key = toLowerCopy(raw_key);
    if (key == "ranging" || key == "range") {
        return "RANGING";
    }
    if (key == "trending_up" || key == "trending-up" || key == "trending up" || key == "up") {
        return "TRENDING_UP";
    }
    if (key == "trending_down" || key == "trending-down" || key == "trending down" || key == "down") {
        return "TRENDING_DOWN";
    }
    if (key == "high_volatility" || key == "high-volatility" || key == "high volatility" ||
        key == "volatile" || key == "hostile") {
        return "HIGH_VOLATILITY";
    }
    if (key == "unknown") {
        return "UNKNOWN";
    }
    if (key == "any" || key == "default") {
        return "ANY";
    }
    return std::string();
}

std::string normalizeRegimeEntryDisableKey(analytics::MarketRegime regime) {
    return normalizeRegimeEntryDisableKey(
        autolife::common::runtime_diag::marketRegimeLabel(regime)
    );
}

bool isRegimeEntryDisabled(
    const std::map<std::string, bool>& disable_map,
    analytics::MarketRegime regime,
    std::string* matched_key = nullptr
) {
    const std::string regime_key = normalizeRegimeEntryDisableKey(regime);
    if (!regime_key.empty()) {
        const auto it = disable_map.find(regime_key);
        if (it != disable_map.end() && it->second) {
            if (matched_key != nullptr) {
                *matched_key = regime_key;
            }
            return true;
        }
    }
    const auto any_it = disable_map.find("ANY");
    if (any_it != disable_map.end() && any_it->second) {
        if (matched_key != nullptr) {
            *matched_key = "ANY";
        }
        return true;
    }
    return false;
}

using autolife::common::signal_policy::normalizeStrategyToken;
using autolife::common::signal_policy::isStrategyEnabledByConfig;
using autolife::common::signal_policy::rebalanceSignalRiskReward;
using autolife::common::execution_guard::computeRealtimeEntryVetoThresholds;
using autolife::common::execution_guard::computeDynamicSlippageThresholds;
using autolife::common::execution_guard::computeMarketHostilityScore;
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
    std::string prob_model_backend = "sgd";
    double prob_h1_raw = 0.5;
    double prob_h5_raw = 0.5;
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
    double expected_edge_calibrated_bps = 0.0;
    double expected_edge_calibrated_raw_bps = 0.0;
    double expected_edge_calibrated_corrected_bps = 0.0;
    double expected_edge_used_for_gate_bps = 0.0;
    double expected_edge_pct = 0.0;
    std::string edge_semantics = "net";
    bool root_cost_model_enabled_configured = false;
    bool phase3_cost_model_enabled_configured = false;
    bool root_cost_model_enabled_effective = false;
    bool phase3_cost_model_enabled_effective = false;
    bool edge_semantics_guard_violation = false;
    bool edge_semantics_guard_forced_off = false;
    std::string edge_semantics_guard_action = "none";
    double ev_confidence = 1.0;
    bool edge_regressor_used = false;
    bool ev_calibration_applied = false;
    double cost_entry_pct = 0.0;
    double cost_exit_pct = 0.0;
    double cost_tail_pct = 0.0;
    double cost_used_pct = 0.0;
    double cost_used_bps_estimate = 0.0;
    std::string cost_mode = "mean_mode";
    bool phase3_ev_calibration_enabled = false;
    bool phase3_cost_tail_enabled = false;
    bool phase3_adaptive_ev_blend_enabled = false;
    bool phase3_diagnostics_v2_enabled = false;
    bool phase3_regime_entry_disable_enabled = false;
    bool phase3_disable_ranging_entry = false;
    std::string phase3_strategy_exit_mode = "enforce";
    double phase3_tp_distance_trending_multiplier = 1.0;
    bool lgbm_ev_affine_enabled = false;
    bool lgbm_ev_affine_applied = false;
    double lgbm_ev_affine_scale = 1.0;
    double lgbm_ev_affine_shift = 0.0;
    std::map<std::string, bool> phase3_regime_entry_disable;
    bool phase4_portfolio_allocator_enabled = false;
    bool phase4_correlation_control_enabled = false;
    bool phase4_risk_budget_enabled = false;
    bool phase4_drawdown_governor_enabled = false;
    bool phase4_execution_aware_sizing_enabled = false;
    bool phase4_portfolio_diagnostics_enabled = false;
    autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::PortfolioAllocatorPolicy
        phase4_portfolio_allocator_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::RiskBudgetPolicy
        phase4_risk_budget_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::DrawdownGovernorPolicy
        phase4_drawdown_governor_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::CorrelationControlPolicy
        phase4_correlation_control_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::ExecutionAwareSizingPolicy
        phase4_execution_aware_sizing_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase3LiquidityVolumeGatePolicy
        phase3_liq_vol_gate_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase3FoundationStructureGatePolicy
        phase3_foundation_structure_gate_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase3BearReboundGuardPolicy
        phase3_bear_rebound_guard_policy{};
    autolife::analytics::ProbabilisticRuntimeModel::Phase3StopLossRiskPolicy
        phase3_stop_loss_risk_policy{};
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

constexpr size_t kPhase4CandidateSnapshotSampleLimit = 1200;
constexpr size_t kPhase4CorrelationNearCapSampleLimit = 48;
constexpr size_t kPhase4CorrelationPenaltyScoreSampleLimit = 96;
constexpr size_t kPhase4ClusterCapDebugSampleLimit = 64;

double correlationNearCapDistance(
    const BacktestEngine::Result::Phase4CorrelationNearCapSample& sample
) {
    const double cap = std::max(1.0e-9, sample.cluster_cap_value);
    return std::fabs(sample.headroom_before) / cap;
}

void upsertCorrelationNearCapSample(
    BacktestEngine::Result::Phase4PortfolioDiagnostics& diagnostics,
    BacktestEngine::Result::Phase4CorrelationNearCapSample sample
) {
    auto& rows = diagnostics.correlation_near_cap_candidates;
    if (rows.size() < kPhase4CorrelationNearCapSampleLimit) {
        rows.push_back(std::move(sample));
        return;
    }
    const auto worst_it = std::max_element(
        rows.begin(),
        rows.end(),
        [](const auto& lhs, const auto& rhs) {
            return correlationNearCapDistance(lhs) < correlationNearCapDistance(rhs);
        }
    );
    if (worst_it == rows.end()) {
        return;
    }
    if (correlationNearCapDistance(sample) >= correlationNearCapDistance(*worst_it)) {
        return;
    }
    *worst_it = std::move(sample);
}

double correlationPenaltyAbsMagnitude(
    const BacktestEngine::Result::Phase4CorrelationPenaltyScoreSample& sample
) {
    return std::fabs(sample.penalty);
}

void upsertCorrelationPenaltyScoreSample(
    BacktestEngine::Result::Phase4PortfolioDiagnostics& diagnostics,
    BacktestEngine::Result::Phase4CorrelationPenaltyScoreSample sample
) {
    auto& rows = diagnostics.correlation_penalty_score_samples;
    if (rows.size() < kPhase4CorrelationPenaltyScoreSampleLimit) {
        rows.push_back(std::move(sample));
        return;
    }
    const auto weakest_it = std::min_element(
        rows.begin(),
        rows.end(),
        [](const auto& lhs, const auto& rhs) {
            return correlationPenaltyAbsMagnitude(lhs) < correlationPenaltyAbsMagnitude(rhs);
        }
    );
    if (weakest_it == rows.end()) {
        return;
    }
    if (correlationPenaltyAbsMagnitude(sample) <= correlationPenaltyAbsMagnitude(*weakest_it)) {
        return;
    }
    *weakest_it = std::move(sample);
}

std::filesystem::path phase4CandidateArtifactPath() {
    return autolife::utils::PathUtils::resolveRelativePath("logs/phase4_portfolio_candidates_backtest.jsonl");
}

void resetPhase4CandidateArtifact() {
    const auto path = phase4CandidateArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
}

void appendPhase4CandidateArtifact(
    long long timestamp_ms,
    const std::string& market,
    const ProbabilisticRuntimeSnapshot& snapshot,
    const std::vector<autolife::strategy::Signal>& candidates,
    int selected_count,
    const autolife::risk::Position* open_position
) {
    if (candidates.empty()) {
        return;
    }

    nlohmann::json line;
    line["ts"] = normalizeTimestampMs(timestamp_ms);
    line["source"] = "backtest";
    line["market"] = market;
    line["phase4"] = {
        {"portfolio_allocator_enabled", snapshot.phase4_portfolio_allocator_enabled},
        {"allocator_top_k", snapshot.phase4_portfolio_allocator_policy.top_k},
        {"allocator_min_score", snapshot.phase4_portfolio_allocator_policy.min_score},
        {"risk_budget_per_market_cap", snapshot.phase4_risk_budget_policy.per_market_cap},
        {"risk_budget_gross_cap", snapshot.phase4_risk_budget_policy.gross_cap},
        {"risk_budget_cap", snapshot.phase4_risk_budget_policy.risk_budget_cap},
        {"risk_budget_regime_multipliers", snapshot.phase4_risk_budget_policy.regime_budget_multipliers},
        {"risk_budget_proxy_stop_pct", snapshot.phase4_risk_budget_policy.risk_proxy_stop_pct},
        {"dd_threshold_soft", snapshot.phase4_drawdown_governor_policy.dd_threshold_soft},
        {"dd_threshold_hard", snapshot.phase4_drawdown_governor_policy.dd_threshold_hard},
        {"dd_budget_multiplier_soft",
         snapshot.phase4_drawdown_governor_policy.budget_multiplier_soft},
        {"dd_budget_multiplier_hard",
         snapshot.phase4_drawdown_governor_policy.budget_multiplier_hard},
        {"correlation_default_cluster_cap",
         snapshot.phase4_correlation_control_policy.default_cluster_cap},
        {"correlation_market_cluster_count",
         static_cast<int>(snapshot.phase4_correlation_control_policy.market_cluster_map.size())},
        {"execution_liquidity_low_threshold",
         snapshot.phase4_execution_aware_sizing_policy.liquidity_low_threshold},
        {"execution_liquidity_mid_threshold",
         snapshot.phase4_execution_aware_sizing_policy.liquidity_mid_threshold},
        {"execution_min_position_size",
         snapshot.phase4_execution_aware_sizing_policy.min_position_size},
        {"correlation_control_enabled", snapshot.phase4_correlation_control_enabled},
        {"risk_budget_enabled", snapshot.phase4_risk_budget_enabled},
        {"drawdown_governor_enabled", snapshot.phase4_drawdown_governor_enabled},
        {"execution_aware_sizing_enabled", snapshot.phase4_execution_aware_sizing_enabled},
        {"portfolio_diagnostics_enabled", snapshot.phase4_portfolio_diagnostics_enabled},
    };
    line["position_state"] = {
        {"has_open_position", open_position != nullptr},
        {"quantity", open_position != nullptr ? open_position->quantity : 0.0},
        {"unrealized_pnl", open_position != nullptr ? open_position->unrealized_pnl : 0.0},
        {"time_in_position_minutes",
         open_position != nullptr
             ? std::max(0.0, static_cast<double>(normalizeTimestampMs(timestamp_ms) - open_position->entry_time) / 60000.0)
             : 0.0},
    };
    line["candidates"] = nlohmann::json::array();

    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& signal = candidates[i];
        nlohmann::json item;
        item["rank"] = static_cast<int>(i);
        item["selected"] = (static_cast<int>(i) < selected_count);
        item["market_id"] = signal.market;
        item["strategy_name"] = signal.strategy_name;
        item["decision_time"] = normalizeTimestampMs(timestamp_ms);
        item["expected_edge_after_cost_pct"] = signal.expected_value;
        item["expected_edge_tail_after_cost_pct"] = signal.expected_value;
        item["expected_edge_calibrated_bps"] = signal.phase3.expected_edge_calibrated_bps;
        item["expected_edge_used_for_gate_bps"] = signal.phase3.expected_edge_used_for_gate_bps;
        item["margin"] = signal.probabilistic_h5_margin;
        item["implied_win"] = signal.phase3.implied_win_runtime;
        item["prob_confidence_raw"] = snapshot.prob_h5_raw;
        item["prob_confidence_calibrated"] = signal.probabilistic_h5_calibrated;
        item["prob_confidence"] = signal.probabilistic_h5_calibrated;
        item["ev_confidence"] = signal.phase3.ev_confidence;
        item["regime"] = marketRegimeLabel(signal.market_regime);
        item["volatility_bucket"] = volatilityBucket(signal.volatility);
        item["liquidity_bucket"] = liquidityBucket(signal.liquidity_score);
        item["cost"] = {
            {"mode", signal.phase3.cost_mode},
            {"entry_pct", signal.phase3.cost_entry_pct},
            {"exit_pct", signal.phase3.cost_exit_pct},
            {"tail_pct", signal.phase3.cost_tail_pct},
            {"used_pct", signal.phase3.cost_used_pct},
            {"used_bps_estimate", signal.phase3.cost_used_bps_estimate},
        };
        item["execution_proxy"] = {
            {"signal_strength", signal.strength},
            {"liquidity_score", signal.liquidity_score},
            {"volatility", signal.volatility},
            {"position_size", signal.position_size},
        };
        item["policy_tags"] = {
            {"phase3_cost_mode", signal.phase3.cost_mode},
            {"edge_regressor_used", signal.phase3.edge_regressor_used},
            {"prob_model_backend", signal.phase3.prob_model_backend},
            {"edge_semantics", signal.phase3.edge_semantics},
            {"root_cost_model_enabled_configured", signal.phase3.root_cost_model_enabled_configured},
            {"phase3_cost_model_enabled_configured", signal.phase3.phase3_cost_model_enabled_configured},
            {"root_cost_model_enabled_effective", signal.phase3.root_cost_model_enabled_effective},
            {"phase3_cost_model_enabled_effective", signal.phase3.phase3_cost_model_enabled_effective},
            {"edge_semantics_guard_violation", signal.phase3.edge_semantics_guard_violation},
            {"edge_semantics_guard_forced_off", signal.phase3.edge_semantics_guard_forced_off},
            {"edge_semantics_guard_action", signal.phase3.edge_semantics_guard_action},
        };
        line["candidates"].push_back(std::move(item));
    }

    const auto path = phase4CandidateArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        LOG_WARN("Phase4 candidate artifact open failed: {}", path.string());
        return;
    }
    out << line.dump() << "\n";
}

std::filesystem::path entryStageFunnelArtifactPath() {
    return autolife::utils::PathUtils::resolveRelativePath("logs/entry_stage_funnel_backtest.jsonl");
}

void resetEntryStageFunnelArtifact() {
    const auto path = entryStageFunnelArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
}

void appendEntryStageFunnelAudit(
    long long timestamp_ms,
    const std::string& market,
    int candidates_total,
    int candidates_after_quality_topk,
    int candidates_after_sizing,
    int candidates_after_portfolio,
    int orders_submitted,
    int orders_filled,
    const std::map<std::string, int>& reject_reason_counts,
    double ev_at_selection,
    double ev_at_order_submit_check,
    int ev_mismatch_count
) {
    nlohmann::json line;
    line["ts"] = normalizeTimestampMs(timestamp_ms);
    line["source"] = "backtest";
    line["market"] = market;
    line["stages"] = {
        {"candidates_total", std::max(0, candidates_total)},
        {"candidates_after_quality_topk", std::max(0, candidates_after_quality_topk)},
        {"candidates_after_sizing", std::max(0, candidates_after_sizing)},
        {"candidates_after_portfolio", std::max(0, candidates_after_portfolio)},
        {"orders_submitted", std::max(0, orders_submitted)},
        {"orders_filled", std::max(0, orders_filled)}
    };
    line["stage_funnel_vnext"] = {
        {"s0_snapshots_valid", std::max(0, candidates_total)},
        {"s1_selected_topk", std::max(0, candidates_after_quality_topk)},
        {"s2_sized_count", std::max(0, candidates_after_sizing)},
        {"s3_exec_gate_pass", std::max(0, candidates_after_portfolio)},
        {"s4_submitted", std::max(0, orders_submitted)},
        {"s5_filled", std::max(0, orders_filled)},
    };
    nlohmann::json ev_consistency;
    ev_consistency["source"] = "signal.expected_value_ssot";
    if (std::isfinite(ev_at_selection)) {
        ev_consistency["ev_at_selection"] = ev_at_selection;
    } else {
        ev_consistency["ev_at_selection"] = nullptr;
    }
    if (std::isfinite(ev_at_order_submit_check)) {
        ev_consistency["ev_at_order_submit_check"] = ev_at_order_submit_check;
    } else {
        ev_consistency["ev_at_order_submit_check"] = nullptr;
    }
    ev_consistency["ev_mismatch_count"] = std::max(0, ev_mismatch_count);
    line["ev_consistency"] = std::move(ev_consistency);

    nlohmann::json reason_payload = nlohmann::json::object();
    for (const auto& kv : reject_reason_counts) {
        if (kv.first.empty() || kv.second <= 0) {
            continue;
        }
        reason_payload[kv.first] = kv.second;
    }
    line["reject_reason_counts"] = std::move(reason_payload);

    const auto path = entryStageFunnelArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        LOG_WARN("Entry stage funnel artifact open failed: {}", path.string());
        return;
    }
    out << line.dump() << "\n";
}

std::filesystem::path stageFunnelVNextArtifactPath() {
    return autolife::utils::PathUtils::resolveRelativePath("logs/stage_funnel_vnext.json");
}

void resetStageFunnelVNextArtifact() {
    const auto path = stageFunnelVNextArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    out << "{}\n";
}

nlohmann::json topRejectRows(
    const std::map<std::string, int>& counts,
    std::size_t limit
) {
    std::vector<std::pair<std::string, int>> rows;
    rows.reserve(counts.size());
    for (const auto& kv : counts) {
        if (kv.first.empty() || kv.second <= 0) {
            continue;
        }
        rows.push_back(kv);
    }
    std::sort(
        rows.begin(),
        rows.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
            }
            return lhs.first < rhs.first;
        }
    );
    nlohmann::json out = nlohmann::json::array();
    for (std::size_t i = 0; i < rows.size() && i < limit; ++i) {
        out.push_back({
            {"reason", rows[i].first},
            {"count", rows[i].second},
        });
    }
    return out;
}

void writeStageFunnelVNextArtifact(
    const BacktestEngine::Result::EntryFunnelSummary& entry_funnel,
    const std::map<std::string, int>& reject_reason_counts
) {
    const auto path = stageFunnelVNextArtifactPath();
    std::filesystem::create_directories(path.parent_path());

    const int topk_effective = std::max(1, entry_funnel.quality_topk_effective);
    const int scan_rounds = std::max(0, entry_funnel.gate_vnext_scan_rounds);
    const int s1_selected = std::max(0, entry_funnel.gate_vnext_s1_selected_topk);
    const int s1_topk_bound = scan_rounds * topk_effective;

    nlohmann::json stage2_reasons = nlohmann::json::array();
    if (entry_funnel.gate_vnext_drop_ev_negative_count > 0) {
        stage2_reasons.push_back({
            {"reason", "ev_negative_size_zero"},
            {"count", entry_funnel.gate_vnext_drop_ev_negative_count},
        });
    }

    nlohmann::json payload;
    payload["generated_at_ms"] = getCurrentTimeMs();
    payload["backend_request"] = entry_funnel.gate_vnext_backend_request;
    payload["backend_effective"] = entry_funnel.gate_vnext_backend_effective;
    payload["lgbm_model_sha256"] = entry_funnel.gate_vnext_lgbm_model_sha256;
    payload["topk_effective"] = topk_effective;
    payload["scans_count"] = scan_rounds;
    payload["stage_funnel_vnext"] = {
        {"s0_snapshots_valid", std::max(0, entry_funnel.gate_vnext_s0_snapshots_valid)},
        {"s1_selected_topk", s1_selected},
        {"s2_sized_count", std::max(0, entry_funnel.gate_vnext_s2_sized_count)},
        {"s3_exec_gate_pass", std::max(0, entry_funnel.gate_vnext_s3_exec_gate_pass)},
        {"s4_submitted", std::max(0, entry_funnel.gate_vnext_s4_submitted)},
        {"s5_filled", std::max(0, entry_funnel.gate_vnext_s5_filled)},
        {"drop_ev_negative_count", std::max(0, entry_funnel.gate_vnext_drop_ev_negative_count)},
        {"scan_rounds", scan_rounds},
    };
    payload["s1_topk_bound"] = s1_topk_bound;
    payload["s1_exceeds_topk_bound"] = (scan_rounds > 0 && s1_selected > s1_topk_bound);
    payload["top_reject_reasons"] = topRejectRows(reject_reason_counts, 10);
    payload["top_reject_reasons_by_stage"] = {
        {"stage0_snapshot", topRejectRows(reject_reason_counts, 5)},
        {"stage1_selection", topRejectRows(reject_reason_counts, 5)},
        {"stage2_sizing", stage2_reasons},
        {"stage3_execution", topRejectRows(reject_reason_counts, 5)},
    };

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARN("Stage funnel vNext artifact open failed: {}", path.string());
        return;
    }
    out << payload.dump(2) << "\n";
}

std::filesystem::path gateVNextEvSamplesArtifactPath() {
    return autolife::utils::PathUtils::resolveRelativePath("logs/vnext_ev_samples.json");
}

void resetGateVNextEvSamplesArtifact() {
    const auto path = gateVNextEvSamplesArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    out << "[]\n";
}

void writeGateVNextEvSamplesArtifact(const std::vector<nlohmann::json>& samples) {
    const auto path = gateVNextEvSamplesArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARN("GateVNext EV samples artifact open failed: {}", path.string());
        return;
    }
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : samples) {
        if (!row.is_object()) {
            continue;
        }
        payload.push_back(row);
    }
    out << payload.dump(2) << "\n";
}

std::filesystem::path rangingShadowArtifactPath() {
    return autolife::utils::PathUtils::resolveRelativePath("logs/ranging_shadow_signals.jsonl");
}

void resetRangingShadowArtifact() {
    const auto path = rangingShadowArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
}

void appendRangingShadowEvent(const nlohmann::json& line) {
    const auto path = rangingShadowArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        LOG_WARN("Ranging shadow artifact open failed: {}", path.string());
        return;
    }
    out << line.dump() << "\n";
}

struct RealtimeEntryVetoProbeResult {
    bool would_pass = true;
    std::string veto_reason;
};

RealtimeEntryVetoProbeResult probeRealtimeEntryVeto(
    const autolife::engine::EngineConfig& cfg,
    const autolife::strategy::Signal& signal,
    const autolife::analytics::OrderbookSnapshot& snapshot,
    double market_hostility_ewma,
    double recent_best_ask_price,
    long long recent_best_ask_timestamp_ms,
    long long now_ms
) {
    RealtimeEntryVetoProbeResult out;
    out.would_pass = true;
    out.veto_reason.clear();
    if (!cfg.enable_realtime_entry_veto) {
        return out;
    }
    const auto veto_thresholds = computeRealtimeEntryVetoThresholds(
        cfg,
        signal.market_regime,
        signal.strength,
        signal.liquidity_score,
        market_hostility_ewma
    );
    if (!snapshot.valid || snapshot.best_ask <= 0.0) {
        out.would_pass = false;
        out.veto_reason = "blocked_realtime_entry_veto_invalid_orderbook";
        return out;
    }
    if (snapshot.spread_pct > veto_thresholds.max_spread_pct) {
        out.would_pass = false;
        out.veto_reason = "blocked_realtime_entry_veto_spread";
        return out;
    }
    if (snapshot.imbalance < veto_thresholds.min_orderbook_imbalance) {
        out.would_pass = false;
        out.veto_reason = "blocked_realtime_entry_veto_imbalance";
        return out;
    }
    const double max_drop_pct = std::max(0.0001, veto_thresholds.max_drop_pct);
    const double drop_vs_signal = (signal.entry_price > 0.0)
        ? ((signal.entry_price - snapshot.best_ask) / signal.entry_price)
        : 0.0;
    if (drop_vs_signal > max_drop_pct) {
        out.would_pass = false;
        out.veto_reason = "blocked_realtime_entry_veto_drop_vs_signal";
        return out;
    }
    if (recent_best_ask_price > 0.0 && recent_best_ask_timestamp_ms > 0) {
        const long long age_ms = now_ms - recent_best_ask_timestamp_ms;
        const int tracking_window_seconds = std::max(
            10,
            cfg.realtime_entry_veto_tracking_window_seconds
        );
        if (age_ms >= 0 &&
            age_ms <= static_cast<long long>(tracking_window_seconds) * 1000LL) {
            const double drop_vs_recent =
                (recent_best_ask_price - snapshot.best_ask) / recent_best_ask_price;
            if (drop_vs_recent > max_drop_pct) {
                out.would_pass = false;
                out.veto_reason = "blocked_realtime_entry_veto_drop_vs_recent";
                return out;
            }
        }
    }
    return out;
}

bool inferProbabilisticRuntimeSnapshot(
    const autolife::analytics::ProbabilisticRuntimeModel& model,
    const autolife::engine::EngineConfig& cfg,
    const autolife::analytics::CoinMetrics& metrics,
    const std::string& market,
    autolife::analytics::MarketRegime regime,
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
        LOG_WARN("{} probabilistic runtime snapshot feature build skipped: {}", market, feature_error);
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
            "{} probabilistic runtime snapshot feature dimension mismatch: runtime={} bundle={}",
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
        LOG_WARN("{} probabilistic runtime snapshot inference failed", market);
        if (strict_primary_mode) {
            if (reject_reason != nullptr) {
                *reject_reason = "probabilistic_inference_failed";
            }
            return false;
        }
        return true;
    }

    out_snapshot.applied = true;
    out_snapshot.prob_model_backend = model.probModelBackend();
    out_snapshot.prob_h1_raw = std::clamp(inference.prob_h1_raw, 0.0, 1.0);
    out_snapshot.prob_h5_raw = std::clamp(inference.prob_h5_raw, 0.0, 1.0);
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
    out_snapshot.expected_edge_calibrated_bps = inference.expected_edge_calibrated_bps;
    out_snapshot.expected_edge_calibrated_raw_bps = inference.expected_edge_calibrated_raw_bps;
    out_snapshot.expected_edge_calibrated_corrected_bps =
        inference.expected_edge_calibrated_corrected_bps;
    out_snapshot.expected_edge_used_for_gate_bps = inference.expected_edge_bps;
    out_snapshot.expected_edge_pct = std::clamp(inference.expected_edge_pct, -0.05, 0.05);
    out_snapshot.edge_semantics = inference.edge_semantics;
    out_snapshot.root_cost_model_enabled_configured = inference.root_cost_model_enabled_configured;
    out_snapshot.phase3_cost_model_enabled_configured = inference.phase3_cost_model_enabled_configured;
    out_snapshot.root_cost_model_enabled_effective = inference.root_cost_model_enabled_effective;
    out_snapshot.phase3_cost_model_enabled_effective = inference.phase3_cost_model_enabled_effective;
    out_snapshot.edge_semantics_guard_violation = inference.edge_semantics_guard_violation;
    out_snapshot.edge_semantics_guard_forced_off = inference.edge_semantics_guard_forced_off;
    out_snapshot.edge_semantics_guard_action = inference.edge_semantics_guard_action;
    out_snapshot.ev_confidence = std::clamp(inference.ev_confidence, 0.0, 1.0);
    out_snapshot.edge_regressor_used = inference.edge_regressor_used;
    out_snapshot.ev_calibration_applied = inference.ev_calibration_applied;
    out_snapshot.cost_entry_pct = std::clamp(inference.entry_cost_bps_estimate / 10000.0, 0.0, 0.10);
    out_snapshot.cost_exit_pct = std::clamp(inference.exit_cost_bps_estimate / 10000.0, 0.0, 0.10);
    out_snapshot.cost_tail_pct = std::clamp(inference.tail_cost_bps_estimate / 10000.0, 0.0, 0.10);
    out_snapshot.cost_used_pct = std::clamp(inference.cost_used_bps_estimate / 10000.0, 0.0, 0.10);
    out_snapshot.cost_used_bps_estimate = inference.cost_used_bps_estimate;
    out_snapshot.cost_mode = inference.cost_mode;
    const auto& phase3_policy = model.phase3Policy();
    out_snapshot.phase3_ev_calibration_enabled = inference.phase3_ev_calibration_enabled;
    out_snapshot.phase3_cost_tail_enabled = inference.phase3_cost_tail_enabled;
    out_snapshot.phase3_adaptive_ev_blend_enabled = inference.phase3_adaptive_ev_blend_enabled;
    out_snapshot.phase3_diagnostics_v2_enabled = inference.phase3_diagnostics_v2_enabled;
    out_snapshot.phase3_liq_vol_gate_policy = phase3_policy.liq_vol_gate;
    out_snapshot.phase3_foundation_structure_gate_policy = phase3_policy.foundation_structure_gate;
    out_snapshot.phase3_bear_rebound_guard_policy = phase3_policy.bear_rebound_guard;
    out_snapshot.phase3_stop_loss_risk_policy = phase3_policy.risk;
    out_snapshot.phase3_regime_entry_disable_enabled = phase3_policy.regime_entry_disable_enabled;
    out_snapshot.phase3_disable_ranging_entry = phase3_policy.disable_ranging_entry;
    out_snapshot.phase3_strategy_exit_mode = phase3_policy.exit.strategy_exit_mode;
    out_snapshot.phase3_tp_distance_trending_multiplier =
        phase3_policy.exit.tp_distance_trending_multiplier;
    out_snapshot.lgbm_ev_affine_enabled = inference.lgbm_ev_affine_enabled;
    out_snapshot.lgbm_ev_affine_applied = inference.lgbm_ev_affine_applied;
    out_snapshot.lgbm_ev_affine_scale = inference.lgbm_ev_affine_scale;
    out_snapshot.lgbm_ev_affine_shift = inference.lgbm_ev_affine_shift;
    out_snapshot.phase3_regime_entry_disable.clear();
    for (const auto& kv : phase3_policy.regime_entry_disable) {
        out_snapshot.phase3_regime_entry_disable[kv.first] = kv.second;
    }
    out_snapshot.margin_h5 = std::clamp(
        inference.prob_h5_calibrated - inference.selection_threshold_h5,
        -1.0,
        1.0
    );

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

    return true;
}

bool applyProbabilisticRuntimeAdjustment(
    const autolife::engine::EngineConfig& cfg,
    const ProbabilisticRuntimeSnapshot& snapshot,
    autolife::strategy::Signal& signal
) {
    (void)cfg;
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

    const double effective_margin = std::clamp(
        snapshot.margin_h5 * std::max(0.50, snapshot.online_strength_gain),
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

    signal.phase3.ev_calibration_enabled = snapshot.phase3_ev_calibration_enabled;
    signal.phase3.cost_tail_enabled = snapshot.phase3_cost_tail_enabled;
    signal.phase3.adaptive_ev_blend_enabled = snapshot.phase3_adaptive_ev_blend_enabled;
    signal.phase3.diagnostics_v2_enabled = snapshot.phase3_diagnostics_v2_enabled;
    signal.phase3.edge_regressor_used = snapshot.edge_regressor_used;
    signal.phase3.ev_calibration_applied = snapshot.ev_calibration_applied;
    signal.phase3.prob_model_backend = snapshot.prob_model_backend;
    signal.phase3.ev_confidence = snapshot.ev_confidence;
    signal.phase3.edge_semantics = snapshot.edge_semantics;
    signal.phase3.root_cost_model_enabled_configured = snapshot.root_cost_model_enabled_configured;
    signal.phase3.phase3_cost_model_enabled_configured = snapshot.phase3_cost_model_enabled_configured;
    signal.phase3.root_cost_model_enabled_effective = snapshot.root_cost_model_enabled_effective;
    signal.phase3.phase3_cost_model_enabled_effective = snapshot.phase3_cost_model_enabled_effective;
    signal.phase3.edge_semantics_guard_violation = snapshot.edge_semantics_guard_violation;
    signal.phase3.edge_semantics_guard_forced_off = snapshot.edge_semantics_guard_forced_off;
    signal.phase3.edge_semantics_guard_action = snapshot.edge_semantics_guard_action;

    signal.phase3.expected_edge_raw_pct = snapshot.expected_edge_raw_pct;
    signal.phase3.expected_edge_calibrated_pct = snapshot.expected_edge_calibrated_pct;
    signal.phase3.expected_edge_calibrated_bps = snapshot.expected_edge_calibrated_bps;
    signal.phase3.expected_edge_calibrated_raw_bps = snapshot.expected_edge_calibrated_raw_bps;
    signal.phase3.expected_edge_used_for_gate_bps = snapshot.expected_edge_used_for_gate_bps;

    signal.phase3.lgbm_ev_affine_enabled = snapshot.lgbm_ev_affine_enabled;
    signal.phase3.lgbm_ev_affine_applied = snapshot.lgbm_ev_affine_applied;
    signal.phase3.lgbm_ev_affine_scale = snapshot.lgbm_ev_affine_scale;
    signal.phase3.lgbm_ev_affine_shift = snapshot.lgbm_ev_affine_shift;

    signal.phase3.cost_entry_pct = snapshot.cost_entry_pct;
    signal.phase3.cost_exit_pct = snapshot.cost_exit_pct;
    signal.phase3.cost_tail_pct = snapshot.cost_tail_pct;
    signal.phase3.cost_used_pct = snapshot.cost_used_pct;
    signal.phase3.cost_used_bps_estimate = snapshot.cost_used_bps_estimate;
    signal.phase3.cost_mode = snapshot.cost_mode;

    if (std::isfinite(snapshot.expected_edge_pct)) {
        signal.expected_value = snapshot.expected_edge_pct;
    }
    signal.phase3.adaptive_ev_blend = 1.0;
    return true;
}

} // namespace

BacktestEngine::BacktestEngine() : balance_krw_(0), balance_asset_(0), 
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
    resetEntryStageFunnelArtifact();
    resetStageFunnelVNextArtifact();
    resetPhase4CandidateArtifact();
    resetGateVNextEvSamplesArtifact();
    resetRangingShadowArtifact();
    balance_krw_ = config.getInitialCapital();
    balance_asset_ = 0.0;
    max_balance_ = balance_krw_;
    loaded_tf_cursors_.clear();
    entry_funnel_ = Result::EntryFunnelSummary{};
    shadow_funnel_ = Result::ShadowFunnelSummary{};
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
    adaptive_stop_update_count_ = 0;
    adaptive_tp_recalibration_count_ = 0;
    adaptive_partial_ratio_samples_ = 0;
    adaptive_partial_ratio_sum_ = 0.0;
    adaptive_partial_ratio_histogram_ = {0, 0, 0, 0, 0};
    be_move_attempt_count_ = 0;
    be_move_applied_count_ = 0;
    be_move_skipped_due_to_delay_count_ = 0;
    stop_loss_after_partial_tp_count_ = 0;
    stop_loss_before_partial_tp_count_ = 0;
    be_after_partial_tp_delay_sec_ = 0;
    pending_be_after_partial_due_ms_.clear();
    strategyless_position_checks_ = 0;
    strategyless_runtime_archetype_checks_ = 0;
    strategyless_risk_exit_signals_ = 0;
    strategyless_current_stop_hits_ = 0;
    strategyless_current_tp1_hits_ = 0;
    strategyless_current_tp2_hits_ = 0;
    ranging_shadow_summary_ = Result::RangingShadowSummary{};
    phase4_portfolio_diagnostics_ = Result::Phase4PortfolioDiagnostics{};
    phase4_candidate_snapshot_samples_.clear();
    gate_vnext_ev_samples_.clear();
    recent_best_ask_price_ = 0.0;
    recent_best_ask_timestamp_ms_ = 0;
    current_candle_index_ = 0;
    
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
        const auto gate_settings = loadGateVNextBundleSettings(bundle_path);
        gate_vnext_quality_topk_ = std::max(1, gate_settings.quality_topk);
        gate_vnext_ev_scale_bps_ = std::max(1e-9, gate_settings.ev_scale_bps);
        entry_funnel_.gate_system_version_effective = "vnext";
        entry_funnel_.quality_topk_effective = gate_vnext_quality_topk_;
        entry_funnel_.gate_vnext_backend_request = "unknown";
        entry_funnel_.gate_vnext_backend_effective = "unknown";
        entry_funnel_.gate_vnext_lgbm_model_sha256.clear();
        std::string error_message;
        probabilistic_runtime_model_loaded_ =
            probabilistic_runtime_model_.loadFromFile(bundle_path.string(), &error_message);
        if (probabilistic_runtime_model_loaded_) {
            entry_funnel_.gate_vnext_backend_request = probabilistic_runtime_model_.probModelBackend();
            entry_funnel_.gate_vnext_backend_effective = probabilistic_runtime_model_.probModelBackend();
            entry_funnel_.gate_vnext_lgbm_model_sha256 = probabilistic_runtime_model_.lgbmModelSha256();
            LOG_INFO(
                "Backtest probabilistic runtime bundle loaded: path={}, features={}, backend={}, model_sha256={}",
                bundle_path.string(),
                probabilistic_runtime_model_.featureColumns().size(),
                probabilistic_runtime_model_.probModelBackend(),
                probabilistic_runtime_model_.lgbmModelSha256()
            );
        } else {
            LOG_WARN(
                "Backtest probabilistic runtime bundle load skipped: path={}, reason={}",
                bundle_path.string(),
                error_message.empty() ? std::string("unknown") : error_message
            );
        }
        LOG_INFO(
            "Backtest GateVNext settings: gate_system_version=vnext, quality_topk={}, ev_scale_bps={:.4f}",
            gate_vnext_quality_topk_,
            gate_vnext_ev_scale_bps_
        );
    }
    
    LOG_INFO(
        "BacktestEngine initialized (core_bridge={}, core_policy={}, core_risk={}, core_execution={}, shadow_policy_only={})",
        engine_config_.enable_core_plane_bridge ? "on" : "off",
        engine_config_.enable_core_policy_plane ? "on" : "off",
        engine_config_.enable_core_risk_plane ? "on" : "off",
        engine_config_.enable_core_execution_plane ? "on" : "off",
        engine_config_.backtest_shadow_policy_only ? "on" : "off"
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
    if (risk_manager_) {
        risk_manager_->clearTimeOverride();
    }
    
    for (size_t idx = 0; idx < history_data_.size(); ++idx) {
        current_candle_index_ = idx;
        processCandle(history_data_[idx]);
    }

    if (risk_manager_ && !history_data_.empty()) {
        const auto open_positions = risk_manager_->getAllPositions();
        if (!open_positions.empty()) {
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

            const Candle& final_candle = history_data_.back();
            risk_manager_->setTimeOverrideMs(toMsTimestamp(final_candle.timestamp));
            int forced_closes = 0;
            for (const auto& pos : open_positions) {
                const double mark_price = (final_candle.close > 0.0)
                    ? final_candle.close
                    : ((pos.current_price > 0.0) ? pos.current_price : pos.entry_price);
                if (!(mark_price > 0.0) || !(pos.quantity > 0.0)) {
                    continue;
                }

                const auto eod_slippage = computeDynamicSlippageThresholds(
                    engine_config_,
                    market_hostility_ewma_,
                    false,
                    pos.market_regime,
                    pos.signal_strength,
                    pos.liquidity_score,
                    pos.expected_value,
                    "backtest_end"
                );
                const double fill_slippage = std::min(
                    exitSlippagePct(engine_config_),
                    eod_slippage.max_slippage_pct
                );
                const double forced_exit = mark_price * (1.0 - fill_slippage);
                if (!(forced_exit > 0.0)) {
                    continue;
                }

                Order eod_order;
                eod_order.market = pos.market;
                eod_order.side = OrderSide::SELL;
                eod_order.volume = pos.quantity;
                eod_order.price = forced_exit;
                eod_order.strategy_name = pos.strategy_name;
                executeOrder(eod_order, forced_exit);
                risk_manager_->exitPosition(pos.market, forced_exit, "BacktestEOD");
                pending_be_after_partial_due_ms_.erase(pos.market);
                notifyStrategyClosed(pos, forced_exit);
                forced_closes++;
            }

            if (forced_closes > 0) {
                checkOrders(final_candle);
                LOG_INFO("Backtest EOD forced close applied: market={}, positions_closed={}",
                         market_name_, forced_closes);
            }
        }
    }
    
    LOG_INFO("Backtest Completed.");
    writeStageFunnelVNextArtifact(entry_funnel_, entry_rejection_reason_counts_);
    writeGateVNextEvSamplesArtifact(gate_vnext_ev_samples_);
    if (risk_manager_) {
        risk_manager_->clearTimeOverride();
        auto metrics = risk_manager_->getRiskMetrics();
        LOG_INFO("Final Balance: {}", metrics.total_capital);
    } else {
        LOG_INFO("Final Balance: {}", balance_krw_ + (balance_asset_ * history_data_.back().close));
    }
}

void BacktestEngine::processCandle(const Candle& candle) {
    if (risk_manager_) {
        risk_manager_->setTimeOverrideMs(toMsTimestamp(candle.timestamp));
    }
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
    const std::string strategy_exit_mode_effective =
        probabilistic_runtime_model_.phase3Policy().exit.strategy_exit_mode;
    const bool strategy_exit_observe_only = strategy_exit_mode_effective == "observe_only";
    const bool strategy_exit_clamp_to_stop = strategy_exit_mode_effective == "clamp_to_stop";
    const bool strategy_exit_disabled = strategy_exit_mode_effective == "disabled";
    const int be_after_partial_tp_delay_sec = std::max(
        0,
        probabilistic_runtime_model_.phase3Policy().exit.be_after_partial_tp_delay_sec
    );
    be_after_partial_tp_delay_sec_ = be_after_partial_tp_delay_sec;
    entry_funnel_.strategy_exit_mode_effective = strategy_exit_mode_effective;
    auto computeNetPnlAtExit = [&](const risk::Position& closed_position, double exit_price) {
        const double fee_rate = Config::getInstance().getFeeRate();
        const double exit_value = exit_price * closed_position.quantity;
        const double entry_fee = closed_position.invested_amount * fee_rate;
        const double exit_fee = exit_value * fee_rate;
        return exit_value - closed_position.invested_amount - entry_fee - exit_fee;
    };
    auto recordStrategyExitTrigger = [&](const risk::Position& position, const char* reason_code) {
        entry_funnel_.strategy_exit_triggered_count++;
        entry_funnel_.strategy_exit_triggered_by_market[position.market]++;
        const std::string regime_key = marketRegimeLabel(position.market_regime);
        entry_funnel_.strategy_exit_triggered_by_regime[regime_key]++;
        if (entry_funnel_.strategy_exit_trigger_samples.size() < 10) {
            Result::EntryFunnelSummary::StrategyExitTriggerSample sample;
            sample.ts_ms = toMsTimestamp(candle.timestamp);
            sample.market = position.market;
            sample.regime = regime_key;
            sample.unrealized_pnl_at_trigger =
                (current_price - position.entry_price) * position.quantity;
            sample.holding_time_seconds = std::max(
                0.0,
                static_cast<double>(sample.ts_ms - position.entry_time) / 1000.0
            );
            sample.reason_code = reason_code == nullptr ? std::string() : std::string(reason_code);
            entry_funnel_.strategy_exit_trigger_samples.push_back(std::move(sample));
        }
    };
    auto scheduleOrApplyBreakevenAfterPartial = [&](const std::string& market, long long trigger_ts_ms) {
        if (!risk_manager_ || market.empty()) {
            return;
        }
        be_move_attempt_count_++;
        if (be_after_partial_tp_delay_sec <= 0) {
            risk_manager_->moveStopToBreakeven(market);
            be_move_applied_count_++;
            pending_be_after_partial_due_ms_.erase(market);
            return;
        }
        const long long delay_ms = static_cast<long long>(be_after_partial_tp_delay_sec) * 1000LL;
        pending_be_after_partial_due_ms_[market] = trigger_ts_ms + delay_ms;
        be_move_skipped_due_to_delay_count_++;
    };
    auto flushPendingBreakevenAfterPartial = [&](long long now_ts_ms) {
        if (!risk_manager_ || pending_be_after_partial_due_ms_.empty()) {
            return;
        }
        std::vector<std::string> ready_markets;
        ready_markets.reserve(pending_be_after_partial_due_ms_.size());
        for (const auto& kv : pending_be_after_partial_due_ms_) {
            if (now_ts_ms >= kv.second) {
                ready_markets.push_back(kv.first);
            }
        }
        for (const auto& market : ready_markets) {
            pending_be_after_partial_due_ms_.erase(market);
            if (risk_manager_->getPosition(market) == nullptr) {
                continue;
            }
            risk_manager_->moveStopToBreakeven(market);
            be_move_applied_count_++;
        }
    };
    flushPendingBreakevenAfterPartial(toMsTimestamp(candle.timestamp));
    
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
            pending_be_after_partial_due_ms_.erase(market_name_);
            notifyStrategyClosed(closed_position, forced_exit);
            position = nullptr;
        }
    }

    if (position) {
        auto strategy = strategy_manager_->getStrategy(position->strategy_name);
        if (!strategy) {
            strategyless_position_checks_++;
            const bool current_stop_hit = current_price <= position->stop_loss;
            const bool current_tp1_hit = (!position->half_closed && current_price >= position->take_profit_1);
            const bool current_tp2_hit = current_price >= position->take_profit_2;
            if (current_stop_hit) {
                strategyless_current_stop_hits_++;
            }
            if (current_tp1_hit) {
                strategyless_current_tp1_hits_++;
            }
            if (current_tp2_hit) {
                strategyless_current_tp2_hits_++;
            }
            if (current_stop_hit || current_tp2_hit) {
                strategyless_risk_exit_signals_++;
            }
        }
        if (strategy) {
            // Check Exit Condition
            bool should_exit = strategy->shouldExit(
                market_name_,
                position->entry_price,
                current_price,
                (candle.timestamp - position->entry_time) / 1000.0 // holding seconds
            );
            const bool risk_manager_exit = risk_manager_->shouldExitPosition(market_name_);
            
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
                if (position->half_closed) {
                    stop_loss_after_partial_tp_count_++;
                } else {
                    stop_loss_before_partial_tp_count_++;
                }
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
                pending_be_after_partial_due_ms_.erase(market_name_);
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
                            pending_be_after_partial_due_ms_.erase(market_name_);
                            notifyStrategyClosed(closed_position, tp1_fill);
                            position = nullptr;
                        } else {
                            risk_manager_->setHalfClosed(market_name_, true);
                            scheduleOrApplyBreakevenAfterPartial(
                                market_name_,
                                toMsTimestamp(candle.timestamp)
                            );
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
                            scheduleOrApplyBreakevenAfterPartial(
                                market_name_,
                                toMsTimestamp(candle.timestamp)
                            );
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
                        pending_be_after_partial_due_ms_.erase(market_name_);
                        notifyStrategyClosed(closed_position, tp2_fill);
                        position = nullptr;
                    } else if (should_exit || risk_manager_exit) {
                        const char* reason_code = should_exit
                            ? (risk_manager_exit ? "strategy_and_risk_manager" : "strategy_should_exit")
                            : "risk_manager_should_exit";
                        recordStrategyExitTrigger(*position, reason_code);
                        if (strategy_exit_observe_only || strategy_exit_disabled) {
                            entry_funnel_.strategy_exit_observe_only_suppressed_count++;
                            LOG_INFO(
                                "Backtest strategy_exit {} suppress: market={} reason={} hold_sec={:.1f}",
                                strategy_exit_disabled ? "disabled" : "observe_only",
                                market_name_,
                                reason_code,
                                std::max(
                                    0.0,
                                    static_cast<double>(toMsTimestamp(candle.timestamp) - position->entry_time) /
                                        1000.0
                                )
                            );
                        } else {
                            entry_funnel_.strategy_exit_executed_count++;
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
                            const double stop_loss_price = position->stop_loss;
                            const double exit_price_before_clamp =
                                current_price * (1.0 - strategy_exit_fill_slippage);
                            double exit_price_after_clamp = exit_price_before_clamp;
                            if (strategy_exit_clamp_to_stop &&
                                std::isfinite(stop_loss_price) &&
                                stop_loss_price > 0.0 &&
                                exit_price_after_clamp < stop_loss_price) {
                                exit_price_after_clamp = stop_loss_price;
                                entry_funnel_.strategy_exit_clamp_applied_count++;
                            }
                            if (strategy_exit_clamp_to_stop &&
                                entry_funnel_.strategy_exit_clamp_samples.size() < 10) {
                                Result::EntryFunnelSummary::StrategyExitClampSample sample;
                                sample.ts_ms = toMsTimestamp(candle.timestamp);
                                sample.market = position->market;
                                sample.regime = marketRegimeLabel(position->market_regime);
                                sample.stop_loss_price = stop_loss_price;
                                sample.exit_price_before_clamp = exit_price_before_clamp;
                                sample.exit_price_after_clamp = exit_price_after_clamp;
                                sample.pnl_before_clamp =
                                    computeNetPnlAtExit(closed_position, exit_price_before_clamp);
                                sample.pnl_after_clamp =
                                    computeNetPnlAtExit(closed_position, exit_price_after_clamp);
                                sample.reason_code = reason_code;
                                entry_funnel_.strategy_exit_clamp_samples.push_back(std::move(sample));
                            }
                            // Execute Sell
                            Order order;
                            order.market = market_name_;
                            order.side = OrderSide::SELL;
                            order.volume = position->quantity;
                            order.price = exit_price_after_clamp;
                            order.strategy_name = position->strategy_name;
                            executeOrder(order, order.price);
                            const std::string exit_reason = risk_manager_exit
                                ? "RiskManagerExit"
                                : "StrategyExit";
                            risk_manager_->exitPosition(market_name_, order.price, exit_reason);
                            pending_be_after_partial_due_ms_.erase(market_name_);
                            notifyStrategyClosed(closed_position, order.price);
                            position = nullptr;
                        }
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
        int stage_candidates_total = 0;
        int stage_candidates_after_quality_topk = 0;
        int stage_candidates_after_sizing = 0;
        int stage_candidates_after_portfolio = 0;
        int stage_orders_submitted = 0;
        int stage_orders_filled = 0;
        double stage_ev_at_selection = std::numeric_limits<double>::quiet_NaN();
        double stage_ev_at_order_submit_check = std::numeric_limits<double>::quiet_NaN();
        int stage_ev_mismatch_count = 0;
        std::map<std::string, int> stage_reject_reason_counts;
        auto markEntryReject = [&](const char* reason) {
            entry_rejection_reason_counts_[reason]++;
            if (reason != nullptr && reason[0] != '\0') {
                stage_reject_reason_counts[reason]++;
            }
        };
        auto markEntryRejectCount = [&](const std::string& reason, int count) {
            if (reason.empty() || count <= 0) {
                return;
            }
            entry_rejection_reason_counts_[reason] += count;
            stage_reject_reason_counts[reason] += count;
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
        std::string probabilistic_snapshot_reason;
        std::vector<strategy::Signal> signals;
        bool signals_suppressed_by_regime_shadow = false;
        if (!inferProbabilisticRuntimeSnapshot(
                probabilistic_runtime_model_,
                engine_config_,
                metrics,
                market_name_,
                regime.regime,
                probabilistic_snapshot,
                &probabilistic_snapshot_reason)) {
            entry_funnel_.no_signal_generated++;
            markEntryReject(
                probabilistic_snapshot_reason.empty()
                    ? "probabilistic_snapshot_build_failed"
                    : probabilistic_snapshot_reason.c_str()
            );
            const std::string no_signal_pattern_key =
                std::string("regime=") + marketRegimeLabel(regime.regime) +
                "|vol=" + volatilityBucket(metrics.volatility) +
                "|liq=" + liquidityBucket(metrics.liquidity_score);
            no_signal_pattern_counts_[no_signal_pattern_key]++;
        } else {
            metrics.liq_vol_gate_policy.enabled =
                probabilistic_snapshot.phase3_liq_vol_gate_policy.enabled;
            metrics.liq_vol_gate_policy.mode =
                probabilistic_snapshot.phase3_liq_vol_gate_policy.mode;
            metrics.liq_vol_gate_policy.window_minutes =
                probabilistic_snapshot.phase3_liq_vol_gate_policy.window_minutes;
            metrics.liq_vol_gate_policy.quantile_q =
                probabilistic_snapshot.phase3_liq_vol_gate_policy.quantile_q;
            metrics.liq_vol_gate_policy.min_samples_required =
                probabilistic_snapshot.phase3_liq_vol_gate_policy.min_samples_required;
            metrics.liq_vol_gate_policy.low_conf_action =
                probabilistic_snapshot.phase3_liq_vol_gate_policy.low_conf_action;
            metrics.foundation_structure_gate_policy.enabled =
                probabilistic_snapshot.phase3_foundation_structure_gate_policy.enabled;
            metrics.foundation_structure_gate_policy.mode =
                probabilistic_snapshot.phase3_foundation_structure_gate_policy.mode;
            metrics.foundation_structure_gate_policy.relax_delta =
                probabilistic_snapshot.phase3_foundation_structure_gate_policy.relax_delta;
            metrics.bear_rebound_guard_policy.enabled =
                probabilistic_snapshot.phase3_bear_rebound_guard_policy.enabled;
            metrics.bear_rebound_guard_policy.mode =
                probabilistic_snapshot.phase3_bear_rebound_guard_policy.mode;
            metrics.bear_rebound_guard_policy.window_minutes =
                probabilistic_snapshot.phase3_bear_rebound_guard_policy.window_minutes;
            metrics.bear_rebound_guard_policy.quantile_q =
                probabilistic_snapshot.phase3_bear_rebound_guard_policy.quantile_q;
            metrics.bear_rebound_guard_policy.min_samples_required =
                probabilistic_snapshot.phase3_bear_rebound_guard_policy.min_samples_required;
            metrics.bear_rebound_guard_policy.low_conf_action =
                probabilistic_snapshot.phase3_bear_rebound_guard_policy.low_conf_action;
            metrics.bear_rebound_guard_policy.static_threshold =
                probabilistic_snapshot.phase3_bear_rebound_guard_policy.static_threshold;
            metrics.stop_loss_risk_policy.enabled =
                probabilistic_snapshot.phase3_stop_loss_risk_policy.enabled;
            metrics.stop_loss_risk_policy.stop_loss_trending_multiplier =
                probabilistic_snapshot.phase3_stop_loss_risk_policy.stop_loss_trending_multiplier;
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
        entry_funnel_.regime_entry_disable_enabled =
            probabilistic_snapshot.phase3_regime_entry_disable_enabled;
        entry_funnel_.regime_entry_disable = probabilistic_snapshot.phase3_regime_entry_disable;
        if (!signals.empty()) {
            std::vector<strategy::Signal> adjusted_signals;
            adjusted_signals.reserve(signals.size());
            for (auto& signal : signals) {
                if (!applyProbabilisticRuntimeAdjustment(
                        engine_config_,
                        probabilistic_snapshot,
                        signal)) {
                    markEntryReject(
                        "probabilistic_runtime_adjustment_failed"
                    );
                    continue;
                }
                adjusted_signals.push_back(std::move(signal));
            }
            signals = std::move(adjusted_signals);
        }
        if (!signals.empty() && probabilistic_snapshot.phase3_regime_entry_disable_enabled) {
            std::string disabled_regime_key;
            if (isRegimeEntryDisabled(
                    probabilistic_snapshot.phase3_regime_entry_disable,
                    regime.regime,
                    &disabled_regime_key)) {
                signals_suppressed_by_regime_shadow = true;
                const int skipped_candidates = static_cast<int>(signals.size());
                const std::string regime_key =
                    disabled_regime_key.empty() ? marketRegimeLabel(regime.regime) : disabled_regime_key;

                const double hostility_alpha =
                    std::clamp(engine_config_.hostility_ewma_alpha, 0.01, 0.99);
                const double hostility_now = computeMarketHostilityScore(
                    engine_config_,
                    metrics,
                    regime.regime
                );
                const double hostility_probe = (market_hostility_ewma_ <= 1e-9)
                    ? hostility_now
                    : std::clamp(
                        market_hostility_ewma_ * (1.0 - hostility_alpha) +
                            hostility_now * hostility_alpha,
                        0.0,
                        1.0
                    );
                const long long now_ms = toMsTimestamp(candle.timestamp);
                const bool future_available =
                    (current_candle_index_ + static_cast<size_t>(5)) < history_data_.size();
                const double entry_price_proxy = candle.close;
                double future_close_t5 = std::numeric_limits<double>::quiet_NaN();
                double return_bps_t5 = std::numeric_limits<double>::quiet_NaN();
                double shadow_pnl_bps = std::numeric_limits<double>::quiet_NaN();
                if (future_available && entry_price_proxy > 0.0) {
                    future_close_t5 = history_data_[current_candle_index_ + 5].close;
                    if (std::isfinite(future_close_t5) && future_close_t5 > 0.0) {
                        return_bps_t5 = ((future_close_t5 / entry_price_proxy) - 1.0) * 10000.0;
                        shadow_pnl_bps = return_bps_t5 - 12.0;
                    }
                }

                for (const auto& shadow_signal : signals) {
                    const bool would_pass_quality_selection = true;
                    const auto veto_probe = probeRealtimeEntryVeto(
                        engine_config_,
                        shadow_signal,
                        metrics.orderbook_snapshot,
                        hostility_probe,
                        recent_best_ask_price_,
                        recent_best_ask_timestamp_ms_,
                        now_ms
                    );
                    const bool would_pass_execution_guard = veto_probe.would_pass;

                    nlohmann::json shadow_line;
                    shadow_line["ts_ms"] = normalizeTimestampMs(candle.timestamp);
                    shadow_line["source"] = "backtest";
                    shadow_line["mode"] = "ranging_shadow";
                    shadow_line["market"] = market_name_;
                    shadow_line["regime"] = regime_key;
                    shadow_line["vol_bucket_pct"] = volatilityBucket(metrics.volatility);
                    shadow_line["atr_pct"] = regime.atr_pct;
                    shadow_line["adx"] = regime.adx;
                    shadow_line["p_h5_calibrated"] = shadow_signal.probabilistic_h5_calibrated;
                    shadow_line["selection_threshold_h5"] = shadow_signal.probabilistic_h5_threshold;
                    shadow_line["margin"] = shadow_signal.probabilistic_h5_margin;
                    shadow_line["expected_edge_calibrated_bps"] =
                        shadow_signal.phase3.expected_edge_calibrated_bps;
                    shadow_line["expected_edge_bps_after_cost"] =
                        shadow_signal.phase3.expected_edge_used_for_gate_bps;
                    shadow_line["signal_expected_value"] = shadow_signal.expected_value;
                    shadow_line["signal_strength"] = shadow_signal.strength;
                    shadow_line["ev_blend"] = shadow_signal.phase3.adaptive_ev_blend;
                    shadow_line["disabled_reason"] = "REGIME_RANGING_SHADOW";
                    shadow_line["would_pass_quality_selection"] = would_pass_quality_selection;
                    shadow_line["would_pass_execution_guard"] = would_pass_execution_guard;
                    shadow_line["note"] = veto_probe.veto_reason;
                    shadow_line["entry_price_proxy_close_t"] = entry_price_proxy;
                    shadow_line["future_horizon_bars"] = 5;
                    shadow_line["future_close_t5_available"] = future_available;
                    if (std::isfinite(future_close_t5)) {
                        shadow_line["future_close_t5"] = future_close_t5;
                    } else {
                        shadow_line["future_close_t5"] = nullptr;
                    }
                    shadow_line["label_cost_bps"] = 12.0;
                    if (std::isfinite(return_bps_t5)) {
                        shadow_line["return_bps_t5"] = return_bps_t5;
                    } else {
                        shadow_line["return_bps_t5"] = nullptr;
                    }
                    if (std::isfinite(shadow_pnl_bps)) {
                        shadow_line["shadow_pnl_bps"] = shadow_pnl_bps;
                    } else {
                        shadow_line["shadow_pnl_bps"] = nullptr;
                    }
                    appendRangingShadowEvent(shadow_line);

                    ranging_shadow_summary_.shadow_count_total++;
                    ranging_shadow_summary_.shadow_count_by_regime[regime_key]++;
                    ranging_shadow_summary_.shadow_count_by_market[market_name_]++;
                    if (would_pass_execution_guard) {
                        ranging_shadow_summary_.shadow_would_pass_execution_guard_count++;
                    }
                    if (shadow_signal.phase3.expected_edge_calibrated_bps < 0.0) {
                        ranging_shadow_summary_.shadow_edge_neg_count++;
                    } else if (shadow_signal.phase3.expected_edge_calibrated_bps > 0.0) {
                        ranging_shadow_summary_.shadow_edge_pos_count++;
                    }
                }

                entry_funnel_.reject_regime_entry_disabled_count += skipped_candidates;
                entry_funnel_.reject_regime_entry_disabled_by_regime[regime_key] += skipped_candidates;
                markEntryRejectCount("reject_regime_entry_disabled_count", skipped_candidates);
                markEntryRejectCount("reject_regime_entry_disabled", skipped_candidates);
                markEntryRejectCount(
                    std::string("reject_regime_entry_disabled_by_regime::") + regime_key,
                    skipped_candidates
                );
                LOG_INFO(
                    "{} regime entry disabled: regime={}, skipped_candidates={}",
                    market_name_,
                    regime_key,
                    skipped_candidates
                );
                signals.clear();
                markEntryReject("reject_regime_ranging_shadow_mode");
            }
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
        if (signals.empty()) {
            if (!signals_suppressed_by_regime_shadow) {
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
        }
        if (!signals.empty()) {
            autolife::gate_vnext::GateVNext::Params gate_params{};
            gate_params.quality_topk = gate_vnext_quality_topk_;
            gate_params.base_size = 1.0;
            gate_params.ev_scale_bps = gate_vnext_ev_scale_bps_;
            gate_params.backend_request = entry_funnel_.gate_vnext_backend_request;
            gate_params.backend_effective = entry_funnel_.gate_vnext_backend_effective;
            gate_params.model_sha256 = entry_funnel_.gate_vnext_lgbm_model_sha256;
            autolife::gate_vnext::GateVNext gate_vnext(gate_params);

            std::vector<autolife::gate_vnext::CandidateSnapshot> gate_inputs;
            gate_inputs.reserve(signals.size());
            for (std::size_t i = 0; i < signals.size(); ++i) {
                const auto& signal = signals[i];
                autolife::gate_vnext::CandidateSnapshot snap{};
                snap.source_index = static_cast<int>(i);
                snap.market = market_name_;
                snap.ts_ms = normalizeTimestampMs(candle.timestamp);
                snap.regime = marketRegimeLabel(regime.regime);
                snap.atr_pct_14 = regime.atr_pct;
                snap.adx = regime.adx;
                snap.spread_pct = metrics.orderbook_snapshot.spread_pct;
                snap.notional = metrics.orderbook_snapshot.ask_notional;
                snap.volume_surge = metrics.volume_surge_ratio;
                snap.imbalance = metrics.orderbook_snapshot.imbalance;
                snap.drop_vs_recent = metrics.price_change_rate;
                snap.drop_vs_signal = metrics.price_momentum;
                snap.p_calibrated = signal.probabilistic_h5_calibrated;
                snap.selection_threshold = signal.probabilistic_h5_threshold;
                snap.margin = signal.probabilistic_h5_margin;
                snap.expected_edge_calibrated_bps = signal.phase3.expected_edge_calibrated_bps;
                snap.expected_edge_used_for_gate_bps = signal.phase3.expected_edge_used_for_gate_bps;
                snap.edge_bps = signal.phase3.expected_edge_used_for_gate_bps;
                snap.snapshot_valid =
                    std::isfinite(snap.p_calibrated) &&
                    std::isfinite(snap.selection_threshold) &&
                    std::isfinite(snap.margin) &&
                    std::isfinite(snap.expected_edge_used_for_gate_bps);
                if (!snap.snapshot_valid) {
                    if (!std::isfinite(snap.expected_edge_used_for_gate_bps)) {
                        snap.fail_reason = "missing_ev_source";
                    } else {
                        snap.fail_reason = "invalid_margin_inputs";
                    }
                }
                gate_inputs.push_back(std::move(snap));
            }
            autolife::gate_vnext::GateVNextTelemetry gate_telemetry{};
            const auto gate_outputs = gate_vnext.run(gate_inputs, &gate_telemetry);
            if (gate_telemetry.funnel.s1_selected_topk > gate_params.quality_topk) {
                LOG_WARN(
                    "GateVNext invariant warning(backtest): S1_selected_topk={} exceeds quality_topk={} market={} ts={}",
                    gate_telemetry.funnel.s1_selected_topk,
                    gate_params.quality_topk,
                    market_name_,
                    normalizeTimestampMs(candle.timestamp)
                );
            }
            entry_funnel_.gate_system_version_effective = "vnext";
            entry_funnel_.quality_topk_effective = gate_params.quality_topk;
            if (!gate_telemetry.provenance.backend_request.empty()) {
                entry_funnel_.gate_vnext_backend_request = gate_telemetry.provenance.backend_request;
            }
            if (!gate_telemetry.provenance.backend_effective.empty()) {
                entry_funnel_.gate_vnext_backend_effective = gate_telemetry.provenance.backend_effective;
            }
            if (!gate_telemetry.provenance.model_sha256.empty()) {
                entry_funnel_.gate_vnext_lgbm_model_sha256 = gate_telemetry.provenance.model_sha256;
            }
            entry_funnel_.gate_vnext_scan_rounds += 1;
            entry_funnel_.gate_vnext_s0_snapshots_valid +=
                static_cast<int>(gate_telemetry.funnel.s0_snapshots_valid);
            entry_funnel_.gate_vnext_s1_selected_topk +=
                static_cast<int>(gate_telemetry.funnel.s1_selected_topk);
            entry_funnel_.gate_vnext_s2_sized_count +=
                static_cast<int>(gate_telemetry.funnel.s2_sized_count);
            entry_funnel_.gate_vnext_s3_exec_gate_pass +=
                static_cast<int>(gate_telemetry.funnel.s3_exec_gate_pass);
            stage_candidates_total =
                static_cast<int>(std::max<long long>(0, gate_telemetry.funnel.s0_snapshots_valid));
            stage_candidates_after_quality_topk =
                static_cast<int>(std::max<long long>(0, gate_telemetry.funnel.s1_selected_topk));
            stage_candidates_after_sizing =
                static_cast<int>(std::max<long long>(0, gate_telemetry.funnel.s2_sized_count));
            stage_candidates_after_portfolio =
                static_cast<int>(std::max<long long>(0, gate_telemetry.funnel.s3_exec_gate_pass));

            std::vector<strategy::Signal> selected_signals;
            selected_signals.reserve(gate_outputs.size());
            for (const auto& gated : gate_outputs) {
                if (gated.execution_reject_reason == "ev_negative_size_zero") {
                    entry_funnel_.gate_vnext_drop_ev_negative_count++;
                }
                if (gate_vnext_ev_samples_.size() < 50 &&
                    gated.source_index >= 0 &&
                    gated.source_index < static_cast<int>(signals.size())) {
                    const auto& src_signal = signals[static_cast<std::size_t>(gated.source_index)];
                    nlohmann::json sample{
                        {"ts_ms", normalizeTimestampMs(candle.timestamp)},
                        {"market", market_name_},
                        {"regime", marketRegimeLabel(regime.regime)},
                        {"source_index", gated.source_index},
                        {"sample_stage", "stage1_selected_topk"},
                        {"snapshot_valid", true},
                        {"snapshot_fail_reason", ""},
                        {"p_raw", nullptr},
                        {"p_cal", gated.p_calibrated},
                        {"p_calibrated", gated.p_calibrated},
                        {"threshold", gated.selection_threshold},
                        {"selection_threshold", gated.selection_threshold},
                        {"margin", gated.margin},
                        {"backend_request", gate_params.backend_request},
                        {"backend_effective", gate_params.backend_effective},
                        {"lgbm_model_sha256", gate_params.model_sha256},
                        {"expected_edge_calibrated_bps", src_signal.phase3.expected_edge_calibrated_bps},
                        {"expected_edge_used_for_gate_bps", src_signal.phase3.expected_edge_used_for_gate_bps},
                        {"signal_expected_value", src_signal.expected_value},
                        {"edge_bps_from_snapshot", gated.edge_bps},
                        {"expected_value_vnext_bps", gated.expected_value_vnext_bps},
                        {"size_fraction", gated.size_fraction},
                        {"execution_gate_pass", gated.execution_gate_pass},
                        {"execution_reject_reason", gated.execution_reject_reason},
                    };
                    gate_vnext_ev_samples_.push_back(std::move(sample));
                }
                if (!gated.execution_gate_pass) {
                    continue;
                }
                if (gated.source_index < 0 ||
                    gated.source_index >= static_cast<int>(signals.size())) {
                    continue;
                }
                selected_signals.push_back(signals[static_cast<std::size_t>(gated.source_index)]);
            }
            if (selected_signals.empty()) {
                entry_funnel_.filtered_out_by_policy++;
                markEntryReject("gate_vnext_selected_zero");
            }
            signals = std::move(selected_signals);
        }
        for (const auto& signal : signals) {
            if (!signal.strategy_name.empty()) {
                strategy_generated_counts_[signal.strategy_name]++;
            }
        }
        if (stage_candidates_total <= 0) {
            stage_candidates_total = std::max(0, static_cast<int>(signals.size()));
        }
        
        std::vector<strategy::Signal> candidate_signals;
        std::vector<strategy::Signal> phase4_ranked_snapshot;
        int phase4_selected_count = 0;

        const bool core_bridge_enabled = engine_config_.enable_core_plane_bridge;
        const bool core_risk_enabled = core_bridge_enabled && engine_config_.enable_core_risk_plane;
        const double hostility_alpha = std::clamp(engine_config_.hostility_ewma_alpha, 0.01, 0.99);

        const double hostility_now = computeMarketHostilityScore(
            engine_config_,
            metrics,
            regime.regime
        );
        if (market_hostility_ewma_ <= 1e-9) {
            market_hostility_ewma_ = hostility_now;
        } else {
            market_hostility_ewma_ = std::clamp(
                market_hostility_ewma_ * (1.0 - hostility_alpha) + hostility_now * hostility_alpha,
                0.0,
                1.0
            );
        }
        candidate_signals = signals;
        phase4_ranked_snapshot = candidate_signals;
        phase4_selected_count = candidate_signals.empty() ? 0 : 1;
        stage_candidates_after_portfolio = std::max(
            0,
            static_cast<int>(candidate_signals.size())
        );
        strategy::Signal best_signal;
        if (!candidate_signals.empty()) {
            best_signal = candidate_signals.front();
            LOG_INFO(
                "{} probabilistic-primary best selected [{}]: p_h5={:.4f}, margin={:+.4f}, liq={:.1f}, strength={:.3f}, candidates={}, regime_state={}, vol_z={:+.3f}, dd_speed_bps={:.2f}, ens_n={}, u_std={:.4f}, ev_blend={:.3f}, ev_conf={:.3f}, edge_raw={:+.4f}%, edge_cal={:+.4f}%, cost_mode={}, c_entry={:.4f}%, c_exit={:.4f}%, c_tail={:.4f}%",
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
        }
        if (!candidate_signals.empty() && best_signal.type == strategy::SignalType::NONE) {
            entry_funnel_.no_best_signal++;
            markEntryReject("no_best_signal");
        }
        if (best_signal.type != strategy::SignalType::NONE) {
            if (phase4_portfolio_diagnostics_.enabled) {
                phase4_portfolio_diagnostics_.selected_total += 1;
            }
            if (!best_signal.strategy_name.empty()) {
                strategy_selected_best_counts_[best_signal.strategy_name]++;
            }
            if (std::isfinite(best_signal.expected_value)) {
                stage_ev_at_selection = best_signal.expected_value;
            }
        }

        // Shadow-only candidate probe:
        // evaluate relaxed candidate supply without touching actual order flow.
        shadow_funnel_.rounds++;
        shadow_funnel_.primary_generated_signals += static_cast<int>(signals.size());
        shadow_funnel_.primary_after_policy_filter += static_cast<int>(candidate_signals.size());
                if (best_signal.type != strategy::SignalType::NONE) {
            best_signal.market_regime = regime.regime;

            autolife::common::signal_policy::normalizeSignalStopLossByRegime(
                best_signal,
                best_signal.market_regime
            );
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
                    const bool archetype_required =
                        autolife::common::signal_policy::requiresTypedArchetype(
                            best_signal.strategy_name
                        );
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
                const double stop_guard_min_pct = std::clamp(
                    engine_config_.entry_capacity_stop_guard_min_pct,
                    0.0,
                    1.0
                );
                const double stop_guard_max_pct = std::clamp(
                    engine_config_.entry_capacity_stop_guard_max_pct,
                    stop_guard_min_pct,
                    1.0
                );
                const double exit_retention_floor = std::clamp(
                    engine_config_.entry_capacity_exit_retention_floor,
                    0.01,
                    0.99
                );
                const double fee_rate = Config::getInstance().getFeeRate();
                const double stop_guard_pct = [&]() {
                    if (best_signal.entry_price > 0.0 &&
                        best_signal.stop_loss > 0.0 &&
                        best_signal.stop_loss < best_signal.entry_price) {
                        const double rp = (best_signal.entry_price - best_signal.stop_loss) / best_signal.entry_price;
                        return std::clamp(rp, stop_guard_min_pct, stop_guard_max_pct);
                    }
                    return std::clamp(
                        engine_config_.entry_capacity_default_stop_guard_pct,
                        stop_guard_min_pct,
                        stop_guard_max_pct
                    );
                }();
                const double slip_guard_pct = dynamic_buy_slippage.guard_slippage_pct;
                const double exit_retention = std::max(
                    exit_retention_floor,
                    1.0 - stop_guard_pct - slip_guard_pct - fee_rate
                );
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
                            // A10-3 SSoT: order stage must reuse manager-pass EV without
                            // recalculation so negative-edge blocking is applied at one stage.
                            const double tracked_expected_edge = std::isfinite(best_signal.expected_value)
                                ? best_signal.expected_value
                                : 0.0;
                            stage_ev_at_order_submit_check = tracked_expected_edge;
                            if (std::isfinite(stage_ev_at_selection) &&
                                std::fabs(stage_ev_at_selection - stage_ev_at_order_submit_check) > 1.0e-12) {
                                stage_ev_mismatch_count += 1;
                                LOG_WARN(
                                    "{} EV SSoT mismatch detected: selection={:+.8f} order_submit={:+.8f}",
                                    market_name_,
                                    stage_ev_at_selection,
                                    stage_ev_at_order_submit_check
                                );
                            }

                            const double reward_pct =
                                (best_signal.take_profit_2 - best_signal.entry_price) /
                                std::max(1e-9, best_signal.entry_price);
                            if (engine_config_.backtest_shadow_policy_only) {
                                // Shadow evidence mode: keep decision stream deterministic
                                // without mutating position state from entry execution.
                            } else {
                                Order order;
                                order.market = market_name_;
                                order.side = OrderSide::BUY;
                                order.price = fill_price;
                                order.volume = quantity;
                                order.strategy_name = best_signal.strategy_name;

                                stage_orders_submitted += 1;
                                executeOrder(order, fill_price);
                                stage_orders_filled += 1;

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
                                pending_be_after_partial_due_ms_.erase(market_name_);
                                const double risk_pct = (best_signal.entry_price - best_signal.stop_loss) / std::max(1e-9, best_signal.entry_price);
                                const double rr = (risk_pct > 1e-9) ? (reward_pct / risk_pct) : 0.0;
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

        entry_funnel_.gate_vnext_s4_submitted += stage_orders_submitted;
        entry_funnel_.gate_vnext_s5_filled += stage_orders_filled;

        appendEntryStageFunnelAudit(
            candle.timestamp,
            market_name_,
            stage_candidates_total,
            stage_candidates_after_quality_topk,
            stage_candidates_after_sizing,
            stage_candidates_after_portfolio,
            stage_orders_submitted,
            stage_orders_filled,
            stage_reject_reason_counts,
            stage_ev_at_selection,
            stage_ev_at_order_submit_check,
            stage_ev_mismatch_count
        );
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
    if (engine_config_.backtest_shadow_policy_only) {
        (void)order;
        (void)price;
        return;
    }

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
    int stop_loss_trigger_count = 0;
    double stop_loss_pnl_sum_krw = 0.0;
    int partial_tp_exit_count = 0;
    int take_profit_full_count = 0;
    double take_profit_full_pnl_sum_krw = 0.0;
    int trending_trade_count = 0;
    double trending_holding_minutes_sum = 0.0;
    int stop_loss_distance_samples_trending = 0;
    double stop_loss_distance_sum_pct_trending = 0.0;
    int take_profit_distance_samples_trending = 0;
    double take_profit_distance_sum_pct_trending = 0.0;
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
            if (reason == "StopLoss") {
                stop_loss_trigger_count++;
                stop_loss_pnl_sum_krw += trade.profit_loss;
            } else if (reason == "TakeProfit1") {
                partial_tp_exit_count++;
            } else if (reason == "TakeProfit2" || reason == "TakeProfitFullDueToMinOrder") {
                take_profit_full_count++;
                take_profit_full_pnl_sum_krw += trade.profit_loss;
            }

            if (trade.market_regime == analytics::MarketRegime::TRENDING_UP ||
                trade.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
                trending_trade_count++;
                if (trade.exit_time > trade.entry_time) {
                    trending_holding_minutes_sum +=
                        static_cast<double>(trade.exit_time - trade.entry_time) / 60000.0;
                }
                if (std::isfinite(trade.initial_stop_loss_distance_pct) &&
                    trade.initial_stop_loss_distance_pct > 0.0) {
                    stop_loss_distance_samples_trending++;
                    stop_loss_distance_sum_pct_trending +=
                        trade.initial_stop_loss_distance_pct;
                }
                if (std::isfinite(trade.initial_take_profit_distance_pct) &&
                    trade.initial_take_profit_distance_pct > 0.0) {
                    take_profit_distance_samples_trending++;
                    take_profit_distance_sum_pct_trending +=
                        trade.initial_take_profit_distance_pct;
                }
            }

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

            Result::TradeHistorySample sample;
            sample.market = trade.market;
            sample.strategy_name = strategy_name;
            sample.entry_archetype = archetype_label;
            sample.regime = regime_label;
            sample.exit_reason = reason;
            sample.entry_time = trade.entry_time;
            sample.exit_time = trade.exit_time;
            sample.entry_price = trade.entry_price;
            sample.exit_price = trade.exit_price;
            sample.quantity = trade.quantity;
            sample.initial_stop_loss_distance_pct = trade.initial_stop_loss_distance_pct;
            sample.initial_take_profit_distance_pct = trade.initial_take_profit_distance_pct;
            sample.holding_minutes = (trade.exit_time > trade.entry_time)
                ? (static_cast<double>(trade.exit_time - trade.entry_time) / 60000.0)
                : 0.0;
            sample.profit_loss_krw = trade.profit_loss;
            sample.profit_loss_pct = trade.profit_loss_pct;
            sample.fee_paid_krw = trade.fee_paid;
            sample.signal_filter = trade.signal_filter;
            sample.signal_strength = trade.signal_strength;
            sample.liquidity_score = trade.liquidity_score;
            sample.volatility = trade.volatility;
            sample.expected_value = trade.expected_value;
            sample.reward_risk_ratio = trade.reward_risk_ratio;
            sample.probabilistic_runtime_applied = trade.probabilistic_runtime_applied;
            sample.probabilistic_h5_calibrated = trade.probabilistic_h5_calibrated;
            sample.probabilistic_h5_margin = trade.probabilistic_h5_margin;
            result.trade_history_samples.push_back(std::move(sample));
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
    result.ranging_shadow = ranging_shadow_summary_;
    result.shadow_funnel = shadow_funnel_;
    if (result.shadow_funnel.rounds > 0) {
        const double rounds = static_cast<double>(result.shadow_funnel.rounds);
        result.shadow_funnel.avg_policy_supply_lift =
            result.shadow_funnel.policy_supply_lift_sum / rounds;
    } else {
        result.shadow_funnel.avg_policy_supply_lift = 0.0;
    }
    result.post_entry_risk_telemetry.adaptive_stop_updates = adaptive_stop_update_count_;
    result.post_entry_risk_telemetry.adaptive_tp_recalibration_updates = adaptive_tp_recalibration_count_;
    result.post_entry_risk_telemetry.adaptive_partial_ratio_samples = adaptive_partial_ratio_samples_;
    result.post_entry_risk_telemetry.adaptive_partial_ratio_sum = adaptive_partial_ratio_sum_;
    result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram = adaptive_partial_ratio_histogram_;
    result.post_entry_risk_telemetry.be_move_attempt_count = be_move_attempt_count_;
    result.post_entry_risk_telemetry.be_move_applied_count = be_move_applied_count_;
    result.post_entry_risk_telemetry.be_move_skipped_due_to_delay_count =
        be_move_skipped_due_to_delay_count_;
    result.post_entry_risk_telemetry.stop_loss_after_partial_tp_count =
        stop_loss_after_partial_tp_count_;
    result.post_entry_risk_telemetry.stop_loss_before_partial_tp_count =
        stop_loss_before_partial_tp_count_;
    result.post_entry_risk_telemetry.be_after_partial_tp_delay_sec =
        be_after_partial_tp_delay_sec_;
    result.post_entry_risk_telemetry.stop_loss_trigger_count = stop_loss_trigger_count;
    result.post_entry_risk_telemetry.stop_loss_pnl_sum_krw = stop_loss_pnl_sum_krw;
    result.post_entry_risk_telemetry.stop_loss_avg_pnl_krw =
        (stop_loss_trigger_count > 0)
            ? (stop_loss_pnl_sum_krw / static_cast<double>(stop_loss_trigger_count))
            : 0.0;
    result.post_entry_risk_telemetry.partial_tp_exit_count = partial_tp_exit_count;
    result.post_entry_risk_telemetry.take_profit_full_count = take_profit_full_count;
    result.post_entry_risk_telemetry.take_profit_full_pnl_sum_krw = take_profit_full_pnl_sum_krw;
    result.post_entry_risk_telemetry.take_profit_full_avg_pnl_krw =
        (take_profit_full_count > 0)
            ? (take_profit_full_pnl_sum_krw / static_cast<double>(take_profit_full_count))
            : 0.0;
    result.post_entry_risk_telemetry.tp_hit_rate =
        (closed_trades > 0)
            ? (static_cast<double>(take_profit_full_count) / static_cast<double>(closed_trades))
            : 0.0;
    result.post_entry_risk_telemetry.trending_trade_count = trending_trade_count;
    result.post_entry_risk_telemetry.trending_holding_minutes_sum = trending_holding_minutes_sum;
    result.post_entry_risk_telemetry.avg_holding_minutes_trending =
        (trending_trade_count > 0)
            ? (trending_holding_minutes_sum / static_cast<double>(trending_trade_count))
            : 0.0;
    result.post_entry_risk_telemetry.stop_loss_distance_samples_trending =
        stop_loss_distance_samples_trending;
    result.post_entry_risk_telemetry.stop_loss_distance_sum_pct_trending =
        stop_loss_distance_sum_pct_trending;
    result.post_entry_risk_telemetry.avg_stop_loss_distance_pct_trending =
        (stop_loss_distance_samples_trending > 0)
            ? (stop_loss_distance_sum_pct_trending /
               static_cast<double>(stop_loss_distance_samples_trending))
            : 0.0;
    result.post_entry_risk_telemetry.take_profit_distance_samples_trending =
        take_profit_distance_samples_trending;
    result.post_entry_risk_telemetry.take_profit_distance_sum_pct_trending =
        take_profit_distance_sum_pct_trending;
    result.post_entry_risk_telemetry.avg_take_profit_distance_pct_trending =
        (take_profit_distance_samples_trending > 0)
            ? (take_profit_distance_sum_pct_trending /
               static_cast<double>(take_profit_distance_samples_trending))
            : 0.0;
    result.strategyless_position_checks = strategyless_position_checks_;
    result.strategyless_runtime_archetype_checks = strategyless_runtime_archetype_checks_;
    result.strategyless_risk_exit_signals = strategyless_risk_exit_signals_;
    result.strategyless_current_stop_hits = strategyless_current_stop_hits_;
    result.strategyless_current_tp1_hits = strategyless_current_tp1_hits_;
    result.strategyless_current_tp2_hits = strategyless_current_tp2_hits_;
    auto phase4_diag = phase4_portfolio_diagnostics_;
    phase4_diag.regime_budget_multiplier_avg_by_regime.clear();
    for (const auto& kv : phase4_diag.regime_budget_multiplier_count_by_regime) {
        if (kv.second <= 0) {
            continue;
        }
        const double sum = phase4_diag.regime_budget_multiplier_sum_by_regime.count(kv.first) > 0
            ? phase4_diag.regime_budget_multiplier_sum_by_regime.at(kv.first)
            : 0.0;
        phase4_diag.regime_budget_multiplier_avg_by_regime[kv.first] =
            sum / static_cast<double>(kv.second);
    }
    result.phase4_portfolio_diagnostics = std::move(phase4_diag);
    result.phase4_portfolio_diagnostics.candidate_snapshot_sampled =
        static_cast<int>(phase4_candidate_snapshot_samples_.size());
    result.phase4_portfolio_diagnostics.selection_rate =
        phase4_portfolio_diagnostics_.candidates_total > 0
            ? (static_cast<double>(phase4_portfolio_diagnostics_.selected_total) /
               static_cast<double>(phase4_portfolio_diagnostics_.candidates_total))
            : 0.0;
    result.phase4_candidate_snapshot_samples = phase4_candidate_snapshot_samples_;
    return result;
}



} // namespace backtest
} // namespace autolife

