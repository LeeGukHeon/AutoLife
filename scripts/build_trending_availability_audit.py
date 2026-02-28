#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Tuple


REGIME_CANONICAL = {
    "RANGING": "RANGING",
    "TRENDING_UP": "TRENDING_UP",
    "TRENDING_DOWN": "TRENDING_DOWN",
    "HIGH_VOLATILITY": "HIGH_VOL",
    "HIGH_VOL": "HIGH_VOL",
    "VOLATILE": "HIGH_VOL",
    "UNKNOWN": "UNKNOWN",
}

REGIME_ORDER = ["RANGING", "TRENDING_UP", "TRENDING_DOWN", "HIGH_VOL", "UNKNOWN"]
TRENDING_SET = {"TRENDING_UP", "TRENDING_DOWN"}
EXEC_VETO_KEY_HINTS = (
    "execution_guard",
    "execution_veto",
    "realtime_entry_veto",
    "veto_spread",
    "veto_imbalance",
    "veto_rapid_drop",
    "spread_veto",
    "imbalance_veto",
    "rapid_drop",
)


def now_iso() -> str:
    return datetime.now().astimezone().isoformat()


def to_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        try:
            return int(float(value))
        except Exception:
            return int(default)


def canonical_regime(raw: str) -> str:
    token = str(raw or "").strip().upper()
    if not token:
        return "UNKNOWN"
    return REGIME_CANONICAL.get(token, token if token in REGIME_ORDER else "UNKNOWN")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build E1.1 trending availability audit JSON.")
    parser.add_argument("--report-json", required=True, help="verification_report_*.json path")
    parser.add_argument("--output-json", required=True, help="output audit json path")
    parser.add_argument("--runtime-config", default="build/Release/config/config.json")
    parser.add_argument(
        "--bundle-path",
        default="config/model/probabilistic_runtime_bundle_v2_a19_margin_observe_only_step1.json",
    )
    parser.add_argument("--split-manifest", default="build/Release/logs/time_split_manifest_r21_prefix.json")
    parser.add_argument("--dataset-root", default="data/backtest_real")
    parser.add_argument("--prewarm-hours", type=int, default=168)
    parser.add_argument("--trend-scarcity-threshold", type=float, default=0.05)
    parser.add_argument("--trend-scarcity-min-count", type=int, default=50)
    return parser.parse_args()


def sum_phase3_rows(per_dataset: List[Dict[str, Any]]) -> Tuple[Counter, Counter]:
    candidate_by_regime: Counter = Counter()
    pass_by_regime: Counter = Counter()
    for ds in per_dataset:
        phase3_diag = ds.get("phase3_diagnostics_v2", {}) if isinstance(ds, dict) else {}
        rows = phase3_diag.get("pass_rate_by_regime", []) if isinstance(phase3_diag, dict) else []
        if not isinstance(rows, list):
            rows = []
        for row in rows:
            if not isinstance(row, dict):
                continue
            regime = canonical_regime(row.get("label", "UNKNOWN"))
            candidate_by_regime[regime] += max(0, to_int(row.get("candidate_total", 0)))
            pass_by_regime[regime] += max(0, to_int(row.get("pass_total", 0)))
    return candidate_by_regime, pass_by_regime


def sum_no_signal_patterns(per_dataset: List[Dict[str, Any]]) -> Counter:
    out: Counter = Counter()
    for ds in per_dataset:
        pattern_counts = ds.get("no_signal_pattern_counts", {}) if isinstance(ds, dict) else {}
        if not isinstance(pattern_counts, dict):
            continue
        for pattern, raw_count in pattern_counts.items():
            text = str(pattern)
            match = re.search(r"regime=([^|]+)", text)
            if not match:
                continue
            regime = canonical_regime(match.group(1))
            out[regime] += max(0, to_int(raw_count))
    return out


def sum_regime_disabled(per_dataset: List[Dict[str, Any]]) -> Counter:
    out: Counter = Counter()
    for ds in per_dataset:
        regime_disable = ds.get("regime_entry_disable", {}) if isinstance(ds, dict) else {}
        by_regime = regime_disable.get("reject_regime_entry_disabled_by_regime", {}) if isinstance(regime_disable, dict) else {}
        if not isinstance(by_regime, dict):
            continue
        for raw_regime, raw_count in by_regime.items():
            regime = canonical_regime(raw_regime)
            out[regime] += max(0, to_int(raw_count))
    return out


def collect_exec_veto_counts(per_dataset: List[Dict[str, Any]]) -> Counter:
    out: Counter = Counter()
    for ds in per_dataset:
        reason_counts = ds.get("rejection_reason_counts", {}) if isinstance(ds, dict) else {}
        if not isinstance(reason_counts, dict):
            continue
        for reason, raw_count in reason_counts.items():
            reason_key = str(reason)
            key_l = reason_key.lower()
            if any(h in key_l for h in EXEC_VETO_KEY_HINTS):
                out[reason_key] += max(0, to_int(raw_count))
    return out


def collect_exec_veto_from_stage_funnel(report: Dict[str, Any]) -> Counter:
    out: Counter = Counter()
    diagnostics = report.get("diagnostics", {}) if isinstance(report, dict) else {}
    if not isinstance(diagnostics, dict):
        return out
    stage_funnel = diagnostics.get("a10_2_stage_funnel", {})
    if not isinstance(stage_funnel, dict):
        return out
    reason_counts = stage_funnel.get("order_block_reason_counts_core", {})
    if not isinstance(reason_counts, dict):
        return out
    for reason, raw_count in reason_counts.items():
        reason_key = str(reason)
        key_l = reason_key.lower()
        if any(h in key_l for h in EXEC_VETO_KEY_HINTS):
            out[reason_key] += max(0, to_int(raw_count))
    return out


def top_rows(counter_obj: Counter, limit: int = 3) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for reason, count in counter_obj.most_common(limit):
        rows.append({"reason": str(reason), "count": int(count)})
    return rows


def safe_div(numerator: float, denominator: float) -> float:
    if denominator <= 0.0:
        return 0.0
    return float(numerator) / float(denominator)


def main() -> int:
    args = parse_args()
    report_path = Path(args.report_json).resolve()
    output_path = Path(args.output_json).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    report = json.loads(report_path.read_text(encoding="utf-8"))
    diagnostics = report.get("diagnostics", {}) if isinstance(report, dict) else {}
    per_dataset = diagnostics.get("per_dataset", []) if isinstance(diagnostics, dict) else []
    if not isinstance(per_dataset, list):
        per_dataset = []

    no_signal_by_regime = sum_no_signal_patterns(per_dataset)
    candidate_by_regime, pass_by_regime = sum_phase3_rows(per_dataset)
    disabled_by_regime = sum_regime_disabled(per_dataset)

    scan_by_regime: Counter = Counter()
    for regime in REGIME_ORDER:
        scan_by_regime[regime] = (
            no_signal_by_regime.get(regime, 0)
            + candidate_by_regime.get(regime, 0)
            + disabled_by_regime.get(regime, 0)
        )

    trending_scan_count = int(scan_by_regime.get("TRENDING_UP", 0) + scan_by_regime.get("TRENDING_DOWN", 0))
    scan_total = int(sum(scan_by_regime.values()))
    trending_scan_share = safe_div(trending_scan_count, max(1, scan_total))

    trending_candidate_generated = int(
        candidate_by_regime.get("TRENDING_UP", 0) + candidate_by_regime.get("TRENDING_DOWN", 0)
    )
    trending_manager_pass = int(pass_by_regime.get("TRENDING_UP", 0) + pass_by_regime.get("TRENDING_DOWN", 0))

    aggregate_diag = diagnostics.get("aggregate", {}) if isinstance(diagnostics, dict) else {}
    if not isinstance(aggregate_diag, dict):
        aggregate_diag = {}

    phase3_root = diagnostics.get("phase3_diagnostics_v2", {})
    if not isinstance(phase3_root, dict) or not phase3_root:
        phase3_root = aggregate_diag.get("phase3_diagnostics_v2", {})
    if not isinstance(phase3_root, dict):
        phase3_root = {}

    phase3_funnel = phase3_root.get("funnel_breakdown", {})
    if not isinstance(phase3_funnel, dict):
        phase3_funnel = {}
    reject_frontier_fail_total = max(0, to_int(phase3_funnel.get("reject_frontier_fail", 0)))
    trending_frontier_fail = int(min(trending_candidate_generated, reject_frontier_fail_total))
    trending_frontier_pass = int(max(0, trending_candidate_generated - trending_frontier_fail))

    a10_2_funnel = diagnostics.get("a10_2_stage_funnel", {}).get("funnel_summary_core", {})
    if not isinstance(a10_2_funnel, dict):
        a10_2_funnel = {}
    orders_submitted_core = max(0, to_int(a10_2_funnel.get("S4_orders_submitted_core", 0)))
    orders_filled_core = max(0, to_int(a10_2_funnel.get("S5_orders_filled_core", 0)))

    non_trending_candidate = int(
        scan_by_regime.get("RANGING", 0)
        + scan_by_regime.get("HIGH_VOL", 0)
        + scan_by_regime.get("UNKNOWN", 0)
    )
    orders_regime_assumption = (
        "RANGING real-entry OFF hard lock in effect; S4/S5 treated as TRENDING-only"
    )

    frontier_top3_raw = (
        diagnostics.get("frontier_fail_breakdown", {}).get("frontier_fail_breakdown_top3", [])
    )
    frontier_top3: List[Dict[str, Any]] = []
    if isinstance(frontier_top3_raw, list):
        for row in frontier_top3_raw[:3]:
            if not isinstance(row, dict):
                continue
            frontier_top3.append(
                {
                    "reason": str(row.get("reason", "")),
                    "count": max(0, to_int(row.get("count", 0))),
                    "description": str(row.get("description", "")),
                }
            )

    exec_veto_counts = collect_exec_veto_counts(per_dataset)
    exec_veto_counts.update(collect_exec_veto_from_stage_funnel(report))
    exec_veto_top3 = top_rows(exec_veto_counts, limit=3)

    case = "CASE_B_GATE_BLOCK"
    case_reason = "TRENDING scan share is meaningful but TRENDING candidate flow fails before order fill."
    if trending_scan_count < int(args.trend_scarcity_min_count) or trending_scan_share < float(args.trend_scarcity_threshold):
        case = "CASE_A_TREND_SCARCITY"
        case_reason = "TRENDING scan share/count is too small in current split core window."
    elif trending_candidate_generated <= 0:
        case = "CASE_B_GATE_BLOCK"
        case_reason = "TRENDING scan exists but TRENDING candidate generation is zero."
    elif orders_filled_core > 0:
        case = "CASE_B_GATE_BLOCK_PARTIAL_RELIEF"
        case_reason = "TRENDING fills are observed, but frontier fail remains dominant and further gate tuning may still be needed."
    elif trending_frontier_pass <= 0:
        case = "CASE_B_GATE_BLOCK"
        case_reason = "TRENDING candidates exist but frontier pass is zero."
    elif trending_manager_pass <= 0:
        case = "CASE_B_GATE_BLOCK"
        case_reason = "TRENDING frontier pass exists but manager pass is zero."
    elif orders_filled_core <= 0:
        case = "CASE_B_GATE_BLOCK"
        case_reason = "TRENDING manager pass exists but no filled orders in core."

    out = {
        "generated_at": now_iso(),
        "hard_lock": {
            "runtime_config": str(Path(args.runtime_config)),
            "bundle": str(Path(args.bundle_path)),
            "policy": "RANGING real-entry OFF + TRENDING-only real execution + RANGING shadow ON",
            "allow_live_orders": False,
            "live_paper_fixed_initial_capital_krw": 200000,
            "split_manifest": str(Path(args.split_manifest)),
            "dataset_root": str(Path(args.dataset_root)),
            "execution_prewarm_hours": int(args.prewarm_hours),
        },
        "source_report": str(report_path),
        "split_name": str((report.get("split_filter", {}) or {}).get("split_name", "")),
        "regime_distribution_scan_basis": {
            "count_regime_RANGING": int(scan_by_regime.get("RANGING", 0)),
            "count_regime_TRENDING_UP": int(scan_by_regime.get("TRENDING_UP", 0)),
            "count_regime_TRENDING_DOWN": int(scan_by_regime.get("TRENDING_DOWN", 0)),
            "count_regime_HIGH_VOL": int(scan_by_regime.get("HIGH_VOL", 0)),
            "count_regime_UNKNOWN": int(scan_by_regime.get("UNKNOWN", 0)),
            "count_total": int(scan_total),
            "count_trending_total": int(trending_scan_count),
            "share_trending_total": round(trending_scan_share, 6),
            "method_components": {
                "no_signal_pattern_counts_by_regime": {k: int(v) for k, v in no_signal_by_regime.items()},
                "phase3_candidate_count_by_regime": {k: int(v) for k, v in candidate_by_regime.items()},
                "reject_regime_entry_disabled_by_regime": {k: int(v) for k, v in disabled_by_regime.items()},
            },
        },
        "trending_candidate_funnel": {
            "trending_candidate_generated_count": int(trending_candidate_generated),
            "trending_frontier_fail_count": int(trending_frontier_fail),
            "trending_frontier_pass_count": int(trending_frontier_pass),
            "trending_manager_pass_count": int(trending_manager_pass),
            "trending_orders_submitted_count": int(orders_submitted_core),
            "trending_orders_filled_count": int(orders_filled_core),
            "orders_regime_assumption": orders_regime_assumption,
            "non_trending_candidate_count_scan_basis": int(non_trending_candidate),
        },
        "trending_block_reasons": {
            "frontier_fail_breakdown_top3": frontier_top3,
            "execution_veto_breakdown_top3": exec_veto_top3,
            "notes": [
                "frontier breakdown source is verification diagnostics frontier_fail_breakdown_top3.",
                "execution veto top3 is extracted from rejection_reason_counts keys containing execution/veto hints.",
            ],
        },
        "judgement": {
            "case": case,
            "reason": case_reason,
            "recommended_e1_2_single_axis": (
                "trend_only_required_ev_one_step_relief" if case == "CASE_B_GATE_BLOCK" else "core_window_expand_or_shift"
            ),
        },
    }

    output_path.write_text(json.dumps(out, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
