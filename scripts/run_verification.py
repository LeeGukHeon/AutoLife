#!/usr/bin/env python3
import argparse
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


def parse_backtest_json(proc: subprocess.CompletedProcess) -> Dict[str, Any]:
    lines: List[str] = []
    if proc.stdout:
        lines.extend(proc.stdout.splitlines())
    if proc.stderr:
        lines.extend(proc.stderr.splitlines())
    for line in reversed(lines):
        text = line.strip()
        if text.startswith("{") and text.endswith("}"):
            return json.loads(text)
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
    strategy_diag = build_strategy_funnel_diagnostics(backtest_result.get("strategy_signal_funnel", []))
    strategy_collection_diag = build_strategy_collection_diagnostics(
        backtest_result.get("strategy_collection_summaries", [])
    )

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
        "strategy_funnel": strategy_diag,
        "strategy_collection": strategy_collection_diag,
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
    top_group_votes: Dict[str, int] = {}

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

        top_group = item.get("top_non_execution_group", {})
        if isinstance(top_group, dict):
            name = str(top_group.get("name", "")).strip()
            if name:
                top_group_votes[name] = top_group_votes.get(name, 0) + 1

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

    return {
        "dataset_count": len(dataset_diagnostics),
        "aggregate_bottleneck_groups": aggregate_groups,
        "primary_non_execution_group": {
            "name": primary_group_name,
            "count": int(primary_group_count),
            "share_of_non_execution": round(primary_group_share, 4),
        },
        "top_rejection_reasons": top_reason_rows(aggregate_reasons, limit=8),
        "top_non_execution_group_vote_counts": top_group_votes,
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


def build_adaptive_dataset_profile(dataset_name: str, backtest_result: Dict[str, Any]) -> Dict[str, Any]:
    regime_metrics = build_regime_metrics_from_patterns(backtest_result)
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
        }

    scores = [to_float(x.get("risk_adjusted_score", 0.0)) for x in dataset_profiles]
    downtrend_share = [to_float(x.get("downtrend_trade_share", 0.0)) for x in dataset_profiles]
    downtrend_loss = [to_float(x.get("downtrend_loss_per_trade_krw", 0.0)) for x in dataset_profiles]
    uptrend_exp = [to_float(x.get("uptrend_expectancy_krw", 0.0)) for x in dataset_profiles]
    return {
        "dataset_count": len(dataset_profiles),
        "avg_risk_adjusted_score": round(safe_avg(scores), 4),
        "avg_downtrend_trade_share": round(safe_avg(downtrend_share), 4),
        "avg_downtrend_loss_per_trade_krw": round(safe_avg(downtrend_loss), 4),
        "avg_uptrend_expectancy_krw": round(safe_avg(uptrend_exp), 4),
    }


def build_adaptive_verdict(
    adaptive_aggregates: Dict[str, Any],
    max_downtrend_loss_per_trade_krw: float,
    max_downtrend_trade_share: float,
    min_uptrend_expectancy_krw: float,
    min_risk_adjusted_score: float,
    peak_max_drawdown_pct: float,
    max_drawdown_pct_limit: float,
) -> Dict[str, Any]:
    checks = {
        "drawdown_guard_pass": float(peak_max_drawdown_pct) <= float(max_drawdown_pct_limit),
        "downtrend_loss_guard_pass": to_float(adaptive_aggregates.get("avg_downtrend_loss_per_trade_krw", 0.0))
        <= float(max_downtrend_loss_per_trade_krw),
        "downtrend_trade_share_guard_pass": to_float(adaptive_aggregates.get("avg_downtrend_trade_share", 0.0))
        <= float(max_downtrend_trade_share),
        "uptrend_expectancy_guard_pass": to_float(adaptive_aggregates.get("avg_uptrend_expectancy_krw", 0.0))
        >= float(min_uptrend_expectancy_krw),
        "risk_adjusted_score_guard_pass": to_float(adaptive_aggregates.get("avg_risk_adjusted_score", 0.0))
        >= float(min_risk_adjusted_score),
    }
    failed = [k for k, v in checks.items() if not bool(v)]
    if not failed:
        verdict = "pass"
    elif checks["drawdown_guard_pass"] and checks["downtrend_loss_guard_pass"]:
        verdict = "monitor"
    else:
        verdict = "fail"
    return {
        "verdict": verdict,
        "checks": checks,
        "failed_checks": failed,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Minimal sequential verification pipeline (reset baseline)."
    )
    parser.add_argument("--exe-path", default=r".\build\Release\AutoLifeTrading.exe")
    parser.add_argument("--config-path", default=r".\build\Release\config\config.json")
    parser.add_argument("--source-config-path", default=r".\config\config.json")
    parser.add_argument("--data-dir", default=r".\data\backtest")
    parser.add_argument("--dataset-names", nargs="*", default=["simulation_2000.csv", "simulation_large.csv"])
    parser.add_argument(
        "--data-mode",
        choices=["fixed", "refresh_if_missing", "refresh_force"],
        default="fixed",
    )
    parser.add_argument("--output-csv", default=r".\build\Release\logs\verification_matrix.csv")
    parser.add_argument("--output-json", default=r".\build\Release\logs\verification_report.json")
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
        peak_max_drawdown_pct=peak_max_drawdown_pct,
        max_drawdown_pct_limit=float(args.max_drawdown_pct),
    )
    if str(args.validation_profile) == "legacy_gate":
        overall_gate_pass = legacy_overall_gate_pass
    else:
        overall_gate_pass = str(adaptive_verdict.get("verdict", "fail")) != "fail"

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
            "config_path": str(config_path),
            "source_config_path": str(source_config_path),
        },
    }

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
    print(f"[Verification] report_json={output_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
