#!/usr/bin/env python3
import argparse
import datetime as dt
import json
from typing import Any, Dict, List, Tuple

from _script_common import dump_json, resolve_repo_path


EXPECTED_REASON_CODES: List[str] = [
    "no_signal_generated",
    "filtered_out_by_manager",
    "filtered_out_by_manager_policy_block",
    "filtered_out_by_manager_strength",
    "filtered_out_by_manager_expected_value",
    "filtered_out_by_manager_ev_quality_floor",
    "filtered_out_by_manager_low_reward_risk",
    "filtered_out_by_manager_trend_off_regime_strength",
    "filtered_out_by_manager_trend_off_regime_ev",
    "filtered_out_by_policy",
    "no_best_signal",
    "no_best_signal_no_directional_candidates",
    "no_best_signal_high_stress_single_candidate",
    "no_best_signal_single_candidate_quality_floor",
    "no_best_signal_policy_block",
    "no_best_signal_negative_expected_value",
    "no_best_signal_low_reward_risk",
    "no_best_signal_trend_off_regime_low_edge",
    "no_best_signal_no_trade_bias_low_edge",
    "no_best_signal_no_trade_bias_low_reward_risk",
    "no_best_signal_low_win_rate_history",
    "no_best_signal_low_profit_factor_history",
    "no_best_signal_low_reliability_combo",
    "no_best_signal_no_scored_candidates",
    "blocked_pattern_gate",
    "blocked_pattern_missing_archetype",
    "blocked_pattern_strength_or_regime",
    "blocked_rr_rebalance",
    "blocked_risk_gate",
    "blocked_risk_gate_strategy_ev",
    "blocked_risk_gate_regime",
    "blocked_risk_gate_entry_quality",
    "blocked_risk_gate_other",
    "blocked_second_stage_confirmation",
    "blocked_min_order_or_capital",
    "blocked_risk_manager",
    "blocked_order_sizing",
    "skipped_due_to_open_position",
]


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--entry-rejection-summary-json",
        "-EntryRejectionSummaryJson",
        default=r".\build\Release\logs\entry_rejection_summary_realdata.json",
    )
    parser.add_argument(
        "--output-json",
        "-OutputJson",
        default=r".\build\Release\logs\strategy_rejection_taxonomy_report.json",
    )
    parser.add_argument(
        "--live-signal-funnel-taxonomy-json",
        "-LiveSignalFunnelTaxonomyJson",
        default=r".\build\Release\logs\live_signal_funnel_taxonomy_report.json",
    )
    parser.add_argument("--top-k", "-TopK", type=int, default=12)
    return parser.parse_args(argv)


def sorted_items(counts: Dict[str, int]) -> List[Tuple[str, int]]:
    return sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))


def merge_reason_counts(dst: Dict[str, int], src: Dict[str, Any]) -> None:
    for raw_reason, raw_count in src.items():
        reason = str(raw_reason).strip()
        if not reason:
            continue
        try:
            count = int(raw_count)
        except Exception:
            continue
        if count <= 0:
            continue
        dst[reason] = dst.get(reason, 0) + count


def classify_reason_group(reason: str) -> str:
    if reason == "no_signal_generated":
        return "signal_generation"
    if reason == "filtered_out_by_manager" or reason.startswith("filtered_out_by_manager_"):
        return "manager_prefilter"
    if reason == "filtered_out_by_policy":
        return "policy_gate"
    if reason == "no_best_signal" or reason.startswith("no_best_signal_"):
        return "best_signal_selection"
    if reason == "skipped_due_to_open_position":
        return "position_state"
    if reason.startswith("blocked_pattern_") or reason == "blocked_pattern_gate":
        return "pattern_gate"
    if reason == "blocked_risk_gate" or reason.startswith("blocked_risk_gate_"):
        return "risk_gate"
    if reason in ("blocked_rr_rebalance", "blocked_risk_manager"):
        return "risk_gate"
    if reason in ("blocked_second_stage_confirmation", "blocked_min_order_or_capital", "blocked_order_sizing"):
        return "execution_constraints"
    return "other"


def build_live_hint_impact_snapshot(path_value) -> Dict[str, Any]:
    source_path = resolve_repo_path(str(path_value))
    snapshot: Dict[str, Any] = {
        "source_json": str(source_path),
        "source_exists": False,
        "selection_call_count": 0,
        "selection_scored_candidate_count": 0,
        "selection_hint_adjusted_candidate_count": 0,
        "selection_hint_adjusted_ratio": 0.0,
        "selection_hint_adjustment_counts": {},
        "top_selection_hint_adjustments": [],
        "no_trade_bias_active": False,
        "signal_generation_share": 0.0,
        "manager_prefilter_share": 0.0,
        "position_state_share": 0.0,
        "dominant_rejection_group": "",
    }
    if not source_path.exists():
        return snapshot

    try:
        payload = json.loads(source_path.read_text(encoding="utf-8-sig"))
    except Exception:
        return snapshot

    snapshot["source_exists"] = True
    snapshot["selection_call_count"] = int(payload.get("selection_call_count", 0) or 0)
    snapshot["selection_scored_candidate_count"] = int(payload.get("selection_scored_candidate_count", 0) or 0)
    snapshot["selection_hint_adjusted_candidate_count"] = int(
        payload.get("selection_hint_adjusted_candidate_count", 0) or 0
    )
    snapshot["selection_hint_adjusted_ratio"] = float(payload.get("selection_hint_adjusted_ratio", 0.0) or 0.0)
    hint_counts = payload.get("selection_hint_adjustment_counts") or {}
    if isinstance(hint_counts, dict):
        snapshot["selection_hint_adjustment_counts"] = {
            str(k): int(v)
            for k, v in hint_counts.items()
            if str(k).strip() and isinstance(v, (int, float))
        }
    top_adjustments = payload.get("top_selection_hint_adjustments") or []
    if isinstance(top_adjustments, list):
        snapshot["top_selection_hint_adjustments"] = [
            row for row in top_adjustments if isinstance(row, dict)
        ][:8]

    snapshot["no_trade_bias_active"] = bool(payload.get("no_trade_bias_active", False))
    snapshot["signal_generation_share"] = float(payload.get("signal_generation_share", 0.0) or 0.0)
    snapshot["manager_prefilter_share"] = float(payload.get("manager_prefilter_share", 0.0) or 0.0)
    snapshot["position_state_share"] = float(payload.get("position_state_share", 0.0) or 0.0)

    top_groups = payload.get("top_rejection_groups") or []
    if isinstance(top_groups, list) and top_groups:
        first = top_groups[0]
        if isinstance(first, dict):
            snapshot["dominant_rejection_group"] = str(first.get("group", "") or "")

    return snapshot


def main(argv=None) -> int:
    args = parse_args(argv)
    summary_path = resolve_repo_path(args.entry_rejection_summary_json)
    output_path = resolve_repo_path(args.output_json)
    live_signal_funnel_path = resolve_repo_path(args.live_signal_funnel_taxonomy_json)
    top_k = max(1, int(args.top_k))

    if not summary_path.exists():
        raise FileNotFoundError(f"entry rejection summary not found: {summary_path}")

    payload = json.loads(summary_path.read_text(encoding="utf-8-sig"))
    profile_counts = payload.get("profile_overall_reason_counts") or {}

    observed_counts: Dict[str, int] = {}
    per_profile_counts: Dict[str, Dict[str, int]] = {}
    if isinstance(profile_counts, dict):
        for raw_profile_id, raw_counts in profile_counts.items():
            profile_id = str(raw_profile_id).strip()
            if not profile_id or not isinstance(raw_counts, dict):
                continue
            bucket: Dict[str, int] = {}
            merge_reason_counts(bucket, raw_counts)
            per_profile_counts[profile_id] = bucket
            merge_reason_counts(observed_counts, bucket)

    if not observed_counts:
        overall_top = payload.get("overall_top_reasons") or []
        for row in overall_top:
            if not isinstance(row, dict):
                continue
            reason = str(row.get("reason", "")).strip()
            if not reason:
                continue
            try:
                count = int(row.get("count", 0))
            except Exception:
                continue
            if count > 0:
                observed_counts[reason] = observed_counts.get(reason, 0) + count

    expected_set = set(EXPECTED_REASON_CODES)
    observed_set = set(observed_counts.keys())

    unknown_reason_codes = sorted(observed_set - expected_set)
    missing_expected_reason_codes = sorted(expected_set - observed_set)

    group_counts: Dict[str, int] = {}
    for reason, count in observed_counts.items():
        group = classify_reason_group(reason)
        group_counts[group] = group_counts.get(group, 0) + int(count)

    group_top_reasons: Dict[str, List[Dict[str, Any]]] = {}
    for group in sorted(group_counts.keys()):
        rows = [
            {"reason": reason, "count": count}
            for reason, count in sorted_items(observed_counts)
            if classify_reason_group(reason) == group
        ][:top_k]
        group_top_reasons[group] = rows

    per_profile_top: Dict[str, Dict[str, Any]] = {}
    for profile_id, counts in sorted(per_profile_counts.items(), key=lambda kv: kv[0]):
        top = sorted_items(counts)
        per_profile_top[profile_id] = {
            "total": int(sum(counts.values())),
            "top_reason": top[0][0] if top else "",
            "top_reason_count": int(top[0][1]) if top else 0,
            "top_reasons": [{"reason": reason, "count": count} for reason, count in top[:top_k]],
        }

    taxonomy_coverage_ratio = (
        float(len(observed_set & expected_set)) / float(max(1, len(observed_set)))
    )
    live_hint_impact = build_live_hint_impact_snapshot(live_signal_funnel_path)
    report = {
        "generated_at": dt.datetime.now().astimezone().isoformat(),
        "taxonomy_version": "v1",
        "entry_rejection_summary_json": str(summary_path),
        "live_signal_funnel_taxonomy_json": str(live_signal_funnel_path),
        "observed_reason_count": len(observed_set),
        "expected_reason_count": len(expected_set),
        "taxonomy_coverage_ratio": round(taxonomy_coverage_ratio, 6),
        "unknown_reason_codes": unknown_reason_codes,
        "missing_expected_reason_codes": missing_expected_reason_codes,
        "overall_reason_counts": dict(sorted_items(observed_counts)),
        "overall_top_reasons": [
            {"reason": reason, "count": count} for reason, count in sorted_items(observed_counts)[:top_k]
        ],
        "group_counts": dict(sorted_items(group_counts)),
        "group_top_reasons": group_top_reasons,
        "per_profile_top_reasons": per_profile_top,
        "expected_reason_codes": EXPECTED_REASON_CODES,
        "live_hint_impact": live_hint_impact,
    }

    dump_json(output_path, report)
    print("[StrategyRejectionTaxonomy] Completed")
    print(f"entry_rejection_summary_json={summary_path}")
    print(f"output_json={output_path}")
    if report["overall_top_reasons"]:
        top = report["overall_top_reasons"][0]
        print(f"overall_top_reason={top['reason']}:{top['count']}")
    print(
        "live_hint_adjusted_ratio="
        f"{report['live_hint_impact'].get('selection_hint_adjusted_ratio', 0.0)}"
    )
    print(f"taxonomy_coverage_ratio={report['taxonomy_coverage_ratio']}")
    print(f"unknown_reason_codes={len(report['unknown_reason_codes'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
