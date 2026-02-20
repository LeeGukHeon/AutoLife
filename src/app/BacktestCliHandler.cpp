#include "app/BacktestCliHandler.h"

#include "app/BacktestReportFormatter.h"
#include "common/Logger.h"
#include "runtime/BacktestRuntime.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace autolife::app {

namespace {

using BacktestResult = backtest::BacktestEngine::Result;

static std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

struct CompanionCheckResult {
    bool applicable = false;
    std::vector<std::string> missing_tokens;
    std::vector<std::string> found_tokens;
};

static CompanionCheckResult checkHigherTfCompanions(const std::string& csv_path) {
    CompanionCheckResult out;
    std::filesystem::path primary(csv_path);
    if (!std::filesystem::exists(primary) || !primary.has_parent_path()) {
        return out;
    }

    const std::string stem_lower = toLowerCopy(primary.stem().string());
    const std::string prefix = "upbit_";
    const std::string pivot = "_1m_";
    if (!startsWith(stem_lower, prefix)) {
        return out;
    }

    const size_t market_begin = prefix.size();
    const size_t market_end = stem_lower.find(pivot, market_begin);
    if (market_end == std::string::npos || market_end <= market_begin) {
        return out;
    }
    out.applicable = true;

    const std::string market_token = stem_lower.substr(market_begin, market_end - market_begin);
    const std::filesystem::path parent = primary.parent_path();

    auto hasCompanion = [&](const std::string& token) {
        const std::string expected_prefix = "upbit_" + market_token + "_" + token + "_";
        for (const auto& entry : std::filesystem::directory_iterator(parent)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (toLowerCopy(entry.path().extension().string()) != ".csv") {
                continue;
            }
            const std::string candidate_stem = toLowerCopy(entry.path().stem().string());
            if (startsWith(candidate_stem, expected_prefix)) {
                return true;
            }
        }
        return false;
    };

    for (const auto& token : {"5m", "15m", "60m", "240m"}) {
        if (hasCompanion(token)) {
            out.found_tokens.push_back(token);
        } else {
            out.missing_tokens.push_back(token);
        }
    }

    return out;
}

static void printCompanionRequirementError(const std::string& csv_path, const CompanionCheckResult& check) {
    std::cout << "실거래 동등 MTF 모드 검증 실패: " << csv_path << "\n";
    if (!check.applicable) {
        std::cout << "  파일명 규칙이 맞지 않습니다. 예: upbit_KRW_BTC_1m_12000.csv\n";
        std::cout << "  companion(5m/15m/60m/240m) 자동 매칭이 가능한 1m 파일을 지정하세요.\n";
        return;
    }

    if (!check.missing_tokens.empty()) {
        std::cout << "  누락된 companion TF: ";
        for (size_t i = 0; i < check.missing_tokens.size(); ++i) {
            if (i > 0) {
                std::cout << ", ";
            }
            std::cout << check.missing_tokens[i];
        }
        std::cout << "\n";
        std::cout << "  같은 폴더에 upbit_<market>_5m_*.csv / 15m / 60m / 240m 파일이 필요합니다.\n";
    }
}

nlohmann::json preCatSnapshotBranchToJson(const BacktestResult::PreCatFeatureSnapshotBranch& branch) {
    return nlohmann::json{
        {"samples", branch.samples},
        {"recovery_quality_context_hits", branch.recovery_quality_context_hits},
        {"recovery_evidence_hits", branch.recovery_evidence_hits},
        {"recovery_evidence_relaxed_hits", branch.recovery_evidence_relaxed_hits},
        {"recovery_evidence_hysteresis_hits", branch.recovery_evidence_hysteresis_hits},
        {"non_hostile_regime_hits", branch.non_hostile_regime_hits},
        {"severe_active_hits", branch.severe_active_hits},
        {"contextual_downgrade_hits", branch.contextual_downgrade_hits},
        {"soften_ready_hits", branch.soften_ready_hits},
        {"soften_candidate_rr_ok_hits", branch.soften_candidate_rr_ok_hits},
        {"severe_axis_ge_threshold_hits", branch.severe_axis_ge_threshold_hits},
        {"avg_signal_strength", branch.avg_signal_strength},
        {"avg_signal_expected_value", branch.avg_signal_expected_value},
        {"avg_signal_liquidity", branch.avg_signal_liquidity},
        {"avg_signal_reward_risk", branch.avg_signal_reward_risk},
        {"avg_history_expectancy_krw", branch.avg_history_expectancy_krw},
        {"avg_history_profit_factor", branch.avg_history_profit_factor},
        {"avg_history_win_rate", branch.avg_history_win_rate},
        {"avg_history_trades", branch.avg_history_trades},
        {"avg_history_loss_to_win", branch.avg_history_loss_to_win},
        {"avg_full_history_pressure", branch.avg_full_history_pressure},
        {"avg_recent_history_pressure", branch.avg_recent_history_pressure},
        {"avg_regime_history_pressure", branch.avg_regime_history_pressure},
        {"avg_severe_axis_count", branch.avg_severe_axis_count}
    };
}

nlohmann::json buildBacktestResultJson(const BacktestResult& result) {
    nlohmann::json j;
    j["final_balance"] = result.final_balance;
    j["total_profit"] = result.total_profit;
    j["max_drawdown"] = result.max_drawdown;
    j["total_trades"] = result.total_trades;
    j["winning_trades"] = result.winning_trades;
    j["losing_trades"] = result.losing_trades;
    j["win_rate"] = result.win_rate;
    j["avg_win_krw"] = result.avg_win_krw;
    j["avg_loss_krw"] = result.avg_loss_krw;
    j["profit_factor"] = result.profit_factor;
    j["expectancy_krw"] = result.expectancy_krw;
    j["avg_holding_minutes"] = result.avg_holding_minutes;
    j["avg_fee_krw"] = result.avg_fee_krw;
    j["intrabar_stop_tp_collision_count"] = result.intrabar_stop_tp_collision_count;
    j["exit_reason_counts"] = result.exit_reason_counts;
    j["entry_rejection_reason_counts"] = result.entry_rejection_reason_counts;
    j["no_signal_pattern_counts"] = result.no_signal_pattern_counts;
    j["entry_quality_edge_gap_buckets"] = result.entry_quality_edge_gap_buckets;
    j["intrabar_collision_by_strategy"] = result.intrabar_collision_by_strategy;
    j["strategy_collect_exception_count"] = result.strategy_collect_exception_count;
    const int partial_ratio_samples = std::max(0, result.post_entry_risk_telemetry.adaptive_partial_ratio_samples);
    const double partial_ratio_avg = (partial_ratio_samples > 0)
        ? (result.post_entry_risk_telemetry.adaptive_partial_ratio_sum / static_cast<double>(partial_ratio_samples))
        : 0.0;
    j["post_entry_risk_telemetry"] = {
        {"adaptive_stop_updates", result.post_entry_risk_telemetry.adaptive_stop_updates},
        {"adaptive_tp_recalibration_updates", result.post_entry_risk_telemetry.adaptive_tp_recalibration_updates},
        {"adaptive_partial_ratio_samples", partial_ratio_samples},
        {"adaptive_partial_ratio_avg", partial_ratio_avg},
        {"adaptive_partial_ratio_histogram", {
            {"0.35_0.44", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[0]},
            {"0.45_0.54", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[1]},
            {"0.55_0.64", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[2]},
            {"0.65_0.74", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[3]},
            {"0.75_0.80", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[4]}
        }}
    };

    j["entry_funnel"] = {
        {"entry_rounds", result.entry_funnel.entry_rounds},
        {"skipped_due_to_open_position", result.entry_funnel.skipped_due_to_open_position},
        {"no_signal_generated", result.entry_funnel.no_signal_generated},
        {"filtered_out_by_manager", result.entry_funnel.filtered_out_by_manager},
        {"filtered_out_by_policy", result.entry_funnel.filtered_out_by_policy},
        {"no_best_signal", result.entry_funnel.no_best_signal},
        {"blocked_pattern_gate", result.entry_funnel.blocked_pattern_gate},
        {"blocked_rr_rebalance", result.entry_funnel.blocked_rr_rebalance},
        {"blocked_risk_gate", result.entry_funnel.blocked_risk_gate},
        {"blocked_risk_gate_strategy_ev", result.entry_funnel.blocked_risk_gate_strategy_ev},
        {"blocked_risk_gate_strategy_ev_pre_catastrophic", result.entry_funnel.blocked_risk_gate_strategy_ev_pre_catastrophic},
        {"blocked_risk_gate_strategy_ev_severe_threshold", result.entry_funnel.blocked_risk_gate_strategy_ev_severe_threshold},
        {"blocked_risk_gate_strategy_ev_catastrophic_history", result.entry_funnel.blocked_risk_gate_strategy_ev_catastrophic_history},
        {"blocked_risk_gate_strategy_ev_loss_asymmetry", result.entry_funnel.blocked_risk_gate_strategy_ev_loss_asymmetry},
        {"blocked_risk_gate_strategy_ev_unknown", result.entry_funnel.blocked_risk_gate_strategy_ev_unknown},
        {"strategy_ev_pre_cat_observed", result.entry_funnel.strategy_ev_pre_cat_observed},
        {"strategy_ev_pre_cat_recovery_quality_context", result.entry_funnel.strategy_ev_pre_cat_recovery_quality_context},
        {"strategy_ev_pre_cat_recovery_evidence_any", result.entry_funnel.strategy_ev_pre_cat_recovery_evidence_any},
        {"strategy_ev_pre_cat_recovery_evidence_relaxed_any", result.entry_funnel.strategy_ev_pre_cat_recovery_evidence_relaxed_any},
        {"strategy_ev_pre_cat_recovery_evidence_relaxed_recent_regime", result.entry_funnel.strategy_ev_pre_cat_recovery_evidence_relaxed_recent_regime},
        {"strategy_ev_pre_cat_recovery_evidence_relaxed_full_history", result.entry_funnel.strategy_ev_pre_cat_recovery_evidence_relaxed_full_history},
        {"strategy_ev_pre_cat_recovery_evidence_for_soften", result.entry_funnel.strategy_ev_pre_cat_recovery_evidence_for_soften},
        {"strategy_ev_pre_cat_recovery_evidence_bridge", result.entry_funnel.strategy_ev_pre_cat_recovery_evidence_bridge},
        {"strategy_ev_pre_cat_recovery_evidence_bridge_surrogate", result.entry_funnel.strategy_ev_pre_cat_recovery_evidence_bridge_surrogate},
        {"strategy_ev_pre_cat_recovery_evidence_hysteresis_override", result.entry_funnel.strategy_ev_pre_cat_recovery_evidence_hysteresis_override},
        {"strategy_ev_pre_cat_quality_hysteresis_override", result.entry_funnel.strategy_ev_pre_cat_quality_hysteresis_override},
        {"strategy_ev_pre_cat_quality_context_relaxed_overlap", result.entry_funnel.strategy_ev_pre_cat_quality_context_relaxed_overlap},
        {"strategy_ev_pre_cat_quality_fail_regime", result.entry_funnel.strategy_ev_pre_cat_quality_fail_regime},
        {"strategy_ev_pre_cat_quality_fail_strength", result.entry_funnel.strategy_ev_pre_cat_quality_fail_strength},
        {"strategy_ev_pre_cat_quality_fail_expected_value", result.entry_funnel.strategy_ev_pre_cat_quality_fail_expected_value},
        {"strategy_ev_pre_cat_quality_fail_liquidity", result.entry_funnel.strategy_ev_pre_cat_quality_fail_liquidity},
        {"strategy_ev_pre_cat_soften_ready", result.entry_funnel.strategy_ev_pre_cat_soften_ready},
        {"strategy_ev_pre_cat_soften_candidate_quality_and_evidence", result.entry_funnel.strategy_ev_pre_cat_soften_candidate_quality_and_evidence},
        {"strategy_ev_pre_cat_soften_candidate_non_severe", result.entry_funnel.strategy_ev_pre_cat_soften_candidate_non_severe},
        {"strategy_ev_pre_cat_soften_candidate_non_hostile", result.entry_funnel.strategy_ev_pre_cat_soften_candidate_non_hostile},
        {"strategy_ev_pre_cat_soften_candidate_rr_ok", result.entry_funnel.strategy_ev_pre_cat_soften_candidate_rr_ok},
        {"strategy_ev_pre_cat_severe_legacy_hits", result.entry_funnel.strategy_ev_pre_cat_severe_legacy_hits},
        {"strategy_ev_pre_cat_severe_composite_hits", result.entry_funnel.strategy_ev_pre_cat_severe_composite_hits},
        {"strategy_ev_pre_cat_severe_composite_catastrophic_hits", result.entry_funnel.strategy_ev_pre_cat_severe_composite_catastrophic_hits},
        {"strategy_ev_pre_cat_severe_composite_pressure_axis_hits", result.entry_funnel.strategy_ev_pre_cat_severe_composite_pressure_axis_hits},
        {"strategy_ev_pre_cat_severe_composite_pressure_only_hits", result.entry_funnel.strategy_ev_pre_cat_severe_composite_pressure_only_hits},
        {"strategy_ev_pre_cat_contextual_severe_downgrade_hits", result.entry_funnel.strategy_ev_pre_cat_contextual_severe_downgrade_hits},
        {"strategy_ev_pre_cat_severe_active_hits", result.entry_funnel.strategy_ev_pre_cat_severe_active_hits},
        {"strategy_ev_pre_cat_softened_contextual", result.entry_funnel.strategy_ev_pre_cat_softened_contextual},
        {"strategy_ev_pre_cat_softened_override", result.entry_funnel.strategy_ev_pre_cat_softened_override},
        {"strategy_ev_pre_cat_softened_no_soft_quality_relief", result.entry_funnel.strategy_ev_pre_cat_softened_no_soft_quality_relief},
        {"strategy_ev_pre_cat_softened_candidate_rr_failsafe", result.entry_funnel.strategy_ev_pre_cat_softened_candidate_rr_failsafe},
        {"strategy_ev_pre_cat_softened_pressure_rebound_relief", result.entry_funnel.strategy_ev_pre_cat_softened_pressure_rebound_relief},
        {"strategy_ev_pre_cat_negative_history_quarantine_set", result.entry_funnel.strategy_ev_pre_cat_negative_history_quarantine_set},
        {"strategy_ev_pre_cat_negative_history_quarantine_active", result.entry_funnel.strategy_ev_pre_cat_negative_history_quarantine_active},
        {"strategy_ev_pre_cat_blocked_severe_sync", result.entry_funnel.strategy_ev_pre_cat_blocked_severe_sync},
        {"strategy_ev_pre_cat_blocked_no_soft_path", result.entry_funnel.strategy_ev_pre_cat_blocked_no_soft_path},
        {"blocked_risk_gate_regime", result.entry_funnel.blocked_risk_gate_regime},
        {"blocked_risk_gate_entry_quality", result.entry_funnel.blocked_risk_gate_entry_quality},
        {"blocked_risk_gate_entry_quality_rr", result.entry_funnel.blocked_risk_gate_entry_quality_rr},
        {"blocked_risk_gate_entry_quality_rr_base", result.entry_funnel.blocked_risk_gate_entry_quality_rr_base},
        {"blocked_risk_gate_entry_quality_rr_adaptive", result.entry_funnel.blocked_risk_gate_entry_quality_rr_adaptive},
        {"blocked_risk_gate_entry_quality_rr_adaptive_history", result.entry_funnel.blocked_risk_gate_entry_quality_rr_adaptive_history},
        {"blocked_risk_gate_entry_quality_rr_adaptive_regime", result.entry_funnel.blocked_risk_gate_entry_quality_rr_adaptive_regime},
        {"blocked_risk_gate_entry_quality_rr_adaptive_mixed", result.entry_funnel.blocked_risk_gate_entry_quality_rr_adaptive_mixed},
        {"blocked_risk_gate_entry_quality_edge", result.entry_funnel.blocked_risk_gate_entry_quality_edge},
        {"blocked_risk_gate_entry_quality_edge_base", result.entry_funnel.blocked_risk_gate_entry_quality_edge_base},
        {"blocked_risk_gate_entry_quality_edge_adaptive", result.entry_funnel.blocked_risk_gate_entry_quality_edge_adaptive},
        {"blocked_risk_gate_entry_quality_edge_adaptive_history", result.entry_funnel.blocked_risk_gate_entry_quality_edge_adaptive_history},
        {"blocked_risk_gate_entry_quality_edge_adaptive_regime", result.entry_funnel.blocked_risk_gate_entry_quality_edge_adaptive_regime},
        {"blocked_risk_gate_entry_quality_edge_adaptive_mixed", result.entry_funnel.blocked_risk_gate_entry_quality_edge_adaptive_mixed},
        {"blocked_risk_gate_entry_quality_rr_edge", result.entry_funnel.blocked_risk_gate_entry_quality_rr_edge},
        {"blocked_risk_gate_entry_quality_rr_edge_base", result.entry_funnel.blocked_risk_gate_entry_quality_rr_edge_base},
        {"blocked_risk_gate_entry_quality_rr_edge_adaptive", result.entry_funnel.blocked_risk_gate_entry_quality_rr_edge_adaptive},
        {"blocked_risk_gate_entry_quality_rr_edge_adaptive_history", result.entry_funnel.blocked_risk_gate_entry_quality_rr_edge_adaptive_history},
        {"blocked_risk_gate_entry_quality_rr_edge_adaptive_regime", result.entry_funnel.blocked_risk_gate_entry_quality_rr_edge_adaptive_regime},
        {"blocked_risk_gate_entry_quality_rr_edge_adaptive_mixed", result.entry_funnel.blocked_risk_gate_entry_quality_rr_edge_adaptive_mixed},
        {"blocked_risk_gate_entry_quality_invalid_levels", result.entry_funnel.blocked_risk_gate_entry_quality_invalid_levels},
        {"blocked_risk_gate_other", result.entry_funnel.blocked_risk_gate_other},
        {"blocked_second_stage_confirmation", result.entry_funnel.blocked_second_stage_confirmation},
        {"blocked_second_stage_confirmation_rr_margin", result.entry_funnel.blocked_second_stage_confirmation_rr_margin},
        {"blocked_second_stage_confirmation_rr_margin_near_miss", result.entry_funnel.blocked_second_stage_confirmation_rr_margin_near_miss},
        {"blocked_second_stage_confirmation_edge_margin", result.entry_funnel.blocked_second_stage_confirmation_edge_margin},
        {"blocked_second_stage_confirmation_hostile_safety_adders", result.entry_funnel.blocked_second_stage_confirmation_hostile_safety_adders},
        {"blocked_second_stage_confirmation_hostile_regime_safety_adders", result.entry_funnel.blocked_second_stage_confirmation_hostile_regime_safety_adders},
        {"blocked_second_stage_confirmation_hostile_liquidity_safety_adders", result.entry_funnel.blocked_second_stage_confirmation_hostile_liquidity_safety_adders},
        {"blocked_second_stage_confirmation_hostile_history_safety_adders", result.entry_funnel.blocked_second_stage_confirmation_hostile_history_safety_adders},
        {"blocked_second_stage_confirmation_hostile_history_mild_safety_adders", result.entry_funnel.blocked_second_stage_confirmation_hostile_history_mild_safety_adders},
        {"blocked_second_stage_confirmation_hostile_history_moderate_safety_adders", result.entry_funnel.blocked_second_stage_confirmation_hostile_history_moderate_safety_adders},
        {"blocked_second_stage_confirmation_hostile_history_severe_safety_adders", result.entry_funnel.blocked_second_stage_confirmation_hostile_history_severe_safety_adders},
        {"blocked_second_stage_confirmation_hostile_dynamic_tighten_safety_adders", result.entry_funnel.blocked_second_stage_confirmation_hostile_dynamic_tighten_safety_adders},
        {"second_stage_rr_margin_near_miss_observed", result.entry_funnel.second_stage_rr_margin_near_miss_observed},
        {"second_stage_rr_margin_soft_score_applied", result.entry_funnel.second_stage_rr_margin_soft_score_applied},
        {"second_stage_rr_margin_near_miss_relief_applied", result.entry_funnel.second_stage_rr_margin_near_miss_relief_applied},
        {"two_head_aggregation_rr_margin_near_miss_head_score_floor_applied", result.entry_funnel.two_head_aggregation_rr_margin_near_miss_head_score_floor_applied},
        {"two_head_aggregation_rr_margin_near_miss_floor_relax_applied", result.entry_funnel.two_head_aggregation_rr_margin_near_miss_floor_relax_applied},
        {"two_head_aggregation_rr_margin_near_miss_adaptive_floor_relax_applied", result.entry_funnel.two_head_aggregation_rr_margin_near_miss_adaptive_floor_relax_applied},
        {"two_head_aggregation_rr_margin_near_miss_surplus_compensation_applied", result.entry_funnel.two_head_aggregation_rr_margin_near_miss_surplus_compensation_applied},
        {"two_head_aggregation_override_accept", result.entry_funnel.two_head_aggregation_override_accept},
        {"two_head_aggregation_override_accept_rr_margin_near_miss", result.entry_funnel.two_head_aggregation_override_accept_rr_margin_near_miss},
        {"two_head_aggregation_rr_margin_near_miss_relief_blocked", result.entry_funnel.two_head_aggregation_rr_margin_near_miss_relief_blocked},
        {"two_head_aggregation_rr_margin_near_miss_relief_blocked_override_disallowed", result.entry_funnel.two_head_aggregation_rr_margin_near_miss_relief_blocked_override_disallowed},
        {"two_head_aggregation_rr_margin_near_miss_relief_blocked_entry_floor", result.entry_funnel.two_head_aggregation_rr_margin_near_miss_relief_blocked_entry_floor},
        {"two_head_aggregation_rr_margin_near_miss_relief_blocked_second_stage_floor", result.entry_funnel.two_head_aggregation_rr_margin_near_miss_relief_blocked_second_stage_floor},
        {"two_head_aggregation_rr_margin_near_miss_relief_blocked_aggregate_score", result.entry_funnel.two_head_aggregation_rr_margin_near_miss_relief_blocked_aggregate_score},
        {"two_head_aggregation_blocked", result.entry_funnel.two_head_aggregation_blocked},
        {"blocked_risk_manager", result.entry_funnel.blocked_risk_manager},
        {"blocked_min_order_or_capital", result.entry_funnel.blocked_min_order_or_capital},
        {"blocked_order_sizing", result.entry_funnel.blocked_order_sizing},
        {"entries_executed", result.entry_funnel.entries_executed}
    };
    j["shadow_funnel"] = {
        {"rounds", result.shadow_funnel.rounds},
        {"primary_generated_signals", result.shadow_funnel.primary_generated_signals},
        {"primary_after_manager_filter", result.shadow_funnel.primary_after_manager_filter},
        {"shadow_after_manager_filter", result.shadow_funnel.shadow_after_manager_filter},
        {"primary_after_policy_filter", result.shadow_funnel.primary_after_policy_filter},
        {"shadow_after_policy_filter", result.shadow_funnel.shadow_after_policy_filter},
        {"primary_best_signal_available", result.shadow_funnel.primary_best_signal_available},
        {"shadow_best_signal_available", result.shadow_funnel.shadow_best_signal_available},
        {"supply_improved_rounds", result.shadow_funnel.supply_improved_rounds},
        {"manager_supply_lift_sum", result.shadow_funnel.manager_supply_lift_sum},
        {"policy_supply_lift_sum", result.shadow_funnel.policy_supply_lift_sum},
        {"avg_manager_supply_lift", result.shadow_funnel.avg_manager_supply_lift},
        {"avg_policy_supply_lift", result.shadow_funnel.avg_policy_supply_lift}
    };

    j["pre_cat_feature_snapshot"] = {
        {"observed", preCatSnapshotBranchToJson(result.pre_cat_feature_snapshot.observed)},
        {"softened_contextual", preCatSnapshotBranchToJson(result.pre_cat_feature_snapshot.softened_contextual)},
        {"softened_override", preCatSnapshotBranchToJson(result.pre_cat_feature_snapshot.softened_override)},
        {"blocked_severe_sync", preCatSnapshotBranchToJson(result.pre_cat_feature_snapshot.blocked_severe_sync)},
        {"blocked_no_soft_path", preCatSnapshotBranchToJson(result.pre_cat_feature_snapshot.blocked_no_soft_path)}
    };

    j["strategy_signal_funnel"] = nlohmann::json::array();
    for (const auto& sf : result.strategy_signal_funnel) {
        j["strategy_signal_funnel"].push_back({
            {"strategy_name", sf.strategy_name},
            {"generated_signals", sf.generated_signals},
            {"selected_best", sf.selected_best},
            {"blocked_by_risk_manager", sf.blocked_by_risk_manager},
            {"entries_executed", sf.entries_executed}
        });
    }
    j["strategy_collection_summaries"] = nlohmann::json::array();
    for (const auto& sc : result.strategy_collection_summaries) {
        j["strategy_collection_summaries"].push_back({
            {"strategy_name", sc.strategy_name},
            {"skipped_disabled", sc.skipped_disabled},
            {"no_signal", sc.no_signal},
            {"generated", sc.generated}
        });
    }

    j["strategy_summaries"] = nlohmann::json::array();
    for (const auto& s : result.strategy_summaries) {
        j["strategy_summaries"].push_back({
            {"strategy_name", s.strategy_name},
            {"total_trades", s.total_trades},
            {"winning_trades", s.winning_trades},
            {"losing_trades", s.losing_trades},
            {"win_rate", s.win_rate},
            {"total_profit", s.total_profit},
            {"avg_win_krw", s.avg_win_krw},
            {"avg_loss_krw", s.avg_loss_krw},
            {"profit_factor", s.profit_factor}
        });
    }

    j["pattern_summaries"] = nlohmann::json::array();
    for (const auto& p : result.pattern_summaries) {
        j["pattern_summaries"].push_back({
            {"strategy_name", p.strategy_name},
            {"entry_archetype", p.entry_archetype},
            {"regime", p.regime},
            {"volatility_bucket", p.volatility_bucket},
            {"liquidity_bucket", p.liquidity_bucket},
            {"strength_bucket", p.strength_bucket},
            {"expected_value_bucket", p.expected_value_bucket},
            {"reward_risk_bucket", p.reward_risk_bucket},
            {"total_trades", p.total_trades},
            {"winning_trades", p.winning_trades},
            {"losing_trades", p.losing_trades},
            {"win_rate", p.win_rate},
            {"total_profit", p.total_profit},
            {"avg_profit_krw", p.avg_profit_krw},
            {"profit_factor", p.profit_factor}
        });
    }

    return j;
}

}  // namespace

bool tryRunCliBacktest(int argc, char* argv[], Config& config, int& out_exit_code) {
    if (!(argc > 1 && std::string(argv[1]) == "--backtest" && argc > 2)) {
        return false;
    }

    bool json_mode = false;
    std::vector<std::string> cli_enabled_strategies;
    double cli_initial_capital = -1.0;
    bool cli_require_higher_tf_companions = false;

    auto trim_copy = [](std::string s) {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::string();
        }
        const auto last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    };
    auto normalize_strategy_name = [&](std::string s) {
        s = trim_copy(s);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == "grid") {
            return std::string("grid_trading");
        }
        if (s == "foundation" ||
            s == "foundation_adaptive_strategy" ||
            s == "foundation adaptive strategy") {
            return std::string("foundation_adaptive");
        }
        return s;
    };

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            json_mode = true;
            continue;
        }
        if (arg == "--require-higher-tf-companions") {
            cli_require_higher_tf_companions = true;
            continue;
        }
        if (arg == "--strategies" && i + 1 < argc) {
            std::string csv = argv[++i];
            size_t start = 0;
            while (start <= csv.size()) {
                const size_t comma = csv.find(',', start);
                std::string token = (comma == std::string::npos)
                    ? csv.substr(start)
                    : csv.substr(start, comma - start);
                token = normalize_strategy_name(token);
                if (!token.empty()) {
                    cli_enabled_strategies.push_back(token);
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            continue;
        }
        if (arg == "--initial-capital" && i + 1 < argc) {
            try {
                cli_initial_capital = std::stod(argv[++i]);
            } catch (...) {
                std::cerr << "Invalid --initial-capital value. Ignored.\n";
            }
        }
    }

    if (cli_initial_capital > 0.0) {
        config.setInitialCapital(cli_initial_capital);
    }
    if (!cli_enabled_strategies.empty()) {
        config.setEnabledStrategies(cli_enabled_strategies);
    }

    std::cout << "백테스트 모드(CLI) 실행\n";
    const std::string cli_backtest_path = argv[2];
    if (!std::filesystem::exists(cli_backtest_path)) {
        std::cerr << "백테스트 파일을 찾을 수 없습니다: " << cli_backtest_path << "\n";
        out_exit_code = 1;
        return true;
    }
    if (cli_require_higher_tf_companions) {
        const auto check = checkHigherTfCompanions(cli_backtest_path);
        if (!check.applicable || !check.missing_tokens.empty()) {
            printCompanionRequirementError(cli_backtest_path, check);
            out_exit_code = 1;
            return true;
        }
    }
    LOG_INFO("Starting Backtest Mode with file: {}", cli_backtest_path);

    backtest::BacktestEngine bt_engine;
    bt_engine.init(config);
    bt_engine.loadData(cli_backtest_path);
    bt_engine.run();

    auto result = bt_engine.getResult();
    if (json_mode) {
        std::cout << buildBacktestResultJson(result).dump() << "\n";
        out_exit_code = 0;
        return true;
    }

    BacktestSummaryOptions summary_options;
    summary_options.include_extended_metrics = true;
    printBacktestResultSummary(result, summary_options, std::cout);
    out_exit_code = 0;
    return true;
}

}  // namespace autolife::app
