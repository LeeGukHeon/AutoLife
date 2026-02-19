#include "app/BacktestReportFormatter.h"

#include <algorithm>
#include <iomanip>
#include <vector>

namespace autolife::app {

void printTopEntryRejectionReasons(
    const std::map<std::string, int>& rejection_counts,
    std::ostream& out,
    size_t top_n
) {
    if (rejection_counts.empty()) {
        return;
    }

    std::vector<std::pair<std::string, int>> reject_pairs(
        rejection_counts.begin(),
        rejection_counts.end()
    );
    std::sort(
        reject_pairs.begin(),
        reject_pairs.end(),
        [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        }
    );

    out << "Top entry rejection reasons: ";
    const size_t limit = std::min(top_n, reject_pairs.size());
    for (size_t i = 0; i < limit; ++i) {
        if (i > 0) out << ", ";
        out << reject_pairs[i].first << "=" << reject_pairs[i].second;
    }
    out << "\n";
}

namespace {

using EntryFunnel = backtest::BacktestEngine::Result::EntryFunnelSummary;
using StrategySummary = backtest::BacktestEngine::Result::StrategySummary;

void printEntryFunnelSummary(const EntryFunnel& f, std::ostream& out) {
    out << "Entry funnel: rounds=" << f.entry_rounds
        << ", no_signal=" << f.no_signal_generated
        << ", manager_drop=" << f.filtered_out_by_manager
        << ", policy_drop=" << f.filtered_out_by_policy
        << ", no_best=" << f.no_best_signal
        << ", pattern_drop=" << f.blocked_pattern_gate
        << ", rr_drop=" << f.blocked_rr_rebalance
        << ", risk_drop=" << f.blocked_risk_gate
        << " [ev=" << f.blocked_risk_gate_strategy_ev
        << " {pre_cat=" << f.blocked_risk_gate_strategy_ev_pre_catastrophic
        << ", severe=" << f.blocked_risk_gate_strategy_ev_severe_threshold
        << ", catastrophic=" << f.blocked_risk_gate_strategy_ev_catastrophic_history
        << ", asym=" << f.blocked_risk_gate_strategy_ev_loss_asymmetry
        << ", unknown=" << f.blocked_risk_gate_strategy_ev_unknown
        << ", pre_diag={obs=" << f.strategy_ev_pre_cat_observed
        << ", q_ctx=" << f.strategy_ev_pre_cat_recovery_quality_context
        << ", rec_any=" << f.strategy_ev_pre_cat_recovery_evidence_any
        << ", rec_relaxed_any=" << f.strategy_ev_pre_cat_recovery_evidence_relaxed_any
        << ", rec_relaxed_rr=" << f.strategy_ev_pre_cat_recovery_evidence_relaxed_recent_regime
        << ", rec_relaxed_full=" << f.strategy_ev_pre_cat_recovery_evidence_relaxed_full_history
        << ", rec_soften=" << f.strategy_ev_pre_cat_recovery_evidence_for_soften
        << ", rec_bridge=" << f.strategy_ev_pre_cat_recovery_evidence_bridge
        << ", rec_bridge_surr=" << f.strategy_ev_pre_cat_recovery_evidence_bridge_surrogate
        << ", rec_hyst=" << f.strategy_ev_pre_cat_recovery_evidence_hysteresis_override
        << ", q_hyst=" << f.strategy_ev_pre_cat_quality_hysteresis_override
        << ", q_relaxed_overlap=" << f.strategy_ev_pre_cat_quality_context_relaxed_overlap
        << ", q_fail_regime=" << f.strategy_ev_pre_cat_quality_fail_regime
        << ", q_fail_strength=" << f.strategy_ev_pre_cat_quality_fail_strength
        << ", q_fail_ev=" << f.strategy_ev_pre_cat_quality_fail_expected_value
        << ", q_fail_liq=" << f.strategy_ev_pre_cat_quality_fail_liquidity
        << ", soften_ready=" << f.strategy_ev_pre_cat_soften_ready
        << ", cand_qe=" << f.strategy_ev_pre_cat_soften_candidate_quality_and_evidence
        << ", cand_ns=" << f.strategy_ev_pre_cat_soften_candidate_non_severe
        << ", cand_nh=" << f.strategy_ev_pre_cat_soften_candidate_non_hostile
        << ", cand_rr=" << f.strategy_ev_pre_cat_soften_candidate_rr_ok
        << ", severe_legacy=" << f.strategy_ev_pre_cat_severe_legacy_hits
        << ", severe_composite=" << f.strategy_ev_pre_cat_severe_composite_hits
        << ", severe_cmp_cat=" << f.strategy_ev_pre_cat_severe_composite_catastrophic_hits
        << ", severe_cmp_axis=" << f.strategy_ev_pre_cat_severe_composite_pressure_axis_hits
        << ", severe_cmp_pressure_only=" << f.strategy_ev_pre_cat_severe_composite_pressure_only_hits
        << ", severe_ctx_downgrade=" << f.strategy_ev_pre_cat_contextual_severe_downgrade_hits
        << ", severe_active=" << f.strategy_ev_pre_cat_severe_active_hits
        << ", soft_ctx=" << f.strategy_ev_pre_cat_softened_contextual
        << ", soft_ovr=" << f.strategy_ev_pre_cat_softened_override
        << ", soft_no_soft_q_relief=" << f.strategy_ev_pre_cat_softened_no_soft_quality_relief
        << ", soft_rr_failsafe=" << f.strategy_ev_pre_cat_softened_candidate_rr_failsafe
        << ", soft_pressure_rebound=" << f.strategy_ev_pre_cat_softened_pressure_rebound_relief
        << ", nh_quarantine_set=" << f.strategy_ev_pre_cat_negative_history_quarantine_set
        << ", nh_quarantine_active=" << f.strategy_ev_pre_cat_negative_history_quarantine_active
        << ", blk_severe=" << f.strategy_ev_pre_cat_blocked_severe_sync
        << ", blk_other=" << f.strategy_ev_pre_cat_blocked_no_soft_path
        << "}"
        << "}"
        << ", regime=" << f.blocked_risk_gate_regime
        << ", quality=" << f.blocked_risk_gate_entry_quality
        << ", second_stage=" << f.blocked_second_stage_confirmation
        << ", near_miss_obs=" << f.second_stage_rr_margin_near_miss_observed
        << ", near_miss_soft_score=" << f.second_stage_rr_margin_soft_score_applied
        << ", near_miss_relief=" << f.second_stage_rr_margin_near_miss_relief_applied
        << ", two_head_override=" << f.two_head_aggregation_override_accept
        << ", two_head_blocked=" << f.two_head_aggregation_blocked
        << ", risk_manager_drop=" << f.blocked_risk_manager
        << ", capital_drop=" << f.blocked_min_order_or_capital
        << ", sizing_drop=" << f.blocked_order_sizing
        << ", entries=" << f.entries_executed << "\n";
}

void printStrategySummaries(const std::vector<StrategySummary>& summaries, std::ostream& out) {
    if (summaries.empty()) {
        return;
    }
    out << "전략별 요약:\n";
    for (const auto& s : summaries) {
        out << "  - " << s.strategy_name
            << " | trades=" << s.total_trades
            << " | win=" << std::fixed << std::setprecision(1) << (s.win_rate * 100.0) << "%"
            << " | pnl=" << static_cast<long long>(s.total_profit)
            << " | pf=" << std::setprecision(3) << s.profit_factor << "\n";
    }
}

}  // namespace

void printBacktestResultSummary(
    const backtest::BacktestEngine::Result& result,
    const BacktestSummaryOptions& options,
    std::ostream& out
) {
    out << "\n백테스트 결과\n";
    out << "---------------------------------------------\n";
    if (options.include_initial_capital) {
        out << "초기 자본:   " << static_cast<long long>(options.initial_capital_krw) << " KRW\n";
    }
    out << "최종 잔고:   " << static_cast<long long>(result.final_balance) << " KRW\n";
    out << "총 수익:     " << static_cast<long long>(result.total_profit) << " KRW\n";
    if (options.include_profit_rate && options.initial_capital_krw > 0.0) {
        const double profit_pct = (result.total_profit / options.initial_capital_krw) * 100.0;
        out << "수익률:      " << std::fixed << std::setprecision(2) << profit_pct << "%\n";
    }
    out << "MDD:         " << std::setprecision(3) << (result.max_drawdown * 100.0) << "%\n";
    out << "총 거래 수:  " << result.total_trades << "\n";
    out << "승리 거래:   " << result.winning_trades << "\n";
    out << "패배 거래:   " << result.losing_trades << "\n";
    out << "승률:        " << std::setprecision(2) << (result.win_rate * 100.0) << "%\n";
    out << "평균 이익:   " << static_cast<long long>(result.avg_win_krw) << " KRW\n";
    out << "평균 손실:   " << static_cast<long long>(result.avg_loss_krw) << " KRW\n";
    out << "Profit Factor: " << std::setprecision(3) << result.profit_factor << "\n";
    out << "Expectancy:  " << static_cast<long long>(result.expectancy_krw) << " KRW/trade\n";
    if (options.include_extended_metrics) {
        out << "평균 보유시간: " << std::fixed << std::setprecision(2) << result.avg_holding_minutes << " 분\n";
        out << "평균 수수료:  " << static_cast<long long>(result.avg_fee_krw) << " KRW/trade\n";
        out << "봉내 SL/TP 동시터치: " << result.intrabar_stop_tp_collision_count << "\n";
    }
    printEntryFunnelSummary(result.entry_funnel, out);
    printTopEntryRejectionReasons(result.entry_rejection_reason_counts, out);
    printStrategySummaries(result.strategy_summaries, out);
    out << "---------------------------------------------";
    if (options.include_trailing_blank_line) {
        out << "\n\n";
    } else {
        out << "\n";
    }
}

}  // namespace autolife::app
