#!/usr/bin/env python3
import argparse
import csv
import json
import statistics
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict

import run_candidate_auto_improvement_loop
from _script_common import resolve_repo_path


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-json",
        "-OutputJson",
        default="./build/Release/logs/candidate_patch_action_override_ab_summary.json",
    )
    parser.add_argument(
        "--multirun-output-json",
        "-MultirunOutputJson",
        default="./build/Release/logs/candidate_patch_action_override_ab_multirun_summary.json",
    )
    parser.add_argument(
        "--policy-decision-json",
        "-PolicyDecisionJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_decision.json",
    )
    parser.add_argument(
        "--policy-registry-json",
        "-PolicyRegistryJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_registry.json",
    )
    parser.add_argument(
        "--policy-registry-feedback-json",
        "-PolicyRegistryFeedbackJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_registry_feedback.json",
    )
    parser.add_argument(
        "--update-policy-registry",
        "-UpdatePolicyRegistry",
        dest="update_policy_registry",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-update-policy-registry",
        dest="update_policy_registry",
        action="store_false",
    )
    parser.add_argument(
        "--update-policy-registry-feedback",
        "-UpdatePolicyRegistryFeedback",
        dest="update_policy_registry_feedback",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-update-policy-registry-feedback",
        dest="update_policy_registry_feedback",
        action="store_false",
    )
    parser.add_argument(
        "--feedback-min-repeat-runs-floor",
        "-FeedbackMinRepeatRunsFloor",
        type=int,
        default=2,
    )
    parser.add_argument(
        "--feedback-min-repeat-runs-ceiling",
        "-FeedbackMinRepeatRunsCeiling",
        type=int,
        default=6,
    )
    parser.add_argument(
        "--feedback-promote-min-consecutive-keeps",
        "-FeedbackPromoteMinConsecutiveKeeps",
        type=int,
        default=2,
    )
    parser.add_argument(
        "--summary-json-on",
        "-SummaryJsonOn",
        default="./build/Release/logs/candidate_auto_improvement_summary_action_override_on.json",
    )
    parser.add_argument(
        "--summary-json-off",
        "-SummaryJsonOff",
        default="./build/Release/logs/candidate_auto_improvement_summary_action_override_off.json",
    )
    parser.add_argument(
        "--iteration-csv-on",
        "-IterationCsvOn",
        default="./build/Release/logs/candidate_auto_improvement_iterations_action_override_on.csv",
    )
    parser.add_argument(
        "--iteration-csv-off",
        "-IterationCsvOff",
        default="./build/Release/logs/candidate_auto_improvement_iterations_action_override_off.csv",
    )
    parser.add_argument("--max-iterations", "-MaxIterations", type=int, default=1)
    parser.add_argument("--max-runtime-minutes", "-MaxRuntimeMinutes", type=int, default=60)
    parser.add_argument("--tune-max-scenarios", "-TuneMaxScenarios", type=int, default=2)
    parser.add_argument("--tune-screen-dataset-limit", "-TuneScreenDatasetLimit", type=int, default=1)
    parser.add_argument("--tune-screen-top-k", "-TuneScreenTopK", type=int, default=1)
    parser.add_argument(
        "--matrix-max-workers",
        "-MatrixMaxWorkers",
        type=int,
        default=1,
        help="Deprecated. Validation is forced to sequential execution.",
    )
    parser.add_argument("--matrix-backtest-retry-count", "-MatrixBacktestRetryCount", type=int, default=1)
    parser.add_argument(
        "--tune-holdout-suppression-hint-ratio-threshold",
        "-TuneHoldoutSuppressionHintRatioThreshold",
        type=float,
        default=0.60,
    )
    parser.add_argument("--real-data-only", "-RealDataOnly", action="store_true", default=True)
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
    parser.add_argument("--skip-core-vs-legacy-gate", "-SkipCoreVsLegacyGate", action="store_true", default=True)
    parser.add_argument(
        "--disable-consume-patch-plan-handoff",
        "-DisableConsumePatchPlanHandoff",
        action="store_true",
        default=False,
    )
    parser.add_argument(
        "--patch-plan-json",
        "-PatchPlanJson",
        default="./build/Release/logs/candidate_auto_improvement_patch_plan.json",
    )
    parser.add_argument(
        "--seed-patch-template",
        "-SeedPatchTemplate",
        choices=["entry_timing_v1", "exit_risk_v1"],
        default="",
    )
    parser.add_argument(
        "--require-handoff-applied",
        "-RequireHandoffApplied",
        action="store_true",
        default=False,
    )
    parser.add_argument("--repeat-runs", "-RepeatRuns", type=int, default=1)
    parser.add_argument(
        "--policy-min-expectancy-delta",
        "-PolicyMinExpectancyDelta",
        type=float,
        default=0.25,
    )
    parser.add_argument(
        "--policy-min-profit-factor-delta",
        "-PolicyMinProfitFactorDelta",
        type=float,
        default=0.03,
    )
    parser.add_argument(
        "--policy-min-profitable-ratio-delta",
        "-PolicyMinProfitableRatioDelta",
        type=float,
        default=0.02,
    )
    parser.add_argument(
        "--policy-neutral-band-epsilon",
        "-PolicyNeutralBandEpsilon",
        type=float,
        default=0.05,
    )
    return parser.parse_args(argv)


def _extract_snapshot_metrics(summary_payload: Dict[str, Any]) -> Dict[str, Any]:
    best = summary_payload.get("best_snapshot") or {}
    active = best.get("active_thresholds") or {}
    pivot = summary_payload.get("persist_directional_pivot") or {}
    last_action_override = pivot.get("last_action_override") or {}
    resolved_override = last_action_override.get("resolved") or {}
    return {
        "status": str(summary_payload.get("status", "")),
        "reason": str(summary_payload.get("reason", "")),
        "best_objective_score": float(summary_payload.get("best_objective_score", 0.0) or 0.0),
        "avg_profit_factor": float(best.get("avg_profit_factor", 0.0) or 0.0),
        "avg_expectancy_krw": float(best.get("avg_expectancy_krw", 0.0) or 0.0),
        "avg_total_trades": float(best.get("avg_total_trades", 0.0) or 0.0),
        "profitable_ratio": float(best.get("profitable_ratio", 0.0) or 0.0),
        "avg_win_rate_pct": float(best.get("avg_win_rate_pct", 0.0) or 0.0),
        "core_full_gate_pass": bool(best.get("core_full_gate_pass", False)),
        "overall_gate_pass": bool(best.get("overall_gate_pass", False)),
        "active_min_profit_factor": float(active.get("min_profit_factor", 0.0) or 0.0),
        "active_min_expectancy_krw": float(active.get("min_expectancy_krw", 0.0) or 0.0),
        "active_min_profitable_ratio": float(active.get("min_profitable_ratio", 0.0) or 0.0),
        "active_min_avg_trades": float(active.get("min_avg_trades", 0.0) or 0.0),
        "handoff_applied": bool(pivot.get("patch_plan_handoff_applied", False)),
        "handoff_direction": str((pivot.get("patch_plan_handoff_snapshot") or {}).get("direction", "")),
        "action_override_applied": bool(last_action_override.get("applied", False)),
        "action_override_template_id": str(last_action_override.get("template_id", "")),
        "action_override_matched_count": int(len(last_action_override.get("matched_actions", []) or [])),
        "action_override_resolved_objective_mode": str(resolved_override.get("objective_mode", "")),
        "action_override_resolved_min_trades": float(resolved_override.get("objective_min_avg_trades", 0.0) or 0.0),
        "action_override_resolved_gate_min_trades": int(resolved_override.get("gate_min_avg_trades", 0) or 0),
    }


def _extract_iteration_metrics(iteration_csv: Path) -> Dict[str, Any]:
    if not iteration_csv.exists():
        return {
            "exists": False,
            "post_apply_found": False,
            "baseline_found": False,
            "selected_combo_objective_with_gate_bonus_post_apply": 0.0,
            "objective_score_post_apply": 0.0,
            "objective_score_baseline": 0.0,
            "selected_combo_post_apply": "",
            "tune_objective_mode_baseline": "",
        }
    rows = []
    with iteration_csv.open("r", encoding="utf-8-sig", newline="") as fh:
        rows = list(csv.DictReader(fh))
    baseline = next((r for r in reversed(rows) if str(r.get("phase", "")) == "baseline"), None)
    post_apply = next((r for r in reversed(rows) if str(r.get("phase", "")) == "post_apply"), None)

    def _f(row: Dict[str, Any], key: str, default: float = 0.0) -> float:
        if row is None:
            return float(default)
        try:
            return float(row.get(key, default) or default)
        except (TypeError, ValueError):
            return float(default)

    return {
        "exists": True,
        "post_apply_found": bool(post_apply),
        "baseline_found": bool(baseline),
        "selected_combo_objective_with_gate_bonus_post_apply": _f(
            post_apply, "selected_combo_objective_with_gate_bonus", 0.0
        ),
        "objective_score_post_apply": _f(post_apply, "objective_score", 0.0),
        "objective_score_baseline": _f(baseline, "objective_score", 0.0),
        "selected_combo_post_apply": str((post_apply or {}).get("selected_combo", "") or ""),
        "tune_objective_mode_baseline": str((baseline or {}).get("tune_objective_mode_iter", "") or ""),
    }


def _build_loop_args(args, summary_json: Path, iteration_csv: Path, enable_action_overrides: bool):
    argv = [
        "--max-iterations",
        str(max(1, int(args.max_iterations))),
        "--max-runtime-minutes",
        str(max(1, int(args.max_runtime_minutes))),
        "--tune-max-scenarios",
        str(max(1, int(args.tune_max_scenarios))),
        "--tune-screen-dataset-limit",
        str(max(1, int(args.tune_screen_dataset_limit))),
        "--tune-screen-top-k",
        str(max(1, int(args.tune_screen_top_k))),
        "--matrix-max-workers",
        str(max(1, int(args.matrix_max_workers))),
        "--matrix-backtest-retry-count",
        str(max(1, int(args.matrix_backtest_retry_count))),
        "--tune-holdout-suppression-hint-ratio-threshold",
        str(float(args.tune_holdout_suppression_hint_ratio_threshold)),
        "--summary-json",
        str(summary_json),
        "--iteration-csv",
        str(iteration_csv),
    ]
    if args.real_data_only:
        argv.append("--real-data-only")
    if args.require_higher_tf_companions:
        argv.append("--require-higher-tf-companions")
    else:
        argv.append("--allow-missing-higher-tf-companions")
    if args.skip_core_vs_legacy_gate:
        argv.append("--skip-core-vs-legacy-gate")
    if args.disable_consume_patch_plan_handoff:
        argv.append("--disable-consume-patch-plan-handoff")
    if enable_action_overrides:
        argv.append("--enable-patch-plan-action-overrides")
    else:
        argv.append("--disable-patch-plan-action-overrides")
    return argv


def _run_one(args, summary_json: Path, iteration_csv: Path, enable_action_overrides: bool) -> Dict[str, Any]:
    argv = _build_loop_args(args, summary_json, iteration_csv, enable_action_overrides)
    rc = run_candidate_auto_improvement_loop.main(argv)
    if rc != 0:
        raise RuntimeError(
            f"Auto-improvement loop failed for action_override={'on' if enable_action_overrides else 'off'} "
            f"(exit={rc})"
        )
    if not summary_json.exists():
        raise RuntimeError(f"Summary json missing: {summary_json}")
    payload = json.loads(summary_json.read_text(encoding="utf-8-sig"))
    metrics = _extract_snapshot_metrics(payload)
    iteration_metrics = _extract_iteration_metrics(iteration_csv)
    return {
        "enable_patch_plan_action_overrides": bool(enable_action_overrides),
        "summary_json": str(summary_json),
        "iteration_csv": str(iteration_csv),
        "metrics": metrics,
        "iteration_metrics": iteration_metrics,
    }


def _aggregate_numeric(values):
    seq = [float(x) for x in values]
    if not seq:
        return {"count": 0, "mean": 0.0, "stdev": 0.0, "min": 0.0, "max": 0.0}
    return {
        "count": len(seq),
        "mean": float(round(statistics.fmean(seq), 6)),
        "stdev": float(round(statistics.pstdev(seq), 6)) if len(seq) > 1 else 0.0,
        "min": float(round(min(seq), 6)),
        "max": float(round(max(seq), 6)),
    }


def _build_policy_decision(summary: Dict[str, Any], args) -> Dict[str, Any]:
    aggregate = summary.get("aggregate") or {}
    seed_template = str(summary.get("seed_patch_template", "") or "")
    delta_pf = float(
        ((aggregate.get("avg_profit_factor_delta_on_minus_off") or {}).get("mean", 0.0) or 0.0)
    )
    delta_exp = float(
        ((aggregate.get("avg_expectancy_krw_delta_on_minus_off") or {}).get("mean", 0.0) or 0.0)
    )
    delta_ratio = float(
        ((aggregate.get("profitable_ratio_delta_on_minus_off") or {}).get("mean", 0.0) or 0.0)
    )
    delta_combo_obj = float(
        (
            (
                aggregate.get(
                    "selected_combo_objective_with_gate_bonus_post_apply_delta_on_minus_off"
                )
                or {}
            ).get("mean", 0.0)
            or 0.0
        )
    )
    handoff_on = bool(aggregate.get("handoff_applied_on_all_rounds", False))
    handoff_off = bool(aggregate.get("handoff_applied_off_all_rounds", False))
    override_on = bool(aggregate.get("action_override_applied_on_all_rounds", False))
    eps = float(max(0.0, args.policy_neutral_band_epsilon))

    decision = "manual_review"
    reason_code = "insufficient_signal"
    recommendation = (
        "A/B signal is inconclusive. Increase repeat count and data scope before making a policy change."
    )

    if not handoff_on or not handoff_off:
        decision = "invalid_experiment"
        reason_code = "handoff_not_applied_consistently"
        recommendation = (
            "Handoff consistency failed. Recheck template seeding and execution order, then rerun the experiment."
        )
    elif not override_on:
        decision = "invalid_experiment"
        reason_code = "override_not_applied_on_enabled_branch"
        recommendation = (
            "Action override was not applied on the override-enabled branch. Verify action mapping rules."
        )
    elif (
        delta_exp >= float(args.policy_min_expectancy_delta)
        or delta_pf >= float(args.policy_min_profit_factor_delta)
        or delta_ratio >= float(args.policy_min_profitable_ratio_delta)
    ):
        decision = "keep_override"
        reason_code = "post_apply_improvement_detected"
        recommendation = (
            "Post-apply improvement detected. Keep this template override and continue repeated validation."
        )
    elif (
        abs(delta_exp) <= eps
        and abs(delta_pf) <= eps
        and abs(delta_ratio) <= eps
        and abs(delta_combo_obj) > eps
    ):
        decision = "keep_override"
        reason_code = "primary_metrics_neutral_combo_shift_only"
        recommendation = (
            "Primary post-apply metrics are neutral. Keep override and continue repeated monitoring."
        )
    elif delta_exp < -eps or delta_pf < -eps or delta_ratio < -eps:
        decision = "disable_override"
        reason_code = "post_apply_regression_detected"
        recommendation = (
            "Post-apply regression detected. Disable this template override and fall back to baseline settings."
        )

    return {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "template_id": seed_template,
        "seed_patch_template": seed_template,
        "repeat_runs": int(summary.get("repeat_runs", 1) or 1),
        "inputs": {
            "policy_min_expectancy_delta": float(args.policy_min_expectancy_delta),
            "policy_min_profit_factor_delta": float(args.policy_min_profit_factor_delta),
            "policy_min_profitable_ratio_delta": float(args.policy_min_profitable_ratio_delta),
            "policy_neutral_band_epsilon": float(args.policy_neutral_band_epsilon),
        },
        "signals": {
            "avg_expectancy_krw_delta_on_minus_off_mean": delta_exp,
            "avg_profit_factor_delta_on_minus_off_mean": delta_pf,
            "profitable_ratio_delta_on_minus_off_mean": delta_ratio,
            "selected_combo_objective_with_gate_bonus_delta_on_minus_off_mean": delta_combo_obj,
            "handoff_applied_on_all_rounds": handoff_on,
            "handoff_applied_off_all_rounds": handoff_off,
            "action_override_applied_on_all_rounds": override_on,
        },
        "decision": decision,
        "reason_code": reason_code,
        "recommendation": recommendation,
    }


def _read_policy_registry(path_value: Path) -> Dict[str, Any]:
    if path_value.exists():
        try:
            payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
            if isinstance(payload, dict):
                return payload
        except Exception:
            pass
    return {
        "schema_version": 1,
        "updated_at": "",
        "latest_template_id": "",
        "latest_decision": "",
        "template_policies": {},
        "history": [],
    }


def _resolve_template_id_for_registry(summary: Dict[str, Any], seed_template: str) -> str:
    template_id = str(seed_template or "").strip()
    if template_id:
        return template_id
    runs = summary.get("runs") or {}
    on_metrics = ((runs.get("action_override_on") or {}).get("metrics") or {}) if isinstance(runs, dict) else {}
    off_metrics = ((runs.get("action_override_off") or {}).get("metrics") or {}) if isinstance(runs, dict) else {}
    for source in (on_metrics, off_metrics):
        value = str((source or {}).get("action_override_template_id", "") or "").strip()
        if value:
            return value
    return ""


def _update_policy_registry(
    path_value: Path,
    summary: Dict[str, Any],
    policy_decision: Dict[str, Any],
    template_id: str,
) -> Dict[str, Any]:
    now_iso = datetime.now(tz=timezone.utc).isoformat()
    registry = _read_policy_registry(path_value)
    if not isinstance(registry.get("template_policies"), dict):
        registry["template_policies"] = {}
    if not isinstance(registry.get("history"), list):
        registry["history"] = []

    entry = {
        "template_id": str(template_id),
        "updated_at": now_iso,
        "decision": str(policy_decision.get("decision", "") or ""),
        "reason_code": str(policy_decision.get("reason_code", "") or ""),
        "recommendation": str(policy_decision.get("recommendation", "") or ""),
        "repeat_runs": int(summary.get("repeat_runs", 1) or 1),
        "signals": dict(policy_decision.get("signals") or {}),
        "inputs": dict(policy_decision.get("inputs") or {}),
        "source": {
            "seed_patch_template": str(summary.get("seed_patch_template", "") or ""),
            "output_json": str((summary.get("artifact_paths") or {}).get("output_json", "") or ""),
            "multirun_output_json": str((summary.get("artifact_paths") or {}).get("multirun_output_json", "") or ""),
        },
    }
    registry["schema_version"] = int(registry.get("schema_version", 1) or 1)
    registry["updated_at"] = now_iso
    registry["latest_template_id"] = str(template_id)
    registry["latest_decision"] = str(policy_decision.get("decision", "") or "")
    registry["template_policies"][str(template_id)] = entry
    registry["history"].append(
        {
            "updated_at": now_iso,
            "template_id": str(template_id),
            "decision": str(policy_decision.get("decision", "") or ""),
            "reason_code": str(policy_decision.get("reason_code", "") or ""),
        }
    )
    if len(registry["history"]) > 100:
        registry["history"] = registry["history"][-100:]
    path_value.parent.mkdir(parents=True, exist_ok=True)
    path_value.write_text(json.dumps(registry, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return entry


def _read_policy_registry_feedback(path_value: Path) -> Dict[str, Any]:
    if path_value.exists():
        try:
            payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
            if isinstance(payload, dict):
                return payload
        except Exception:
            pass
    return {
        "schema_version": 1,
        "updated_at": "",
        "latest_template_id": "",
        "latest_decision": "",
        "template_feedbacks": {},
        "history": [],
    }


def _update_policy_registry_feedback(
    path_value: Path,
    policy_decision: Dict[str, Any],
    template_id: str,
    args,
) -> Dict[str, Any]:
    now_iso = datetime.now(tz=timezone.utc).isoformat()
    payload = _read_policy_registry_feedback(path_value)
    if not isinstance(payload.get("template_feedbacks"), dict):
        payload["template_feedbacks"] = {}
    if not isinstance(payload.get("history"), list):
        payload["history"] = []

    feedbacks = payload["template_feedbacks"]
    prev = feedbacks.get(str(template_id)) if isinstance(feedbacks, dict) else {}
    if not isinstance(prev, dict):
        prev = {}

    floor = max(1, int(args.feedback_min_repeat_runs_floor))
    ceiling = max(floor, int(args.feedback_min_repeat_runs_ceiling))
    promote_keep_streak_min = max(1, int(args.feedback_promote_min_consecutive_keeps))
    prev_required_runs = int(prev.get("recommended_min_repeat_runs", floor) or floor)
    prev_required_runs = max(floor, min(ceiling, prev_required_runs))

    decision = str(policy_decision.get("decision", "") or "")
    reason_code = str(policy_decision.get("reason_code", "") or "")
    repeat_runs = max(1, int(policy_decision.get("repeat_runs", 1) or 1))
    signals = dict(policy_decision.get("signals") or {})

    total_experiments = int(prev.get("total_experiments", 0) or 0) + 1
    keep_count = int(prev.get("keep_count", 0) or 0)
    decrease_count = int(prev.get("decrease_count", 0) or 0)
    disable_count = int(prev.get("disable_count", 0) or 0)
    invalid_count = int(prev.get("invalid_count", 0) or 0)
    manual_review_count = int(prev.get("manual_review_count", 0) or 0)

    if decision == "keep_override":
        keep_count += 1
    elif decision == "decrease_override_strength":
        decrease_count += 1
    elif decision == "disable_override":
        disable_count += 1
    elif decision == "invalid_experiment":
        invalid_count += 1
    else:
        manual_review_count += 1

    prev_keep_streak = int(prev.get("consecutive_keep_streak", 0) or 0)
    prev_non_keep_streak = int(prev.get("consecutive_non_keep_streak", 0) or 0)
    if decision == "keep_override":
        keep_streak = prev_keep_streak + 1
        non_keep_streak = 0
    else:
        keep_streak = 0
        non_keep_streak = prev_non_keep_streak + 1

    recommended_min_runs = int(prev_required_runs)
    allow_keep_promotion = False
    block_auto_loop_consumption = True
    reason_codes = []

    if decision == "keep_override":
        # Promote only after repeated stable keep signals.
        if repeat_runs >= prev_required_runs and keep_streak >= promote_keep_streak_min:
            allow_keep_promotion = True
            block_auto_loop_consumption = False
            recommended_min_runs = max(floor, prev_required_runs - 1)
            reason_codes.append("keep_promotion_allowed")
        else:
            recommended_min_runs = min(ceiling, max(prev_required_runs, repeat_runs))
            if repeat_runs < prev_required_runs:
                reason_codes.append("repeat_runs_below_recommended")
            if keep_streak < promote_keep_streak_min:
                reason_codes.append("insufficient_consecutive_keeps")
    elif decision == "decrease_override_strength":
        if repeat_runs >= prev_required_runs:
            block_auto_loop_consumption = False
            recommended_min_runs = min(ceiling, max(prev_required_runs, repeat_runs))
            reason_codes.append("protective_downshift_allowed")
        else:
            recommended_min_runs = int(prev_required_runs)
            reason_codes.append("repeat_runs_below_recommended")
    elif decision == "disable_override":
        if repeat_runs >= prev_required_runs:
            block_auto_loop_consumption = False
            recommended_min_runs = min(ceiling, max(prev_required_runs, repeat_runs))
            reason_codes.append("protective_disable_allowed")
        else:
            recommended_min_runs = int(prev_required_runs)
            reason_codes.append("repeat_runs_below_recommended")
    elif decision == "invalid_experiment":
        recommended_min_runs = min(ceiling, max(prev_required_runs, repeat_runs) + 2)
        reason_codes.append("invalid_experiment")
    else:
        recommended_min_runs = min(ceiling, max(prev_required_runs, repeat_runs) + 1)
        reason_codes.append("insufficient_signal")

    if not reason_codes:
        reason_codes = ["accepted"]

    entry = {
        "template_id": str(template_id),
        "updated_at": now_iso,
        "latest_decision": decision,
        "latest_reason_code": reason_code,
        "last_repeat_runs": int(repeat_runs),
        "total_experiments": int(total_experiments),
        "keep_count": int(keep_count),
        "decrease_count": int(decrease_count),
        "disable_count": int(disable_count),
        "invalid_count": int(invalid_count),
        "manual_review_count": int(manual_review_count),
        "consecutive_keep_streak": int(keep_streak),
        "consecutive_non_keep_streak": int(non_keep_streak),
        "recommended_min_repeat_runs": int(recommended_min_runs),
        "allow_keep_promotion": bool(allow_keep_promotion),
        "block_auto_loop_consumption": bool(block_auto_loop_consumption),
        "feedback_reason_codes": list(reason_codes),
        "last_signals": signals,
        "policy_thresholds": dict(policy_decision.get("inputs") or {}),
    }

    payload["schema_version"] = int(payload.get("schema_version", 1) or 1)
    payload["updated_at"] = now_iso
    payload["latest_template_id"] = str(template_id)
    payload["latest_decision"] = decision
    payload["template_feedbacks"][str(template_id)] = entry
    payload["history"].append(
        {
            "updated_at": now_iso,
            "template_id": str(template_id),
            "decision": decision,
            "reason_code": reason_code,
            "repeat_runs": int(repeat_runs),
            "recommended_min_repeat_runs": int(recommended_min_runs),
            "block_auto_loop_consumption": bool(block_auto_loop_consumption),
            "allow_keep_promotion": bool(allow_keep_promotion),
            "feedback_reason_codes": list(reason_codes),
        }
    )
    if len(payload["history"]) > 200:
        payload["history"] = payload["history"][-200:]
    path_value.parent.mkdir(parents=True, exist_ok=True)
    path_value.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return entry


def _seed_patch_plan_template(path_value: Path, template_id: str) -> None:
    if str(template_id or "").strip() == "":
        return
    if template_id == "entry_timing_v1":
        direction = "entry_timing"
        reason_code = "entry_relax_family_selected"
        next_mode = "diverse_wide"
        recommendation = (
            "entry timing emphasis: improve trigger confirmation and late-entry avoidance "
            "before further risk/exit tightening."
        )
        checklist = [
            "Add late-entry guard using recent candle extension/mean distance.",
            "Require one extra confirmation feature for weak-momentum entries.",
            "Reduce duplicate entries after first failed breakout attempt.",
            "Add cooldown after consecutive no-follow-through entries.",
        ]
        focus = "signal quality before entry"
    else:
        direction = "exit_risk"
        reason_code = "quality_exit_family_or_expansion"
        next_mode = "quality_focus"
        recommendation = (
            "exit/risk emphasis: tighten invalidation exits, volatility-aware RR, and position throttles "
            "before relaxing entry density."
        )
        checklist = [
            "Tighten invalidation exits when volatility expands after entry.",
            "Raise minimum realized RR for weak-signal positions.",
            "Throttle position adds when expectancy remains negative.",
            "Shorten hostile-regime hold time with stricter trailing exits.",
        ]
        focus = "loss control and exit quality"

    payload: Dict[str, Any] = {}
    if path_value.exists():
        try:
            raw = json.loads(path_value.read_text(encoding="utf-8-sig"))
            if isinstance(raw, dict):
                payload = raw
        except Exception:
            payload = {}

    payload["pivot"] = {
        "direction": direction,
        "reason_code": reason_code,
        "recommendation": recommendation,
        "next_scenario_mode": next_mode,
    }
    payload["patch_template"] = {
        "template_id": template_id,
        "direction": direction,
        "focus": focus,
        "checklist": checklist,
    }
    payload["actions"] = [
        {"step": idx + 1, "action_type": "code_change", "description": text}
        for idx, text in enumerate(checklist)
    ]
    payload["generated_at"] = datetime.now(tz=timezone.utc).isoformat()
    payload["status"] = str(payload.get("status", "seeded_handoff"))
    payload["reason"] = str(payload.get("reason", "seeded for action override A/B"))
    path_value.parent.mkdir(parents=True, exist_ok=True)
    path_value.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main(argv=None) -> int:
    args = parse_args(argv)
    if int(getattr(args, "matrix_max_workers", 1)) > 1:
        print(
            "[PatchActionOverrideAB] Parallel matrix workers are disabled; "
            "forcing --matrix-max-workers=1."
        )
    args.matrix_max_workers = 1
    output_json = resolve_repo_path(args.output_json)
    multirun_output_json = resolve_repo_path(args.multirun_output_json)
    policy_decision_json = resolve_repo_path(args.policy_decision_json)
    policy_registry_json = resolve_repo_path(args.policy_registry_json)
    policy_registry_feedback_json = resolve_repo_path(args.policy_registry_feedback_json)
    patch_plan_json = resolve_repo_path(args.patch_plan_json)
    summary_json_on = resolve_repo_path(args.summary_json_on)
    summary_json_off = resolve_repo_path(args.summary_json_off)
    iteration_csv_on = resolve_repo_path(args.iteration_csv_on)
    iteration_csv_off = resolve_repo_path(args.iteration_csv_off)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    multirun_output_json.parent.mkdir(parents=True, exist_ok=True)
    policy_decision_json.parent.mkdir(parents=True, exist_ok=True)
    policy_registry_json.parent.mkdir(parents=True, exist_ok=True)
    policy_registry_feedback_json.parent.mkdir(parents=True, exist_ok=True)
    patch_plan_json.parent.mkdir(parents=True, exist_ok=True)
    seed_template = str(args.seed_patch_template or "").strip()
    repeat_runs = max(1, int(args.repeat_runs))
    round_results = []
    all_warnings = []

    for round_index in range(1, repeat_runs + 1):
        if seed_template:
            _seed_patch_plan_template(patch_plan_json, seed_template)
        run_on = _run_one(args, summary_json_on, iteration_csv_on, True)
        if seed_template:
            _seed_patch_plan_template(patch_plan_json, seed_template)
        run_off = _run_one(args, summary_json_off, iteration_csv_off, False)

        on_metrics = run_on["metrics"]
        off_metrics = run_off["metrics"]
        on_iter_metrics = run_on.get("iteration_metrics") or {}
        off_iter_metrics = run_off.get("iteration_metrics") or {}
        delta = {
            "best_objective_score_delta_on_minus_off": round(
                float(on_metrics["best_objective_score"]) - float(off_metrics["best_objective_score"]), 6
            ),
            "avg_profit_factor_delta_on_minus_off": round(
                float(on_metrics["avg_profit_factor"]) - float(off_metrics["avg_profit_factor"]), 6
            ),
            "avg_expectancy_krw_delta_on_minus_off": round(
                float(on_metrics["avg_expectancy_krw"]) - float(off_metrics["avg_expectancy_krw"]), 6
            ),
            "avg_total_trades_delta_on_minus_off": round(
                float(on_metrics["avg_total_trades"]) - float(off_metrics["avg_total_trades"]), 6
            ),
            "profitable_ratio_delta_on_minus_off": round(
                float(on_metrics["profitable_ratio"]) - float(off_metrics["profitable_ratio"]), 6
            ),
            "avg_win_rate_pct_delta_on_minus_off": round(
                float(on_metrics["avg_win_rate_pct"]) - float(off_metrics["avg_win_rate_pct"]), 6
            ),
            "selected_combo_objective_with_gate_bonus_post_apply_delta_on_minus_off": round(
                float(on_iter_metrics.get("selected_combo_objective_with_gate_bonus_post_apply", 0.0) or 0.0)
                - float(off_iter_metrics.get("selected_combo_objective_with_gate_bonus_post_apply", 0.0) or 0.0),
                6,
            ),
            "objective_score_post_apply_delta_on_minus_off": round(
                float(on_iter_metrics.get("objective_score_post_apply", 0.0) or 0.0)
                - float(off_iter_metrics.get("objective_score_post_apply", 0.0) or 0.0),
                6,
            ),
            "handoff_applied_on": bool(on_metrics.get("handoff_applied", False)),
            "handoff_applied_off": bool(off_metrics.get("handoff_applied", False)),
            "action_override_applied_on": bool(on_metrics.get("action_override_applied", False)),
            "action_override_applied_off": bool(off_metrics.get("action_override_applied", False)),
        }
        warnings = []
        if not bool(on_metrics.get("handoff_applied", False)):
            warnings.append("handoff_not_applied_on_override_on")
        if not bool(off_metrics.get("handoff_applied", False)):
            warnings.append("handoff_not_applied_on_override_off")
        if args.require_handoff_applied and (
            not bool(on_metrics.get("handoff_applied", False))
            or not bool(off_metrics.get("handoff_applied", False))
        ):
            warnings.append("handoff_required_check_failed")
        if bool(on_metrics.get("handoff_applied", False)) and not bool(
            on_metrics.get("action_override_applied", False)
        ):
            warnings.append("action_override_not_applied_on_override_enabled_run")

        round_result = {
            "round": int(round_index),
            "runs": {
                "action_override_on": run_on,
                "action_override_off": run_off,
            },
            "delta_on_minus_off": delta,
            "warnings": warnings,
        }
        round_results.append(round_result)
        all_warnings.extend(warnings)

    last_round = round_results[-1]
    aggregate = {
        "best_objective_score_delta_on_minus_off": _aggregate_numeric(
            [r["delta_on_minus_off"]["best_objective_score_delta_on_minus_off"] for r in round_results]
        ),
        "avg_profit_factor_delta_on_minus_off": _aggregate_numeric(
            [r["delta_on_minus_off"]["avg_profit_factor_delta_on_minus_off"] for r in round_results]
        ),
        "avg_expectancy_krw_delta_on_minus_off": _aggregate_numeric(
            [r["delta_on_minus_off"]["avg_expectancy_krw_delta_on_minus_off"] for r in round_results]
        ),
        "avg_total_trades_delta_on_minus_off": _aggregate_numeric(
            [r["delta_on_minus_off"]["avg_total_trades_delta_on_minus_off"] for r in round_results]
        ),
        "profitable_ratio_delta_on_minus_off": _aggregate_numeric(
            [r["delta_on_minus_off"]["profitable_ratio_delta_on_minus_off"] for r in round_results]
        ),
        "avg_win_rate_pct_delta_on_minus_off": _aggregate_numeric(
            [r["delta_on_minus_off"]["avg_win_rate_pct_delta_on_minus_off"] for r in round_results]
        ),
        "selected_combo_objective_with_gate_bonus_post_apply_delta_on_minus_off": _aggregate_numeric(
            [
                r["delta_on_minus_off"]["selected_combo_objective_with_gate_bonus_post_apply_delta_on_minus_off"]
                for r in round_results
            ]
        ),
        "objective_score_post_apply_delta_on_minus_off": _aggregate_numeric(
            [r["delta_on_minus_off"]["objective_score_post_apply_delta_on_minus_off"] for r in round_results]
        ),
        "handoff_applied_on_all_rounds": bool(
            all(bool(r["delta_on_minus_off"]["handoff_applied_on"]) for r in round_results)
        ),
        "handoff_applied_off_all_rounds": bool(
            all(bool(r["delta_on_minus_off"]["handoff_applied_off"]) for r in round_results)
        ),
        "action_override_applied_on_all_rounds": bool(
            all(bool(r["delta_on_minus_off"]["action_override_applied_on"]) for r in round_results)
        ),
    }

    summary = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "seed_patch_template": str(args.seed_patch_template or ""),
        "repeat_runs": int(repeat_runs),
        "runs": dict(last_round["runs"]),
        "delta_on_minus_off": dict(last_round["delta_on_minus_off"]),
        "warnings": list(last_round["warnings"]),
        "round_results": round_results,
        "aggregate": aggregate,
        "artifact_paths": {
            "output_json": str(output_json),
            "multirun_output_json": str(multirun_output_json),
            "policy_decision_json": str(policy_decision_json),
            "policy_registry_json": str(policy_registry_json),
            "policy_registry_feedback_json": str(policy_registry_feedback_json),
            "patch_plan_json": str(patch_plan_json),
            "summary_json_on": str(summary_json_on),
            "summary_json_off": str(summary_json_off),
            "iteration_csv_on": str(iteration_csv_on),
            "iteration_csv_off": str(iteration_csv_off),
        },
    }
    output_json.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    multirun_output_json.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    policy_decision = _build_policy_decision(summary, args)
    policy_decision_json.write_text(json.dumps(policy_decision, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    registry_template_id = _resolve_template_id_for_registry(summary, seed_template)
    policy_registry_entry = {}
    policy_registry_feedback_entry = {}
    if bool(args.update_policy_registry):
        if registry_template_id:
            policy_registry_entry = _update_policy_registry(
                policy_registry_json,
                summary,
                policy_decision,
                registry_template_id,
            )
        else:
            all_warnings.append("policy_registry_template_id_missing")
    if bool(args.update_policy_registry_feedback):
        if registry_template_id:
            policy_registry_feedback_entry = _update_policy_registry_feedback(
                policy_registry_feedback_json,
                policy_decision,
                registry_template_id,
                args,
            )
        else:
            all_warnings.append("policy_registry_feedback_template_id_missing")

    if all_warnings:
        print(f"warnings={','.join(all_warnings)}")
    if args.require_handoff_applied and all_warnings:
        raise RuntimeError("패치 액션 오버라이드 A/B 실행에서 필수 handoff 검증에 실패했습니다.")

    print("[PatchActionOverrideAB] 완료")
    print(f"output_json={output_json}")
    print(f"multirun_output_json={multirun_output_json}")
    print(f"policy_decision_json={policy_decision_json}")
    print(f"policy_registry_json={policy_registry_json}")
    print(f"policy_registry_feedback_json={policy_registry_feedback_json}")
    if policy_registry_entry:
        print(
            "policy_registry_update="
            f"{policy_registry_entry.get('template_id', '')}:{policy_registry_entry.get('decision', '')}"
        )
    if policy_registry_feedback_entry:
        print(
            "policy_registry_feedback_update="
            f"{policy_registry_feedback_entry.get('template_id', '')}:"
            f"min_runs={policy_registry_feedback_entry.get('recommended_min_repeat_runs', 0)},"
            f"block={policy_registry_feedback_entry.get('block_auto_loop_consumption', True)},"
            f"allow_keep={policy_registry_feedback_entry.get('allow_keep_promotion', False)}"
        )
    print(f"repeat_runs={repeat_runs}")
    print(
        "objective_delta_on_minus_off_mean="
        f"{aggregate['best_objective_score_delta_on_minus_off']['mean']}"
    )
    print(
        "selected_combo_objective_with_gate_bonus_delta_on_minus_off_mean="
        f"{aggregate['selected_combo_objective_with_gate_bonus_post_apply_delta_on_minus_off']['mean']}"
    )
    print(
        "expectancy_delta_on_minus_off_mean="
        f"{aggregate['avg_expectancy_krw_delta_on_minus_off']['mean']}"
    )
    print(
        "pf_delta_on_minus_off_mean="
        f"{aggregate['avg_profit_factor_delta_on_minus_off']['mean']}"
    )
    print(
        "trades_delta_on_minus_off_mean="
        f"{aggregate['avg_total_trades_delta_on_minus_off']['mean']}"
    )
    print(
        "profitable_ratio_delta_on_minus_off_mean="
        f"{aggregate['profitable_ratio_delta_on_minus_off']['mean']}"
    )
    print(
        "policy_decision="
        f"{policy_decision.get('decision', '')} ({policy_decision.get('reason_code', '')})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
