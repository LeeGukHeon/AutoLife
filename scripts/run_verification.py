#!/usr/bin/env python3
import argparse
from collections import Counter
import csv
import json
import os
import pathlib
import subprocess
import sys
from typing import Any, Dict, List

from _script_common import verification_lock


def resolve_path(path_value: str, label: str, must_exist: bool = True) -> pathlib.Path:
    path_obj = pathlib.Path(path_value)
    if not path_obj.is_absolute():
        path_obj = (pathlib.Path.cwd() / path_obj).resolve()
    if must_exist and not path_obj.exists():
        raise FileNotFoundError(f"{label} not found: {path_obj}")
    return path_obj


def ensure_parent(path_value: pathlib.Path) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)


def is_upbit_primary_1m_dataset(dataset_path: pathlib.Path) -> bool:
    stem = dataset_path.stem.lower()
    return stem.startswith("upbit_") and "_1m_" in stem


def has_higher_tf_companions(data_dir: pathlib.Path, primary_1m_dataset: pathlib.Path) -> bool:
    if not is_upbit_primary_1m_dataset(primary_1m_dataset):
        return False
    prefix = primary_1m_dataset.stem.split("_1m_", 1)[0]
    for tf in ("5m", "15m", "60m", "240m"):
        if not any(data_dir.glob(f"{prefix}_{tf}_*.csv")):
            return False
    return True


def parse_backtest_json(proc: subprocess.CompletedProcess) -> Dict[str, Any]:
    lines: List[str] = []
    if proc.stdout:
        lines.extend(proc.stdout.splitlines())
    if proc.stderr:
        lines.extend(proc.stderr.splitlines())
    parsed: Dict[str, Any] = {}
    has_parsed = False
    for line in reversed(lines):
        text = line.strip()
        if text.startswith("{") and text.endswith("}"):
            try:
                value = json.loads(text)
            except Exception:
                continue
            if isinstance(value, dict):
                parsed = value
                has_parsed = True
                break
    if int(proc.returncode) != 0:
        tail = " || ".join([x.strip() for x in lines[-5:] if str(x).strip()])[:800]
        parsed_hint = ""
        if has_parsed:
            parsed_hint = f" parsed_json={json.dumps(parsed, ensure_ascii=False)[:400]}"
        raise RuntimeError(
            f"Backtest failed (exit={proc.returncode}).{parsed_hint} tail={tail}"
        )
    if has_parsed:
        return parsed
    raise RuntimeError(f"Backtest JSON output not found (exit={proc.returncode})")


def run_backtest(
    exe_path: pathlib.Path,
    dataset_path: pathlib.Path,
    require_higher_tf_companions: bool,
    disable_adaptive_state_io: bool,
) -> Dict[str, Any]:
    cmd = [str(exe_path), "--backtest", str(dataset_path), "--json"]
    if require_higher_tf_companions and is_upbit_primary_1m_dataset(dataset_path):
        cmd.append("--require-higher-tf-companions")
    env = os.environ.copy()
    if disable_adaptive_state_io:
        env["AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO"] = "1"
    else:
        env.pop("AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO", None)
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        cwd=str(exe_path.parent),
        env=env,
    )
    return parse_backtest_json(proc)


def safe_avg(values: List[float]) -> float:
    return sum(values) / float(len(values)) if values else 0.0


def safe_weighted_avg(values: List[float], weights: List[float]) -> float:
    if not values or not weights or len(values) != len(weights):
        return safe_avg(values)
    positive_weights = [max(0.0, float(x)) for x in weights]
    total_weight = sum(positive_weights)
    if total_weight <= 0.0:
        return safe_avg(values)
    weighted_sum = 0.0
    for value, weight in zip(values, positive_weights):
        weighted_sum += float(value) * weight
    return weighted_sum / total_weight


def to_int(value: Any) -> int:
    try:
        return int(float(value))
    except Exception:
        return 0


def to_float(value: Any) -> float:
    try:
        return float(value)
    except Exception:
        return 0.0


def nested_get(payload: Any, path: List[str], default: Any) -> Any:
    current = payload
    for key in path:
        if not isinstance(current, dict):
            return default
        current = current.get(key)
    if current is None:
        return default
    return current


def load_report_json(path_value: pathlib.Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {}
    try:
        with path_value.open("r", encoding="utf-8") as fp:
            payload = json.load(fp)
    except Exception:
        return {}
    return payload if isinstance(payload, dict) else {}


def normalize_dataset_list(value: Any) -> List[str]:
    if not isinstance(value, list):
        return []
    out: List[str] = []
    for item in value:
        token = str(item).strip()
        if token:
            out.append(token)
    return out


def extract_report_metrics(report: Dict[str, Any]) -> Dict[str, Any]:
    failure_primary_group = nested_get(
        report,
        ["diagnostics", "failure_attribution", "primary_non_execution_group"],
        "",
    )
    if not str(failure_primary_group).strip():
        failure_primary_group = nested_get(
            report,
            ["diagnostics", "aggregate", "primary_non_execution_group", "name"],
            "unknown",
        )
    return {
        "dataset_count": max(0, to_int(report.get("dataset_count", 0))),
        "datasets": normalize_dataset_list(report.get("datasets", [])),
        "avg_profit_factor": to_float(nested_get(report, ["aggregates", "avg_profit_factor"], 0.0)),
        "avg_expectancy_krw": to_float(nested_get(report, ["aggregates", "avg_expectancy_krw"], 0.0)),
        "avg_total_trades": to_float(nested_get(report, ["aggregates", "avg_total_trades"], 0.0)),
        "peak_max_drawdown_pct": to_float(
            nested_get(report, ["aggregates", "peak_max_drawdown_pct"], 0.0)
        ),
        "avg_primary_candidate_conversion": to_float(
            nested_get(
                report,
                ["adaptive_validation", "aggregates", "avg_primary_candidate_conversion"],
                0.0,
            )
        ),
        "avg_shadow_candidate_conversion": to_float(
            nested_get(
                report,
                ["adaptive_validation", "aggregates", "avg_shadow_candidate_conversion"],
                0.0,
            )
        ),
        "avg_shadow_candidate_supply_lift": to_float(
            nested_get(
                report,
                ["adaptive_validation", "aggregates", "avg_shadow_candidate_supply_lift"],
                0.0,
            )
        ),
        "overall_gate_pass": bool(report.get("overall_gate_pass", False)),
        "adaptive_verdict": str(
            nested_get(report, ["adaptive_validation", "verdict", "verdict"], "fail")
        ).strip()
        or "fail",
        "primary_non_execution_group": str(failure_primary_group).strip() or "unknown",
        "generated_at": str(report.get("generated_at", "")).strip(),
    }


def build_baseline_comparison(
    current_report: Dict[str, Any],
    baseline_report_path: pathlib.Path,
) -> Dict[str, Any]:
    baseline_report = load_report_json(baseline_report_path)
    output: Dict[str, Any] = {
        "baseline_report_path": str(baseline_report_path),
        "available": bool(baseline_report),
        "reason": "",
    }
    if not baseline_report:
        output["reason"] = "baseline_report_missing_or_invalid"
        return output

    current_metrics = extract_report_metrics(current_report)
    baseline_metrics = extract_report_metrics(baseline_report)
    current_datasets = normalize_dataset_list(current_metrics.get("datasets", []))
    baseline_datasets = normalize_dataset_list(baseline_metrics.get("datasets", []))
    current_dataset_counter = Counter(current_datasets)
    baseline_dataset_counter = Counter(baseline_datasets)
    comparable_dataset_set = (
        bool(current_datasets)
        and bool(baseline_datasets)
        and (current_dataset_counter == baseline_dataset_counter)
    )
    overlap_count = int(sum((current_dataset_counter & baseline_dataset_counter).values()))
    missing_in_current = sorted(list((baseline_dataset_counter - current_dataset_counter).elements()))
    added_in_current = sorted(list((current_dataset_counter - baseline_dataset_counter).elements()))

    deltas = {
        "avg_profit_factor": round(
            to_float(current_metrics.get("avg_profit_factor", 0.0))
            - to_float(baseline_metrics.get("avg_profit_factor", 0.0)),
            6,
        ),
        "avg_expectancy_krw": round(
            to_float(current_metrics.get("avg_expectancy_krw", 0.0))
            - to_float(baseline_metrics.get("avg_expectancy_krw", 0.0)),
            6,
        ),
        "avg_total_trades": round(
            to_float(current_metrics.get("avg_total_trades", 0.0))
            - to_float(baseline_metrics.get("avg_total_trades", 0.0)),
            6,
        ),
        "peak_max_drawdown_pct": round(
            to_float(current_metrics.get("peak_max_drawdown_pct", 0.0))
            - to_float(baseline_metrics.get("peak_max_drawdown_pct", 0.0)),
            6,
        ),
        "avg_primary_candidate_conversion": round(
            to_float(current_metrics.get("avg_primary_candidate_conversion", 0.0))
            - to_float(baseline_metrics.get("avg_primary_candidate_conversion", 0.0)),
            6,
        ),
        "avg_shadow_candidate_conversion": round(
            to_float(current_metrics.get("avg_shadow_candidate_conversion", 0.0))
            - to_float(baseline_metrics.get("avg_shadow_candidate_conversion", 0.0)),
            6,
        ),
        "avg_shadow_candidate_supply_lift": round(
            to_float(current_metrics.get("avg_shadow_candidate_supply_lift", 0.0))
            - to_float(baseline_metrics.get("avg_shadow_candidate_supply_lift", 0.0)),
            6,
        ),
    }

    checks: Dict[str, Any] = {}
    failed_checks: List[str] = []
    if comparable_dataset_set:
        checks = {
            "profit_factor_non_degrade_pass": to_float(current_metrics.get("avg_profit_factor", 0.0))
            >= to_float(baseline_metrics.get("avg_profit_factor", 0.0)),
            "expectancy_non_degrade_pass": to_float(current_metrics.get("avg_expectancy_krw", 0.0))
            >= to_float(baseline_metrics.get("avg_expectancy_krw", 0.0)),
            "drawdown_non_worse_pass": to_float(current_metrics.get("peak_max_drawdown_pct", 0.0))
            <= to_float(baseline_metrics.get("peak_max_drawdown_pct", 0.0)),
            "primary_candidate_conversion_non_degrade_pass": to_float(
                current_metrics.get("avg_primary_candidate_conversion", 0.0)
            )
            >= to_float(baseline_metrics.get("avg_primary_candidate_conversion", 0.0)),
            "shadow_supply_lift_non_degrade_pass": to_float(
                current_metrics.get("avg_shadow_candidate_supply_lift", 0.0)
            )
            >= to_float(baseline_metrics.get("avg_shadow_candidate_supply_lift", 0.0)),
        }
        failed_checks = [key for key, value in checks.items() if not bool(value)]

    output.update(
        {
            "baseline_generated_at": str(baseline_metrics.get("generated_at", "")),
            "current_generated_at": str(current_metrics.get("generated_at", "")),
            "comparable_dataset_set": comparable_dataset_set,
            "dataset_overlap_count": int(overlap_count),
            "dataset_missing_in_current": missing_in_current,
            "dataset_added_in_current": added_in_current,
            "current": current_metrics,
            "baseline": baseline_metrics,
            "deltas": deltas,
            "status_changes": {
                "overall_gate_pass_changed": bool(current_metrics.get("overall_gate_pass", False))
                != bool(baseline_metrics.get("overall_gate_pass", False)),
                "adaptive_verdict_changed": str(current_metrics.get("adaptive_verdict", "fail"))
                != str(baseline_metrics.get("adaptive_verdict", "fail")),
                "primary_non_execution_group_changed": str(
                    current_metrics.get("primary_non_execution_group", "unknown")
                )
                != str(baseline_metrics.get("primary_non_execution_group", "unknown")),
            },
            "non_degradation_contract": {
                "applied": comparable_dataset_set,
                "all_pass": (len(failed_checks) == 0) if comparable_dataset_set else None,
                "checks": checks,
                "failed_checks": failed_checks,
                "reason": "" if comparable_dataset_set else "dataset_set_mismatch",
            },
        }
    )
    return output


def normalized_reason_counts(value: Any) -> Dict[str, int]:
    out: Dict[str, int] = {}
    if not isinstance(value, dict):
        return out
    for k, v in value.items():
        key = str(k).strip()
        if not key:
            continue
        out[key] = out.get(key, 0) + max(0, to_int(v))
    return out


def top_reason_rows(reason_counts: Dict[str, int], limit: int = 5) -> List[Dict[str, Any]]:
    ordered = sorted(reason_counts.items(), key=lambda item: (-int(item[1]), item[0]))
    return [{"reason": key, "count": int(count)} for key, count in ordered[: max(1, int(limit))]]


def top_pattern_rows(pattern_counts: Dict[str, int], limit: int = 5) -> List[Dict[str, Any]]:
    ordered = sorted(pattern_counts.items(), key=lambda item: (-int(item[1]), item[0]))
    return [{"pattern": key, "count": int(count)} for key, count in ordered[: max(1, int(limit))]]


def build_pattern_cell_profit_map(pattern_rows: Any) -> Dict[str, Dict[str, float]]:
    out: Dict[str, Dict[str, float]] = {}
    if not isinstance(pattern_rows, list):
        return out
    for row in pattern_rows:
        if not isinstance(row, dict):
            continue
        regime = str(row.get("regime", "")).strip() or "UNKNOWN"
        vol_bucket = str(row.get("volatility_bucket", "")).strip() or "vol_unknown"
        liq_bucket = str(row.get("liquidity_bucket", "")).strip() or "liq_unknown"
        strength_bucket = str(row.get("strength_bucket", "")).strip() or "strength_unknown"
        archetype = str(row.get("entry_archetype", "")).strip() or "UNSPECIFIED"
        pattern_key = (
            f"regime={regime}|vol={vol_bucket}|liq={liq_bucket}|"
            f"strength={strength_bucket}|arch={archetype}"
        )
        trades = max(0, to_int(row.get("total_trades", 0)))
        total_profit = to_float(row.get("total_profit", row.get("total_profit_krw", 0.0)))
        slot = out.setdefault(pattern_key, {"trades": 0.0, "total_profit_krw": 0.0})
        slot["trades"] += float(trades)
        slot["total_profit_krw"] += float(total_profit)
    return out


def top_loss_pattern_cells(
    pattern_profit_map: Dict[str, Dict[str, float]],
    limit: int = 6,
) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for key, item in pattern_profit_map.items():
        if not isinstance(item, dict):
            continue
        trades = max(0, to_int(item.get("trades", 0)))
        total_profit = to_float(item.get("total_profit_krw", 0.0))
        if trades <= 0:
            continue
        rows.append(
            {
                "pattern": str(key),
                "trades": int(trades),
                "total_profit_krw": round(total_profit, 4),
                "avg_profit_krw": round(total_profit / float(trades), 4),
            }
        )
    rows.sort(key=lambda x: (float(x["total_profit_krw"]), -int(x["trades"]), str(x["pattern"])))
    return rows[: max(1, int(limit))]


def build_strategy_funnel_diagnostics(strategy_signal_funnel: Any) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    if isinstance(strategy_signal_funnel, list):
        for raw in strategy_signal_funnel:
            if not isinstance(raw, dict):
                continue
            strategy_name = str(raw.get("strategy_name", "")).strip() or "unknown"
            generated = max(0, to_int(raw.get("generated_signals", 0)))
            selected = max(0, to_int(raw.get("selected_best", 0)))
            blocked_by_risk_manager = max(0, to_int(raw.get("blocked_by_risk_manager", 0)))
            executed = max(0, to_int(raw.get("entries_executed", 0)))
            selection_rate = (float(selected) / float(generated)) if generated > 0 else 0.0
            execution_rate = (float(executed) / float(generated)) if generated > 0 else 0.0
            rows.append(
                {
                    "strategy_name": strategy_name,
                    "generated_signals": generated,
                    "selected_best": selected,
                    "blocked_by_risk_manager": blocked_by_risk_manager,
                    "entries_executed": executed,
                    "selection_rate": round(selection_rate, 4),
                    "execution_rate": round(execution_rate, 4),
                }
            )

    top_generated = sorted(
        rows,
        key=lambda item: (-int(item["generated_signals"]), item["strategy_name"]),
    )[:3]
    weak_execution = sorted(
        [x for x in rows if int(x["generated_signals"]) >= 10],
        key=lambda item: (float(item["execution_rate"]), -int(item["generated_signals"]), item["strategy_name"]),
    )[:3]
    return {
        "strategy_count": len(rows),
        "top_generated": top_generated,
        "weak_execution_under_load": weak_execution,
    }


def build_strategy_collection_diagnostics(strategy_collection_summaries: Any) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    if isinstance(strategy_collection_summaries, list):
        for raw in strategy_collection_summaries:
            if not isinstance(raw, dict):
                continue
            strategy_name = str(raw.get("strategy_name", "")).strip() or "unknown"
            skipped_disabled = max(0, to_int(raw.get("skipped_disabled", 0)))
            no_signal = max(0, to_int(raw.get("no_signal", 0)))
            generated = max(0, to_int(raw.get("generated", 0)))
            total_attempted = skipped_disabled + no_signal + generated
            no_signal_rate = (float(no_signal) / float(total_attempted)) if total_attempted > 0 else 0.0
            rows.append(
                {
                    "strategy_name": strategy_name,
                    "skipped_disabled": skipped_disabled,
                    "no_signal": no_signal,
                    "generated": generated,
                    "total_attempted": total_attempted,
                    "no_signal_rate": round(no_signal_rate, 4),
                }
            )

    top_no_signal = sorted(
        [x for x in rows if int(x["no_signal"]) > 0],
        key=lambda item: (-int(item["no_signal"]), item["strategy_name"]),
    )[:3]
    top_skipped_disabled = sorted(
        [x for x in rows if int(x["skipped_disabled"]) > 0],
        key=lambda item: (-int(item["skipped_disabled"]), item["strategy_name"]),
    )[:3]
    return {
        "strategy_count": len(rows),
        "top_no_signal": top_no_signal,
        "top_skipped_disabled": top_skipped_disabled,
    }


def build_dataset_diagnostics(dataset_name: str, backtest_result: Dict[str, Any]) -> Dict[str, Any]:
    entry_funnel = backtest_result.get("entry_funnel", {})
    if not isinstance(entry_funnel, dict):
        entry_funnel = {}
    shadow_funnel_raw = backtest_result.get("shadow_funnel", {})
    if not isinstance(shadow_funnel_raw, dict):
        shadow_funnel_raw = {}

    entry_rounds = max(0, to_int(entry_funnel.get("entry_rounds", 0)))
    skipped_due_to_open_position = max(0, to_int(entry_funnel.get("skipped_due_to_open_position", 0)))
    no_signal_generated = max(0, to_int(entry_funnel.get("no_signal_generated", 0)))
    filtered_out_by_manager = max(0, to_int(entry_funnel.get("filtered_out_by_manager", 0)))
    filtered_out_by_policy = max(0, to_int(entry_funnel.get("filtered_out_by_policy", 0)))
    no_best_signal = max(0, to_int(entry_funnel.get("no_best_signal", 0)))
    blocked_pattern_gate = max(0, to_int(entry_funnel.get("blocked_pattern_gate", 0)))
    blocked_rr_rebalance = max(0, to_int(entry_funnel.get("blocked_rr_rebalance", 0)))
    blocked_risk_gate = max(0, to_int(entry_funnel.get("blocked_risk_gate", 0)))
    blocked_second_stage_confirmation = max(0, to_int(entry_funnel.get("blocked_second_stage_confirmation", 0)))
    two_head_aggregation_blocked = max(0, to_int(entry_funnel.get("two_head_aggregation_blocked", 0)))
    blocked_risk_manager = max(0, to_int(entry_funnel.get("blocked_risk_manager", 0)))
    blocked_min_order_or_capital = max(0, to_int(entry_funnel.get("blocked_min_order_or_capital", 0)))
    blocked_order_sizing = max(0, to_int(entry_funnel.get("blocked_order_sizing", 0)))
    entries_executed = max(0, to_int(entry_funnel.get("entries_executed", 0)))
    shadow_rounds = max(0, to_int(shadow_funnel_raw.get("rounds", 0)))
    shadow_primary_generated_signals = max(
        0, to_int(shadow_funnel_raw.get("primary_generated_signals", 0))
    )
    shadow_primary_after_manager_filter = max(
        0, to_int(shadow_funnel_raw.get("primary_after_manager_filter", 0))
    )
    shadow_shadow_after_manager_filter = max(
        0, to_int(shadow_funnel_raw.get("shadow_after_manager_filter", 0))
    )
    shadow_primary_after_policy_filter = max(
        0, to_int(shadow_funnel_raw.get("primary_after_policy_filter", 0))
    )
    shadow_shadow_after_policy_filter = max(
        0, to_int(shadow_funnel_raw.get("shadow_after_policy_filter", 0))
    )
    shadow_primary_best_signal_available = max(
        0, to_int(shadow_funnel_raw.get("primary_best_signal_available", 0))
    )
    shadow_shadow_best_signal_available = max(
        0, to_int(shadow_funnel_raw.get("shadow_best_signal_available", 0))
    )
    shadow_supply_improved_rounds = max(
        0, to_int(shadow_funnel_raw.get("supply_improved_rounds", 0))
    )
    shadow_avg_manager_supply_lift = to_float(
        shadow_funnel_raw.get("avg_manager_supply_lift", 0.0)
    )
    shadow_avg_policy_supply_lift = to_float(
        shadow_funnel_raw.get("avg_policy_supply_lift", 0.0)
    )
    shadow_generated_denominator = max(1, shadow_primary_generated_signals)
    shadow_policy_supply_lift_per_signal = (
        float(shadow_shadow_after_policy_filter - shadow_primary_after_policy_filter)
        / float(shadow_generated_denominator)
    )
    shadow_contract = {
        "shadow_probe_active": shadow_rounds > 0,
        "candidate_supply_lift_positive": (
            shadow_shadow_after_policy_filter > shadow_primary_after_policy_filter
        ),
        "best_signal_lift_non_negative": (
            shadow_shadow_best_signal_available >= shadow_primary_best_signal_available
        ),
    }
    shadow_contract["all_pass"] = all(bool(v) for v in shadow_contract.values())

    candidate_generation = (
        no_signal_generated
        + filtered_out_by_manager
        + filtered_out_by_policy
        + no_best_signal
    )
    quality_and_risk_gate = (
        blocked_pattern_gate
        + blocked_rr_rebalance
        + blocked_risk_gate
        + blocked_second_stage_confirmation
        + two_head_aggregation_blocked
    )
    execution_constraints = (
        skipped_due_to_open_position
        + blocked_risk_manager
        + blocked_min_order_or_capital
        + blocked_order_sizing
    )
    non_execution_total = candidate_generation + quality_and_risk_gate + execution_constraints

    bottleneck_groups = {
        "candidate_generation": int(candidate_generation),
        "quality_and_risk_gate": int(quality_and_risk_gate),
        "execution_constraints": int(execution_constraints),
        "entries_executed": int(entries_executed),
    }
    ordered_non_execution_groups = sorted(
        [
            ("candidate_generation", candidate_generation),
            ("quality_and_risk_gate", quality_and_risk_gate),
            ("execution_constraints", execution_constraints),
        ],
        key=lambda item: (-int(item[1]), item[0]),
    )
    top_group_name, top_group_count = ordered_non_execution_groups[0]
    top_group_share = (
        (float(top_group_count) / float(non_execution_total)) if non_execution_total > 0 else 0.0
    )
    round_denominator = float(max(1, entry_rounds))
    bottleneck_group_rates = {
        "candidate_generation_rate": round(candidate_generation / round_denominator, 4),
        "quality_and_risk_gate_rate": round(quality_and_risk_gate / round_denominator, 4),
        "execution_constraints_rate": round(execution_constraints / round_denominator, 4),
        "entry_execution_rate": round(entries_executed / round_denominator, 4),
    }

    reason_counts = normalized_reason_counts(backtest_result.get("entry_rejection_reason_counts", {}))
    no_signal_pattern_counts = normalized_reason_counts(backtest_result.get("no_signal_pattern_counts", {}))
    edge_gap_bucket_counts = normalized_reason_counts(
        backtest_result.get("entry_quality_edge_gap_buckets", {})
    )
    pattern_profit_map = build_pattern_cell_profit_map(backtest_result.get("pattern_summaries", []))
    strategy_diag = build_strategy_funnel_diagnostics(backtest_result.get("strategy_signal_funnel", []))
    strategy_collection_diag = build_strategy_collection_diagnostics(
        backtest_result.get("strategy_collection_summaries", [])
    )
    post_entry_telemetry = parse_post_entry_risk_telemetry(backtest_result)

    return {
        "dataset": dataset_name,
        "entry_rounds": int(entry_rounds),
        "non_execution_total": int(non_execution_total),
        "bottleneck_groups": bottleneck_groups,
        "bottleneck_group_rates": bottleneck_group_rates,
        "top_non_execution_group": {
            "name": top_group_name,
            "count": int(top_group_count),
            "share_of_non_execution": round(top_group_share, 4),
        },
        "top_rejection_reasons": top_reason_rows(reason_counts, limit=5),
        "rejection_reason_counts": reason_counts,
        "no_signal_pattern_counts": no_signal_pattern_counts,
        "top_no_signal_patterns": top_pattern_rows(no_signal_pattern_counts, limit=5),
        "entry_quality_edge_gap_buckets": edge_gap_bucket_counts,
        "top_entry_quality_edge_gap_buckets": top_pattern_rows(edge_gap_bucket_counts, limit=5),
        "pattern_cell_profit_map": pattern_profit_map,
        "top_loss_pattern_cells": top_loss_pattern_cells(pattern_profit_map, limit=6),
        "strategy_funnel": strategy_diag,
        "strategy_collection": strategy_collection_diag,
        "post_entry_risk_telemetry": post_entry_telemetry,
        "shadow_funnel": {
            "rounds": int(shadow_rounds),
            "primary_generated_signals": int(shadow_primary_generated_signals),
            "primary_after_manager_filter": int(shadow_primary_after_manager_filter),
            "shadow_after_manager_filter": int(shadow_shadow_after_manager_filter),
            "primary_after_policy_filter": int(shadow_primary_after_policy_filter),
            "shadow_after_policy_filter": int(shadow_shadow_after_policy_filter),
            "primary_best_signal_available": int(shadow_primary_best_signal_available),
            "shadow_best_signal_available": int(shadow_shadow_best_signal_available),
            "supply_improved_rounds": int(shadow_supply_improved_rounds),
            "avg_manager_supply_lift": round(shadow_avg_manager_supply_lift, 6),
            "avg_policy_supply_lift": round(shadow_avg_policy_supply_lift, 6),
            "policy_supply_lift_per_generated_signal": round(
                shadow_policy_supply_lift_per_signal,
                6,
            ),
        },
        "shadow_contract": shadow_contract,
        "strategy_collect_exception_count": max(
            0, to_int(backtest_result.get("strategy_collect_exception_count", 0))
        ),
    }


def aggregate_dataset_diagnostics(dataset_diagnostics: List[Dict[str, Any]]) -> Dict[str, Any]:
    aggregate_groups = {
        "candidate_generation": 0,
        "quality_and_risk_gate": 0,
        "execution_constraints": 0,
        "entries_executed": 0,
    }
    aggregate_reasons: Dict[str, int] = {}
    aggregate_no_signal_patterns: Dict[str, int] = {}
    aggregate_edge_gap_buckets: Dict[str, int] = {}
    aggregate_pattern_profit_map: Dict[str, Dict[str, float]] = {}
    top_group_votes: Dict[str, int] = {}
    aggregate_post_entry_telemetry: Dict[str, Any] = {
        "adaptive_stop_updates": 0,
        "adaptive_tp_recalibration_updates": 0,
        "adaptive_partial_ratio_samples": 0,
        "adaptive_partial_ratio_histogram": {
            "0.35_0.44": 0,
            "0.45_0.54": 0,
            "0.55_0.64": 0,
            "0.65_0.74": 0,
            "0.75_0.80": 0,
        },
    }
    aggregate_shadow: Dict[str, Any] = {
        "rounds": 0,
        "primary_generated_signals": 0,
        "primary_after_manager_filter": 0,
        "shadow_after_manager_filter": 0,
        "primary_after_policy_filter": 0,
        "shadow_after_policy_filter": 0,
        "primary_best_signal_available": 0,
        "shadow_best_signal_available": 0,
        "supply_improved_rounds": 0,
        "avg_manager_supply_lift_acc": 0.0,
        "avg_policy_supply_lift_acc": 0.0,
        "dataset_with_shadow_probe": 0,
        "contract_all_pass_count": 0,
    }

    for item in dataset_diagnostics:
        groups = item.get("bottleneck_groups", {})
        if isinstance(groups, dict):
            for key in aggregate_groups:
                aggregate_groups[key] += max(0, to_int(groups.get(key, 0)))

        reasons = item.get("rejection_reason_counts", {})
        if isinstance(reasons, dict):
            for k, v in reasons.items():
                reason_key = str(k).strip()
                if not reason_key:
                    continue
                aggregate_reasons[reason_key] = aggregate_reasons.get(reason_key, 0) + max(0, to_int(v))

        no_signal_patterns = item.get("no_signal_pattern_counts", {})
        if isinstance(no_signal_patterns, dict):
            for k, v in no_signal_patterns.items():
                pattern_key = str(k).strip()
                if not pattern_key:
                    continue
                aggregate_no_signal_patterns[pattern_key] = (
                    aggregate_no_signal_patterns.get(pattern_key, 0) + max(0, to_int(v))
                )

        edge_gap_buckets = item.get("entry_quality_edge_gap_buckets", {})
        if isinstance(edge_gap_buckets, dict):
            for k, v in edge_gap_buckets.items():
                bucket_key = str(k).strip()
                if not bucket_key:
                    continue
                aggregate_edge_gap_buckets[bucket_key] = (
                    aggregate_edge_gap_buckets.get(bucket_key, 0) + max(0, to_int(v))
                )

        pattern_profit_map = item.get("pattern_cell_profit_map", {})
        if isinstance(pattern_profit_map, dict):
            for k, v in pattern_profit_map.items():
                pattern_key = str(k).strip()
                if not pattern_key or not isinstance(v, dict):
                    continue
                slot = aggregate_pattern_profit_map.setdefault(
                    pattern_key,
                    {"trades": 0.0, "total_profit_krw": 0.0},
                )
                slot["trades"] += float(max(0, to_int(v.get("trades", 0))))
                slot["total_profit_krw"] += float(to_float(v.get("total_profit_krw", 0.0)))

        top_group = item.get("top_non_execution_group", {})
        if isinstance(top_group, dict):
            name = str(top_group.get("name", "")).strip()
            if name:
                top_group_votes[name] = top_group_votes.get(name, 0) + 1

        post_entry_telemetry = item.get("post_entry_risk_telemetry", {})
        if isinstance(post_entry_telemetry, dict):
            aggregate_post_entry_telemetry["adaptive_stop_updates"] += max(
                0, to_int(post_entry_telemetry.get("adaptive_stop_updates", 0))
            )
            aggregate_post_entry_telemetry["adaptive_tp_recalibration_updates"] += max(
                0,
                to_int(post_entry_telemetry.get("adaptive_tp_recalibration_updates", 0)),
            )
            aggregate_post_entry_telemetry["adaptive_partial_ratio_samples"] += max(
                0,
                to_int(post_entry_telemetry.get("adaptive_partial_ratio_samples", 0)),
            )
            histogram = post_entry_telemetry.get("adaptive_partial_ratio_histogram", {})
            if isinstance(histogram, dict):
                for key in aggregate_post_entry_telemetry["adaptive_partial_ratio_histogram"]:
                    aggregate_post_entry_telemetry["adaptive_partial_ratio_histogram"][key] += max(
                        0,
                        to_int(histogram.get(key, 0)),
                    )

        shadow_funnel = item.get("shadow_funnel", {})
        if isinstance(shadow_funnel, dict):
            aggregate_shadow["rounds"] += max(0, to_int(shadow_funnel.get("rounds", 0)))
            aggregate_shadow["primary_generated_signals"] += max(
                0,
                to_int(shadow_funnel.get("primary_generated_signals", 0)),
            )
            aggregate_shadow["primary_after_manager_filter"] += max(
                0,
                to_int(shadow_funnel.get("primary_after_manager_filter", 0)),
            )
            aggregate_shadow["shadow_after_manager_filter"] += max(
                0,
                to_int(shadow_funnel.get("shadow_after_manager_filter", 0)),
            )
            aggregate_shadow["primary_after_policy_filter"] += max(
                0,
                to_int(shadow_funnel.get("primary_after_policy_filter", 0)),
            )
            aggregate_shadow["shadow_after_policy_filter"] += max(
                0,
                to_int(shadow_funnel.get("shadow_after_policy_filter", 0)),
            )
            aggregate_shadow["primary_best_signal_available"] += max(
                0,
                to_int(shadow_funnel.get("primary_best_signal_available", 0)),
            )
            aggregate_shadow["shadow_best_signal_available"] += max(
                0,
                to_int(shadow_funnel.get("shadow_best_signal_available", 0)),
            )
            aggregate_shadow["supply_improved_rounds"] += max(
                0,
                to_int(shadow_funnel.get("supply_improved_rounds", 0)),
            )
            aggregate_shadow["avg_manager_supply_lift_acc"] += to_float(
                shadow_funnel.get("avg_manager_supply_lift", 0.0)
            )
            aggregate_shadow["avg_policy_supply_lift_acc"] += to_float(
                shadow_funnel.get("avg_policy_supply_lift", 0.0)
            )
            aggregate_shadow["dataset_with_shadow_probe"] += 1

        shadow_contract = item.get("shadow_contract", {})
        if isinstance(shadow_contract, dict) and bool(shadow_contract.get("all_pass", False)):
            aggregate_shadow["contract_all_pass_count"] += 1

    non_execution_total = (
        aggregate_groups["candidate_generation"]
        + aggregate_groups["quality_and_risk_gate"]
        + aggregate_groups["execution_constraints"]
    )
    ordered_non_execution_groups = sorted(
        [
            ("candidate_generation", aggregate_groups["candidate_generation"]),
            ("quality_and_risk_gate", aggregate_groups["quality_and_risk_gate"]),
            ("execution_constraints", aggregate_groups["execution_constraints"]),
        ],
        key=lambda item: (-int(item[1]), item[0]),
    )
    primary_group_name, primary_group_count = ordered_non_execution_groups[0]
    primary_group_share = (
        (float(primary_group_count) / float(non_execution_total)) if non_execution_total > 0 else 0.0
    )
    shadow_dataset_count = max(0, to_int(aggregate_shadow.get("dataset_with_shadow_probe", 0)))
    aggregate_shadow_summary = {
        "rounds": int(aggregate_shadow["rounds"]),
        "primary_generated_signals": int(aggregate_shadow["primary_generated_signals"]),
        "primary_after_manager_filter": int(aggregate_shadow["primary_after_manager_filter"]),
        "shadow_after_manager_filter": int(aggregate_shadow["shadow_after_manager_filter"]),
        "primary_after_policy_filter": int(aggregate_shadow["primary_after_policy_filter"]),
        "shadow_after_policy_filter": int(aggregate_shadow["shadow_after_policy_filter"]),
        "primary_best_signal_available": int(aggregate_shadow["primary_best_signal_available"]),
        "shadow_best_signal_available": int(aggregate_shadow["shadow_best_signal_available"]),
        "supply_improved_rounds": int(aggregate_shadow["supply_improved_rounds"]),
        "avg_manager_supply_lift": round(
            (aggregate_shadow["avg_manager_supply_lift_acc"] / float(shadow_dataset_count))
            if shadow_dataset_count > 0 else 0.0,
            6,
        ),
        "avg_policy_supply_lift": round(
            (aggregate_shadow["avg_policy_supply_lift_acc"] / float(shadow_dataset_count))
            if shadow_dataset_count > 0 else 0.0,
            6,
        ),
        "contract_all_pass_count": int(aggregate_shadow["contract_all_pass_count"]),
        "contract_all_pass_rate": round(
            (float(aggregate_shadow["contract_all_pass_count"]) / float(shadow_dataset_count))
            if shadow_dataset_count > 0 else 0.0,
            4,
        ),
    }

    return {
        "dataset_count": len(dataset_diagnostics),
        "aggregate_bottleneck_groups": aggregate_groups,
        "primary_non_execution_group": {
            "name": primary_group_name,
            "count": int(primary_group_count),
            "share_of_non_execution": round(primary_group_share, 4),
        },
        "top_rejection_reasons": top_reason_rows(aggregate_reasons, limit=8),
        "top_no_signal_patterns": top_pattern_rows(aggregate_no_signal_patterns, limit=8),
        "top_entry_quality_edge_gap_buckets": top_pattern_rows(aggregate_edge_gap_buckets, limit=8),
        "top_loss_pattern_cells": top_loss_pattern_cells(aggregate_pattern_profit_map, limit=10),
        "top_non_execution_group_vote_counts": top_group_votes,
        "post_entry_risk_telemetry": aggregate_post_entry_telemetry,
        "shadow_funnel": aggregate_shadow_summary,
    }


def build_failure_attribution(
    gate: Dict[str, bool],
    avg_total_trades: float,
    min_avg_trades: float,
    aggregate_diagnostics: Dict[str, Any],
) -> Dict[str, Any]:
    failed_gates = [k for k, v in gate.items() if not bool(v)]
    primary_group_name = "candidate_generation"
    primary_group = aggregate_diagnostics.get("primary_non_execution_group", {})
    if isinstance(primary_group, dict):
        name = str(primary_group.get("name", "")).strip()
        if name:
            primary_group_name = name
    primary_no_signal_pattern = ""
    top_no_signal_patterns = aggregate_diagnostics.get("top_no_signal_patterns", [])
    if isinstance(top_no_signal_patterns, list) and top_no_signal_patterns:
        first = top_no_signal_patterns[0]
        if isinstance(first, dict):
            primary_no_signal_pattern = str(first.get("pattern", "")).strip()
    primary_edge_gap_bucket = ""
    top_edge_gap_buckets = aggregate_diagnostics.get("top_entry_quality_edge_gap_buckets", [])
    if isinstance(top_edge_gap_buckets, list) and top_edge_gap_buckets:
        first_gap = top_edge_gap_buckets[0]
        if isinstance(first_gap, dict):
            primary_edge_gap_bucket = str(first_gap.get("pattern", "")).strip()
    primary_loss_pattern_cell = ""
    top_loss_pattern_cells_rows = aggregate_diagnostics.get("top_loss_pattern_cells", [])
    if isinstance(top_loss_pattern_cells_rows, list) and top_loss_pattern_cells_rows:
        first_loss = top_loss_pattern_cells_rows[0]
        if isinstance(first_loss, dict):
            primary_loss_pattern_cell = str(first_loss.get("pattern", "")).strip()

    hypothesis = "balanced_or_data_limited"
    next_focus: List[str] = []
    if primary_group_name == "candidate_generation":
        hypothesis = "signal_supply_or_prefilter_bottleneck"
        next_focus = [
            "Inspect strategy-level generated->selected->executed conversion first.",
            "Check whether manager prefilter and expected-value floor are too strict.",
            "Extract shared market-pattern signatures where no_signal_generated dominates.",
        ]
    elif primary_group_name == "quality_and_risk_gate":
        hypothesis = "risk_gate_overconstraint_or_quality_mismatch"
        next_focus = [
            "Inspect blocked_risk_gate and blocked_second_stage_confirmation composition.",
            "Check if risk-gate thresholds are overly strict for current regimes.",
            "Prefer regime-specific gate design over blanket threshold relaxation.",
        ]
    elif primary_group_name == "execution_constraints":
        hypothesis = "execution_path_or_position_constraints"
        next_focus = [
            "Inspect blocked_min_order_or_capital and blocked_order_sizing share.",
            "Check opportunity-loss structure from open-position occupancy.",
            "Review engine sizing and concurrent-position limit logic.",
        ]

    low_trade_condition = float(avg_total_trades) < float(min_avg_trades)
    return {
        "failed_gates": failed_gates,
        "primary_non_execution_group": primary_group_name,
        "primary_no_signal_pattern": primary_no_signal_pattern,
        "primary_entry_quality_edge_gap_bucket": primary_edge_gap_bucket,
        "primary_loss_pattern_cell": primary_loss_pattern_cell,
        "low_trade_condition": low_trade_condition,
        "hypothesis": hypothesis,
        "next_focus": next_focus,
    }


def build_regime_metrics_from_patterns(backtest_result: Dict[str, Any]) -> Dict[str, Any]:
    pattern_rows = backtest_result.get("pattern_summaries", [])
    regime_bucket: Dict[str, Dict[str, float]] = {}
    if isinstance(pattern_rows, list):
        for row in pattern_rows:
            if not isinstance(row, dict):
                continue
            regime = str(row.get("regime", "")).strip() or "UNKNOWN"
            trades = max(0, to_int(row.get("total_trades", 0)))
            total_profit = to_float(row.get("total_profit", 0.0))
            if regime not in regime_bucket:
                regime_bucket[regime] = {"trades": 0.0, "total_profit": 0.0}
            regime_bucket[regime]["trades"] += float(trades)
            regime_bucket[regime]["total_profit"] += float(total_profit)

    total_pattern_trades = 0.0
    for value in regime_bucket.values():
        total_pattern_trades += float(value.get("trades", 0.0))

    output: Dict[str, Any] = {"regimes": {}, "total_pattern_trades": int(total_pattern_trades)}
    for regime, value in regime_bucket.items():
        trades = max(0.0, float(value.get("trades", 0.0)))
        total_profit = float(value.get("total_profit", 0.0))
        output["regimes"][regime] = {
            "trades": int(trades),
            "total_profit_krw": round(total_profit, 4),
            "profit_per_trade_krw": round(total_profit / trades, 4) if trades > 0 else 0.0,
            "trade_share": round(trades / total_pattern_trades, 4) if total_pattern_trades > 0 else 0.0,
        }
    return output


def parse_post_entry_risk_telemetry(backtest_result: Dict[str, Any]) -> Dict[str, Any]:
    telemetry = backtest_result.get("post_entry_risk_telemetry", {})
    if not isinstance(telemetry, dict):
        telemetry = {}

    histogram_raw = telemetry.get("adaptive_partial_ratio_histogram", {})
    histogram: Dict[str, int] = {}
    if isinstance(histogram_raw, dict):
        for key in ("0.35_0.44", "0.45_0.54", "0.55_0.64", "0.65_0.74", "0.75_0.80"):
            histogram[key] = max(0, to_int(histogram_raw.get(key, 0)))

    return {
        "adaptive_stop_updates": max(0, to_int(telemetry.get("adaptive_stop_updates", 0))),
        "adaptive_tp_recalibration_updates": max(
            0,
            to_int(telemetry.get("adaptive_tp_recalibration_updates", 0)),
        ),
        "adaptive_partial_ratio_samples": max(
            0,
            to_int(telemetry.get("adaptive_partial_ratio_samples", 0)),
        ),
        "adaptive_partial_ratio_avg": max(
            0.0,
            to_float(telemetry.get("adaptive_partial_ratio_avg", 0.0)),
        ),
        "adaptive_partial_ratio_histogram": histogram,
    }


def build_adaptive_dataset_profile(dataset_name: str, backtest_result: Dict[str, Any]) -> Dict[str, Any]:
    regime_metrics = build_regime_metrics_from_patterns(backtest_result)
    post_entry_telemetry = parse_post_entry_risk_telemetry(backtest_result)
    regimes = regime_metrics.get("regimes", {})
    downtrend = regimes.get("TRENDING_DOWN", {})
    uptrend = regimes.get("TRENDING_UP", {})

    downtrend_trade_share = to_float(downtrend.get("trade_share", 0.0))
    downtrend_profit_per_trade = to_float(downtrend.get("profit_per_trade_krw", 0.0))
    downtrend_loss_per_trade = max(0.0, -downtrend_profit_per_trade)
    uptrend_expectancy = to_float(uptrend.get("profit_per_trade_krw", 0.0))
    total_trades = max(0, to_int(backtest_result.get("total_trades", 0)))
    profit_factor = max(0.0, to_float(backtest_result.get("profit_factor", 0.0)))
    expectancy_krw = to_float(backtest_result.get("expectancy_krw", 0.0))
    max_drawdown_pct = max(0.0, to_float(backtest_result.get("max_drawdown", 0.0)) * 100.0)

    score = 0.0
    score += max(-3.0, min(3.0, expectancy_krw / 10.0))
    score += max(-2.0, min(2.0, (profit_factor - 1.0) * 2.0))
    score -= max(0.0, min(3.0, max_drawdown_pct / 6.0))
    score -= max(0.0, min(3.0, downtrend_loss_per_trade / 8.0))
    if downtrend_trade_share > 0.50:
        score -= max(0.0, min(2.0, (downtrend_trade_share - 0.50) * 4.0))
    if total_trades < 10:
        score -= 0.5

    generated_signals = 0
    entries_executed = 0
    entry_funnel = backtest_result.get("entry_funnel", {})
    if not isinstance(entry_funnel, dict):
        entry_funnel = {}
    shadow_funnel_raw = backtest_result.get("shadow_funnel", {})
    if not isinstance(shadow_funnel_raw, dict):
        shadow_funnel_raw = {}
    strategy_signal_funnel = backtest_result.get("strategy_signal_funnel", [])
    if isinstance(strategy_signal_funnel, list):
        for row in strategy_signal_funnel:
            if not isinstance(row, dict):
                continue
            generated_signals += max(0, to_int(row.get("generated_signals", 0)))
            entries_executed += max(0, to_int(row.get("entries_executed", 0)))
    if generated_signals <= 0:
        generated_signals = max(0, to_int(entry_funnel.get("entry_rounds", 0)))
    if entries_executed <= 0:
        entries_executed = max(0, to_int(entry_funnel.get("entries_executed", 0)))
    opportunity_conversion = (
        float(entries_executed) / float(generated_signals)
        if generated_signals > 0 else 0.0
    )
    shadow_generated_signals = max(
        generated_signals,
        max(0, to_int(shadow_funnel_raw.get("primary_generated_signals", 0))),
    )
    shadow_policy_candidates = max(
        0,
        to_int(shadow_funnel_raw.get("shadow_after_policy_filter", 0)),
    )
    primary_policy_candidates = max(
        0,
        to_int(shadow_funnel_raw.get("primary_after_policy_filter", 0)),
    )
    shadow_opportunity_conversion = (
        float(shadow_policy_candidates) / float(shadow_generated_signals)
        if shadow_generated_signals > 0
        else 0.0
    )
    primary_candidate_conversion = (
        float(primary_policy_candidates) / float(shadow_generated_signals)
        if shadow_generated_signals > 0
        else 0.0
    )

    return {
        "dataset": dataset_name,
        "total_trades": int(total_trades),
        "profit_factor": round(profit_factor, 4),
        "expectancy_krw": round(expectancy_krw, 4),
        "max_drawdown_pct": round(max_drawdown_pct, 4),
        "downtrend_trade_share": round(downtrend_trade_share, 4),
        "downtrend_loss_per_trade_krw": round(downtrend_loss_per_trade, 4),
        "uptrend_expectancy_krw": round(uptrend_expectancy, 4),
        "risk_adjusted_score": round(score, 4),
        "generated_signals": int(generated_signals),
        "entries_executed": int(entries_executed),
        "opportunity_conversion": round(opportunity_conversion, 4),
        "primary_candidate_conversion": round(primary_candidate_conversion, 4),
        "shadow_candidate_conversion": round(shadow_opportunity_conversion, 4),
        "shadow_candidate_supply_lift": round(
            shadow_opportunity_conversion - primary_candidate_conversion,
            4,
        ),
        "post_entry_risk_telemetry": post_entry_telemetry,
        "regime_metrics": regime_metrics,
    }


def aggregate_adaptive_validation(dataset_profiles: List[Dict[str, Any]]) -> Dict[str, Any]:
    if not dataset_profiles:
        return {
            "dataset_count": 0,
            "avg_risk_adjusted_score": 0.0,
            "avg_downtrend_trade_share": 0.0,
            "avg_downtrend_loss_per_trade_krw": 0.0,
            "avg_uptrend_expectancy_krw": 0.0,
            "avg_primary_candidate_conversion": 0.0,
            "avg_shadow_candidate_conversion": 0.0,
            "avg_shadow_candidate_supply_lift": 0.0,
            "avg_adaptive_stop_updates": 0.0,
            "avg_adaptive_tp_recalibration_updates": 0.0,
            "avg_adaptive_partial_ratio_samples": 0.0,
            "avg_adaptive_partial_ratio": 0.0,
            "adaptive_partial_ratio_histogram": {
                "0.35_0.44": 0,
                "0.45_0.54": 0,
                "0.55_0.64": 0,
                "0.65_0.74": 0,
                "0.75_0.80": 0,
            },
        }

    scores = [to_float(x.get("risk_adjusted_score", 0.0)) for x in dataset_profiles]
    downtrend_share = [to_float(x.get("downtrend_trade_share", 0.0)) for x in dataset_profiles]
    downtrend_loss = [to_float(x.get("downtrend_loss_per_trade_krw", 0.0)) for x in dataset_profiles]
    uptrend_exp = [to_float(x.get("uptrend_expectancy_krw", 0.0)) for x in dataset_profiles]
    generated = [max(0.0, to_float(x.get("generated_signals", 0.0))) for x in dataset_profiles]
    executed = [max(0.0, to_float(x.get("entries_executed", 0.0))) for x in dataset_profiles]
    primary_candidate_conversion = [
        max(0.0, to_float(x.get("primary_candidate_conversion", 0.0))) for x in dataset_profiles
    ]
    shadow_candidate_conversion = [
        max(0.0, to_float(x.get("shadow_candidate_conversion", 0.0))) for x in dataset_profiles
    ]
    shadow_candidate_supply_lift = [
        to_float(x.get("shadow_candidate_supply_lift", 0.0)) for x in dataset_profiles
    ]
    stop_updates = []
    tp_recalibration_updates = []
    partial_ratio_samples = []
    partial_ratio_weighted_sum = 0.0
    partial_ratio_histogram = {
        "0.35_0.44": 0,
        "0.45_0.54": 0,
        "0.55_0.64": 0,
        "0.65_0.74": 0,
        "0.75_0.80": 0,
    }
    for profile in dataset_profiles:
        telemetry = profile.get("post_entry_risk_telemetry", {})
        if not isinstance(telemetry, dict):
            telemetry = {}
        stop_update_count = max(0.0, to_float(telemetry.get("adaptive_stop_updates", 0.0)))
        tp_recalibration_count = max(
            0.0,
            to_float(telemetry.get("adaptive_tp_recalibration_updates", 0.0)),
        )
        ratio_samples = max(0.0, to_float(telemetry.get("adaptive_partial_ratio_samples", 0.0)))
        ratio_avg = max(0.0, to_float(telemetry.get("adaptive_partial_ratio_avg", 0.0)))
        stop_updates.append(stop_update_count)
        tp_recalibration_updates.append(tp_recalibration_count)
        partial_ratio_samples.append(ratio_samples)
        partial_ratio_weighted_sum += ratio_avg * ratio_samples

        hist = telemetry.get("adaptive_partial_ratio_histogram", {})
        if isinstance(hist, dict):
            for key in partial_ratio_histogram:
                partial_ratio_histogram[key] += max(0, to_int(hist.get(key, 0)))

    total_generated = sum(generated)
    total_executed = sum(executed)
    total_partial_ratio_samples = sum(partial_ratio_samples)
    avg_opportunity_conversion = (
        (total_executed / total_generated) if total_generated > 0.0 else 0.0
    )
    avg_adaptive_partial_ratio = (
        partial_ratio_weighted_sum / total_partial_ratio_samples
        if total_partial_ratio_samples > 0.0
        else 0.0
    )
    return {
        "dataset_count": len(dataset_profiles),
        "avg_risk_adjusted_score": round(safe_avg(scores), 4),
        "avg_downtrend_trade_share": round(safe_avg(downtrend_share), 4),
        "avg_downtrend_loss_per_trade_krw": round(safe_avg(downtrend_loss), 4),
        "avg_uptrend_expectancy_krw": round(safe_avg(uptrend_exp), 4),
        "avg_generated_signals": round(safe_avg(generated), 4),
        "avg_entries_executed": round(safe_avg(executed), 4),
        "avg_opportunity_conversion": round(avg_opportunity_conversion, 4),
        "avg_primary_candidate_conversion": round(
            safe_avg(primary_candidate_conversion),
            4,
        ),
        "avg_shadow_candidate_conversion": round(
            safe_avg(shadow_candidate_conversion),
            4,
        ),
        "avg_shadow_candidate_supply_lift": round(
            safe_avg(shadow_candidate_supply_lift),
            4,
        ),
        "avg_adaptive_stop_updates": round(safe_avg(stop_updates), 4),
        "avg_adaptive_tp_recalibration_updates": round(safe_avg(tp_recalibration_updates), 4),
        "avg_adaptive_partial_ratio_samples": round(safe_avg(partial_ratio_samples), 4),
        "avg_adaptive_partial_ratio": round(avg_adaptive_partial_ratio, 4),
        "adaptive_partial_ratio_histogram": partial_ratio_histogram,
    }


def build_adaptive_verdict(
    adaptive_aggregates: Dict[str, Any],
    max_downtrend_loss_per_trade_krw: float,
    max_downtrend_trade_share: float,
    min_uptrend_expectancy_krw: float,
    min_risk_adjusted_score: float,
    avg_total_trades: float,
    min_avg_trades: float,
    peak_max_drawdown_pct: float,
    max_drawdown_pct_limit: float,
) -> Dict[str, Any]:
    avg_downtrend_trade_share = to_float(adaptive_aggregates.get("avg_downtrend_trade_share", 0.0))
    avg_generated_signals = to_float(adaptive_aggregates.get("avg_generated_signals", 0.0))
    avg_opportunity_conversion = to_float(adaptive_aggregates.get("avg_opportunity_conversion", 0.0))
    avg_primary_candidate_conversion = to_float(
        adaptive_aggregates.get("avg_primary_candidate_conversion", 0.0)
    )
    avg_shadow_candidate_conversion = to_float(
        adaptive_aggregates.get("avg_shadow_candidate_conversion", 0.0)
    )
    avg_shadow_candidate_supply_lift = to_float(
        adaptive_aggregates.get("avg_shadow_candidate_supply_lift", 0.0)
    )

    hostile_sample_relax_factor = 1.0
    if avg_downtrend_trade_share >= 0.45:
        hostile_sample_relax_factor = 0.60
    elif avg_downtrend_trade_share >= 0.30:
        hostile_sample_relax_factor = 0.75
    elif avg_downtrend_trade_share >= 0.15:
        hostile_sample_relax_factor = 0.90
    effective_min_avg_trades = max(3.0, float(min_avg_trades) * hostile_sample_relax_factor)

    min_opportunity_conversion = 0.025
    if avg_downtrend_trade_share >= 0.45:
        min_opportunity_conversion = 0.012
    elif avg_downtrend_trade_share >= 0.30:
        min_opportunity_conversion = 0.018
    low_opportunity_observed = avg_generated_signals < 5.0

    checks = {
        "sample_size_guard_pass": float(avg_total_trades) >= float(effective_min_avg_trades),
        "drawdown_guard_pass": float(peak_max_drawdown_pct) <= float(max_drawdown_pct_limit),
        "downtrend_loss_guard_pass": to_float(adaptive_aggregates.get("avg_downtrend_loss_per_trade_krw", 0.0))
        <= float(max_downtrend_loss_per_trade_krw),
        "downtrend_trade_share_guard_pass": to_float(adaptive_aggregates.get("avg_downtrend_trade_share", 0.0))
        <= float(max_downtrend_trade_share),
        "uptrend_expectancy_guard_pass": to_float(adaptive_aggregates.get("avg_uptrend_expectancy_krw", 0.0))
        >= float(min_uptrend_expectancy_krw),
        "risk_adjusted_score_guard_pass": to_float(adaptive_aggregates.get("avg_risk_adjusted_score", 0.0))
        >= float(min_risk_adjusted_score),
        "opportunity_conversion_guard_pass": (
            True if low_opportunity_observed
            else (avg_opportunity_conversion >= float(min_opportunity_conversion))
        ),
    }
    failed = [k for k, v in checks.items() if not bool(v)]
    hard_fail_keys = [
        "drawdown_guard_pass",
        "downtrend_loss_guard_pass",
        "downtrend_trade_share_guard_pass",
        "uptrend_expectancy_guard_pass",
        "risk_adjusted_score_guard_pass",
    ]
    hard_fail = any(not bool(checks.get(key, True)) for key in hard_fail_keys)

    if hard_fail:
        verdict = "fail"
    elif (
        checks["sample_size_guard_pass"] and
        checks["opportunity_conversion_guard_pass"]
    ):
        verdict = "pass"
    else:
        verdict = "inconclusive"
    return {
        "verdict": verdict,
        "checks": checks,
        "failed_checks": failed,
        "effective_thresholds": {
            "effective_min_avg_trades": round(effective_min_avg_trades, 4),
            "hostile_sample_relax_factor": round(hostile_sample_relax_factor, 4),
            "min_opportunity_conversion": round(min_opportunity_conversion, 4),
        },
        "context": {
            "avg_generated_signals": round(avg_generated_signals, 4),
            "avg_opportunity_conversion": round(avg_opportunity_conversion, 4),
            "avg_primary_candidate_conversion": round(avg_primary_candidate_conversion, 4),
            "avg_shadow_candidate_conversion": round(avg_shadow_candidate_conversion, 4),
            "avg_shadow_candidate_supply_lift": round(avg_shadow_candidate_supply_lift, 4),
            "low_opportunity_observed": bool(low_opportunity_observed),
        },
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Minimal sequential verification pipeline (reset baseline)."
    )
    parser.add_argument("--exe-path", default=r".\build\Release\AutoLifeTrading.exe")
    parser.add_argument("--config-path", default=r".\build\Release\config\config.json")
    parser.add_argument("--source-config-path", default=r".\config\config.json")
    parser.add_argument("--data-dir", default=r".\data\backtest_real")
    parser.add_argument("--dataset-names", nargs="*", default=[])
    parser.add_argument(
        "--data-mode",
        choices=["fixed", "refresh_if_missing", "refresh_force"],
        default="fixed",
    )
    parser.add_argument("--output-csv", default=r".\build\Release\logs\verification_matrix.csv")
    parser.add_argument("--output-json", default=r".\build\Release\logs\verification_report.json")
    parser.add_argument(
        "--baseline-report-path",
        default=r".\build\Release\logs\verification_report_baseline_current.json",
    )
    parser.add_argument(
        "--validation-profile",
        choices=["adaptive", "legacy_gate"],
        default="adaptive",
        help="adaptive: regime-aware dynamic validation (recommended), legacy_gate: threshold gate mode",
    )
    parser.add_argument("--min-profit-factor", type=float, default=1.00)
    parser.add_argument("--min-expectancy-krw", type=float, default=0.0)
    parser.add_argument("--max-drawdown-pct", type=float, default=12.0)
    parser.add_argument("--min-profitable-ratio", type=float, default=0.55)
    parser.add_argument("--min-avg-win-rate-pct", type=float, default=0.0)
    parser.add_argument("--min-avg-trades", type=float, default=10.0)
    parser.add_argument("--max-downtrend-loss-per-trade-krw", type=float, default=12.0)
    parser.add_argument("--max-downtrend-trade-share", type=float, default=0.55)
    parser.add_argument("--min-uptrend-expectancy-krw", type=float, default=0.0)
    parser.add_argument("--min-risk-adjusted-score", type=float, default=-0.10)
    parser.add_argument("--require-higher-tf-companions", action="store_true")
    parser.add_argument("--enable-adaptive-state-io", action="store_true")
    parser.add_argument("--verification-lock-path", default=r".\build\Release\logs\verification_run.lock")
    parser.add_argument("--verification-lock-timeout-sec", type=int, default=1800)
    parser.add_argument("--verification-lock-stale-sec", type=int, default=14400)
    args = parser.parse_args(argv)

    exe_path = resolve_path(args.exe_path, "Executable")
    config_path = resolve_path(args.config_path, "Build config", must_exist=False)
    source_config_path = resolve_path(args.source_config_path, "Source config")
    data_dir = resolve_path(args.data_dir, "Data directory")
    output_csv = resolve_path(args.output_csv, "Output csv", must_exist=False)
    output_json = resolve_path(args.output_json, "Output json", must_exist=False)
    baseline_report_path = resolve_path(
        args.baseline_report_path,
        "Baseline report",
        must_exist=False,
    )
    lock_path = resolve_path(args.verification_lock_path, "Verification lock", must_exist=False)

    ensure_parent(config_path)
    if not config_path.exists():
        config_path.write_text(source_config_path.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")

    dataset_paths: List[pathlib.Path] = []
    for name in args.dataset_names:
        token = str(name).strip()
        if not token:
            continue
        candidate = pathlib.Path(token)
        if not candidate.is_absolute():
            candidate = (data_dir / candidate).resolve()
        if not candidate.exists():
            raise FileNotFoundError(f"Dataset not found: {candidate}")
        dataset_paths.append(candidate)
    if not dataset_paths:
        raise RuntimeError("No datasets configured.")
    dataset_key_counts: Dict[str, int] = {}
    for path_value in dataset_paths:
        key = str(path_value.resolve()).lower()
        dataset_key_counts[key] = dataset_key_counts.get(key, 0) + 1
    duplicated = sorted([k for k, c in dataset_key_counts.items() if int(c) > 1])
    if duplicated:
        duplicate_names = [pathlib.Path(x).name for x in duplicated]
        raise RuntimeError(
            "Duplicate datasets configured (remove duplicates to keep verification weighting stable): "
            + ",".join(duplicate_names)
        )

    if bool(args.require_higher_tf_companions):
        for dataset_path in dataset_paths:
            if not is_upbit_primary_1m_dataset(dataset_path):
                raise RuntimeError(
                    "When --require-higher-tf-companions is enabled, "
                    "only upbit_*_1m_*.csv datasets are allowed: "
                    f"{dataset_path.name}"
                )
            if not has_higher_tf_companions(data_dir, dataset_path):
                raise RuntimeError(
                    "Missing companion timeframe csv (5m/15m/60m/240m) for dataset: "
                    f"{dataset_path.name}"
                )

    rows: List[Dict[str, Any]] = []
    dataset_diagnostics: List[Dict[str, Any]] = []
    adaptive_dataset_profiles: List[Dict[str, Any]] = []
    with verification_lock(
        lock_path,
        timeout_sec=int(args.verification_lock_timeout_sec),
        stale_sec=int(args.verification_lock_stale_sec),
    ):
        for dataset_path in dataset_paths:
            result = run_backtest(
                exe_path=exe_path,
                dataset_path=dataset_path,
                require_higher_tf_companions=bool(args.require_higher_tf_companions),
                disable_adaptive_state_io=not bool(args.enable_adaptive_state_io),
            )
            row = {
                "dataset": dataset_path.name,
                "total_profit_krw": round(float(result.get("total_profit", 0.0)), 4),
                "profit_factor": round(float(result.get("profit_factor", 0.0)), 4),
                "expectancy_krw": round(float(result.get("expectancy_krw", 0.0)), 4),
                "max_drawdown_pct": round(float(result.get("max_drawdown", 0.0)) * 100.0, 4),
                "total_trades": int(result.get("total_trades", 0)),
                "win_rate_pct": round(float(result.get("win_rate", 0.0)) * 100.0, 4),
            }
            row["profitable"] = bool(float(row["total_profit_krw"]) > 0.0)
            rows.append(row)
            dataset_diagnostics.append(
                build_dataset_diagnostics(dataset_name=dataset_path.name, backtest_result=result)
            )
            adaptive_dataset_profiles.append(
                build_adaptive_dataset_profile(dataset_name=dataset_path.name, backtest_result=result)
            )

    ensure_parent(output_csv)
    with output_csv.open("w", encoding="utf-8", newline="\n") as fp:
        fieldnames = [
            "dataset",
            "total_profit_krw",
            "profit_factor",
            "expectancy_krw",
            "max_drawdown_pct",
            "total_trades",
            "win_rate_pct",
            "profitable",
        ]
        writer = csv.DictWriter(fp, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    profits = [float(x["total_profit_krw"]) for x in rows]
    profit_factors = [float(x["profit_factor"]) for x in rows]
    expectancies = [float(x["expectancy_krw"]) for x in rows]
    drawdowns = [float(x["max_drawdown_pct"]) for x in rows]
    win_rates = [float(x["win_rate_pct"]) for x in rows]
    trades = [max(0.0, float(x["total_trades"])) for x in rows]
    profitable_ratio = (sum(1 for x in rows if bool(x["profitable"])) / float(len(rows))) if rows else 0.0

    avg_profit_factor = round(safe_weighted_avg(profit_factors, trades), 4)
    avg_expectancy_krw = round(safe_weighted_avg(expectancies, trades), 4)
    avg_win_rate_pct = round(safe_weighted_avg(win_rates, trades), 4)
    avg_total_trades = round(safe_avg(trades), 4)
    peak_max_drawdown_pct = round(max(drawdowns) if drawdowns else 0.0, 4)
    total_profit_sum_krw = round(sum(profits), 4)

    gate = {
        "gate_profit_factor_pass": avg_profit_factor >= float(args.min_profit_factor),
        "gate_expectancy_pass": avg_expectancy_krw >= float(args.min_expectancy_krw),
        "gate_drawdown_pass": peak_max_drawdown_pct <= float(args.max_drawdown_pct),
        "gate_profitable_ratio_pass": profitable_ratio >= float(args.min_profitable_ratio),
        "gate_avg_win_rate_pass": avg_win_rate_pct >= float(args.min_avg_win_rate_pct),
        "gate_avg_trades_pass": avg_total_trades >= float(args.min_avg_trades),
    }
    legacy_overall_gate_pass = all(bool(x) for x in gate.values())
    aggregate_diagnostics = aggregate_dataset_diagnostics(dataset_diagnostics)
    failure_attribution = build_failure_attribution(
        gate=gate,
        avg_total_trades=avg_total_trades,
        min_avg_trades=float(args.min_avg_trades),
        aggregate_diagnostics=aggregate_diagnostics,
    )
    adaptive_aggregates = aggregate_adaptive_validation(adaptive_dataset_profiles)
    adaptive_verdict = build_adaptive_verdict(
        adaptive_aggregates=adaptive_aggregates,
        max_downtrend_loss_per_trade_krw=float(args.max_downtrend_loss_per_trade_krw),
        max_downtrend_trade_share=float(args.max_downtrend_trade_share),
        min_uptrend_expectancy_krw=float(args.min_uptrend_expectancy_krw),
        min_risk_adjusted_score=float(args.min_risk_adjusted_score),
        avg_total_trades=avg_total_trades,
        min_avg_trades=float(args.min_avg_trades),
        peak_max_drawdown_pct=peak_max_drawdown_pct,
        max_drawdown_pct_limit=float(args.max_drawdown_pct),
    )
    if str(args.validation_profile) == "legacy_gate":
        overall_gate_pass = legacy_overall_gate_pass
    else:
        overall_gate_pass = str(adaptive_verdict.get("verdict", "fail")) == "pass"

    report = {
        "mode": "verification_adaptive",
        "validation_profile": str(args.validation_profile),
        "data_mode": str(args.data_mode),
        "sequential_only": True,
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "dataset_count": len(rows),
        "datasets": [x["dataset"] for x in rows],
        "thresholds": {
            "min_profit_factor": float(args.min_profit_factor),
            "min_expectancy_krw": float(args.min_expectancy_krw),
            "max_drawdown_pct": float(args.max_drawdown_pct),
            "min_profitable_ratio": float(args.min_profitable_ratio),
            "min_avg_win_rate_pct": float(args.min_avg_win_rate_pct),
            "min_avg_trades": float(args.min_avg_trades),
        },
        "aggregates": {
            "avg_profit_factor": avg_profit_factor,
            "avg_expectancy_krw": avg_expectancy_krw,
            "avg_win_rate_pct": avg_win_rate_pct,
            "avg_total_trades": avg_total_trades,
            "peak_max_drawdown_pct": peak_max_drawdown_pct,
            "profitable_ratio": round(profitable_ratio, 4),
            "total_profit_sum_krw": total_profit_sum_krw,
        },
        "legacy_gate": {
            "gate": gate,
            "overall_gate_pass": legacy_overall_gate_pass,
        },
        "adaptive_validation": {
            "thresholds": {
                "min_avg_trades": float(args.min_avg_trades),
                "max_downtrend_loss_per_trade_krw": float(args.max_downtrend_loss_per_trade_krw),
                "max_downtrend_trade_share": float(args.max_downtrend_trade_share),
                "min_uptrend_expectancy_krw": float(args.min_uptrend_expectancy_krw),
                "min_risk_adjusted_score": float(args.min_risk_adjusted_score),
                "max_drawdown_pct": float(args.max_drawdown_pct),
            },
            "aggregates": adaptive_aggregates,
            "verdict": adaptive_verdict,
            "per_dataset": adaptive_dataset_profiles,
        },
        "overall_gate_pass": overall_gate_pass,
        "rows": rows,
        "diagnostics": {
            "aggregate": aggregate_diagnostics,
            "per_dataset": dataset_diagnostics,
            "failure_attribution": failure_attribution,
        },
        "artifacts": {
            "output_csv": str(output_csv),
            "output_json": str(output_json),
            "baseline_report_path": str(baseline_report_path),
            "config_path": str(config_path),
            "source_config_path": str(source_config_path),
        },
    }
    report["baseline_comparison"] = build_baseline_comparison(
        current_report=report,
        baseline_report_path=baseline_report_path,
    )

    ensure_parent(output_json)
    with output_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(report, fp, ensure_ascii=False, indent=4)

    print(f"[Verification] dataset_count={len(rows)}")
    print(f"[Verification] data_mode={args.data_mode}")
    print(f"[Verification] validation_profile={args.validation_profile}")
    print(f"[Verification] avg_profit_factor={avg_profit_factor}")
    print(f"[Verification] avg_expectancy_krw={avg_expectancy_krw}")
    print(f"[Verification] overall_gate_pass={overall_gate_pass}")
    print(
        "[Verification] adaptive_verdict="
        f"{adaptive_verdict.get('verdict', 'fail')}"
    )
    print(
        "[Verification] primary_non_execution_group="
        f"{failure_attribution.get('primary_non_execution_group', 'unknown')}"
    )
    baseline_comparison = report.get("baseline_comparison", {})
    if isinstance(baseline_comparison, dict) and bool(baseline_comparison.get("available", False)):
        deltas = baseline_comparison.get("deltas", {})
        if not isinstance(deltas, dict):
            deltas = {}
        contract = baseline_comparison.get("non_degradation_contract", {})
        contract_tag = "skip"
        if isinstance(contract, dict) and bool(contract.get("applied", False)):
            contract_tag = "pass" if bool(contract.get("all_pass", False)) else "fail"
        print(
            "[Verification] baseline_delta_pf="
            f"{deltas.get('avg_profit_factor', 0.0)} "
            f"baseline_delta_exp={deltas.get('avg_expectancy_krw', 0.0)} "
            f"baseline_contract={contract_tag}"
        )
    else:
        reason = ""
        if isinstance(baseline_comparison, dict):
            reason = str(baseline_comparison.get("reason", "")).strip()
        print(
            "[Verification] baseline_comparison=unavailable "
            f"reason={reason or 'baseline_report_missing_or_invalid'}"
        )
    print(f"[Verification] report_json={output_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
