#!/usr/bin/env python3
import argparse
import csv
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

import analyze_loss_contributors
import run_patch_action_override_feedback_promotion_check
import run_realdata_candidate_loop
import tune_candidate_gate_trade_density
from _script_common import resolve_repo_path, verification_lock


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--real-data-loop-script", "-RealDataLoopScript", default="./scripts/run_realdata_candidate_loop.py")
    parser.add_argument("--tune-script", "-TuneScript", default="./scripts/tune_candidate_gate_trade_density.py")
    parser.add_argument("--loss-analysis-script", "-LossAnalysisScript", default="./scripts/analyze_loss_contributors.py")
    parser.add_argument("--build-config-path", "-BuildConfigPath", default="./build/Release/config/config.json")
    parser.add_argument("--source-config-path", "-SourceConfigPath", default="./config/config.json")
    parser.add_argument(
        "--gate-report-json",
        "-GateReportJson",
        default="./build/Release/logs/profitability_gate_report_realdata.json",
    )
    parser.add_argument(
        "--tune-summary-json",
        "-TuneSummaryJson",
        default="./build/Release/logs/candidate_trade_density_tuning_summary.json",
    )
    parser.add_argument(
        "--iteration-csv",
        "-IterationCsv",
        default="./build/Release/logs/candidate_auto_improvement_iterations.csv",
    )
    parser.add_argument(
        "--summary-json",
        "-SummaryJson",
        default="./build/Release/logs/candidate_auto_improvement_summary.json",
    )
    parser.add_argument(
        "--patch-plan-json",
        "-PatchPlanJson",
        default="./build/Release/logs/candidate_auto_improvement_patch_plan.json",
    )
    parser.add_argument(
        "--patch-action-policy-json",
        "-PatchActionPolicyJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_decision.json",
    )
    parser.add_argument(
        "--patch-action-policy-registry-json",
        "-PatchActionPolicyRegistryJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_registry.json",
    )
    parser.add_argument(
        "--patch-action-policy-registry-feedback-json",
        "-PatchActionPolicyRegistryFeedbackJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_registry_feedback.json",
    )
    parser.add_argument(
        "--patch-action-policy-guard-report-json",
        "-PatchActionPolicyGuardReportJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_registry_guard_report.json",
    )
    parser.add_argument(
        "--patch-action-feedback-promotion-check-json",
        "-PatchActionFeedbackPromotionCheckJson",
        default="./build/Release/logs/candidate_patch_action_override_feedback_promotion_check.json",
    )
    parser.add_argument(
        "--patch-action-feedback-promotion-check-required-keep-streak",
        "-PatchActionFeedbackPromotionCheckRequiredKeepStreak",
        type=int,
        default=2,
    )
    parser.add_argument(
        "--run-patch-action-feedback-promotion-check",
        "-RunPatchActionFeedbackPromotionCheck",
        dest="run_patch_action_feedback_promotion_check",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-run-patch-action-feedback-promotion-check",
        dest="run_patch_action_feedback_promotion_check",
        action="store_false",
    )
    parser.add_argument(
        "--patch-action-policy-min-repeat-runs",
        "-PatchActionPolicyMinRepeatRuns",
        type=int,
        default=1,
    )
    parser.add_argument(
        "--patch-action-policy-max-age-hours",
        "-PatchActionPolicyMaxAgeHours",
        type=float,
        default=72.0,
    )
    parser.add_argument(
        "--enable-patch-action-policy-registry-guards",
        "-EnablePatchActionPolicyRegistryGuards",
        dest="enable_patch_action_policy_registry_guards",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-patch-action-policy-registry-guards",
        dest="enable_patch_action_policy_registry_guards",
        action="store_false",
    )
    parser.add_argument(
        "--consume-patch-action-policy-registry-feedback",
        "-ConsumePatchActionPolicyRegistryFeedback",
        dest="consume_patch_action_policy_registry_feedback",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-consume-patch-action-policy-registry-feedback",
        dest="consume_patch_action_policy_registry_feedback",
        action="store_false",
    )
    parser.add_argument(
        "--entry-rejection-summary-json",
        "-EntryRejectionSummaryJson",
        default="./build/Release/logs/entry_rejection_summary_realdata.json",
    )
    parser.add_argument(
        "--strategy-rejection-taxonomy-json",
        "-StrategyRejectionTaxonomyJson",
        default="./build/Release/logs/strategy_rejection_taxonomy_report.json",
    )
    parser.add_argument(
        "--live-signal-funnel-taxonomy-json",
        "-LiveSignalFunnelTaxonomyJson",
        default="./build/Release/logs/live_signal_funnel_taxonomy_report.json",
    )
    parser.add_argument("--max-iterations", "-MaxIterations", type=int, default=4)
    parser.add_argument("--max-consecutive-no-improvement", "-MaxConsecutiveNoImprovement", type=int, default=2)
    parser.add_argument("--max-runtime-minutes", "-MaxRuntimeMinutes", type=int, default=120)
    parser.add_argument("--min-profit-factor", "-MinProfitFactor", type=float, default=1.00)
    parser.add_argument("--min-expectancy-krw", "-MinExpectancyKrw", type=float, default=0.0)
    parser.add_argument("--min-profitable-ratio", "-MinProfitableRatio", type=float, default=0.55)
    parser.add_argument("--min-avg-win-rate-pct", "-MinAvgWinRatePct", type=float, default=48.0)
    parser.add_argument("--min-avg-trades", "-MinAvgTrades", type=float, default=8.0)
    parser.add_argument("--improvement-epsilon", "-ImprovementEpsilon", type=float, default=0.05)
    parser.add_argument("--tune-scenario-mode", "-TuneScenarioMode", default="quality_focus")
    parser.add_argument(
        "--enable-persist-directional-pivot",
        "-EnablePersistDirectionalPivot",
        dest="enable_persist_directional_pivot",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-persist-directional-pivot",
        dest="enable_persist_directional_pivot",
        action="store_false",
    )
    parser.add_argument(
        "--persist-entry-timing-scenario-mode",
        "-PersistEntryTimingScenarioMode",
        default="diverse_wide",
    )
    parser.add_argument(
        "--persist-exit-risk-scenario-mode",
        "-PersistExitRiskScenarioMode",
        default="quality_focus",
    )
    parser.add_argument("--tune-max-scenarios", "-TuneMaxScenarios", type=int, default=16)
    parser.add_argument("--tune-include-legacy-scenarios", "-TuneIncludeLegacyScenarios", action="store_true")
    parser.add_argument(
        "--tune-enable-hint-impact-guardrail",
        "-TuneEnableHintImpactGuardrail",
        dest="tune_enable_hint_impact_guardrail",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-disable-hint-impact-guardrail",
        dest="tune_enable_hint_impact_guardrail",
        action="store_false",
    )
    parser.add_argument("--tune-hint-impact-guardrail-ratio", "-TuneHintImpactGuardrailRatio", type=float, default=0.65)
    parser.add_argument(
        "--tune-hint-impact-guardrail-tighten-scale",
        "-TuneHintImpactGuardrailTightenScale",
        type=float,
        default=0.55,
    )
    parser.add_argument("--hint-overfit-ratio-threshold", "-HintOverfitRatioThreshold", type=float, default=0.60)
    parser.add_argument(
        "--hint-overfit-force-guardrail-tighten-scale",
        "-HintOverfitForceGuardrailTightenScale",
        type=float,
        default=0.80,
    )
    parser.add_argument(
        "--enable-hint-overfit-quality-pivot",
        "-EnableHintOverfitQualityPivot",
        dest="enable_hint_overfit_quality_pivot",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-hint-overfit-quality-pivot",
        dest="enable_hint_overfit_quality_pivot",
        action="store_false",
    )
    parser.add_argument(
        "--enable-holdout-suppression-persist-stop",
        "-EnableHoldoutSuppressionPersistStop",
        dest="enable_holdout_suppression_persist_stop",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-holdout-suppression-persist-stop",
        dest="enable_holdout_suppression_persist_stop",
        action="store_false",
    )
    parser.add_argument(
        "--holdout-suppression-persist-iterations",
        "-HoldoutSuppressionPersistIterations",
        type=int,
        default=2,
    )
    parser.add_argument(
        "--holdout-suppression-persist-require-both-pf-exp-fail",
        "-HoldoutSuppressionPersistRequireBothPfExpFail",
        dest="holdout_suppression_persist_require_both_pf_exp_fail",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--holdout-suppression-persist-allow-either-pf-or-exp-fail",
        dest="holdout_suppression_persist_require_both_pf_exp_fail",
        action="store_false",
    )
    parser.add_argument(
        "--tune-enable-holdout-failure-family-suppression",
        "-TuneEnableHoldoutFailureFamilySuppression",
        dest="tune_enable_holdout_failure_family_suppression",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-disable-holdout-failure-family-suppression",
        dest="tune_enable_holdout_failure_family_suppression",
        action="store_false",
    )
    parser.add_argument(
        "--tune-holdout-suppression-hint-ratio-threshold",
        "-TuneHoldoutSuppressionHintRatioThreshold",
        type=float,
        default=0.60,
    )
    parser.add_argument(
        "--tune-holdout-suppression-require-both-pf-exp-fail",
        "-TuneHoldoutSuppressionRequireBothPfExpFail",
        dest="tune_holdout_suppression_require_both_pf_exp_fail",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-holdout-suppression-allow-either-pf-or-exp-fail",
        dest="tune_holdout_suppression_require_both_pf_exp_fail",
        action="store_false",
    )
    parser.add_argument(
        "--tune-enable-post-suppression-quality-expansion",
        "-TuneEnablePostSuppressionQualityExpansion",
        dest="tune_enable_post_suppression_quality_expansion",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-disable-post-suppression-quality-expansion",
        dest="tune_enable_post_suppression_quality_expansion",
        action="store_false",
    )
    parser.add_argument(
        "--tune-post-suppression-min-combo-count",
        "-TunePostSuppressionMinComboCount",
        type=int,
        default=3,
    )
    parser.add_argument("--tune-screen-dataset-limit", "-TuneScreenDatasetLimit", type=int, default=8)
    parser.add_argument("--tune-screen-top-k", "-TuneScreenTopK", type=int, default=6)
    parser.add_argument("--tune-objective-min-avg-trades", "-TuneObjectiveMinAvgTrades", type=float, default=None)
    parser.add_argument("--tune-objective-min-profitable-ratio", "-TuneObjectiveMinProfitableRatio", type=float, default=None)
    parser.add_argument("--tune-objective-min-avg-win-rate-pct", "-TuneObjectiveMinAvgWinRatePct", type=float, default=None)
    parser.add_argument("--tune-objective-min-expectancy-krw", "-TuneObjectiveMinExpectancyKrw", type=float, default=None)
    parser.add_argument(
        "--tune-objective-mode",
        "-TuneObjectiveMode",
        choices=["balanced", "profitable_ratio_priority"],
        default="balanced",
    )
    parser.add_argument(
        "--tune-selector-mode",
        "-TuneSelectorMode",
        choices=["objective", "pareto_objective"],
        default="pareto_objective",
    )
    parser.add_argument(
        "--tune-selector-enable-two-stage-gate",
        "-TuneSelectorEnableTwoStageGate",
        dest="tune_selector_enable_two_stage_gate",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-disable-selector-enable-two-stage-gate",
        dest="tune_selector_enable_two_stage_gate",
        action="store_false",
    )
    parser.add_argument(
        "--tune-selector-two-stage-pre-min-avg-trades",
        "-TuneSelectorTwoStagePreMinAvgTrades",
        type=float,
        default=8.0,
    )
    parser.add_argument(
        "--tune-selector-two-stage-pre-min-win-rate-pct",
        "-TuneSelectorTwoStagePreMinWinRatePct",
        type=float,
        default=39.0,
    )
    parser.add_argument(
        "--tune-selector-two-stage-pre-max-market-loss-top-share-pct",
        "-TuneSelectorTwoStagePreMaxMarketLossTopSharePct",
        type=float,
        default=33.0,
    )
    parser.add_argument(
        "--tune-selector-two-stage-pre-max-market-loss-hhi",
        "-TuneSelectorTwoStagePreMaxMarketLossHhi",
        type=float,
        default=0.19,
    )
    parser.add_argument(
        "--tune-selector-two-stage-pre-require-gate-trades-pass",
        "-TuneSelectorTwoStagePreRequireGateTradesPass",
        dest="tune_selector_two_stage_pre_require_gate_trades_pass",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-selector-two-stage-pre-allow-gate-trades-fail",
        dest="tune_selector_two_stage_pre_require_gate_trades_pass",
        action="store_false",
    )
    parser.add_argument(
        "--tune-selector-two-stage-profit-min-profit-factor",
        "-TuneSelectorTwoStageProfitMinProfitFactor",
        type=float,
        default=0.41,
    )
    parser.add_argument(
        "--tune-selector-two-stage-profit-min-expectancy-krw",
        "-TuneSelectorTwoStageProfitMinExpectancyKrw",
        type=float,
        default=-10.0,
    )
    parser.add_argument(
        "--tune-selector-two-stage-profit-min-win-rate-pct",
        "-TuneSelectorTwoStageProfitMinWinRatePct",
        type=float,
        default=40.0,
    )
    parser.add_argument(
        "--tune-selector-two-stage-allow-pre-gate-fallback",
        "-TuneSelectorTwoStageAllowPreGateFallback",
        dest="tune_selector_two_stage_allow_pre_gate_fallback",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-selector-two-stage-disable-pre-gate-fallback",
        dest="tune_selector_two_stage_allow_pre_gate_fallback",
        action="store_false",
    )
    parser.add_argument(
        "--tune-selector-enable-veto-ensemble",
        "-TuneSelectorEnableVetoEnsemble",
        dest="tune_selector_enable_veto_ensemble",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-disable-selector-enable-veto-ensemble",
        dest="tune_selector_enable_veto_ensemble",
        action="store_false",
    )
    parser.add_argument(
        "--tune-selector-veto-max-market-loss-top-share-pct",
        "-TuneSelectorVetoMaxMarketLossTopSharePct",
        type=float,
        default=33.0,
    )
    parser.add_argument(
        "--tune-selector-veto-max-market-loss-hhi",
        "-TuneSelectorVetoMaxMarketLossHhi",
        type=float,
        default=0.19,
    )
    parser.add_argument(
        "--tune-selector-veto-max-avg-total-trades",
        "-TuneSelectorVetoMaxAvgTotalTrades",
        type=float,
        default=150.0,
    )
    parser.add_argument(
        "--tune-enable-selector-baseline-readiness-veto",
        "-TuneEnableSelectorBaselineReadinessVeto",
        dest="tune_enable_selector_baseline_readiness_veto",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-disable-selector-baseline-readiness-veto",
        dest="tune_enable_selector_baseline_readiness_veto",
        action="store_false",
    )
    parser.add_argument(
        "--tune-selector-baseline-readiness-veto-fail-closed",
        "-TuneSelectorBaselineReadinessVetoFailClosed",
        dest="tune_selector_baseline_readiness_veto_fail_closed",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--tune-selector-baseline-readiness-veto-fail-open",
        dest="tune_selector_baseline_readiness_veto_fail_closed",
        action="store_false",
    )
    parser.add_argument(
        "--tune-baseline-readiness-max-drawdown-delta-pct",
        "-TuneBaselineReadinessMaxDrawdownDeltaPct",
        type=float,
        default=0.30,
    )
    parser.add_argument(
        "--tune-baseline-readiness-expectancy-tolerance-krw",
        "-TuneBaselineReadinessExpectancyToleranceKrw",
        type=float,
        default=0.0,
    )
    parser.add_argument("--real-data-only", "-RealDataOnly", action="store_true")
    parser.add_argument(
        "--require-higher-tf-companions",
        "-RequireHigherTfCompanions",
        dest="require_higher_tf_companions",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--allow-missing-higher-tf-companions",
        dest="require_higher_tf_companions",
        action="store_false",
    )
    parser.add_argument("--fetch-each-iteration", "-FetchEachIteration", action="store_true")
    parser.add_argument("--skip-tune-phase", "-SkipTunePhase", action="store_true")
    parser.add_argument("--run-loss-analysis", "-RunLossAnalysis", action="store_true")
    parser.add_argument("--emit-strict-adaptive-pair", "-EmitStrictAdaptivePair", action="store_true")
    parser.add_argument(
        "--enable-adaptive-state-io",
        "-EnableAdaptiveStateIo",
        action="store_true",
        help="Enable adaptive strategy state I/O during matrix backtests inside loop.",
    )
    parser.add_argument("--sync-source-config", "-SyncSourceConfig", action="store_true")
    parser.add_argument(
        "--matrix-max-workers",
        "-MatrixMaxWorkers",
        type=int,
        default=1,
        help="Deprecated. Validation is forced to sequential execution.",
    )
    parser.add_argument("--matrix-backtest-retry-count", "-MatrixBacktestRetryCount", type=int, default=2)
    parser.add_argument(
        "--run-parity-invariant",
        "-RunParityInvariant",
        action="store_true",
        default=False,
        help="Run parity invariant report in nested realdata loop calls (default: off for speed).",
    )
    parser.add_argument("--fail-on-parity-invariant", "-FailOnParityInvariant", action="store_true")
    parser.add_argument(
        "--skip-core-vs-legacy-gate",
        "-SkipCoreVsLegacyGate",
        action="store_true",
        help="Skip legacy comparison gate in matrix/tuning runs (migration mode).",
    )
    parser.add_argument("--verification-lock-path", "-VerificationLockPath", default="./build/Release/logs/verification_run.lock")
    parser.add_argument("--verification-lock-timeout-sec", "-VerificationLockTimeoutSec", type=int, default=1800)
    parser.add_argument("--verification-lock-stale-sec", "-VerificationLockStaleSec", type=int, default=14400)
    parser.add_argument(
        "--enable-hostility-adaptive-targets",
        "-EnableHostilityAdaptiveTargets",
        dest="enable_hostility_adaptive_targets",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-hostility-adaptive-targets",
        dest="enable_hostility_adaptive_targets",
        action="store_false",
    )
    parser.add_argument(
        "--consume-patch-plan-handoff",
        "-ConsumePatchPlanHandoff",
        dest="consume_patch_plan_handoff",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-consume-patch-plan-handoff",
        dest="consume_patch_plan_handoff",
        action="store_false",
    )
    parser.add_argument(
        "--consume-patch-action-policy",
        "-ConsumePatchActionPolicy",
        dest="consume_patch_action_policy",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-consume-patch-action-policy",
        dest="consume_patch_action_policy",
        action="store_false",
    )
    parser.add_argument(
        "--enable-patch-plan-action-overrides",
        "-EnablePatchPlanActionOverrides",
        dest="enable_patch_plan_action_overrides",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-patch-plan-action-overrides",
        dest="enable_patch_plan_action_overrides",
        action="store_false",
    )
    return parser.parse_args(argv)


def _clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def compute_objective_score(
    avg_profit_factor: float,
    avg_expectancy_krw: float,
    profitable_ratio: float,
    avg_win_rate_pct: float,
    avg_total_trades: float,
    min_trades_floor: float,
    min_profitable_ratio: float,
    min_avg_win_rate_pct: float,
    min_expectancy_krw: float,
) -> float:
    penalty = 0.0
    if avg_total_trades < min_trades_floor:
        penalty += 6000.0 + ((min_trades_floor - avg_total_trades) * 800.0)
    if profitable_ratio < min_profitable_ratio:
        penalty += 6000.0 + ((min_profitable_ratio - profitable_ratio) * 9000.0)
    if avg_win_rate_pct < min_avg_win_rate_pct:
        penalty += 4000.0 + ((min_avg_win_rate_pct - avg_win_rate_pct) * 180.0)
    if avg_expectancy_krw < min_expectancy_krw:
        penalty += 6000.0 + ((min_expectancy_krw - avg_expectancy_krw) * 120.0)
    if avg_profit_factor < 1.0:
        penalty += (1.0 - avg_profit_factor) * 2500.0

    if penalty > 0.0:
        return round(-penalty + (avg_profit_factor * 10.0), 6)

    score = 0.0
    score += (avg_expectancy_krw * 25.0)
    score += (profitable_ratio * 4000.0)
    score += (avg_win_rate_pct * 40.0)
    score += ((avg_profit_factor - 1.0) * 300.0)
    score += (min(avg_total_trades, 30.0) * 40.0)
    return round(score, 6)


def target_satisfied(
    core_summary: Optional[Dict[str, Any]],
    min_pf: float,
    min_exp: float,
    min_ratio: float,
    min_trades: float,
    min_win_rate_pct: float,
) -> bool:
    if core_summary is None:
        return False
    return (
        float(core_summary.get("avg_profit_factor", 0.0)) >= min_pf
        and float(core_summary.get("avg_expectancy_krw", 0.0)) >= min_exp
        and float(core_summary.get("profitable_ratio", 0.0)) >= min_ratio
        and float(core_summary.get("avg_total_trades", 0.0)) >= min_trades
        and float(core_summary.get("avg_win_rate_pct", 0.0)) >= min_win_rate_pct
    )


def pf_expectancy_quality_fail(
    core_summary: Optional[Dict[str, Any]],
    active_thresholds: Optional[Dict[str, Any]],
    require_both: bool,
) -> Dict[str, Any]:
    pf_value = float((core_summary or {}).get("avg_profit_factor", 0.0) or 0.0)
    exp_value = float((core_summary or {}).get("avg_expectancy_krw", 0.0) or 0.0)
    pf_floor = float((active_thresholds or {}).get("min_profit_factor", 1.0) or 1.0)
    exp_floor = float((active_thresholds or {}).get("min_expectancy_krw", 0.0) or 0.0)
    pf_fail = pf_value < pf_floor
    exp_fail = exp_value < exp_floor
    return {
        "pf_fail": bool(pf_fail),
        "expectancy_fail": bool(exp_fail),
        "quality_fail": bool((pf_fail and exp_fail) if require_both else (pf_fail or exp_fail)),
        "avg_profit_factor": float(pf_value),
        "avg_expectancy_krw": float(exp_value),
        "min_profit_factor": float(pf_floor),
        "min_expectancy_krw": float(exp_floor),
    }


def resolve_active_thresholds(
    report: Dict[str, Any],
    requested: Dict[str, float],
    use_effective: bool,
) -> Dict[str, Any]:
    thresholds = report.get("thresholds") or {}
    hostility_bundle = thresholds.get("hostility_adaptive") or {}
    effective = hostility_bundle.get("effective") or {}
    active = dict(requested)
    if use_effective and effective:
        # Effective thresholds may relax in hostile datasets. Keep requested
        # targets as hard floors for acceptance/objective checks.
        active["min_profit_factor"] = max(
            float(active["min_profit_factor"]),
            float(effective.get("min_profit_factor", active["min_profit_factor"])),
        )
        active["min_expectancy_krw"] = max(
            float(active["min_expectancy_krw"]),
            float(effective.get("min_expectancy_krw", active["min_expectancy_krw"])),
        )
        active["min_profitable_ratio"] = max(
            float(active["min_profitable_ratio"]),
            float(effective.get("min_profitable_ratio", active["min_profitable_ratio"])),
        )
        active["min_avg_win_rate_pct"] = max(
            float(active["min_avg_win_rate_pct"]),
            float(effective.get("min_avg_win_rate_pct", active["min_avg_win_rate_pct"])),
        )
        # In hostile regimes, allow lower trade-count requirement.
        active["min_avg_trades"] = min(
            float(active["min_avg_trades"]),
            float(effective.get("min_avg_trades", active["min_avg_trades"])),
        )
    return {
        "requested": requested,
        "effective": effective,
        "active": active,
        "hostility": hostility_bundle.get("hostility") or {},
        "quality": hostility_bundle.get("quality") or {},
        "blended_context": hostility_bundle.get("blended_context") or {},
        "adaptive_applied": bool(use_effective and bool(effective)),
    }


def get_core_snapshot(
    report_path,
    min_profit_factor: float,
    min_trades_floor: float,
    min_profitable_ratio: float,
    min_avg_win_rate_pct: float,
    min_expectancy_krw: float,
    use_effective_targets: bool,
) -> Dict[str, Any]:
    report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    core = next((x for x in report.get("profile_summaries", []) if x.get("profile_id") == "core_full"), None)
    if core is None:
        raise RuntimeError(f"core_full summary not found: {report_path}")
    threshold_ctx = resolve_active_thresholds(
        report=report,
        requested={
            "min_profit_factor": float(min_profit_factor),
            "min_expectancy_krw": float(min_expectancy_krw),
            "min_profitable_ratio": float(min_profitable_ratio),
            "min_avg_win_rate_pct": float(min_avg_win_rate_pct),
            "min_avg_trades": float(min_trades_floor),
        },
        use_effective=bool(use_effective_targets),
    )
    active = threshold_ctx["active"]
    objective = compute_objective_score(
        float(core.get("avg_profit_factor", 0.0)),
        float(core.get("avg_expectancy_krw", 0.0)),
        float(core.get("profitable_ratio", 0.0)),
        float(core.get("avg_win_rate_pct", 0.0)),
        float(core.get("avg_total_trades", 0.0)),
        float(active["min_avg_trades"]),
        float(active["min_profitable_ratio"]),
        float(active["min_avg_win_rate_pct"]),
        float(active["min_expectancy_krw"]),
    )
    return {
        "report": report,
        "core": core,
        "objective_score": objective,
        "threshold_context": threshold_ctx,
        "active_thresholds": active,
        "overall_gate_pass": bool(report.get("overall_gate_pass", False)),
        "core_vs_legacy_gate_pass": bool((report.get("core_vs_legacy") or {}).get("gate_pass", False)),
    }


def read_entry_rejection_snapshot(path_value: Path, profile_id: str = "core_full") -> Dict[str, Any]:
    if not path_value.exists():
        return {
            "exists": False,
            "summary_json": str(path_value),
            "overall_top_reason": "",
            "overall_top_count": 0,
            "profile_top_reason": "",
            "profile_top_count": 0,
        }
    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    overall_top = payload.get("overall_top_reasons") or []
    overall_top_reason = ""
    overall_top_count = 0
    if overall_top and isinstance(overall_top[0], dict):
        overall_top_reason = str(overall_top[0].get("reason", ""))
        overall_top_count = int(overall_top[0].get("count", 0) or 0)

    profile_top_reason = ""
    profile_top_count = 0
    profile_top = (payload.get("profile_top_reasons") or {}).get(profile_id) or []
    if profile_top and isinstance(profile_top[0], dict):
        profile_top_reason = str(profile_top[0].get("reason", ""))
        profile_top_count = int(profile_top[0].get("count", 0) or 0)

    return {
        "exists": True,
        "summary_json": str(path_value),
        "overall_top_reason": overall_top_reason,
        "overall_top_count": overall_top_count,
        "profile_id": profile_id,
        "profile_top_reason": profile_top_reason,
        "profile_top_count": profile_top_count,
    }


def read_strategy_rejection_taxonomy_snapshot(path_value: Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {
            "exists": False,
            "taxonomy_json": str(path_value),
            "overall_top_group": "",
            "overall_top_group_count": 0,
            "taxonomy_coverage_ratio": 0.0,
            "unknown_reason_code_count": 0,
        }
    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    group_counts = payload.get("group_counts") or {}
    top_group = ""
    top_group_count = 0
    if isinstance(group_counts, dict) and group_counts:
        group_items = sorted(
            ((str(k), int(v)) for k, v in group_counts.items()),
            key=lambda kv: (-kv[1], kv[0]),
        )
        if group_items:
            top_group = group_items[0][0]
            top_group_count = int(group_items[0][1])
    return {
        "exists": True,
        "taxonomy_json": str(path_value),
        "overall_top_group": top_group,
        "overall_top_group_count": top_group_count,
        "taxonomy_coverage_ratio": float(payload.get("taxonomy_coverage_ratio", 0.0) or 0.0),
        "unknown_reason_code_count": int(len(payload.get("unknown_reason_codes") or [])),
    }


def read_live_signal_funnel_snapshot(path_value: Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {
            "exists": False,
            "funnel_json": str(path_value),
            "scan_count": 0,
            "top_group": "",
            "top_group_count": 0,
            "total_rejections": 0,
            "signal_generation_share": 0.0,
            "manager_prefilter_share": 0.0,
            "position_state_share": 0.0,
            "no_trade_bias_active": False,
            "recommended_trade_floor_scale": 1.0,
            "selection_call_count": 0,
            "selection_scored_candidate_count": 0,
            "selection_hint_adjusted_candidate_count": 0,
            "selection_hint_adjusted_ratio": 0.0,
        }

    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    group_counts_raw = payload.get("rejection_group_counts") or {}
    group_counts: Dict[str, int] = {}
    if isinstance(group_counts_raw, dict):
        for key, value in group_counts_raw.items():
            try:
                group_counts[str(key)] = int(value or 0)
            except (TypeError, ValueError):
                continue
    total_rejections = int(sum(max(0, v) for v in group_counts.values()))

    top_group = ""
    top_group_count = 0
    if group_counts:
        sorted_groups = sorted(group_counts.items(), key=lambda kv: (-kv[1], kv[0]))
        top_group = str(sorted_groups[0][0])
        top_group_count = int(sorted_groups[0][1])

    def _share(name: str) -> float:
        if total_rejections <= 0:
            return 0.0
        return float(max(0, group_counts.get(name, 0))) / float(total_rejections)

    signal_generation_share = _share("signal_generation")
    manager_prefilter_share = _share("manager_prefilter")
    position_state_share = _share("position_state")
    scan_count = int(payload.get("scan_count", 0) or 0)

    # If "no signal generated" + manager prefilter dominate in enough scans,
    # avoid forcing trade-density optimization pressure.
    no_trade_bias_active = bool(
        scan_count >= 3
        and total_rejections >= 20
        and position_state_share < 0.40
        and (
            signal_generation_share >= 0.55
            or (signal_generation_share + manager_prefilter_share) >= 0.75
        )
    )

    recommended_trade_floor_scale = 0.75 if no_trade_bias_active else 1.0

    return {
        "exists": True,
        "funnel_json": str(path_value),
        "scan_count": scan_count,
        "top_group": top_group,
        "top_group_count": top_group_count,
        "total_rejections": total_rejections,
        "signal_generation_share": float(signal_generation_share),
        "manager_prefilter_share": float(manager_prefilter_share),
        "position_state_share": float(position_state_share),
        "no_trade_bias_active": bool(no_trade_bias_active),
        "recommended_trade_floor_scale": float(recommended_trade_floor_scale),
        "selection_call_count": int(payload.get("selection_call_count", 0) or 0),
        "selection_scored_candidate_count": int(payload.get("selection_scored_candidate_count", 0) or 0),
        "selection_hint_adjusted_candidate_count": int(
            payload.get("selection_hint_adjusted_candidate_count", 0) or 0
        ),
        "selection_hint_adjusted_ratio": float(payload.get("selection_hint_adjusted_ratio", 0.0) or 0.0),
    }


def read_tune_holdout_suppression_snapshot(path_value: Path) -> Dict[str, Any]:
    default_snapshot = {
        "exists": False,
        "summary_json": str(path_value),
        "enabled": False,
        "active": False,
        "reason": "",
        "live_hint_adjusted_ratio": 0.0,
        "hint_ratio_threshold": 0.0,
        "require_both_pf_exp_fail": True,
        "suppressed_families": [],
        "suppressed_combo_count": 0,
        "kept_combo_count": 0,
        "fallback_retained_combo_id": "",
        "fail_open_all_suppressed": False,
    }
    if not path_value.exists():
        return default_snapshot
    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    suppression = (
        ((payload.get("bottleneck_priority") or {}).get("holdout_failure_suppression") or {})
        if isinstance(payload, dict)
        else {}
    )
    if not isinstance(suppression, dict):
        return default_snapshot

    suppressed_families = suppression.get("suppressed_families") or []
    if not isinstance(suppressed_families, list):
        suppressed_families = []

    return {
        "exists": True,
        "summary_json": str(path_value),
        "enabled": bool(suppression.get("enabled", False)),
        "active": bool(suppression.get("active", False)),
        "reason": str(suppression.get("reason", "")),
        "live_hint_adjusted_ratio": float(suppression.get("live_hint_adjusted_ratio", 0.0) or 0.0),
        "hint_ratio_threshold": float(suppression.get("hint_ratio_threshold", 0.0) or 0.0),
        "require_both_pf_exp_fail": bool(suppression.get("require_both_pf_exp_fail", True)),
        "suppressed_families": [str(x) for x in suppressed_families if str(x)],
        "suppressed_combo_count": int(suppression.get("suppressed_combo_count", 0) or 0),
        "kept_combo_count": int(suppression.get("kept_combo_count", 0) or 0),
        "fallback_retained_combo_id": str(suppression.get("fallback_retained_combo_id", "")),
        "fail_open_all_suppressed": bool(suppression.get("fail_open_all_suppressed", False)),
    }


def read_tune_post_suppression_expansion_snapshot(path_value: Path) -> Dict[str, Any]:
    default_snapshot = {
        "exists": False,
        "summary_json": str(path_value),
        "enabled": False,
        "applied": False,
        "reason": "",
        "target_min_combo_count": 0,
        "input_combo_count": 0,
        "output_combo_count": 0,
        "injected_combo_count": 0,
        "injected_combo_ids": [],
        "context_top_group": "",
        "context_top_group_source": "",
    }
    if not path_value.exists():
        return default_snapshot
    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    bottleneck = (payload.get("bottleneck_priority") or {}) if isinstance(payload, dict) else {}
    if not isinstance(bottleneck, dict):
        return default_snapshot
    expansion = bottleneck.get("post_suppression_quality_expansion") or {}
    if not isinstance(expansion, dict):
        return default_snapshot
    context = bottleneck.get("context") or {}
    if not isinstance(context, dict):
        context = {}
    injected_ids = expansion.get("injected_combo_ids") or []
    if not isinstance(injected_ids, list):
        injected_ids = []
    return {
        "exists": True,
        "summary_json": str(path_value),
        "enabled": bool(expansion.get("enabled", False)),
        "applied": bool(expansion.get("applied", False)),
        "reason": str(expansion.get("reason", "")),
        "target_min_combo_count": int(expansion.get("target_min_combo_count", 0) or 0),
        "input_combo_count": int(expansion.get("input_combo_count", 0) or 0),
        "output_combo_count": int(expansion.get("output_combo_count", 0) or 0),
        "injected_combo_count": int(expansion.get("injected_combo_count", 0) or 0),
        "injected_combo_ids": [str(x) for x in injected_ids if str(x)],
        "context_top_group": str(context.get("top_group", "") or ""),
        "context_top_group_source": str(context.get("top_group_source", "") or ""),
    }


def read_patch_plan_handoff(path_value: Path) -> Dict[str, Any]:
    snapshot = {
        "exists": bool(path_value.exists()),
        "path": str(path_value),
        "usable": False,
        "status": "",
        "direction": "",
        "reason_code": "",
        "next_scenario_mode": "",
        "template_id": "",
        "template_focus": "",
        "template_action_count": 0,
        "actions": [],
    }
    if not path_value.exists():
        return snapshot
    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    if not isinstance(payload, dict):
        return snapshot
    pivot = payload.get("pivot") or {}
    patch_template = payload.get("patch_template") or {}
    if not isinstance(pivot, dict):
        pivot = {}
    if not isinstance(patch_template, dict):
        patch_template = {}
    checklist = patch_template.get("checklist") or []
    if not isinstance(checklist, list):
        checklist = []
    actions_raw = payload.get("actions") or []
    normalized_actions: List[str] = []
    if isinstance(actions_raw, list):
        for row in actions_raw:
            if not isinstance(row, dict):
                continue
            desc = str(row.get("description", "") or "").strip()
            if desc:
                normalized_actions.append(desc)
    if not normalized_actions:
        normalized_actions = [str(x).strip() for x in checklist if str(x).strip()]
    direction = str(pivot.get("direction", "") or "").strip()
    next_mode = str(pivot.get("next_scenario_mode", "") or "").strip()
    snapshot.update(
        {
            "usable": bool(direction and next_mode),
            "status": str(payload.get("status", "") or ""),
            "direction": direction,
            "reason_code": str(pivot.get("reason_code", "") or ""),
            "next_scenario_mode": next_mode,
            "template_id": str(patch_template.get("template_id", "") or ""),
            "template_focus": str(patch_template.get("focus", "") or ""),
            "template_action_count": int(len(normalized_actions)),
            "actions": normalized_actions,
        }
    )
    return snapshot


def read_patch_action_policy(path_value: Path) -> Dict[str, Any]:
    snapshot = {
        "exists": bool(path_value.exists()),
        "path": str(path_value),
        "source": "single",
        "template_id": "",
        "decision": "",
        "reason_code": "",
        "recommendation": "",
        "usable": False,
    }
    if not path_value.exists():
        return snapshot
    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    if not isinstance(payload, dict):
        return snapshot
    decision = str(payload.get("decision", "") or "").strip()
    reason_code = str(payload.get("reason_code", "") or "").strip()
    recommendation = str(payload.get("recommendation", "") or "").strip()
    snapshot.update(
        {
            "template_id": str(payload.get("template_id", "") or ""),
            "decision": decision,
            "reason_code": reason_code,
            "recommendation": recommendation,
            "usable": bool(decision in {"keep_override", "decrease_override_strength", "disable_override"}),
        }
    )
    return snapshot


def read_patch_action_policy_registry(path_value: Path, template_id: str) -> Dict[str, Any]:
    resolved_template = str(template_id or "").strip()
    snapshot = {
        "exists": bool(path_value.exists()),
        "path": str(path_value),
        "source": "registry",
        "template_id": resolved_template,
        "decision": "",
        "reason_code": "",
        "recommendation": "",
        "policy_updated_at": "",
        "policy_repeat_runs": 0,
        "usable": False,
        "matched_template": False,
    }
    if not path_value.exists():
        return snapshot
    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    if not isinstance(payload, dict):
        return snapshot
    template_policies = payload.get("template_policies") or {}
    if not isinstance(template_policies, dict):
        template_policies = {}
    selected = template_policies.get(resolved_template) if resolved_template else None
    if not isinstance(selected, dict):
        selected = {}
    decision = str(selected.get("decision", "") or "").strip()
    reason_code = str(selected.get("reason_code", "") or "").strip()
    recommendation = str(selected.get("recommendation", "") or "").strip()
    policy_updated_at = str(selected.get("updated_at", "") or "").strip()
    policy_repeat_runs = int(selected.get("repeat_runs", 0) or 0)
    snapshot.update(
        {
            "decision": decision,
            "reason_code": reason_code,
            "recommendation": recommendation,
            "policy_updated_at": policy_updated_at,
            "policy_repeat_runs": policy_repeat_runs,
            "usable": bool(
                resolved_template
                and decision in {"keep_override", "decrease_override_strength", "disable_override"}
            ),
            "matched_template": bool(resolved_template and bool(selected)),
        }
    )
    return snapshot


def read_patch_action_policy_registry_feedback(path_value: Path, template_id: str) -> Dict[str, Any]:
    resolved_template = str(template_id or "").strip()
    snapshot = {
        "exists": bool(path_value.exists()),
        "path": str(path_value),
        "template_id": resolved_template,
        "matched_template": False,
        "recommended_min_repeat_runs": 0,
        "allow_keep_promotion": False,
        "block_auto_loop_consumption": False,
        "feedback_reason_codes": [],
        "latest_decision": "",
        "latest_reason_code": "",
        "last_repeat_runs": 0,
    }
    if not path_value.exists():
        return snapshot
    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    if not isinstance(payload, dict):
        return snapshot
    template_feedbacks = payload.get("template_feedbacks") or {}
    if not isinstance(template_feedbacks, dict):
        template_feedbacks = {}
    selected = template_feedbacks.get(resolved_template) if resolved_template else None
    if not isinstance(selected, dict):
        selected = {}
    reason_codes = selected.get("feedback_reason_codes") or []
    if not isinstance(reason_codes, list):
        reason_codes = []
    snapshot.update(
        {
            "matched_template": bool(resolved_template and bool(selected)),
            "recommended_min_repeat_runs": int(selected.get("recommended_min_repeat_runs", 0) or 0),
            "allow_keep_promotion": bool(selected.get("allow_keep_promotion", False)),
            "block_auto_loop_consumption": bool(selected.get("block_auto_loop_consumption", False)),
            "feedback_reason_codes": [str(x) for x in reason_codes if str(x)],
            "latest_decision": str(selected.get("latest_decision", "") or ""),
            "latest_reason_code": str(selected.get("latest_reason_code", "") or ""),
            "last_repeat_runs": int(selected.get("last_repeat_runs", 0) or 0),
        }
    )
    return snapshot


def evaluate_patch_action_policy_registry_guard(
    registry_snapshot: Dict[str, Any],
    min_repeat_runs: int,
    max_age_hours: float,
    enable_guards: bool,
    feedback_snapshot: Dict[str, Any],
    consume_feedback: bool,
) -> Dict[str, Any]:
    now_utc = datetime.now(tz=timezone.utc)
    feedback_min_runs = int((feedback_snapshot or {}).get("recommended_min_repeat_runs", 0) or 0)
    feedback_block = bool((feedback_snapshot or {}).get("block_auto_loop_consumption", False))
    feedback_allow_keep = bool((feedback_snapshot or {}).get("allow_keep_promotion", False))
    effective_min_runs = int(max(1, int(min_repeat_runs)))
    if bool(consume_feedback) and bool((feedback_snapshot or {}).get("matched_template", False)):
        if feedback_min_runs > 0:
            effective_min_runs = max(effective_min_runs, int(feedback_min_runs))
    guard = {
        "generated_at": now_utc.isoformat(),
        "enabled": bool(enable_guards),
        "consume_feedback": bool(consume_feedback),
        "registry_exists": bool(registry_snapshot.get("exists", False)),
        "template_id": str(registry_snapshot.get("template_id", "") or ""),
        "matched_template": bool(registry_snapshot.get("matched_template", False)),
        "decision": str(registry_snapshot.get("decision", "") or ""),
        "usable_before_guard": bool(registry_snapshot.get("usable", False)),
        "min_repeat_runs": int(max(1, int(min_repeat_runs))),
        "effective_min_repeat_runs": int(effective_min_runs),
        "max_policy_age_hours": float(max_age_hours),
        "policy_repeat_runs": int(registry_snapshot.get("policy_repeat_runs", 0) or 0),
        "policy_updated_at": str(registry_snapshot.get("policy_updated_at", "") or ""),
        "policy_age_hours": None,
        "feedback_snapshot": dict(feedback_snapshot or {}),
        "accepted": False,
        "reason_codes": [],
    }
    reason_codes: List[str] = []

    if not bool(registry_snapshot.get("exists", False)):
        reason_codes.append("registry_missing")
    if not bool(registry_snapshot.get("matched_template", False)):
        reason_codes.append("template_not_matched")
    if not bool(registry_snapshot.get("usable", False)):
        reason_codes.append("policy_not_usable")
    if bool(consume_feedback) and bool((feedback_snapshot or {}).get("exists", False)):
        if not bool((feedback_snapshot or {}).get("matched_template", False)):
            reason_codes.append("feedback_template_not_matched")
        if feedback_block:
            reason_codes.append("feedback_block_auto_loop_consumption")
        decision = str(registry_snapshot.get("decision", "") or "")
        if decision == "keep_override" and not feedback_allow_keep:
            reason_codes.append("feedback_keep_promotion_not_allowed")

    if bool(enable_guards):
        policy_repeat_runs = int(registry_snapshot.get("policy_repeat_runs", 0) or 0)
        min_runs = int(max(1, int(effective_min_runs)))
        if policy_repeat_runs < min_runs:
            reason_codes.append("repeat_runs_below_min")

        max_hours = float(max_age_hours)
        if max_hours > 0.0:
            updated_at_raw = str(registry_snapshot.get("policy_updated_at", "") or "").strip()
            if not updated_at_raw:
                reason_codes.append("policy_updated_at_missing")
            else:
                try:
                    parsed = datetime.fromisoformat(updated_at_raw)
                    if parsed.tzinfo is None:
                        parsed = parsed.replace(tzinfo=timezone.utc)
                    age_hours = max(0.0, (now_utc - parsed.astimezone(timezone.utc)).total_seconds() / 3600.0)
                    guard["policy_age_hours"] = float(round(age_hours, 6))
                    if age_hours > max_hours:
                        reason_codes.append("policy_age_exceeded")
                except Exception:
                    reason_codes.append("policy_updated_at_invalid")

    accepted = len(reason_codes) == 0
    guard["accepted"] = bool(accepted)
    guard["reason_codes"] = list(reason_codes if reason_codes else ["accepted"])
    return guard


def _action_contains(text: str, phrases: List[str]) -> bool:
    normalized = str(text or "").strip().lower()
    return any(str(p).strip().lower() in normalized for p in phrases if str(p).strip())


def build_patch_plan_action_overrides(
    actions: List[str],
    direction_hint: str,
    template_id: str,
    baseline: Dict[str, Any],
    strength_scale: float = 1.0,
) -> Dict[str, Any]:
    # Deterministic mapping from machine-readable patch-plan actions to tune/runtime
    # overrides for the next iteration.
    scale = _clamp(float(strength_scale), 0.0, 1.0)

    def _scaled_add(value: float) -> float:
        return float(value) * scale

    def _scaled_factor(value: float) -> float:
        # Scale factor around 1.0:
        # scale=1.0 -> original value
        # scale=0.0 -> neutral(1.0)
        v = float(value)
        return 1.0 + ((v - 1.0) * scale)

    overrides: Dict[str, Any] = {
        "enabled": True,
        "applied": False,
        "template_id": str(template_id or ""),
        "direction_hint": str(direction_hint or ""),
        "strength_scale": float(scale),
        "matched_actions": [],
        "unmatched_actions": [],
        "objective_mode_override": "",
        "objective_min_profitable_ratio_delta": 0.0,
        "objective_min_avg_win_rate_pct_delta": 0.0,
        "objective_min_expectancy_krw_delta": 0.0,
        "objective_min_avg_trades_scale": 1.0,
        "gate_min_avg_trades_scale": 1.0,
        "tune_hint_guardrail_ratio_override": None,
        "tune_hint_guardrail_tighten_scale_override": None,
        "tune_enable_hint_guardrail_override": None,
        "tune_holdout_suppression_hint_ratio_threshold_override": None,
        "tune_post_suppression_quality_expansion_enabled_override": None,
        "tune_post_suppression_min_combo_count_override": None,
        "resolved": {},
    }
    action_list = [str(x).strip() for x in (actions or []) if str(x).strip()]
    if not action_list:
        return overrides

    def _set_guardrail_ratio(value: float) -> None:
        current = overrides["tune_hint_guardrail_ratio_override"]
        if current is None:
            overrides["tune_hint_guardrail_ratio_override"] = float(value)
        else:
            overrides["tune_hint_guardrail_ratio_override"] = min(float(current), float(value))

    def _set_guardrail_scale(value: float) -> None:
        current = overrides["tune_hint_guardrail_tighten_scale_override"]
        if current is None:
            overrides["tune_hint_guardrail_tighten_scale_override"] = float(value)
        else:
            overrides["tune_hint_guardrail_tighten_scale_override"] = max(float(current), float(value))

    def _set_holdout_hint_threshold(value: float) -> None:
        current = overrides["tune_holdout_suppression_hint_ratio_threshold_override"]
        if current is None:
            overrides["tune_holdout_suppression_hint_ratio_threshold_override"] = float(value)
        else:
            overrides["tune_holdout_suppression_hint_ratio_threshold_override"] = min(float(current), float(value))

    for action in action_list:
        matched = False
        if _action_contains(action, ["late-entry guard", "late entry guard", "mean distance"]):
            matched = True
            overrides["objective_min_profitable_ratio_delta"] += _scaled_add(0.01)
            overrides["objective_min_avg_win_rate_pct_delta"] += _scaled_add(0.5)
            overrides["tune_enable_hint_guardrail_override"] = True
            _set_guardrail_ratio(0.55)
            _set_guardrail_scale(0.72)

        if _action_contains(action, ["extra confirmation", "weak-momentum entries", "weak momentum entries"]):
            matched = True
            overrides["objective_min_profitable_ratio_delta"] += _scaled_add(0.015)
            overrides["objective_min_avg_win_rate_pct_delta"] += _scaled_add(1.0)
            overrides["objective_min_expectancy_krw_delta"] += _scaled_add(0.1)
            overrides["tune_enable_hint_guardrail_override"] = True
            _set_guardrail_scale(0.70)

        if _action_contains(action, ["reduce duplicate entries", "failed breakout attempt"]):
            matched = True
            overrides["objective_min_avg_trades_scale"] *= _scaled_factor(0.92)
            overrides["gate_min_avg_trades_scale"] *= _scaled_factor(0.92)
            _set_holdout_hint_threshold(0.55)

        if _action_contains(action, ["cooldown", "no-follow-through", "no follow-through"]):
            matched = True
            overrides["objective_min_avg_trades_scale"] *= _scaled_factor(0.88)
            overrides["gate_min_avg_trades_scale"] *= _scaled_factor(0.88)
            overrides["tune_enable_hint_guardrail_override"] = True
            _set_guardrail_scale(0.75)

        if _action_contains(action, ["tighten invalidation exits", "volatility expands after entry"]):
            matched = True
            overrides["objective_mode_override"] = "profitable_ratio_priority"
            overrides["objective_min_expectancy_krw_delta"] += _scaled_add(0.2)
            overrides["objective_min_profitable_ratio_delta"] += _scaled_add(0.015)
            overrides["tune_enable_hint_guardrail_override"] = True
            _set_guardrail_scale(0.75)
            overrides["tune_post_suppression_quality_expansion_enabled_override"] = True

        if _action_contains(action, ["raise minimum realized rr", "weak-signal positions", "weak signal positions"]):
            matched = True
            overrides["objective_mode_override"] = "profitable_ratio_priority"
            overrides["objective_min_expectancy_krw_delta"] += _scaled_add(0.15)
            overrides["objective_min_profitable_ratio_delta"] += _scaled_add(0.015)
            overrides["objective_min_avg_win_rate_pct_delta"] += _scaled_add(1.0)
            overrides["tune_post_suppression_quality_expansion_enabled_override"] = True

        if _action_contains(action, ["throttle position adds", "expectancy remains negative"]):
            matched = True
            overrides["objective_min_avg_trades_scale"] *= _scaled_factor(0.85)
            overrides["gate_min_avg_trades_scale"] *= _scaled_factor(0.85)
            _set_holdout_hint_threshold(0.55)
            current_min_combo = overrides["tune_post_suppression_min_combo_count_override"]
            overrides["tune_post_suppression_min_combo_count_override"] = (
                3 if current_min_combo is None else max(int(current_min_combo), 3)
            )

        if _action_contains(action, ["shorten hostile-regime hold time", "stricter trailing exits", "trailing exits"]):
            matched = True
            overrides["objective_mode_override"] = "profitable_ratio_priority"
            overrides["objective_min_profitable_ratio_delta"] += _scaled_add(0.02)
            overrides["objective_min_avg_win_rate_pct_delta"] += _scaled_add(1.0)
            overrides["objective_min_expectancy_krw_delta"] += _scaled_add(0.1)
            overrides["tune_post_suppression_quality_expansion_enabled_override"] = True
            _set_guardrail_scale(0.72)

        if matched:
            overrides["matched_actions"].append(action)
        else:
            overrides["unmatched_actions"].append(action)

    overrides["applied"] = bool(overrides["matched_actions"])
    if not overrides["applied"]:
        return overrides

    base_min_profitable_ratio = float(baseline.get("objective_min_profitable_ratio", 0.0) or 0.0)
    base_min_win_rate = float(baseline.get("objective_min_avg_win_rate_pct", 0.0) or 0.0)
    base_min_expectancy = float(baseline.get("objective_min_expectancy_krw", 0.0) or 0.0)
    base_min_trades = float(baseline.get("objective_min_avg_trades", 0.0) or 0.0)
    base_gate_min_trades = int(baseline.get("gate_min_avg_trades", 0) or 0)
    base_guardrail_ratio = float(baseline.get("tune_hint_guardrail_ratio", 0.65) or 0.65)
    base_guardrail_scale = float(baseline.get("tune_hint_guardrail_tighten_scale", 0.55) or 0.55)
    base_holdout_hint_ratio = float(
        baseline.get("tune_holdout_suppression_hint_ratio_threshold", 0.60) or 0.60
    )
    base_post_supp_combo_min = int(baseline.get("tune_post_suppression_min_combo_count", 3) or 3)
    base_objective_mode = str(baseline.get("tune_objective_mode", "balanced") or "balanced")
    base_post_supp_expansion = bool(
        baseline.get("tune_enable_post_suppression_quality_expansion", False)
    )
    base_enable_guardrail = bool(baseline.get("tune_enable_hint_guardrail", False))

    resolved_objective_min_profitable_ratio = _clamp(
        base_min_profitable_ratio + float(overrides["objective_min_profitable_ratio_delta"]),
        0.0,
        1.0,
    )
    resolved_objective_min_avg_win_rate_pct = _clamp(
        base_min_win_rate + float(overrides["objective_min_avg_win_rate_pct_delta"]),
        0.0,
        100.0,
    )
    resolved_objective_min_expectancy_krw = float(
        base_min_expectancy + float(overrides["objective_min_expectancy_krw_delta"])
    )
    resolved_objective_min_avg_trades = max(
        4.0,
        round(base_min_trades * float(overrides["objective_min_avg_trades_scale"]), 4),
    )
    resolved_gate_min_avg_trades = max(
        1,
        int(round(float(base_gate_min_trades) * float(overrides["gate_min_avg_trades_scale"]))),
    )
    resolved_tune_hint_guardrail_ratio = _clamp(
        float(
            overrides["tune_hint_guardrail_ratio_override"]
            if overrides["tune_hint_guardrail_ratio_override"] is not None
            else base_guardrail_ratio
        ),
        0.0,
        1.0,
    )
    resolved_tune_hint_guardrail_tighten_scale = _clamp(
        float(
            overrides["tune_hint_guardrail_tighten_scale_override"]
            if overrides["tune_hint_guardrail_tighten_scale_override"] is not None
            else base_guardrail_scale
        ),
        0.0,
        1.0,
    )
    resolved_tune_enable_hint_guardrail = bool(
        overrides["tune_enable_hint_guardrail_override"]
        if overrides["tune_enable_hint_guardrail_override"] is not None
        else base_enable_guardrail
    )
    resolved_tune_holdout_suppression_hint_ratio_threshold = _clamp(
        float(
            overrides["tune_holdout_suppression_hint_ratio_threshold_override"]
            if overrides["tune_holdout_suppression_hint_ratio_threshold_override"] is not None
            else base_holdout_hint_ratio
        ),
        0.0,
        1.0,
    )
    resolved_tune_enable_post_suppression_quality_expansion = bool(
        overrides["tune_post_suppression_quality_expansion_enabled_override"]
        if overrides["tune_post_suppression_quality_expansion_enabled_override"] is not None
        else base_post_supp_expansion
    )
    resolved_tune_post_suppression_min_combo_count = int(
        max(
            1,
            overrides["tune_post_suppression_min_combo_count_override"]
            if overrides["tune_post_suppression_min_combo_count_override"] is not None
            else base_post_supp_combo_min,
        )
    )
    resolved_tune_objective_mode = str(overrides["objective_mode_override"] or base_objective_mode)

    overrides["resolved"] = {
        "objective_mode": resolved_tune_objective_mode,
        "objective_min_profitable_ratio": resolved_objective_min_profitable_ratio,
        "objective_min_avg_win_rate_pct": resolved_objective_min_avg_win_rate_pct,
        "objective_min_expectancy_krw": resolved_objective_min_expectancy_krw,
        "objective_min_avg_trades": resolved_objective_min_avg_trades,
        "gate_min_avg_trades": resolved_gate_min_avg_trades,
        "tune_enable_hint_guardrail": resolved_tune_enable_hint_guardrail,
        "tune_hint_guardrail_ratio": resolved_tune_hint_guardrail_ratio,
        "tune_hint_guardrail_tighten_scale": resolved_tune_hint_guardrail_tighten_scale,
        "tune_holdout_suppression_hint_ratio_threshold": resolved_tune_holdout_suppression_hint_ratio_threshold,
        "tune_enable_post_suppression_quality_expansion": resolved_tune_enable_post_suppression_quality_expansion,
        "tune_post_suppression_min_combo_count": resolved_tune_post_suppression_min_combo_count,
    }
    return overrides


def normalize_tune_scenario_mode(value: str, fallback: str = "quality_focus") -> str:
    allowed = {"legacy_only", "diverse_light", "diverse_wide", "quality_focus"}
    v = str(value or "").strip()
    if v in allowed:
        return v
    return fallback


def build_directional_patch_template(direction: str) -> Dict[str, Any]:
    d = str(direction or "").strip()
    if d == "entry_timing":
        return {
            "template_id": "entry_timing_v1",
            "direction": "entry_timing",
            "focus": "signal quality before entry",
            "checklist": [
                "Add late-entry guard using recent candle extension/mean distance.",
                "Require one extra confirmation feature for weak-momentum entries.",
                "Reduce duplicate entries after first failed breakout attempt.",
                "Add cooldown after consecutive no-follow-through entries.",
            ],
        }
    if d == "exit_risk":
        return {
            "template_id": "exit_risk_v1",
            "direction": "exit_risk",
            "focus": "loss control and exit quality",
            "checklist": [
                "Tighten invalidation exits when volatility expands after entry.",
                "Raise minimum realized RR for weak-signal positions.",
                "Throttle position adds when expectancy remains negative.",
                "Shorten hostile-regime hold time with stricter trailing exits.",
            ],
        }
    return {
        "template_id": "none",
        "direction": "",
        "focus": "",
        "checklist": [],
    }


def build_persist_directional_pivot(
    selected_combo_family: str,
    entry_rejection_top_group: str,
    expansion_snapshot: Dict[str, Any],
    suppression_effective: bool,
    quality_fail: bool,
    current_scenario_mode: str,
    entry_timing_scenario_mode: str,
    exit_risk_scenario_mode: str,
) -> Dict[str, Any]:
    family = str(selected_combo_family or "").strip()
    top_group = str(entry_rejection_top_group or "").strip()
    expansion_applied = bool(expansion_snapshot.get("applied", False))
    if not bool(suppression_effective) or not bool(quality_fail):
        return {
            "active": False,
            "direction": "",
            "reason_code": "inactive",
            "recommendation": "",
            "next_scenario_mode": str(current_scenario_mode),
            "expansion_applied": expansion_applied,
            "patch_template": build_directional_patch_template(""),
        }

    direction = ""
    reason_code = ""
    if family == "quality_exit_rebalance" or expansion_applied:
        direction = "exit_risk"
        reason_code = "quality_exit_family_or_expansion"
    elif family in {"signal_generation_boost", "manager_prefilter_relax"}:
        direction = "entry_timing"
        reason_code = "entry_relax_family_selected"
    elif top_group in {"signal_generation", "manager_prefilter"}:
        direction = "entry_timing"
        reason_code = "entry_rejection_top_group"
    elif top_group in {"risk_gate", "position_state"}:
        direction = "exit_risk"
        reason_code = "risk_or_position_top_group"
    else:
        direction = "entry_timing"
        reason_code = "default_entry_timing_bias"

    if direction == "exit_risk":
        recommendation = (
            "exit/risk emphasis: tighten invalidation exits, volatility-aware RR, and position throttles "
            "before relaxing entry density."
        )
        next_mode = normalize_tune_scenario_mode(exit_risk_scenario_mode, "quality_focus")
    else:
        recommendation = (
            "entry timing emphasis: improve trigger confirmation and late-entry avoidance "
            "before further risk/exit tightening."
        )
        next_mode = normalize_tune_scenario_mode(entry_timing_scenario_mode, "diverse_wide")

    if normalize_tune_scenario_mode(current_scenario_mode, "quality_focus") == "legacy_only":
        next_mode = "legacy_only"

    return {
        "active": True,
        "direction": direction,
        "reason_code": reason_code,
        "recommendation": recommendation,
        "next_scenario_mode": next_mode,
        "expansion_applied": expansion_applied,
        "patch_template": build_directional_patch_template(direction),
    }


def build_machine_readable_patch_plan(
    rows: List[Dict[str, Any]],
    status: str,
    reason: str,
    last_direction: str,
    last_reason_code: str,
    last_recommendation: str,
    last_next_scenario_mode: str,
    last_patch_template: Dict[str, Any],
    holdout_policy: Dict[str, Any],
) -> Dict[str, Any]:
    context_row: Optional[Dict[str, Any]] = None
    for row in reversed(rows):
        if str(row.get("phase", "")) == "post_apply":
            context_row = row
            break
    if context_row is None and rows:
        context_row = rows[-1]

    context = {
        "iteration": int((context_row or {}).get("iteration", 0) or 0),
        "phase": str((context_row or {}).get("phase", "")),
        "selected_combo": str((context_row or {}).get("selected_combo", "")),
        "selected_combo_family": str((context_row or {}).get("selected_combo_family", "")),
        "avg_profit_factor": float((context_row or {}).get("avg_profit_factor", 0.0) or 0.0),
        "avg_expectancy_krw": float((context_row or {}).get("avg_expectancy_krw", 0.0) or 0.0),
        "avg_total_trades": float((context_row or {}).get("avg_total_trades", 0.0) or 0.0),
        "avg_win_rate_pct": float((context_row or {}).get("avg_win_rate_pct", 0.0) or 0.0),
        "profitable_ratio": float((context_row or {}).get("profitable_ratio", 0.0) or 0.0),
        "entry_rejection_top_group": str((context_row or {}).get("entry_rejection_top_group", "")),
        "live_funnel_top_group": str((context_row or {}).get("live_funnel_top_group", "")),
        "blended_hostility_level": str((context_row or {}).get("blended_hostility_level", "")),
        "quality_level": str((context_row or {}).get("quality_level", "")),
        "tune_scenario_mode_iter": str((context_row or {}).get("tune_scenario_mode_iter", "")),
        "tune_scenario_mode_source": str((context_row or {}).get("tune_scenario_mode_source", "")),
        "tune_objective_mode_iter": str((context_row or {}).get("tune_objective_mode_iter", "")),
        "patch_plan_action_override_applied": bool(
            (context_row or {}).get("patch_plan_action_override_applied", False)
        ),
        "patch_plan_action_override_template_id": str(
            (context_row or {}).get("patch_plan_action_override_template_id", "")
        ),
        "patch_plan_action_override_matched_count": int(
            (context_row or {}).get("patch_plan_action_override_matched_count", 0) or 0
        ),
    }

    template_payload = dict(last_patch_template or {})
    checklist = template_payload.get("checklist") or []
    if not isinstance(checklist, list):
        checklist = []
    actions: List[Dict[str, Any]] = []
    step = 1
    for item in checklist:
        text = str(item).strip()
        if not text:
            continue
        actions.append(
            {
                "step": int(step),
                "action_type": "code_change",
                "description": text,
            }
        )
        step += 1

    return {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "status": str(status),
        "reason": str(reason),
        "trigger": {
            "holdout_suppression_persist_triggered": bool(holdout_policy.get("triggered", False)),
            "trigger_iteration": int(holdout_policy.get("trigger_iteration", 0) or 0),
            "final_streak": int(holdout_policy.get("final_streak", 0) or 0),
        },
        "pivot": {
            "direction": str(last_direction),
            "reason_code": str(last_reason_code),
            "recommendation": str(last_recommendation),
            "next_scenario_mode": str(last_next_scenario_mode),
        },
        "patch_template": template_payload,
        "actions": actions,
        "context": context,
    }


def apply_combo_to_config_object(config_obj: Dict[str, Any], combo: Dict[str, Any]) -> None:
    trading = config_obj.setdefault("trading", {})
    for key, cast in [
        ("max_new_orders_per_scan", int),
        ("min_expected_edge_pct", float),
        ("min_reward_risk", float),
        ("min_rr_weak_signal", float),
        ("min_rr_strong_signal", float),
        ("min_strategy_trades_for_ev", int),
        ("min_strategy_expectancy_krw", float),
        ("min_strategy_profit_factor", float),
        ("avoid_high_volatility", bool),
        ("avoid_trending_down", bool),
        ("hostility_ewma_alpha", float),
        ("hostility_hostile_threshold", float),
        ("hostility_severe_threshold", float),
        ("hostility_extreme_threshold", float),
        ("hostility_pause_scans", int),
        ("hostility_pause_scans_extreme", int),
        ("hostility_pause_recent_sample_min", int),
        ("hostility_pause_recent_expectancy_krw", float),
        ("hostility_pause_recent_win_rate", float),
        ("backtest_hostility_pause_candles", int),
        ("backtest_hostility_pause_candles_extreme", int),
        ("enable_entry_quality_adaptive_relief", bool),
        ("entry_quality_adaptive_relief_rr_max_gap", float),
        ("entry_quality_adaptive_relief_edge_max_gap", float),
        ("entry_quality_adaptive_relief_min_signal_strength", float),
        ("entry_quality_adaptive_relief_min_expected_value", float),
        ("entry_quality_adaptive_relief_min_liquidity_score", float),
        ("entry_quality_adaptive_relief_position_scale", float),
        ("entry_quality_adaptive_relief_min_strategy_trades", int),
        ("entry_quality_adaptive_relief_min_strategy_win_rate", float),
        ("entry_quality_adaptive_relief_min_strategy_profit_factor", float),
        ("entry_quality_adaptive_relief_block_high_stress_regime", bool),
        ("second_stage_history_safety_severe_scale", float),
        ("enable_second_stage_history_safety_severe_relief", bool),
        ("second_stage_history_safety_relief_max_scale", float),
        ("second_stage_history_safety_relief_min_strategy_trades", int),
        ("second_stage_history_safety_relief_min_signal_strength", float),
        ("second_stage_history_safety_relief_min_expected_value", float),
        ("second_stage_history_safety_relief_min_liquidity_score", float),
        ("second_stage_history_safety_relief_block_hostile_regime", bool),
        ("enable_two_head_entry_second_stage_aggregation", bool),
        ("two_head_entry_quality_weight", float),
        ("two_head_second_stage_weight", float),
        ("two_head_min_entry_quality_score", float),
        ("two_head_min_second_stage_score", float),
        ("two_head_min_aggregate_score", float),
        ("two_head_aggregation_block_high_stress_regime", bool),
        ("two_head_aggregation_min_strategy_trades", int),
        ("enable_two_head_rr_margin_near_miss_floor_relax", bool),
        ("two_head_rr_margin_near_miss_second_stage_floor_relax", float),
        ("two_head_rr_margin_near_miss_aggregate_floor_relax", float),
        ("enable_two_head_rr_margin_near_miss_adaptive_floor_relax", bool),
        ("two_head_rr_margin_near_miss_adaptive_floor_relax_min_activation", float),
        ("two_head_rr_margin_near_miss_adaptive_floor_relax_max_second_stage", float),
        ("two_head_rr_margin_near_miss_adaptive_floor_relax_max_aggregate", float),
        ("two_head_rr_margin_near_miss_adaptive_floor_relax_quality_weight", float),
        ("two_head_rr_margin_near_miss_adaptive_floor_relax_gap_weight", float),
        ("enable_two_head_rr_margin_near_miss_surplus_compensation", bool),
        ("two_head_rr_margin_near_miss_surplus_min_entry_surplus", float),
        ("two_head_rr_margin_near_miss_surplus_min_edge_score", float),
        ("two_head_rr_margin_near_miss_surplus_max_second_stage_deficit", float),
        ("two_head_rr_margin_near_miss_surplus_max_aggregate_deficit", float),
        ("two_head_rr_margin_near_miss_surplus_entry_weight", float),
        ("two_head_rr_margin_near_miss_surplus_max_aggregate_bonus", float),
        ("enable_second_stage_rr_margin_near_miss_relief", bool),
        ("second_stage_rr_margin_near_miss_max_gap", float),
        ("second_stage_rr_margin_near_miss_min_signal_strength", float),
        ("second_stage_rr_margin_near_miss_min_expected_value", float),
        ("second_stage_rr_margin_near_miss_min_liquidity_score", float),
        ("second_stage_rr_margin_near_miss_min_strategy_trades", int),
        ("second_stage_rr_margin_near_miss_block_high_stress_regime", bool),
        ("second_stage_rr_margin_near_miss_score_boost", float),
        ("enable_second_stage_rr_margin_soft_score", bool),
        ("second_stage_rr_margin_soft_score_max_gap", float),
        ("second_stage_rr_margin_soft_score_floor", float),
        ("second_stage_rr_margin_soft_score_gap_tightness_weight", float),
        ("enable_second_stage_rr_margin_near_miss_head_score_floor", bool),
        ("second_stage_rr_margin_near_miss_head_score_floor_base", float),
        ("second_stage_rr_margin_near_miss_head_score_floor_quality_weight", float),
        ("second_stage_rr_margin_near_miss_head_score_floor_gap_weight", float),
        ("second_stage_rr_margin_near_miss_head_score_floor_max", float),
    ]:
        if key in combo:
            trading[key] = cast(combo[key])

    strategies = config_obj.setdefault("strategies", {})
    for strategy_name in ("scalping", "momentum", "breakout", "mean_reversion"):
        strategies.setdefault(strategy_name, {})
    mapping = [
        ("scalping", "scalping_min_signal_strength"),
        ("momentum", "momentum_min_signal_strength"),
        ("breakout", "breakout_min_signal_strength"),
        ("mean_reversion", "mean_reversion_min_signal_strength"),
    ]
    for strategy_name, key in mapping:
        if key in combo:
            strategies[strategy_name]["min_signal_strength"] = float(combo[key])


def apply_combo_to_config_files(build_config_path, source_config_path, combo, sync_source):
    build_cfg = json.loads(build_config_path.read_text(encoding="utf-8-sig"))
    apply_combo_to_config_object(build_cfg, combo)
    build_config_path.write_text(json.dumps(build_cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if sync_source and source_config_path.exists():
        source_cfg = json.loads(source_config_path.read_text(encoding="utf-8-sig"))
        apply_combo_to_config_object(source_cfg, combo)
        source_config_path.write_text(json.dumps(source_cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def select_best_combo_from_tune_summary(
    tune_summary_path,
    min_trades_floor: float,
    min_profitable_ratio: float,
    min_avg_win_rate_pct: float,
    min_expectancy_krw: float,
) -> Dict[str, Any]:
    payload = json.loads(tune_summary_path.read_text(encoding="utf-8-sig"))
    combos = payload.get("combos")
    summary = payload.get("summary")
    if not isinstance(combos, list) or not isinstance(summary, list):
        raise RuntimeError(f"Invalid tune summary json: {tune_summary_path}")
    selector_meta = payload.get("selector") or {}
    if not isinstance(selector_meta, dict):
        selector_meta = {}
    selected_combo_id = str(payload.get("best_combo_id", "") or "").strip()
    combo_by_id = {}
    for combo in combos:
        if isinstance(combo, dict):
            combo_by_id[str(combo.get("combo_id", ""))] = combo

    candidates: List[Dict[str, Any]] = []
    candidate_by_id: Dict[str, Dict[str, Any]] = {}
    for row in summary:
        if not isinstance(row, dict):
            continue
        combo_id = str(row.get("combo_id", ""))
        if combo_id not in combo_by_id:
            continue
        objective = compute_objective_score(
            float(row.get("avg_profit_factor", 0.0)),
            float(row.get("avg_expectancy_krw", 0.0)),
            float(row.get("profitable_ratio", 0.0)),
            float(row.get("avg_win_rate_pct", 0.0)),
            float(row.get("avg_total_trades", 0.0)),
            min_trades_floor,
            min_profitable_ratio,
            min_avg_win_rate_pct,
            min_expectancy_krw,
        )
        gate_bonus = 0.0
        if bool(row.get("overall_gate_pass", False)):
            gate_bonus += 300.0
        if bool(row.get("profile_gate_pass", False)):
            gate_bonus += 80.0
        if bool(row.get("gate_profit_factor_pass", False)):
            gate_bonus += 60.0
        if bool(row.get("gate_trades_pass", False)):
            gate_bonus += 40.0
        candidate = {
            "combo_id": combo_id,
            "combo": combo_by_id[combo_id],
            "objective_score": objective,
            "objective_with_gate_bonus": round(objective + gate_bonus, 6),
            "avg_profit_factor": float(row.get("avg_profit_factor", 0.0)),
            "avg_expectancy_krw": float(row.get("avg_expectancy_krw", 0.0)),
            "profitable_ratio": float(row.get("profitable_ratio", 0.0)),
            "avg_win_rate_pct": float(row.get("avg_win_rate_pct", 0.0)),
            "avg_total_trades": float(row.get("avg_total_trades", 0.0)),
            "overall_gate_pass": bool(row.get("overall_gate_pass", False)),
            "profile_gate_pass": bool(row.get("profile_gate_pass", False)),
            "bottleneck_scenario_family": str(row.get("bottleneck_scenario_family", "")),
            "holdout_failure_suppression_active": bool(
                row.get("holdout_failure_suppression_active", False)
            ),
            "holdout_failure_suppressed_family": bool(
                row.get("holdout_failure_suppressed_family", False)
            ),
            "holdout_failure_suppression_reason": str(
                row.get("holdout_failure_suppression_reason", "")
            ),
            "report_json": str(row.get("report_json", "")),
            "selector_mode_requested": str(selector_meta.get("requested_mode", "")),
            "selector_mode_applied": str(selector_meta.get("applied_mode", "")),
            "selector_reason": str(selector_meta.get("reason", "")),
            "selector_candidate_row_count": int(selector_meta.get("selector_candidate_row_count", 0) or 0),
            "selection_source": "",
        }
        candidates.append(candidate)
        candidate_by_id[combo_id] = candidate
    if not candidates:
        raise RuntimeError("No candidate combo rows from tuning summary.")
    if selected_combo_id and selected_combo_id in candidate_by_id:
        selected = dict(candidate_by_id[selected_combo_id])
        selected["selection_source"] = "tune_summary_best_combo_id"
        return selected

    candidates.sort(
        key=lambda x: (
            float(x["objective_with_gate_bonus"]),
            float(x["avg_expectancy_krw"]),
            float(x["avg_win_rate_pct"]),
            float(x["profitable_ratio"]),
            float(x["avg_profit_factor"]),
            float(x["avg_total_trades"]),
        ),
        reverse=True,
    )
    selected = dict(candidates[0])
    selected["selection_source"] = "auto_objective_fallback"
    return selected


def main(argv=None) -> int:
    args = parse_args(argv)
    if int(getattr(args, "matrix_max_workers", 1)) > 1:
        print(
            "[AutoImprove] Parallel matrix workers are disabled; "
            "forcing --matrix-max-workers=1."
        )
    args.matrix_max_workers = 1
    build_config_path = resolve_repo_path(args.build_config_path)
    source_config_path = resolve_repo_path(args.source_config_path)
    gate_report_path = resolve_repo_path(args.gate_report_json)
    entry_rejection_summary_path = resolve_repo_path(args.entry_rejection_summary_json)
    strategy_rejection_taxonomy_path = resolve_repo_path(args.strategy_rejection_taxonomy_json)
    live_signal_funnel_taxonomy_path = resolve_repo_path(args.live_signal_funnel_taxonomy_json)
    tune_summary_path = resolve_repo_path(args.tune_summary_json)
    iteration_csv = resolve_repo_path(args.iteration_csv)
    summary_json = resolve_repo_path(args.summary_json)
    patch_plan_json = resolve_repo_path(args.patch_plan_json)
    patch_action_policy_json = resolve_repo_path(args.patch_action_policy_json)
    patch_action_policy_registry_json = resolve_repo_path(args.patch_action_policy_registry_json)
    patch_action_policy_registry_feedback_json = resolve_repo_path(
        args.patch_action_policy_registry_feedback_json
    )
    patch_action_policy_guard_report_json = resolve_repo_path(args.patch_action_policy_guard_report_json)
    patch_action_feedback_promotion_check_json = resolve_repo_path(
        args.patch_action_feedback_promotion_check_json
    )
    lock_path = resolve_repo_path(args.verification_lock_path)
    iteration_csv.parent.mkdir(parents=True, exist_ok=True)
    summary_json.parent.mkdir(parents=True, exist_ok=True)
    patch_plan_json.parent.mkdir(parents=True, exist_ok=True)
    patch_action_policy_guard_report_json.parent.mkdir(parents=True, exist_ok=True)
    patch_action_feedback_promotion_check_json.parent.mkdir(parents=True, exist_ok=True)

    status = "running"
    reason = ""
    rows = []
    started_at = datetime.now(tz=timezone.utc)
    best_objective = float("-inf")
    best_snapshot: Optional[Dict[str, Any]] = None
    best_combo_id = ""
    best_entry_rejection_snapshot: Optional[Dict[str, Any]] = None
    best_entry_rejection_taxonomy_snapshot: Optional[Dict[str, Any]] = None
    best_live_signal_funnel_snapshot: Optional[Dict[str, Any]] = None
    last_applied_combo: Optional[Dict[str, Any]] = None
    last_applied_combo_id = ""
    consecutive_no_improve = 0
    holdout_suppression_persist_streak = 0
    holdout_suppression_persist_triggered = False
    holdout_suppression_persist_trigger_iteration = 0
    holdout_suppression_persist_events: List[Dict[str, Any]] = []
    next_tune_scenario_mode_override = ""
    next_tune_scenario_mode_override_reason = ""
    next_tune_directional_hint = ""
    next_tune_patch_plan_template_id = ""
    next_tune_patch_plan_actions: List[str] = []
    last_persist_pivot_direction = ""
    last_persist_pivot_recommendation = ""
    last_persist_pivot_next_scenario_mode = ""
    last_persist_pivot_reason_code = ""
    last_persist_patch_template: Dict[str, Any] = {}
    last_patch_plan_action_override: Dict[str, Any] = {}
    entry_timing_scenario_mode = normalize_tune_scenario_mode(
        str(args.persist_entry_timing_scenario_mode),
        "diverse_wide",
    )
    exit_risk_scenario_mode = normalize_tune_scenario_mode(
        str(args.persist_exit_risk_scenario_mode),
        "quality_focus",
    )
    patch_plan_handoff_snapshot = read_patch_plan_handoff(patch_plan_json)
    patch_plan_handoff_applied = False
    patch_action_policy_template_id = str(patch_plan_handoff_snapshot.get("template_id", "") or "").strip()
    patch_action_policy_snapshot = read_patch_action_policy(patch_action_policy_json)
    patch_action_policy_registry_snapshot = read_patch_action_policy_registry(
        patch_action_policy_registry_json,
        patch_action_policy_template_id,
    )
    patch_action_policy_registry_feedback_snapshot = read_patch_action_policy_registry_feedback(
        patch_action_policy_registry_feedback_json,
        patch_action_policy_template_id,
    )
    patch_action_policy_registry_guard = evaluate_patch_action_policy_registry_guard(
        patch_action_policy_registry_snapshot,
        min_repeat_runs=max(1, int(args.patch_action_policy_min_repeat_runs)),
        max_age_hours=float(args.patch_action_policy_max_age_hours),
        enable_guards=bool(args.enable_patch_action_policy_registry_guards),
        feedback_snapshot=patch_action_policy_registry_feedback_snapshot,
        consume_feedback=bool(args.consume_patch_action_policy_registry_feedback),
    )
    if bool(patch_action_policy_registry_snapshot.get("exists", False)):
        patch_action_policy_registry_snapshot["usable"] = bool(
            patch_action_policy_registry_snapshot.get("usable", False)
            and patch_action_policy_registry_guard.get("accepted", False)
        )
    patch_action_policy_guard_report_json.write_text(
        json.dumps(patch_action_policy_registry_guard, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    if bool(patch_action_policy_registry_snapshot.get("exists", False)):
        patch_action_policy_effective_snapshot = dict(patch_action_policy_registry_snapshot)
        patch_action_policy_effective_snapshot["source"] = "registry"
    else:
        patch_action_policy_effective_snapshot = dict(patch_action_policy_snapshot)
        patch_action_policy_effective_snapshot["source"] = "single"
    patch_action_policy_applied = False
    patch_action_override_strength_scale = 1.0
    effective_enable_patch_plan_action_overrides = bool(args.enable_patch_plan_action_overrides)
    if bool(args.consume_patch_action_policy) and bool(patch_action_policy_effective_snapshot.get("usable", False)):
        decision = str(patch_action_policy_effective_snapshot.get("decision", ""))
        if decision == "disable_override":
            effective_enable_patch_plan_action_overrides = False
            patch_action_policy_applied = True
        elif decision == "decrease_override_strength":
            patch_action_override_strength_scale = 0.5
            patch_action_policy_applied = True
        elif decision == "keep_override":
            patch_action_override_strength_scale = 1.0
            patch_action_policy_applied = True
        print(
            "[AutoImprove] 패치 액션 정책 적용: "
            f"decision={decision}, "
            f"source={patch_action_policy_effective_snapshot.get('source', '')}, "
            f"template={patch_action_policy_effective_snapshot.get('template_id', '')}, "
            f"override_enabled={effective_enable_patch_plan_action_overrides}, "
            f"strength_scale={patch_action_override_strength_scale}"
        )
    elif (
        bool(args.consume_patch_action_policy)
        and bool(patch_action_policy_registry_snapshot.get("exists", False))
        and not bool(patch_action_policy_registry_snapshot.get("usable", False))
    ):
        reason_codes = patch_action_policy_registry_guard.get("reason_codes") or []
        print(
            "[AutoImprove] 패치 액션 정책 미적용: "
            f"registry guard/reason={','.join(str(x) for x in reason_codes)}, "
            f"template={patch_action_policy_template_id or 'none'}"
        )
    if (
        bool(args.consume_patch_plan_handoff)
        and bool(patch_plan_handoff_snapshot.get("usable", False))
        and normalize_tune_scenario_mode(str(args.tune_scenario_mode), "quality_focus") != "legacy_only"
    ):
        next_mode_from_plan = normalize_tune_scenario_mode(
            str(patch_plan_handoff_snapshot.get("next_scenario_mode", "")),
            normalize_tune_scenario_mode(str(args.tune_scenario_mode), "quality_focus"),
        )
        next_tune_scenario_mode_override = next_mode_from_plan
        next_tune_scenario_mode_override_reason = (
            f"patch_plan_handoff:{patch_plan_handoff_snapshot.get('reason_code', '')}"
        )
        next_tune_directional_hint = str(patch_plan_handoff_snapshot.get("direction", ""))
        next_tune_patch_plan_template_id = str(patch_plan_handoff_snapshot.get("template_id", ""))
        next_tune_patch_plan_actions = [
            str(x).strip() for x in (patch_plan_handoff_snapshot.get("actions") or []) if str(x).strip()
        ]
        patch_plan_handoff_applied = True
        print(
            "[AutoImprove] 패치 플랜 handoff 로드: "
            f"방향={next_tune_directional_hint}, "
            f"다음_시나리오={next_mode_from_plan}, "
            f"템플릿={patch_plan_handoff_snapshot.get('template_id', '')}"
        )
    tune_objective_min_avg_trades = (
        float(args.tune_objective_min_avg_trades)
        if args.tune_objective_min_avg_trades is not None
        else float(args.min_avg_trades)
    )
    tune_objective_min_profitable_ratio = (
        float(args.tune_objective_min_profitable_ratio)
        if args.tune_objective_min_profitable_ratio is not None
        else float(args.min_profitable_ratio)
    )
    tune_objective_min_avg_win_rate_pct = (
        float(args.tune_objective_min_avg_win_rate_pct)
        if args.tune_objective_min_avg_win_rate_pct is not None
        else float(args.min_avg_win_rate_pct)
    )
    tune_objective_min_expectancy_krw = (
        float(args.tune_objective_min_expectancy_krw)
        if args.tune_objective_min_expectancy_krw is not None
        else float(args.min_expectancy_krw)
    )
    patch_action_feedback_promotion_check_snapshot: Dict[str, Any] = {
        "enabled": bool(args.run_patch_action_feedback_promotion_check),
        "invoked": False,
        "success": False,
        "exit_code": None,
        "template_id": str(patch_action_policy_template_id or ""),
        "required_keep_streak": int(
            max(1, int(args.patch_action_feedback_promotion_check_required_keep_streak))
        ),
        "promotion_ready": None,
        "blocker_reason_codes": [],
        "output_json": str(patch_action_feedback_promotion_check_json),
    }

    with verification_lock(
        lock_path,
        timeout_sec=int(args.verification_lock_timeout_sec),
        stale_sec=int(args.verification_lock_stale_sec),
    ):
        for iteration in range(1, int(args.max_iterations) + 1):
            elapsed_minutes = (datetime.now(tz=timezone.utc) - started_at).total_seconds() / 60.0
            if elapsed_minutes >= float(args.max_runtime_minutes):
                status = "paused_runtime_limit"
                reason = "Max runtime exceeded before iteration start."
                break

            print(f"[AutoImprove] Iteration {iteration}/{args.max_iterations} - baseline matrix run")
            real_loop_argv = []
            if not args.fetch_each_iteration:
                real_loop_argv.append("--skip-fetch")
            real_loop_argv.append("--skip-tune")
            if args.real_data_only:
                real_loop_argv.append("--real-data-only")
            if not args.run_parity_invariant:
                real_loop_argv.append("--skip-parity-invariant")
            if args.fail_on_parity_invariant:
                real_loop_argv.append("--fail-on-parity-invariant")
            real_loop_argv.extend(["--gate-min-avg-trades", str(int(round(float(args.min_avg_trades))))])
            real_loop_argv.extend(["--matrix-max-workers", str(max(1, int(args.matrix_max_workers)))])
            real_loop_argv.extend(["--matrix-backtest-retry-count", str(max(1, int(args.matrix_backtest_retry_count)))])
            real_loop_argv.extend(["--verification-lock-path", str(lock_path)])
            real_loop_argv.extend(["--verification-lock-timeout-sec", str(max(1, int(args.verification_lock_timeout_sec)))])
            real_loop_argv.extend(["--verification-lock-stale-sec", str(max(10, int(args.verification_lock_stale_sec)))])
            if args.enable_hostility_adaptive_targets:
                real_loop_argv.append("--enable-hostility-adaptive-thresholds")
                real_loop_argv.append("--enable-hostility-adaptive-trades-only")
            else:
                real_loop_argv.append("--disable-hostility-adaptive-thresholds")
                real_loop_argv.append("--disable-hostility-adaptive-trades-only")
            if args.require_higher_tf_companions:
                real_loop_argv.append("--require-higher-tf-companions")
            else:
                real_loop_argv.append("--allow-missing-higher-tf-companions")
            if args.emit_strict_adaptive_pair:
                real_loop_argv.append("--run-both-hostility-modes")
            if args.enable_adaptive_state_io:
                real_loop_argv.append("--enable-adaptive-state-io")
            if args.skip_core_vs_legacy_gate:
                real_loop_argv.append("--skip-core-vs-legacy-gate")
            rc = run_realdata_candidate_loop.main(real_loop_argv)
            if rc != 0:
                raise RuntimeError(f"Realdata candidate loop (baseline) failed (exit={rc})")
            if not gate_report_path.exists():
                raise RuntimeError(f"Gate report not found after baseline run: {gate_report_path}")
    
            snapshot = get_core_snapshot(
                gate_report_path,
                float(args.min_profit_factor),
                tune_objective_min_avg_trades,
                tune_objective_min_profitable_ratio,
                tune_objective_min_avg_win_rate_pct,
                tune_objective_min_expectancy_krw,
                bool(args.enable_hostility_adaptive_targets),
            )
            entry_rejection_snapshot = read_entry_rejection_snapshot(entry_rejection_summary_path)
            entry_rejection_taxonomy_snapshot = read_strategy_rejection_taxonomy_snapshot(
                strategy_rejection_taxonomy_path
            )
            live_signal_funnel_snapshot = read_live_signal_funnel_snapshot(live_signal_funnel_taxonomy_path)
            core = snapshot["core"]
            active_thresholds = snapshot["active_thresholds"]
            threshold_context = snapshot.get("threshold_context") or {}
            effective_thresholds = threshold_context.get("effective") or {}
            blended_ctx = threshold_context.get("blended_context") or {}
            is_target = target_satisfied(
                core,
                float(active_thresholds["min_profit_factor"]),
                float(active_thresholds["min_expectancy_krw"]),
                float(active_thresholds["min_profitable_ratio"]),
                float(active_thresholds["min_avg_trades"]),
                float(active_thresholds["min_avg_win_rate_pct"]),
            )
            tune_min_avg_trades_iter = float(tune_objective_min_avg_trades)
            tune_min_profitable_ratio_iter = float(tune_objective_min_profitable_ratio)
            tune_min_avg_win_rate_iter = float(tune_objective_min_avg_win_rate_pct)
            tune_min_expectancy_iter = float(tune_objective_min_expectancy_krw)
            gate_min_avg_trades_iter = int(round(float(args.min_avg_trades)))
            if args.enable_hostility_adaptive_targets:
                tune_min_avg_trades_iter = min(tune_min_avg_trades_iter, float(active_thresholds["min_avg_trades"]))
                # Objective floors are dynamically relaxed by the blended hostility/quality band.
                # This affects tuning search pressure, while acceptance still uses active thresholds.
                tune_min_profitable_ratio_iter = min(
                    tune_min_profitable_ratio_iter,
                    float(effective_thresholds.get("min_profitable_ratio", tune_min_profitable_ratio_iter)),
                )
                tune_min_avg_win_rate_iter = min(
                    tune_min_avg_win_rate_iter,
                    float(effective_thresholds.get("min_avg_win_rate_pct", tune_min_avg_win_rate_iter)),
                )
                tune_min_expectancy_iter = min(
                    tune_min_expectancy_iter,
                    float(effective_thresholds.get("min_expectancy_krw", tune_min_expectancy_iter)),
                )
                gate_min_avg_trades_iter = int(
                    round(min(float(args.min_avg_trades), float(active_thresholds["min_avg_trades"])))
                )
            if bool(live_signal_funnel_snapshot.get("no_trade_bias_active", False)):
                trade_floor_scale = float(live_signal_funnel_snapshot.get("recommended_trade_floor_scale", 1.0))
                tune_min_avg_trades_iter = min(
                    tune_min_avg_trades_iter,
                    max(4.0, round(tune_min_avg_trades_iter * trade_floor_scale, 4)),
                )
                gate_min_avg_trades_iter = int(
                    round(
                        min(
                            float(gate_min_avg_trades_iter),
                            max(4.0, float(gate_min_avg_trades_iter) * trade_floor_scale),
                        )
                    )
                )
            gate_min_avg_trades_iter = max(1, gate_min_avg_trades_iter)
            live_hint_adjusted_ratio = float(
                live_signal_funnel_snapshot.get("selection_hint_adjusted_ratio", 0.0) or 0.0
            )
            tune_holdout_suppression_snapshot = {
                "exists": False,
                "summary_json": str(tune_summary_path),
                "enabled": False,
                "active": False,
                "reason": "",
                "live_hint_adjusted_ratio": 0.0,
                "hint_ratio_threshold": 0.0,
                "require_both_pf_exp_fail": True,
                "suppressed_families": [],
                "suppressed_combo_count": 0,
                "kept_combo_count": 0,
                "fallback_retained_combo_id": "",
                "fail_open_all_suppressed": False,
            }
            tune_post_suppression_expansion_snapshot = {
                "exists": False,
                "summary_json": str(tune_summary_path),
                "enabled": False,
                "applied": False,
                "reason": "",
                "target_min_combo_count": 0,
                "input_combo_count": 0,
                "output_combo_count": 0,
                "injected_combo_count": 0,
                "injected_combo_ids": [],
                "context_top_group": "",
                "context_top_group_source": "",
            }
            pivot_meta_iter = {
                "active": False,
                "direction": "",
                "reason_code": "",
                "recommendation": "",
                "next_scenario_mode": "",
                "expansion_applied": False,
                "patch_template": build_directional_patch_template(""),
            }
            baseline_quality_fail = pf_expectancy_quality_fail(
                core_summary=core,
                active_thresholds=active_thresholds,
                require_both=bool(args.holdout_suppression_persist_require_both_pf_exp_fail),
            )
            hint_overfit_risk = bool(
                live_hint_adjusted_ratio >= float(args.hint_overfit_ratio_threshold)
                and (
                    float(core.get("avg_profit_factor", 0.0)) < float(active_thresholds["min_profit_factor"])
                    or float(core.get("avg_expectancy_krw", 0.0)) < float(active_thresholds["min_expectancy_krw"])
                )
            )
            tune_scenario_mode_iter = normalize_tune_scenario_mode(str(args.tune_scenario_mode), "quality_focus")
            tune_scenario_mode_source = "default"
            applied_directional_hint_iter = ""
            applied_patch_template_id_iter = ""
            applied_patch_actions_iter: List[str] = []
            if next_tune_scenario_mode_override and tune_scenario_mode_iter != "legacy_only":
                tune_scenario_mode_iter = normalize_tune_scenario_mode(
                    next_tune_scenario_mode_override,
                    tune_scenario_mode_iter,
                )
                if str(next_tune_scenario_mode_override_reason).startswith("patch_plan_handoff:"):
                    tune_scenario_mode_source = "patch_plan_handoff"
                else:
                    tune_scenario_mode_source = "persist_directional_pivot_override"
                applied_directional_hint_iter = str(next_tune_directional_hint or "")
                applied_patch_template_id_iter = str(next_tune_patch_plan_template_id or "")
                applied_patch_actions_iter = list(next_tune_patch_plan_actions or [])
                print(
                    "[AutoImprove] 이전 피벗 기반 시나리오 모드 오버라이드 적용: "
                    f"scenario_mode={tune_scenario_mode_iter}, reason={next_tune_scenario_mode_override_reason}"
                )
                next_tune_scenario_mode_override = ""
                next_tune_scenario_mode_override_reason = ""
                next_tune_directional_hint = ""
                next_tune_patch_plan_template_id = ""
                next_tune_patch_plan_actions = []
            tune_objective_mode_iter = str(args.tune_objective_mode)
            tune_holdout_suppression_hint_ratio_threshold_iter = float(
                args.tune_holdout_suppression_hint_ratio_threshold
            )
            tune_enable_post_suppression_quality_expansion_iter = bool(
                args.tune_enable_post_suppression_quality_expansion
            )
            if applied_directional_hint_iter == "entry_timing":
                tune_enable_post_suppression_quality_expansion_iter = False
            elif applied_directional_hint_iter == "exit_risk":
                tune_enable_post_suppression_quality_expansion_iter = True
            tune_post_suppression_min_combo_count_iter = int(
                max(1, int(args.tune_post_suppression_min_combo_count))
            )
            tune_hint_guardrail_ratio_iter = float(args.tune_hint_impact_guardrail_ratio)
            tune_hint_guardrail_tighten_scale_iter = float(args.tune_hint_impact_guardrail_tighten_scale)
            tune_enable_hint_guardrail_iter = bool(args.tune_enable_hint_impact_guardrail)
            patch_plan_action_override_iter: Dict[str, Any] = {
                "applied": False,
                "template_id": str(applied_patch_template_id_iter),
                "direction_hint": str(applied_directional_hint_iter),
                "matched_actions": [],
                "unmatched_actions": [],
                "resolved": {},
            }
            if (
                bool(effective_enable_patch_plan_action_overrides)
                and tune_scenario_mode_iter != "legacy_only"
                and bool(applied_patch_actions_iter)
            ):
                patch_plan_action_override_iter = build_patch_plan_action_overrides(
                    actions=applied_patch_actions_iter,
                    direction_hint=applied_directional_hint_iter,
                    template_id=applied_patch_template_id_iter,
                    strength_scale=float(patch_action_override_strength_scale),
                    baseline={
                        "tune_objective_mode": tune_objective_mode_iter,
                        "objective_min_profitable_ratio": tune_min_profitable_ratio_iter,
                        "objective_min_avg_win_rate_pct": tune_min_avg_win_rate_iter,
                        "objective_min_expectancy_krw": tune_min_expectancy_iter,
                        "objective_min_avg_trades": tune_min_avg_trades_iter,
                        "gate_min_avg_trades": gate_min_avg_trades_iter,
                        "tune_enable_hint_guardrail": tune_enable_hint_guardrail_iter,
                        "tune_hint_guardrail_ratio": tune_hint_guardrail_ratio_iter,
                        "tune_hint_guardrail_tighten_scale": tune_hint_guardrail_tighten_scale_iter,
                        "tune_holdout_suppression_hint_ratio_threshold": tune_holdout_suppression_hint_ratio_threshold_iter,
                        "tune_enable_post_suppression_quality_expansion": tune_enable_post_suppression_quality_expansion_iter,
                        "tune_post_suppression_min_combo_count": tune_post_suppression_min_combo_count_iter,
                    },
                )
                resolved_action_override = patch_plan_action_override_iter.get("resolved") or {}
                if bool(patch_plan_action_override_iter.get("applied", False)) and isinstance(
                    resolved_action_override, dict
                ):
                    tune_objective_mode_iter = str(
                        resolved_action_override.get("objective_mode", tune_objective_mode_iter)
                    )
                    tune_min_profitable_ratio_iter = float(
                        resolved_action_override.get(
                            "objective_min_profitable_ratio",
                            tune_min_profitable_ratio_iter,
                        )
                    )
                    tune_min_avg_win_rate_iter = float(
                        resolved_action_override.get(
                            "objective_min_avg_win_rate_pct",
                            tune_min_avg_win_rate_iter,
                        )
                    )
                    tune_min_expectancy_iter = float(
                        resolved_action_override.get(
                            "objective_min_expectancy_krw",
                            tune_min_expectancy_iter,
                        )
                    )
                    tune_min_avg_trades_iter = float(
                        resolved_action_override.get(
                            "objective_min_avg_trades",
                            tune_min_avg_trades_iter,
                        )
                    )
                    gate_min_avg_trades_iter = int(
                        resolved_action_override.get(
                            "gate_min_avg_trades",
                            gate_min_avg_trades_iter,
                        )
                    )
                    tune_enable_hint_guardrail_iter = bool(
                        resolved_action_override.get(
                            "tune_enable_hint_guardrail",
                            tune_enable_hint_guardrail_iter,
                        )
                    )
                    tune_hint_guardrail_ratio_iter = float(
                        resolved_action_override.get(
                            "tune_hint_guardrail_ratio",
                            tune_hint_guardrail_ratio_iter,
                        )
                    )
                    tune_hint_guardrail_tighten_scale_iter = float(
                        resolved_action_override.get(
                            "tune_hint_guardrail_tighten_scale",
                            tune_hint_guardrail_tighten_scale_iter,
                        )
                    )
                    tune_holdout_suppression_hint_ratio_threshold_iter = float(
                        resolved_action_override.get(
                            "tune_holdout_suppression_hint_ratio_threshold",
                            tune_holdout_suppression_hint_ratio_threshold_iter,
                        )
                    )
                    tune_enable_post_suppression_quality_expansion_iter = bool(
                        resolved_action_override.get(
                            "tune_enable_post_suppression_quality_expansion",
                            tune_enable_post_suppression_quality_expansion_iter,
                        )
                    )
                    tune_post_suppression_min_combo_count_iter = int(
                        resolved_action_override.get(
                            "tune_post_suppression_min_combo_count",
                            tune_post_suppression_min_combo_count_iter,
                        )
                    )
                    print(
                        "[AutoImprove] 패치 플랜 액션 오버라이드 적용 완료: "
                        f"template={applied_patch_template_id_iter or 'none'}, "
                        f"matched={len(patch_plan_action_override_iter.get('matched_actions', []) or [])}, "
                        f"objective_mode={tune_objective_mode_iter}, "
                        f"expansion={tune_enable_post_suppression_quality_expansion_iter}"
                    )
            last_patch_plan_action_override = dict(patch_plan_action_override_iter or {})
            if hint_overfit_risk:
                tune_enable_hint_guardrail_iter = True
                tune_hint_guardrail_ratio_iter = min(
                    tune_hint_guardrail_ratio_iter,
                    live_hint_adjusted_ratio,
                )
                tune_hint_guardrail_tighten_scale_iter = max(
                    tune_hint_guardrail_tighten_scale_iter,
                    float(args.hint_overfit_force_guardrail_tighten_scale),
                )
                if bool(args.enable_hint_overfit_quality_pivot) and tune_scenario_mode_iter != "legacy_only":
                    tune_scenario_mode_iter = "quality_focus"
                    tune_scenario_mode_source = "hint_overfit_guardrail"
                print(
                    "[AutoImprove] Hint-overfit risk detected: "
                    f"ratio={live_hint_adjusted_ratio:.4f}, "
                    f"scenario_mode->{tune_scenario_mode_iter}, "
                    f"guardrail_scale={tune_hint_guardrail_tighten_scale_iter:.2f}"
                )
            rows.append(
                {
                    "iteration": iteration,
                    "phase": "baseline",
                    "selected_combo": "",
                    "selected_combo_selection_source": "",
                    "selected_combo_selector_mode_applied": "",
                    "selected_combo_selector_reason": "",
                    "post_apply_skipped_same_combo": False,
                    "overall_gate_pass": bool(snapshot["overall_gate_pass"]),
                    "core_vs_legacy_gate_pass": bool(snapshot["core_vs_legacy_gate_pass"]),
                    "core_full_gate_pass": bool(core.get("gate_pass", False)),
                    "avg_profit_factor": float(core.get("avg_profit_factor", 0.0)),
                    "avg_expectancy_krw": float(core.get("avg_expectancy_krw", 0.0)),
                    "avg_total_trades": float(core.get("avg_total_trades", 0.0)),
                    "avg_win_rate_pct": float(core.get("avg_win_rate_pct", 0.0)),
                    "profitable_ratio": float(core.get("profitable_ratio", 0.0)),
                    "objective_score": float(snapshot["objective_score"]),
                    "selected_combo_objective_with_gate_bonus": 0.0,
                    "active_min_profit_factor": float(active_thresholds["min_profit_factor"]),
                    "active_min_expectancy_krw": float(active_thresholds["min_expectancy_krw"]),
                    "active_min_profitable_ratio": float(active_thresholds["min_profitable_ratio"]),
                    "active_min_avg_win_rate_pct": float(active_thresholds["min_avg_win_rate_pct"]),
                    "active_min_avg_trades": float(active_thresholds["min_avg_trades"]),
                    "objective_min_profitable_ratio_iter": float(tune_min_profitable_ratio_iter),
                    "objective_min_avg_win_rate_pct_iter": float(tune_min_avg_win_rate_iter),
                    "objective_min_expectancy_krw_iter": float(tune_min_expectancy_iter),
                    "objective_min_avg_trades_iter": float(tune_min_avg_trades_iter),
                    "blended_hostility_level": str(blended_ctx.get("blended_hostility_level", "unknown")),
                    "blended_hostility_score": float(blended_ctx.get("blended_adversarial_score", 0.0)),
                    "quality_level": str((threshold_context.get("quality") or {}).get("quality_level", "unknown")),
                    "quality_avg_score": float((threshold_context.get("quality") or {}).get("avg_quality_risk_score", 0.0)),
                    "entry_rejection_top_reason": str(entry_rejection_snapshot.get("profile_top_reason", "")),
                    "entry_rejection_top_count": int(entry_rejection_snapshot.get("profile_top_count", 0) or 0),
                    "entry_rejection_overall_top_reason": str(entry_rejection_snapshot.get("overall_top_reason", "")),
                    "entry_rejection_overall_top_count": int(entry_rejection_snapshot.get("overall_top_count", 0) or 0),
                    "entry_rejection_top_group": str(entry_rejection_taxonomy_snapshot.get("overall_top_group", "")),
                    "entry_rejection_top_group_count": int(
                        entry_rejection_taxonomy_snapshot.get("overall_top_group_count", 0) or 0
                    ),
                    "entry_rejection_taxonomy_coverage_ratio": float(
                        entry_rejection_taxonomy_snapshot.get("taxonomy_coverage_ratio", 0.0) or 0.0
                    ),
                    "entry_rejection_taxonomy_unknown_count": int(
                        entry_rejection_taxonomy_snapshot.get("unknown_reason_code_count", 0) or 0
                    ),
                    "live_funnel_top_group": str(live_signal_funnel_snapshot.get("top_group", "")),
                    "live_funnel_top_group_count": int(live_signal_funnel_snapshot.get("top_group_count", 0) or 0),
                    "live_funnel_signal_generation_share": float(
                        live_signal_funnel_snapshot.get("signal_generation_share", 0.0) or 0.0
                    ),
                    "live_funnel_selection_hint_adjusted_ratio": float(live_hint_adjusted_ratio),
                    "live_funnel_no_trade_bias_active": bool(
                        live_signal_funnel_snapshot.get("no_trade_bias_active", False)
                    ),
                    "live_funnel_trade_floor_scale": float(
                        live_signal_funnel_snapshot.get("recommended_trade_floor_scale", 1.0) or 1.0
                    ),
                    "hint_overfit_risk": bool(hint_overfit_risk),
                    "tune_scenario_mode_iter": str(tune_scenario_mode_iter),
                    "tune_scenario_mode_source": str(tune_scenario_mode_source),
                    "tune_objective_mode_iter": str(tune_objective_mode_iter),
                    "tune_directional_hint_iter": str(applied_directional_hint_iter),
                    "tune_holdout_suppression_hint_ratio_threshold_iter": float(
                        tune_holdout_suppression_hint_ratio_threshold_iter
                    ),
                    "tune_hint_guardrail_enabled_iter": bool(tune_enable_hint_guardrail_iter),
                    "tune_hint_guardrail_ratio_iter": float(tune_hint_guardrail_ratio_iter),
                    "tune_hint_guardrail_tighten_scale_iter": float(tune_hint_guardrail_tighten_scale_iter),
                    "patch_plan_action_override_applied": bool(
                        patch_plan_action_override_iter.get("applied", False)
                    ),
                    "patch_plan_action_override_template_id": str(
                        patch_plan_action_override_iter.get("template_id", "")
                    ),
                    "patch_plan_action_override_direction_hint": str(
                        patch_plan_action_override_iter.get("direction_hint", "")
                    ),
                    "patch_plan_action_override_matched_count": int(
                        len(patch_plan_action_override_iter.get("matched_actions", []) or [])
                    ),
                    "patch_plan_action_override_unmatched_count": int(
                        len(patch_plan_action_override_iter.get("unmatched_actions", []) or [])
                    ),
                    "tune_post_suppression_quality_expansion_enabled_iter": bool(
                        tune_enable_post_suppression_quality_expansion_iter
                    ),
                    "tune_post_suppression_min_combo_count_iter": int(
                        tune_post_suppression_min_combo_count_iter
                    ),
                    "holdout_suppression_active": bool(tune_holdout_suppression_snapshot["active"]),
                    "holdout_suppression_reason": str(tune_holdout_suppression_snapshot["reason"]),
                    "holdout_suppression_suppressed_combo_count": int(
                        tune_holdout_suppression_snapshot["suppressed_combo_count"]
                    ),
                    "holdout_suppression_kept_combo_count": int(tune_holdout_suppression_snapshot["kept_combo_count"]),
                    "holdout_suppression_fail_open_all_suppressed": bool(
                        tune_holdout_suppression_snapshot["fail_open_all_suppressed"]
                    ),
                    "holdout_suppression_effective": bool(
                        tune_holdout_suppression_snapshot["active"]
                        and (
                            int(tune_holdout_suppression_snapshot["suppressed_combo_count"] or 0) > 0
                            or bool(tune_holdout_suppression_snapshot["fail_open_all_suppressed"])
                        )
                    ),
                    "post_suppression_expansion_applied": bool(tune_post_suppression_expansion_snapshot["applied"]),
                    "post_suppression_expansion_injected_combo_count": int(
                        tune_post_suppression_expansion_snapshot["injected_combo_count"]
                    ),
                    "post_suppression_expansion_reason": str(tune_post_suppression_expansion_snapshot["reason"]),
                    "selected_combo_family": "",
                    "persist_pivot_active": bool(pivot_meta_iter["active"]),
                    "persist_pivot_direction": str(pivot_meta_iter["direction"]),
                    "persist_pivot_reason_code": str(pivot_meta_iter["reason_code"]),
                    "persist_pivot_recommendation": str(pivot_meta_iter["recommendation"]),
                    "persist_pivot_next_scenario_mode": str(pivot_meta_iter["next_scenario_mode"]),
                    "persist_pivot_template_id": str((pivot_meta_iter.get("patch_template") or {}).get("template_id", "")),
                    "persist_pivot_template_action_count": int(
                        len((pivot_meta_iter.get("patch_template") or {}).get("checklist") or [])
                    ),
                    "holdout_suppression_persist_streak": int(holdout_suppression_persist_streak),
                    "holdout_suppression_persist_triggered": bool(holdout_suppression_persist_triggered),
                    "pf_fail_active_threshold": bool(baseline_quality_fail["pf_fail"]),
                    "expectancy_fail_active_threshold": bool(baseline_quality_fail["expectancy_fail"]),
                    "pf_expectancy_quality_fail": bool(baseline_quality_fail["quality_fail"]),
                    "target_satisfied": bool(is_target),
                    "timestamp": datetime.now(tz=timezone.utc).isoformat(),
                }
            )
    
            if float(snapshot["objective_score"]) > (best_objective + float(args.improvement_epsilon)):
                best_objective = float(snapshot["objective_score"])
                best_snapshot = snapshot
                best_combo_id = ""
                best_entry_rejection_snapshot = entry_rejection_snapshot
                best_entry_rejection_taxonomy_snapshot = entry_rejection_taxonomy_snapshot
                best_live_signal_funnel_snapshot = live_signal_funnel_snapshot
                consecutive_no_improve = 0
            else:
                consecutive_no_improve += 1
    
            if is_target and bool(snapshot["overall_gate_pass"]):
                status = "success_gate_pass"
                reason = "Target metrics and overall gate passed on baseline run."
                break
    
            if args.skip_tune_phase:
                if consecutive_no_improve >= int(args.max_consecutive_no_improvement):
                    status = "paused_no_improvement"
                    reason = "No objective improvement within limit while tune phase skipped."
                    break
                continue
    
            elapsed_before_tune = (datetime.now(tz=timezone.utc) - started_at).total_seconds() / 60.0
            if elapsed_before_tune >= float(args.max_runtime_minutes):
                status = "paused_runtime_limit"
                reason = "Max runtime reached before tune phase."
                break
    
            print(f"[AutoImprove] Iteration {iteration}/{args.max_iterations} - tuning combos")
            tune_argv = [
                "--scenario-mode",
                str(tune_scenario_mode_iter),
                "--max-scenarios",
                str(args.tune_max_scenarios),
                "--matrix-max-workers",
                str(max(1, int(args.matrix_max_workers))),
                "--matrix-backtest-retry-count",
                str(max(1, int(args.matrix_backtest_retry_count))),
                "--screen-dataset-limit",
                str(args.tune_screen_dataset_limit),
                "--screen-top-k",
                str(args.tune_screen_top_k),
                "--objective-min-avg-trades",
                str(tune_min_avg_trades_iter),
                "--gate-min-avg-trades",
                str(gate_min_avg_trades_iter),
                "--objective-min-profitable-ratio",
                str(tune_min_profitable_ratio_iter),
                "--objective-min-avg-win-rate-pct",
                str(tune_min_avg_win_rate_iter),
                "--objective-min-expectancy-krw",
                str(tune_min_expectancy_iter),
                "--objective-mode",
                str(tune_objective_mode_iter),
                "--hint-impact-guardrail-ratio",
                str(tune_hint_guardrail_ratio_iter),
                "--hint-impact-guardrail-tighten-scale",
                str(tune_hint_guardrail_tighten_scale_iter),
                "--selector-mode",
                str(args.tune_selector_mode),
                "--selector-two-stage-pre-min-avg-trades",
                str(float(args.tune_selector_two_stage_pre_min_avg_trades)),
                "--selector-two-stage-pre-min-win-rate-pct",
                str(float(args.tune_selector_two_stage_pre_min_win_rate_pct)),
                "--selector-two-stage-pre-max-market-loss-top-share-pct",
                str(float(args.tune_selector_two_stage_pre_max_market_loss_top_share_pct)),
                "--selector-two-stage-pre-max-market-loss-hhi",
                str(float(args.tune_selector_two_stage_pre_max_market_loss_hhi)),
                "--selector-two-stage-profit-min-profit-factor",
                str(float(args.tune_selector_two_stage_profit_min_profit_factor)),
                "--selector-two-stage-profit-min-expectancy-krw",
                str(float(args.tune_selector_two_stage_profit_min_expectancy_krw)),
                "--selector-two-stage-profit-min-win-rate-pct",
                str(float(args.tune_selector_two_stage_profit_min_win_rate_pct)),
                "--selector-veto-max-market-loss-top-share-pct",
                str(float(args.tune_selector_veto_max_market_loss_top_share_pct)),
                "--selector-veto-max-market-loss-hhi",
                str(float(args.tune_selector_veto_max_market_loss_hhi)),
                "--selector-veto-max-avg-total-trades",
                str(float(args.tune_selector_veto_max_avg_total_trades)),
                "--baseline-readiness-max-drawdown-delta-pct",
                str(float(args.tune_baseline_readiness_max_drawdown_delta_pct)),
                "--baseline-readiness-expectancy-tolerance-krw",
                str(float(args.tune_baseline_readiness_expectancy_tolerance_krw)),
            ]
            if bool(args.tune_selector_enable_two_stage_gate):
                tune_argv.append("--selector-enable-two-stage-gate")
            else:
                tune_argv.append("--disable-selector-enable-two-stage-gate")
            if bool(args.tune_selector_two_stage_pre_require_gate_trades_pass):
                tune_argv.append("--selector-two-stage-pre-require-gate-trades-pass")
            else:
                tune_argv.append("--disable-selector-two-stage-pre-require-gate-trades-pass")
            if bool(args.tune_selector_two_stage_allow_pre_gate_fallback):
                tune_argv.append("--selector-two-stage-allow-pre-gate-fallback")
            else:
                tune_argv.append("--disable-selector-two-stage-allow-pre-gate-fallback")
            if bool(args.tune_selector_enable_veto_ensemble):
                tune_argv.append("--selector-enable-veto-ensemble")
            else:
                tune_argv.append("--disable-selector-enable-veto-ensemble")
            if bool(args.tune_enable_selector_baseline_readiness_veto):
                tune_argv.append("--enable-selector-baseline-readiness-veto")
            else:
                tune_argv.append("--disable-selector-baseline-readiness-veto")
            if bool(args.tune_selector_baseline_readiness_veto_fail_closed):
                tune_argv.append("--selector-baseline-readiness-veto-fail-closed")
            else:
                tune_argv.append("--selector-baseline-readiness-veto-fail-open")
            if tune_enable_hint_guardrail_iter:
                tune_argv.append("--enable-hint-impact-guardrail")
            else:
                tune_argv.append("--disable-hint-impact-guardrail")
            if bool(args.tune_enable_holdout_failure_family_suppression):
                tune_argv.append("--enable-holdout-failure-family-suppression")
            else:
                tune_argv.append("--disable-holdout-failure-family-suppression")
            tune_argv.extend(
                [
                    "--holdout-suppression-hint-ratio-threshold",
                    str(float(tune_holdout_suppression_hint_ratio_threshold_iter)),
                ]
            )
            if bool(args.tune_holdout_suppression_require_both_pf_exp_fail):
                tune_argv.append("--holdout-suppression-require-both-pf-exp-fail")
            else:
                tune_argv.append("--holdout-suppression-allow-either-pf-or-exp-fail")
            if bool(tune_enable_post_suppression_quality_expansion_iter):
                tune_argv.append("--enable-post-suppression-quality-expansion")
            else:
                tune_argv.append("--disable-post-suppression-quality-expansion")
            tune_argv.extend(
                [
                    "--post-suppression-min-combo-count",
                    str(tune_post_suppression_min_combo_count_iter),
                ]
            )
            if args.tune_include_legacy_scenarios:
                tune_argv.append("--include-legacy-scenarios")
            if args.real_data_only:
                tune_argv.append("--real-data-only")
            if args.require_higher_tf_companions:
                tune_argv.append("--require-higher-tf-companions")
            else:
                tune_argv.append("--allow-missing-higher-tf-companions")
            if args.enable_hostility_adaptive_targets:
                tune_argv.append("--enable-hostility-adaptive-thresholds")
                tune_argv.append("--use-effective-thresholds-for-objective")
            else:
                tune_argv.append("--disable-hostility-adaptive-thresholds")
                tune_argv.append("--disable-effective-thresholds-for-objective")
            # Keep tune-stage behavior explicit and stable across downstream default changes.
            tune_argv.append("--disable-hostility-adaptive-trades-only")
            if args.skip_core_vs_legacy_gate:
                tune_argv.append("--skip-core-vs-legacy-gate")
            rc = tune_candidate_gate_trade_density.main(tune_argv)
            if rc != 0:
                raise RuntimeError(f"Candidate tuning failed (exit={rc})")
            if not tune_summary_path.exists():
                raise RuntimeError(f"Tune summary json not found: {tune_summary_path}")
            tune_holdout_suppression_snapshot = read_tune_holdout_suppression_snapshot(tune_summary_path)
            tune_post_suppression_expansion_snapshot = read_tune_post_suppression_expansion_snapshot(
                tune_summary_path
            )

            best_combo = select_best_combo_from_tune_summary(
                tune_summary_path,
                tune_min_avg_trades_iter,
                tune_min_profitable_ratio_iter,
                tune_min_avg_win_rate_iter,
                tune_min_expectancy_iter,
            )
            print(
                f"[AutoImprove] Iteration {iteration} selected_combo={best_combo['combo_id']} "
                f"objective={best_combo['objective_with_gate_bonus']}, "
                f"source={best_combo.get('selection_source', '')}, "
                f"selector={best_combo.get('selector_mode_applied', '')}"
            )
            if bool(tune_holdout_suppression_snapshot.get("active", False)):
                print(
                    "[AutoImprove] Holdout suppression active: "
                    f"reason={tune_holdout_suppression_snapshot.get('reason', '')}, "
                    f"suppressed={tune_holdout_suppression_snapshot.get('suppressed_combo_count', 0)}, "
                    f"kept={tune_holdout_suppression_snapshot.get('kept_combo_count', 0)}"
                )
            if bool(tune_post_suppression_expansion_snapshot.get("applied", False)):
                print(
                    "[AutoImprove] Post-suppression quality expansion: "
                    f"injected={tune_post_suppression_expansion_snapshot.get('injected_combo_count', 0)}, "
                    f"reason={tune_post_suppression_expansion_snapshot.get('reason', '')}"
                )
            selected_combo_id = str(best_combo["combo_id"])
            selected_combo_payload = dict(best_combo["combo"])
            skip_post_apply_run = (
                selected_combo_id == last_applied_combo_id
                and last_applied_combo is not None
                and selected_combo_payload == last_applied_combo
            )
            if skip_post_apply_run:
                print(
                    f"[AutoImprove] Iteration {iteration} post-apply validation skipped "
                    f"(combo unchanged: {selected_combo_id})"
                )
                post_snapshot = snapshot
            else:
                apply_combo_to_config_files(
                    build_config_path,
                    source_config_path,
                    best_combo["combo"],
                    bool(args.sync_source_config),
                )
                last_applied_combo_id = selected_combo_id
                last_applied_combo = selected_combo_payload
    
                print(f"[AutoImprove] Iteration {iteration}/{args.max_iterations} - post-apply validation run")
                rc = run_realdata_candidate_loop.main(real_loop_argv)
                if rc != 0:
                    raise RuntimeError(f"Realdata candidate loop (post-apply) failed (exit={rc})")
                if not gate_report_path.exists():
                    raise RuntimeError(f"Gate report not found after post-apply run: {gate_report_path}")
    
                post_snapshot = get_core_snapshot(
                    gate_report_path,
                    float(args.min_profit_factor),
                    tune_objective_min_avg_trades,
                    tune_objective_min_profitable_ratio,
                    tune_objective_min_avg_win_rate_pct,
                    tune_objective_min_expectancy_krw,
                    bool(args.enable_hostility_adaptive_targets),
                )
            post_entry_rejection_snapshot = read_entry_rejection_snapshot(entry_rejection_summary_path)
            post_entry_rejection_taxonomy_snapshot = read_strategy_rejection_taxonomy_snapshot(
                strategy_rejection_taxonomy_path
            )
            post_live_signal_funnel_snapshot = read_live_signal_funnel_snapshot(
                live_signal_funnel_taxonomy_path
            )
            post_core = post_snapshot["core"]
            post_active_thresholds = post_snapshot["active_thresholds"]
            post_threshold_context = post_snapshot.get("threshold_context") or {}
            post_blended_ctx = post_threshold_context.get("blended_context") or {}
            post_target = target_satisfied(
                post_core,
                float(post_active_thresholds["min_profit_factor"]),
                float(post_active_thresholds["min_expectancy_krw"]),
                float(post_active_thresholds["min_profitable_ratio"]),
                float(post_active_thresholds["min_avg_trades"]),
                float(post_active_thresholds["min_avg_win_rate_pct"]),
            )
            post_quality_fail = pf_expectancy_quality_fail(
                core_summary=post_core,
                active_thresholds=post_active_thresholds,
                require_both=bool(args.holdout_suppression_persist_require_both_pf_exp_fail),
            )
            holdout_suppression_effective = bool(
                tune_holdout_suppression_snapshot.get("active", False)
                and (
                    int(tune_holdout_suppression_snapshot.get("suppressed_combo_count", 0) or 0) > 0
                    or bool(tune_holdout_suppression_snapshot.get("fail_open_all_suppressed", False))
                )
            )
            holdout_suppression_persist_triggered_iter = False
            if (
                bool(args.enable_holdout_suppression_persist_stop)
                and bool(holdout_suppression_effective)
                and bool(post_quality_fail["quality_fail"])
            ):
                holdout_suppression_persist_streak += 1
            else:
                holdout_suppression_persist_streak = 0
            if (
                bool(args.enable_holdout_suppression_persist_stop)
                and holdout_suppression_persist_streak
                >= max(1, int(args.holdout_suppression_persist_iterations))
            ):
                holdout_suppression_persist_triggered_iter = True
                holdout_suppression_persist_triggered = True
                holdout_suppression_persist_trigger_iteration = iteration
            pivot_meta_iter = build_persist_directional_pivot(
                selected_combo_family=str(best_combo.get("bottleneck_scenario_family", "")),
                entry_rejection_top_group=str(post_entry_rejection_taxonomy_snapshot.get("overall_top_group", "")),
                expansion_snapshot=tune_post_suppression_expansion_snapshot,
                suppression_effective=bool(holdout_suppression_effective),
                quality_fail=bool(post_quality_fail["quality_fail"]),
                current_scenario_mode=str(tune_scenario_mode_iter),
                entry_timing_scenario_mode=str(entry_timing_scenario_mode),
                exit_risk_scenario_mode=str(exit_risk_scenario_mode),
            )
            last_persist_pivot_direction = str(pivot_meta_iter.get("direction", ""))
            last_persist_pivot_recommendation = str(pivot_meta_iter.get("recommendation", ""))
            last_persist_pivot_next_scenario_mode = str(pivot_meta_iter.get("next_scenario_mode", ""))
            last_persist_pivot_reason_code = str(pivot_meta_iter.get("reason_code", ""))
            last_persist_patch_template = dict(pivot_meta_iter.get("patch_template") or {})
            if (
                bool(args.enable_persist_directional_pivot)
                and bool(pivot_meta_iter.get("active", False))
                and not bool(holdout_suppression_persist_triggered_iter)
                and normalize_tune_scenario_mode(str(args.tune_scenario_mode), "quality_focus") != "legacy_only"
            ):
                next_mode = normalize_tune_scenario_mode(
                    str(pivot_meta_iter.get("next_scenario_mode", "")),
                    tune_scenario_mode_iter,
                )
                next_tune_scenario_mode_override = next_mode
                next_tune_scenario_mode_override_reason = str(pivot_meta_iter.get("reason_code", ""))
                next_tune_directional_hint = str(pivot_meta_iter.get("direction", ""))
                next_tune_patch_plan_template_id = str(
                    (pivot_meta_iter.get("patch_template") or {}).get("template_id", "")
                )
                next_tune_patch_plan_actions = [
                    str(x).strip()
                    for x in ((pivot_meta_iter.get("patch_template") or {}).get("checklist") or [])
                    if str(x).strip()
                ]
                print(
                    "[AutoImprove] Directional pivot prepared for next iteration: "
                    f"direction={pivot_meta_iter.get('direction', '')}, "
                    f"next_scenario_mode={next_mode}, "
                    f"reason={next_tune_scenario_mode_override_reason}"
                )
            holdout_suppression_persist_events.append(
                {
                    "iteration": iteration,
                    "active": bool(tune_holdout_suppression_snapshot.get("active", False)),
                    "reason": str(tune_holdout_suppression_snapshot.get("reason", "")),
                    "effective": bool(holdout_suppression_effective),
                    "suppressed_combo_count": int(
                        tune_holdout_suppression_snapshot.get("suppressed_combo_count", 0) or 0
                    ),
                    "kept_combo_count": int(tune_holdout_suppression_snapshot.get("kept_combo_count", 0) or 0),
                    "pf_fail": bool(post_quality_fail["pf_fail"]),
                    "expectancy_fail": bool(post_quality_fail["expectancy_fail"]),
                    "quality_fail": bool(post_quality_fail["quality_fail"]),
                    "persist_streak": int(holdout_suppression_persist_streak),
                    "persist_triggered": bool(holdout_suppression_persist_triggered_iter),
                    "selected_combo": selected_combo_id,
                    "selected_combo_family": str(best_combo.get("bottleneck_scenario_family", "")),
                    "selected_combo_selection_source": str(best_combo.get("selection_source", "")),
                    "selected_combo_selector_mode_applied": str(best_combo.get("selector_mode_applied", "")),
                    "selected_combo_selector_reason": str(best_combo.get("selector_reason", "")),
                    "selected_combo_suppressed_family": bool(
                        best_combo.get("holdout_failure_suppressed_family", False)
                    ),
                    "post_suppression_expansion_applied": bool(
                        tune_post_suppression_expansion_snapshot.get("applied", False)
                    ),
                    "post_suppression_expansion_injected_combo_count": int(
                        tune_post_suppression_expansion_snapshot.get("injected_combo_count", 0) or 0
                    ),
                    "pivot_active": bool(pivot_meta_iter.get("active", False)),
                    "pivot_direction": str(pivot_meta_iter.get("direction", "")),
                    "pivot_reason_code": str(pivot_meta_iter.get("reason_code", "")),
                    "pivot_recommendation": str(pivot_meta_iter.get("recommendation", "")),
                    "pivot_next_scenario_mode": str(pivot_meta_iter.get("next_scenario_mode", "")),
                    "pivot_patch_template_id": str(
                        (pivot_meta_iter.get("patch_template") or {}).get("template_id", "")
                    ),
                    "pivot_patch_template_action_count": int(
                        len((pivot_meta_iter.get("patch_template") or {}).get("checklist") or [])
                    ),
                    "pivot_patch_template": dict(pivot_meta_iter.get("patch_template") or {}),
                    "patch_plan_action_override_applied": bool(
                        patch_plan_action_override_iter.get("applied", False)
                    ),
                    "patch_plan_action_override_template_id": str(
                        patch_plan_action_override_iter.get("template_id", "")
                    ),
                    "patch_plan_action_override_matched_count": int(
                        len(patch_plan_action_override_iter.get("matched_actions", []) or [])
                    ),
                }
            )
            rows.append(
                {
                    "iteration": iteration,
                    "phase": "post_apply",
                    "selected_combo": selected_combo_id,
                    "selected_combo_selection_source": str(best_combo.get("selection_source", "")),
                    "selected_combo_selector_mode_applied": str(best_combo.get("selector_mode_applied", "")),
                    "selected_combo_selector_reason": str(best_combo.get("selector_reason", "")),
                    "post_apply_skipped_same_combo": bool(skip_post_apply_run),
                    "overall_gate_pass": bool(post_snapshot["overall_gate_pass"]),
                    "core_vs_legacy_gate_pass": bool(post_snapshot["core_vs_legacy_gate_pass"]),
                    "core_full_gate_pass": bool(post_core.get("gate_pass", False)),
                    "avg_profit_factor": float(post_core.get("avg_profit_factor", 0.0)),
                    "avg_expectancy_krw": float(post_core.get("avg_expectancy_krw", 0.0)),
                    "avg_total_trades": float(post_core.get("avg_total_trades", 0.0)),
                    "avg_win_rate_pct": float(post_core.get("avg_win_rate_pct", 0.0)),
                    "profitable_ratio": float(post_core.get("profitable_ratio", 0.0)),
                    "objective_score": float(post_snapshot["objective_score"]),
                    "selected_combo_objective_with_gate_bonus": float(
                        best_combo.get("objective_with_gate_bonus", 0.0)
                    ),
                    "active_min_profit_factor": float(post_active_thresholds["min_profit_factor"]),
                    "active_min_expectancy_krw": float(post_active_thresholds["min_expectancy_krw"]),
                    "active_min_profitable_ratio": float(post_active_thresholds["min_profitable_ratio"]),
                    "active_min_avg_win_rate_pct": float(post_active_thresholds["min_avg_win_rate_pct"]),
                    "active_min_avg_trades": float(post_active_thresholds["min_avg_trades"]),
                    "objective_min_profitable_ratio_iter": float(tune_min_profitable_ratio_iter),
                    "objective_min_avg_win_rate_pct_iter": float(tune_min_avg_win_rate_iter),
                    "objective_min_expectancy_krw_iter": float(tune_min_expectancy_iter),
                    "objective_min_avg_trades_iter": float(tune_min_avg_trades_iter),
                    "blended_hostility_level": str(post_blended_ctx.get("blended_hostility_level", "unknown")),
                    "blended_hostility_score": float(post_blended_ctx.get("blended_adversarial_score", 0.0)),
                    "quality_level": str((post_threshold_context.get("quality") or {}).get("quality_level", "unknown")),
                    "quality_avg_score": float((post_threshold_context.get("quality") or {}).get("avg_quality_risk_score", 0.0)),
                    "entry_rejection_top_reason": str(post_entry_rejection_snapshot.get("profile_top_reason", "")),
                    "entry_rejection_top_count": int(post_entry_rejection_snapshot.get("profile_top_count", 0) or 0),
                    "entry_rejection_overall_top_reason": str(post_entry_rejection_snapshot.get("overall_top_reason", "")),
                    "entry_rejection_overall_top_count": int(post_entry_rejection_snapshot.get("overall_top_count", 0) or 0),
                    "entry_rejection_top_group": str(post_entry_rejection_taxonomy_snapshot.get("overall_top_group", "")),
                    "entry_rejection_top_group_count": int(
                        post_entry_rejection_taxonomy_snapshot.get("overall_top_group_count", 0) or 0
                    ),
                    "entry_rejection_taxonomy_coverage_ratio": float(
                        post_entry_rejection_taxonomy_snapshot.get("taxonomy_coverage_ratio", 0.0) or 0.0
                    ),
                    "entry_rejection_taxonomy_unknown_count": int(
                        post_entry_rejection_taxonomy_snapshot.get("unknown_reason_code_count", 0) or 0
                    ),
                    "live_funnel_top_group": str(post_live_signal_funnel_snapshot.get("top_group", "")),
                    "live_funnel_top_group_count": int(post_live_signal_funnel_snapshot.get("top_group_count", 0) or 0),
                    "live_funnel_signal_generation_share": float(
                        post_live_signal_funnel_snapshot.get("signal_generation_share", 0.0) or 0.0
                    ),
                    "live_funnel_selection_hint_adjusted_ratio": float(
                        post_live_signal_funnel_snapshot.get("selection_hint_adjusted_ratio", 0.0) or 0.0
                    ),
                    "live_funnel_no_trade_bias_active": bool(
                        post_live_signal_funnel_snapshot.get("no_trade_bias_active", False)
                    ),
                    "live_funnel_trade_floor_scale": float(
                        post_live_signal_funnel_snapshot.get("recommended_trade_floor_scale", 1.0) or 1.0
                    ),
                    "hint_overfit_risk": bool(hint_overfit_risk),
                    "tune_scenario_mode_iter": str(tune_scenario_mode_iter),
                    "tune_scenario_mode_source": str(tune_scenario_mode_source),
                    "tune_objective_mode_iter": str(tune_objective_mode_iter),
                    "tune_directional_hint_iter": str(applied_directional_hint_iter),
                    "tune_holdout_suppression_hint_ratio_threshold_iter": float(
                        tune_holdout_suppression_hint_ratio_threshold_iter
                    ),
                    "tune_hint_guardrail_enabled_iter": bool(tune_enable_hint_guardrail_iter),
                    "tune_hint_guardrail_ratio_iter": float(tune_hint_guardrail_ratio_iter),
                    "tune_hint_guardrail_tighten_scale_iter": float(tune_hint_guardrail_tighten_scale_iter),
                    "patch_plan_action_override_applied": bool(
                        patch_plan_action_override_iter.get("applied", False)
                    ),
                    "patch_plan_action_override_template_id": str(
                        patch_plan_action_override_iter.get("template_id", "")
                    ),
                    "patch_plan_action_override_direction_hint": str(
                        patch_plan_action_override_iter.get("direction_hint", "")
                    ),
                    "patch_plan_action_override_matched_count": int(
                        len(patch_plan_action_override_iter.get("matched_actions", []) or [])
                    ),
                    "patch_plan_action_override_unmatched_count": int(
                        len(patch_plan_action_override_iter.get("unmatched_actions", []) or [])
                    ),
                    "tune_post_suppression_quality_expansion_enabled_iter": bool(
                        tune_enable_post_suppression_quality_expansion_iter
                    ),
                    "tune_post_suppression_min_combo_count_iter": int(
                        tune_post_suppression_min_combo_count_iter
                    ),
                    "holdout_suppression_active": bool(tune_holdout_suppression_snapshot.get("active", False)),
                    "holdout_suppression_reason": str(tune_holdout_suppression_snapshot.get("reason", "")),
                    "holdout_suppression_suppressed_combo_count": int(
                        tune_holdout_suppression_snapshot.get("suppressed_combo_count", 0) or 0
                    ),
                    "holdout_suppression_kept_combo_count": int(
                        tune_holdout_suppression_snapshot.get("kept_combo_count", 0) or 0
                    ),
                    "holdout_suppression_fail_open_all_suppressed": bool(
                        tune_holdout_suppression_snapshot.get("fail_open_all_suppressed", False)
                    ),
                    "holdout_suppression_effective": bool(holdout_suppression_effective),
                    "post_suppression_expansion_applied": bool(
                        tune_post_suppression_expansion_snapshot.get("applied", False)
                    ),
                    "post_suppression_expansion_injected_combo_count": int(
                        tune_post_suppression_expansion_snapshot.get("injected_combo_count", 0) or 0
                    ),
                    "post_suppression_expansion_reason": str(
                        tune_post_suppression_expansion_snapshot.get("reason", "")
                    ),
                    "selected_combo_family": str(best_combo.get("bottleneck_scenario_family", "")),
                    "persist_pivot_active": bool(pivot_meta_iter.get("active", False)),
                    "persist_pivot_direction": str(pivot_meta_iter.get("direction", "")),
                    "persist_pivot_reason_code": str(pivot_meta_iter.get("reason_code", "")),
                    "persist_pivot_recommendation": str(pivot_meta_iter.get("recommendation", "")),
                    "persist_pivot_next_scenario_mode": str(pivot_meta_iter.get("next_scenario_mode", "")),
                    "persist_pivot_template_id": str((pivot_meta_iter.get("patch_template") or {}).get("template_id", "")),
                    "persist_pivot_template_action_count": int(
                        len((pivot_meta_iter.get("patch_template") or {}).get("checklist") or [])
                    ),
                    "holdout_suppression_persist_streak": int(holdout_suppression_persist_streak),
                    "holdout_suppression_persist_triggered": bool(holdout_suppression_persist_triggered_iter),
                    "pf_fail_active_threshold": bool(post_quality_fail["pf_fail"]),
                    "expectancy_fail_active_threshold": bool(post_quality_fail["expectancy_fail"]),
                    "pf_expectancy_quality_fail": bool(post_quality_fail["quality_fail"]),
                    "target_satisfied": bool(post_target),
                    "timestamp": datetime.now(tz=timezone.utc).isoformat(),
                }
            )
    
            if float(post_snapshot["objective_score"]) > (best_objective + float(args.improvement_epsilon)):
                best_objective = float(post_snapshot["objective_score"])
                best_snapshot = post_snapshot
                best_combo_id = str(best_combo["combo_id"])
                best_entry_rejection_snapshot = post_entry_rejection_snapshot
                best_entry_rejection_taxonomy_snapshot = post_entry_rejection_taxonomy_snapshot
                best_live_signal_funnel_snapshot = post_live_signal_funnel_snapshot
                consecutive_no_improve = 0
            else:
                consecutive_no_improve += 1
    
            if args.run_loss_analysis:
                print(f"[AutoImprove] Iteration {iteration}/{args.max_iterations} - loss contributor analysis")
                rc = analyze_loss_contributors.main([])
                if rc != 0:
                    raise RuntimeError(f"Loss contributor analysis failed (exit={rc})")
    
            if post_target and bool(post_snapshot["overall_gate_pass"]):
                status = "success_gate_pass"
                reason = "Target metrics and overall gate passed on post-apply run."
                break

            if holdout_suppression_persist_triggered_iter:
                status = "paused_holdout_suppression_persist"
                pivot_direction = str(pivot_meta_iter.get("direction", ""))
                pivot_reco = str(pivot_meta_iter.get("recommendation", ""))
                pivot_next_mode = str(pivot_meta_iter.get("next_scenario_mode", ""))
                reason = (
                    "Holdout-family suppression remained active while PF/expectancy stayed below active thresholds "
                    f"for {holdout_suppression_persist_streak} consecutive iterations. "
                    "Stop relax-family tuning branch. "
                    f"patch_direction={pivot_direction or 'unspecified'}, "
                    f"next_scenario_mode={pivot_next_mode or tune_scenario_mode_iter}. "
                    f"{pivot_reco}"
                )
                break

            if consecutive_no_improve >= int(args.max_consecutive_no_improvement):
                status = "paused_no_improvement"
                reason = "Objective score did not improve within configured consecutive limit."
                break
    
    if status == "running":
        if args.max_iterations > 0 and rows:
            status = "paused_max_iterations"
            reason = "Reached MaxIterations without full gate pass."
        else:
            status = "paused_no_data"
            reason = "No iteration rows produced."

    with iteration_csv.open("w", encoding="utf-8", newline="") as fh:
        if rows:
            writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        else:
            fh.write("")

    if bool(args.run_patch_action_feedback_promotion_check):
        promotion_check_argv = [
            "--feedback-json",
            str(patch_action_policy_registry_feedback_json),
            "--policy-registry-json",
            str(patch_action_policy_registry_json),
            "--policy-decision-json",
            str(patch_action_policy_json),
            "--required-keep-streak",
            str(max(1, int(args.patch_action_feedback_promotion_check_required_keep_streak))),
            "--output-json",
            str(patch_action_feedback_promotion_check_json),
        ]
        if patch_action_policy_template_id:
            promotion_check_argv.extend(["--template-id", str(patch_action_policy_template_id)])
        promotion_check_rc = run_patch_action_override_feedback_promotion_check.main(
            promotion_check_argv
        )
        patch_action_feedback_promotion_check_snapshot["invoked"] = True
        patch_action_feedback_promotion_check_snapshot["exit_code"] = int(promotion_check_rc)
        if promotion_check_rc != 0:
            raise RuntimeError(
                f"Patch action feedback promotion check failed (exit={promotion_check_rc})"
            )
        if patch_action_feedback_promotion_check_json.exists():
            promotion_payload = json.loads(
                patch_action_feedback_promotion_check_json.read_text(encoding="utf-8-sig")
            )
            if isinstance(promotion_payload, dict):
                inputs = promotion_payload.get("inputs") or {}
                promotion_block = promotion_payload.get("promotion_check") or {}
                if not isinstance(inputs, dict):
                    inputs = {}
                if not isinstance(promotion_block, dict):
                    promotion_block = {}
                blockers = promotion_block.get("blocker_reason_codes") or []
                if not isinstance(blockers, list):
                    blockers = []
                patch_action_feedback_promotion_check_snapshot.update(
                    {
                        "success": True,
                        "template_id": str(inputs.get("template_id", "") or ""),
                        "promotion_ready": bool(
                            promotion_block.get("promotion_ready", False)
                        ),
                        "blocker_reason_codes": [
                            str(x) for x in blockers if str(x)
                        ],
                    }
                )

    summary = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "status": status,
        "reason": reason,
        "started_at": started_at.isoformat(),
        "ended_at": datetime.now(tz=timezone.utc).isoformat(),
        "max_iterations": int(args.max_iterations),
        "max_runtime_minutes": int(args.max_runtime_minutes),
        "max_consecutive_no_improvement": int(args.max_consecutive_no_improvement),
        "tuning": {
            "scenario_mode": args.tune_scenario_mode,
            "max_scenarios": int(args.tune_max_scenarios),
            "include_legacy": bool(args.tune_include_legacy_scenarios),
            "consume_patch_plan_handoff": bool(args.consume_patch_plan_handoff),
            "consume_patch_action_policy": bool(args.consume_patch_action_policy),
            "enable_patch_plan_action_overrides": bool(args.enable_patch_plan_action_overrides),
            "effective_enable_patch_plan_action_overrides": bool(
                effective_enable_patch_plan_action_overrides
            ),
            "patch_action_override_strength_scale": float(patch_action_override_strength_scale),
            "patch_action_policy_source": str(patch_action_policy_effective_snapshot.get("source", "")),
            "patch_action_policy_template_id": str(patch_action_policy_effective_snapshot.get("template_id", "")),
            "patch_action_policy_registry_guards_enabled": bool(
                args.enable_patch_action_policy_registry_guards
            ),
            "consume_patch_action_policy_registry_feedback": bool(
                args.consume_patch_action_policy_registry_feedback
            ),
            "run_patch_action_feedback_promotion_check": bool(
                args.run_patch_action_feedback_promotion_check
            ),
            "patch_action_feedback_promotion_check_required_keep_streak": int(
                max(1, int(args.patch_action_feedback_promotion_check_required_keep_streak))
            ),
            "patch_action_policy_min_repeat_runs": int(max(1, int(args.patch_action_policy_min_repeat_runs))),
            "patch_action_policy_max_age_hours": float(args.patch_action_policy_max_age_hours),
            "patch_action_policy_effective_min_repeat_runs": int(
                patch_action_policy_registry_guard.get("effective_min_repeat_runs", 0) or 0
            ),
            "enable_hint_impact_guardrail": bool(args.tune_enable_hint_impact_guardrail),
            "hint_impact_guardrail_ratio": float(args.tune_hint_impact_guardrail_ratio),
            "hint_impact_guardrail_tighten_scale": float(args.tune_hint_impact_guardrail_tighten_scale),
            "hint_overfit_ratio_threshold": float(args.hint_overfit_ratio_threshold),
            "hint_overfit_force_guardrail_tighten_scale": float(args.hint_overfit_force_guardrail_tighten_scale),
            "enable_hint_overfit_quality_pivot": bool(args.enable_hint_overfit_quality_pivot),
            "enable_persist_directional_pivot": bool(args.enable_persist_directional_pivot),
            "persist_entry_timing_scenario_mode": str(entry_timing_scenario_mode),
            "persist_exit_risk_scenario_mode": str(exit_risk_scenario_mode),
            "enable_holdout_suppression_persist_stop": bool(args.enable_holdout_suppression_persist_stop),
            "holdout_suppression_persist_iterations": int(
                max(1, int(args.holdout_suppression_persist_iterations))
            ),
            "holdout_suppression_persist_require_both_pf_exp_fail": bool(
                args.holdout_suppression_persist_require_both_pf_exp_fail
            ),
            "tune_enable_holdout_failure_family_suppression": bool(
                args.tune_enable_holdout_failure_family_suppression
            ),
            "tune_holdout_suppression_hint_ratio_threshold": float(
                args.tune_holdout_suppression_hint_ratio_threshold
            ),
            "tune_holdout_suppression_require_both_pf_exp_fail": bool(
                args.tune_holdout_suppression_require_both_pf_exp_fail
            ),
            "tune_enable_post_suppression_quality_expansion": bool(
                args.tune_enable_post_suppression_quality_expansion
            ),
            "tune_post_suppression_min_combo_count": int(
                max(1, int(args.tune_post_suppression_min_combo_count))
            ),
            "screen_dataset_limit": int(args.tune_screen_dataset_limit),
            "screen_top_k": int(args.tune_screen_top_k),
            "objective_min_avg_trades": tune_objective_min_avg_trades,
            "objective_min_profitable_ratio": tune_objective_min_profitable_ratio,
            "objective_min_avg_win_rate_pct": tune_objective_min_avg_win_rate_pct,
            "objective_min_expectancy_krw": tune_objective_min_expectancy_krw,
        },
        "targets": {
            "min_profit_factor": float(args.min_profit_factor),
            "min_expectancy_krw": float(args.min_expectancy_krw),
            "min_profitable_ratio": float(args.min_profitable_ratio),
            "min_avg_win_rate_pct": float(args.min_avg_win_rate_pct),
            "min_avg_trades": float(args.min_avg_trades),
            "enable_hostility_adaptive_targets": bool(args.enable_hostility_adaptive_targets),
            "skip_core_vs_legacy_gate": bool(args.skip_core_vs_legacy_gate),
        },
        "best_objective_score": float(best_objective),
        "best_combo_id": best_combo_id,
        "best_snapshot": (
            {
                "overall_gate_pass": bool(best_snapshot["overall_gate_pass"]),
                "core_vs_legacy_gate_pass": bool(best_snapshot["core_vs_legacy_gate_pass"]),
                "avg_profit_factor": float(best_snapshot["core"].get("avg_profit_factor", 0.0)),
                "avg_expectancy_krw": float(best_snapshot["core"].get("avg_expectancy_krw", 0.0)),
                "avg_total_trades": float(best_snapshot["core"].get("avg_total_trades", 0.0)),
                "avg_win_rate_pct": float(best_snapshot["core"].get("avg_win_rate_pct", 0.0)),
                "profitable_ratio": float(best_snapshot["core"].get("profitable_ratio", 0.0)),
                "core_full_gate_pass": bool(best_snapshot["core"].get("gate_pass", False)),
                "active_thresholds": best_snapshot.get("active_thresholds"),
                "threshold_context": best_snapshot.get("threshold_context"),
            }
            if best_snapshot is not None
            else None
        ),
        "best_entry_rejection_snapshot": best_entry_rejection_snapshot,
        "best_entry_rejection_taxonomy_snapshot": best_entry_rejection_taxonomy_snapshot,
        "best_live_signal_funnel_snapshot": best_live_signal_funnel_snapshot,
        "holdout_suppression_persist_policy": {
            "enabled": bool(args.enable_holdout_suppression_persist_stop),
            "iterations_threshold": int(max(1, int(args.holdout_suppression_persist_iterations))),
            "require_both_pf_exp_fail": bool(args.holdout_suppression_persist_require_both_pf_exp_fail),
            "triggered": bool(holdout_suppression_persist_triggered),
            "trigger_iteration": int(holdout_suppression_persist_trigger_iteration),
            "final_streak": int(holdout_suppression_persist_streak),
            "events": holdout_suppression_persist_events,
        },
        "persist_directional_pivot": {
            "enabled": bool(args.enable_persist_directional_pivot),
            "entry_timing_scenario_mode": str(entry_timing_scenario_mode),
            "exit_risk_scenario_mode": str(exit_risk_scenario_mode),
            "last_direction": str(last_persist_pivot_direction),
            "last_reason_code": str(last_persist_pivot_reason_code),
            "last_recommendation": str(last_persist_pivot_recommendation),
            "last_next_scenario_mode": str(last_persist_pivot_next_scenario_mode),
            "last_patch_template": dict(last_persist_patch_template),
            "next_override_pending": str(next_tune_scenario_mode_override),
            "next_override_pending_reason": str(next_tune_scenario_mode_override_reason),
            "next_directional_hint_pending": str(next_tune_directional_hint),
            "next_patch_plan_template_id_pending": str(next_tune_patch_plan_template_id),
            "next_patch_plan_action_count_pending": int(len(next_tune_patch_plan_actions or [])),
            "patch_plan_handoff_snapshot": patch_plan_handoff_snapshot,
            "patch_plan_handoff_applied": bool(patch_plan_handoff_applied),
            "patch_action_policy_snapshot": patch_action_policy_snapshot,
            "patch_action_policy_registry_snapshot": patch_action_policy_registry_snapshot,
            "patch_action_policy_registry_feedback_snapshot": patch_action_policy_registry_feedback_snapshot,
            "patch_action_policy_registry_guard_report": patch_action_policy_registry_guard,
            "patch_action_policy_effective_snapshot": patch_action_policy_effective_snapshot,
            "patch_action_feedback_promotion_check_snapshot": patch_action_feedback_promotion_check_snapshot,
            "patch_action_policy_applied": bool(patch_action_policy_applied),
            "last_action_override": dict(last_patch_plan_action_override),
        },
        "outputs": {
            "iteration_csv": str(iteration_csv),
            "summary_json": str(summary_json),
            "patch_plan_json": str(patch_plan_json),
            "patch_action_policy_json": str(patch_action_policy_json),
            "patch_action_policy_registry_json": str(patch_action_policy_registry_json),
            "patch_action_policy_registry_feedback_json": str(
                patch_action_policy_registry_feedback_json
            ),
            "patch_action_policy_guard_report_json": str(patch_action_policy_guard_report_json),
            "patch_action_feedback_promotion_check_json": str(
                patch_action_feedback_promotion_check_json
            ),
            "gate_report_json": str(gate_report_path),
            "tune_summary_json": str(tune_summary_path),
            "entry_rejection_summary_json": str(entry_rejection_summary_path),
            "strategy_rejection_taxonomy_json": str(strategy_rejection_taxonomy_path),
            "live_signal_funnel_taxonomy_json": str(live_signal_funnel_taxonomy_path),
        },
        "iterations": rows,
    }
    summary_json.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    patch_plan = build_machine_readable_patch_plan(
        rows=rows,
        status=status,
        reason=reason,
        last_direction=last_persist_pivot_direction,
        last_reason_code=last_persist_pivot_reason_code,
        last_recommendation=last_persist_pivot_recommendation,
        last_next_scenario_mode=last_persist_pivot_next_scenario_mode,
        last_patch_template=last_persist_patch_template,
        holdout_policy=summary.get("holdout_suppression_persist_policy") or {},
    )
    patch_plan_json.write_text(json.dumps(patch_plan, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("[AutoImprove] Completed")
    print(f"status={status}")
    print(f"reason={reason}")
    print(f"iteration_csv={iteration_csv}")
    print(f"summary_json={summary_json}")
    print(f"patch_plan_json={patch_plan_json}")
    print(f"patch_action_policy_guard_report_json={patch_action_policy_guard_report_json}")
    print(
        "patch_action_feedback_promotion_check_json="
        f"{patch_action_feedback_promotion_check_json}"
    )
    if best_snapshot is not None:
        print(f"best_objective={best_objective}")
        print(f"best_combo_id={best_combo_id}")
        print(f"best_avg_profit_factor={best_snapshot['core'].get('avg_profit_factor')}")
        print(f"best_avg_expectancy_krw={best_snapshot['core'].get('avg_expectancy_krw')}")
        print(f"best_avg_total_trades={best_snapshot['core'].get('avg_total_trades')}")
        print(f"best_avg_win_rate_pct={best_snapshot['core'].get('avg_win_rate_pct')}")
        print(f"best_profitable_ratio={best_snapshot['core'].get('profitable_ratio')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
