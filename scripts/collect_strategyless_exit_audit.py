#!/usr/bin/env python3
"""Collect correctness-first audit evidence for strategy-less/runtime exits.

This script replays backtests on selected datasets, captures runtime-position
exit distributions, and combines them with shadow/parity log snapshots.
It is analysis-only and does not modify runtime behavior.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


DEFAULT_DATASETS = [
    "upbit_KRW_BTC_1m_12000.csv",
    "upbit_KRW_ETH_1m_12000.csv",
    "upbit_KRW_XRP_1m_12000.csv",
    "upbit_KRW_SOL_1m_12000.csv",
    "upbit_KRW_DOGE_1m_12000.csv",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run backtest JSON replays and aggregate strategy-less/runtime exit "
            "diagnostics for correctness-first audit."
        )
    )
    parser.add_argument(
        "--exe-path",
        default=r".\build\Release\AutoLifeTrading.exe",
        help="Path to AutoLifeTrading executable.",
    )
    parser.add_argument(
        "--data-dir",
        default=r".\data\backtest_real",
        help="Directory containing backtest CSV datasets.",
    )
    parser.add_argument(
        "--datasets",
        default="",
        help="Optional comma-separated dataset filenames/paths.",
    )
    parser.add_argument(
        "--require-higher-tf-companions",
        action="store_true",
        help="Pass --require-higher-tf-companions to backtest CLI.",
    )
    parser.add_argument(
        "--shadow-report-json",
        default=r".\build\Release\logs\probabilistic_shadow_report_v14_asc.json",
        help="Shadow report JSON path (optional).",
    )
    parser.add_argument(
        "--live-decision-log-jsonl",
        default=r".\build\Release\logs\policy_decisions.jsonl",
        help="Live decision log path (optional).",
    )
    parser.add_argument(
        "--backtest-decision-log-jsonl",
        default=r".\build\Release\logs\policy_decisions_backtest.jsonl",
        help="Backtest decision log path (optional).",
    )
    parser.add_argument(
        "--live-execution-log-jsonl",
        default=r".\build\Release\logs\execution_updates_live.jsonl",
        help="Live execution updates path (optional).",
    )
    parser.add_argument(
        "--backtest-execution-log-jsonl",
        default=r".\build\Release\logs\execution_updates_backtest.jsonl",
        help="Backtest execution updates path (optional).",
    )
    parser.add_argument(
        "--live-runtime-log",
        default=r".\build\Release\logs\autolife.log",
        help="Live runtime text log path for exit reason scan (optional).",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\strategyless_exit_audit_5set_20260224.json",
        help="Output audit JSON path.",
    )
    return parser.parse_args()


def resolve_path(raw: str) -> Path:
    return Path(raw).expanduser().resolve()


def load_json(path: Path) -> Optional[Dict[str, Any]]:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def parse_last_json_line(stdout: str) -> Optional[Dict[str, Any]]:
    lines = stdout.splitlines()
    for line in reversed(lines):
        candidate = line.strip()
        if not candidate.startswith("{") or not candidate.endswith("}"):
            continue
        try:
            payload = json.loads(candidate)
            if isinstance(payload, dict):
                return payload
        except Exception:
            continue
    return None


def safe_float(value: Any) -> float:
    try:
        return float(value)
    except Exception:
        return 0.0


def safe_int(value: Any) -> int:
    try:
        return int(value)
    except Exception:
        return 0


def run_backtest_json(
    exe_path: Path,
    dataset_path: Path,
    require_higher_tf_companions: bool,
) -> Dict[str, Any]:
    cmd: List[str] = [
        str(exe_path),
        "--backtest",
        str(dataset_path),
        "--json",
    ]
    if require_higher_tf_companions:
        cmd.append("--require-higher-tf-companions")

    completed = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )

    output: Dict[str, Any] = {
        "dataset": dataset_path.name,
        "dataset_path": str(dataset_path),
        "returncode": int(completed.returncode),
    }
    if completed.returncode != 0:
        output["error"] = "backtest_command_failed"
        output["stderr_tail"] = completed.stderr.splitlines()[-20:]
        output["stdout_tail"] = completed.stdout.splitlines()[-20:]
        return output

    payload = parse_last_json_line(completed.stdout)
    if payload is None:
        output["error"] = "backtest_json_parse_failed"
        output["stdout_tail"] = completed.stdout.splitlines()[-20:]
        return output

    output["payload"] = payload
    return output


def summarize_runtime_trade_distribution(payload: Dict[str, Any]) -> Dict[str, Any]:
    trade_samples = payload.get("trade_history_samples", [])
    if not isinstance(trade_samples, list):
        trade_samples = []

    runtime_samples: List[Dict[str, Any]] = []
    runtime_exit_reason_counts: Dict[str, int] = {}
    for item in trade_samples:
        if not isinstance(item, dict):
            continue
        entry_archetype = str(item.get("entry_archetype", ""))
        strategy_name = str(item.get("strategy_name", ""))
        if "PROBABILISTIC_PRIMARY_RUNTIME" in entry_archetype or strategy_name == "Probabilistic Primary Runtime":
            runtime_samples.append(item)
            exit_reason = str(item.get("exit_reason", "")).strip() or "unknown"
            runtime_exit_reason_counts[exit_reason] = runtime_exit_reason_counts.get(exit_reason, 0) + 1

    holding_minutes = [safe_float(item.get("holding_minutes", 0.0)) for item in runtime_samples]
    runtime_losses = [safe_float(item.get("profit_loss_krw", 0.0)) for item in runtime_samples if safe_float(item.get("profit_loss_krw", 0.0)) < 0.0]
    eod_count = sum(1 for item in runtime_samples if str(item.get("exit_reason", "")) == "BacktestEOD")
    long_hold_1d = sum(1 for value in holding_minutes if value >= 24.0 * 60.0)
    long_hold_3d = sum(1 for value in holding_minutes if value >= 72.0 * 60.0)

    strategy_summary_total = 0
    strategy_summaries = payload.get("strategy_summaries", [])
    if isinstance(strategy_summaries, list):
        for summary in strategy_summaries:
            if not isinstance(summary, dict):
                continue
            if str(summary.get("strategy_name", "")) == "Probabilistic Primary Runtime":
                strategy_summary_total += safe_int(summary.get("total_trades", 0))

    sample_coverage_ratio = (
        safe_float(len(runtime_samples)) / float(strategy_summary_total)
        if strategy_summary_total > 0
        else 1.0
    )

    return {
        "runtime_trade_sample_count": len(runtime_samples),
        "runtime_trade_total_from_strategy_summary": strategy_summary_total,
        "runtime_trade_sample_coverage_ratio": round(sample_coverage_ratio, 6),
        "runtime_exit_reason_backtest_eod_count": eod_count,
        "runtime_exit_reason_backtest_eod_ratio_in_samples": round(
            safe_float(eod_count) / float(max(1, len(runtime_samples))), 6
        ),
        "runtime_long_hold_ge_1d_count": long_hold_1d,
        "runtime_long_hold_ge_3d_count": long_hold_3d,
        "runtime_holding_minutes_avg": round(
            sum(holding_minutes) / float(max(1, len(holding_minutes))), 6
        ),
        "runtime_holding_minutes_max": round(max(holding_minutes), 6) if holding_minutes else 0.0,
        "runtime_negative_profit_sum_samples_krw": round(sum(runtime_losses), 6),
        "runtime_exit_reason_counts_in_samples": runtime_exit_reason_counts,
    }


def summarize_strategyless_diag(payload: Dict[str, Any]) -> Dict[str, Any]:
    diag = payload.get("strategyless_exit_diagnostics", {})
    if not isinstance(diag, dict):
        diag = {}
    runtime_checks = max(1, safe_int(diag.get("runtime_archetype_checks", 0)))
    risk_exit_signals = safe_int(diag.get("risk_exit_signals", 0))
    stop_hits = safe_int(diag.get("current_stop_hits", 0))
    tp1_hits = safe_int(diag.get("current_tp1_hits", 0))
    tp2_hits = safe_int(diag.get("current_tp2_hits", 0))

    return {
        "position_checks": safe_int(diag.get("position_checks", 0)),
        "runtime_archetype_checks": safe_int(diag.get("runtime_archetype_checks", 0)),
        "risk_exit_signals": risk_exit_signals,
        "current_stop_hits": stop_hits,
        "current_tp1_hits": tp1_hits,
        "current_tp2_hits": tp2_hits,
        "risk_exit_signal_rate": round(risk_exit_signals / float(runtime_checks), 6),
        "current_stop_hit_rate": round(stop_hits / float(runtime_checks), 6),
        "current_tp1_hit_rate": round(tp1_hits / float(runtime_checks), 6),
        "current_tp2_hit_rate": round(tp2_hits / float(runtime_checks), 6),
    }


def read_jsonl(path: Path) -> List[Dict[str, Any]]:
    if not path.exists():
        return []
    rows: List[Dict[str, Any]] = []
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except Exception:
            continue
        if isinstance(obj, dict):
            rows.append(obj)
    return rows


def summarize_policy_decision_log(path: Path) -> Dict[str, Any]:
    rows = read_jsonl(path)
    selected_runtime = 0
    selected_total = 0
    decision_count = 0
    for row in rows:
        decisions = row.get("decisions", [])
        if not isinstance(decisions, list):
            continue
        for decision in decisions:
            if not isinstance(decision, dict):
                continue
            decision_count += 1
            if not bool(decision.get("selected", False)):
                continue
            selected_total += 1
            if str(decision.get("strategy", "")) == "Probabilistic Primary Runtime":
                selected_runtime += 1
    return {
        "path": str(path),
        "exists": path.exists(),
        "scan_count": len(rows),
        "decision_count": decision_count,
        "selected_count": selected_total,
        "selected_runtime_count": selected_runtime,
    }


def summarize_execution_log(path: Path) -> Dict[str, Any]:
    rows = read_jsonl(path)
    by_strategy: Dict[str, int] = {}
    by_side: Dict[str, int] = {}
    by_event: Dict[str, int] = {}
    terminal_count = 0
    for row in rows:
        strategy = str(row.get("strategy_name", "")).strip() or "unknown"
        by_strategy[strategy] = by_strategy.get(strategy, 0) + 1
        side = str(row.get("side", "")).strip() or "unknown"
        by_side[side] = by_side.get(side, 0) + 1
        event = str(row.get("event", "")).strip() or "unknown"
        by_event[event] = by_event.get(event, 0) + 1
        if bool(row.get("terminal", False)):
            terminal_count += 1
    return {
        "path": str(path),
        "exists": path.exists(),
        "event_count": len(rows),
        "events_by_strategy": by_strategy,
        "events_by_side": by_side,
        "events_by_event": by_event,
        "terminal_event_count": terminal_count,
    }


def aggregate_dataset_results(items: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
    agg_diag = {
        "position_checks": 0,
        "runtime_archetype_checks": 0,
        "risk_exit_signals": 0,
        "current_stop_hits": 0,
        "current_tp1_hits": 0,
        "current_tp2_hits": 0,
    }
    runtime_trade_sample_count = 0
    runtime_backtest_eod_count = 0
    runtime_long_hold_1d_count = 0
    runtime_long_hold_3d_count = 0
    runtime_negative_profit_sum_samples = 0.0
    runtime_exit_reason_counts_total: Dict[str, int] = {}

    for item in items:
        diag = item.get("strategyless_diag", {})
        for key in agg_diag:
            agg_diag[key] += safe_int(diag.get(key, 0))
        runtime = item.get("runtime_trade_distribution", {})
        runtime_trade_sample_count += safe_int(runtime.get("runtime_trade_sample_count", 0))
        runtime_backtest_eod_count += safe_int(runtime.get("runtime_exit_reason_backtest_eod_count", 0))
        runtime_long_hold_1d_count += safe_int(runtime.get("runtime_long_hold_ge_1d_count", 0))
        runtime_long_hold_3d_count += safe_int(runtime.get("runtime_long_hold_ge_3d_count", 0))
        runtime_negative_profit_sum_samples += safe_float(runtime.get("runtime_negative_profit_sum_samples_krw", 0.0))
        reason_counts = runtime.get("runtime_exit_reason_counts_in_samples", {})
        if isinstance(reason_counts, dict):
            for reason, count in reason_counts.items():
                key = str(reason).strip() or "unknown"
                runtime_exit_reason_counts_total[key] = runtime_exit_reason_counts_total.get(key, 0) + safe_int(count)

    runtime_checks = max(1, agg_diag["runtime_archetype_checks"])
    runtime_trade_den = max(1, runtime_trade_sample_count)
    return {
        "strategyless_diag_totals": agg_diag,
        "risk_exit_signal_rate": round(agg_diag["risk_exit_signals"] / float(runtime_checks), 6),
        "current_stop_hit_rate": round(agg_diag["current_stop_hits"] / float(runtime_checks), 6),
        "current_tp1_hit_rate": round(agg_diag["current_tp1_hits"] / float(runtime_checks), 6),
        "current_tp2_hit_rate": round(agg_diag["current_tp2_hits"] / float(runtime_checks), 6),
        "runtime_trade_sample_count": runtime_trade_sample_count,
        "runtime_trade_backtest_eod_count": runtime_backtest_eod_count,
        "runtime_trade_backtest_eod_ratio": round(
            runtime_backtest_eod_count / float(runtime_trade_den), 6
        ),
        "runtime_long_hold_ge_1d_count": runtime_long_hold_1d_count,
        "runtime_long_hold_ge_3d_count": runtime_long_hold_3d_count,
        "runtime_negative_profit_sum_samples_krw": round(runtime_negative_profit_sum_samples, 6),
        "runtime_exit_reason_counts_in_samples": runtime_exit_reason_counts_total,
    }


def summarize_live_exit_reasons(path: Path) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "path": str(path),
        "exists": path.exists(),
        "matched_exit_count": 0,
        "exit_reason_counts": {},
        "exit_market_counts": {},
    }
    if not path.exists():
        return result

    reason_counts: Dict[str, int] = {}
    market_counts: Dict[str, int] = {}
    pattern = re.compile(r"Position exited:\s*([A-Z0-9\-]+)\s*\|.*?\|\s*reason=([A-Za-z0-9_]+)")
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pattern.search(raw)
        if not match:
            continue
        market = str(match.group(1)).strip() or "unknown"
        reason = str(match.group(2)).strip() or "unknown"
        reason_counts[reason] = reason_counts.get(reason, 0) + 1
        market_counts[market] = market_counts.get(market, 0) + 1

    result["matched_exit_count"] = int(sum(reason_counts.values()))
    result["exit_reason_counts"] = reason_counts
    result["exit_market_counts"] = market_counts
    return result


def build_dataset_paths(data_dir: Path, datasets_arg: str) -> List[Path]:
    if datasets_arg.strip():
        output: List[Path] = []
        for token in datasets_arg.split(","):
            value = token.strip()
            if not value:
                continue
            path = Path(value)
            if not path.is_absolute():
                path = data_dir / path
            output.append(path.resolve())
        return output

    paths: List[Path] = []
    for name in DEFAULT_DATASETS:
        path = (data_dir / name).resolve()
        if path.exists():
            paths.append(path)
    return paths


def main() -> int:
    args = parse_args()

    exe_path = resolve_path(args.exe_path)
    data_dir = resolve_path(args.data_dir)
    shadow_report_path = resolve_path(args.shadow_report_json)
    live_decision_log_path = resolve_path(args.live_decision_log_jsonl)
    backtest_decision_log_path = resolve_path(args.backtest_decision_log_jsonl)
    live_execution_log_path = resolve_path(args.live_execution_log_jsonl)
    backtest_execution_log_path = resolve_path(args.backtest_execution_log_jsonl)
    live_runtime_log_path = resolve_path(args.live_runtime_log)
    output_path = resolve_path(args.output_json)

    if not exe_path.exists():
        print(f"[error] exe not found: {exe_path}", file=sys.stderr)
        return 1
    if not data_dir.exists():
        print(f"[error] data dir not found: {data_dir}", file=sys.stderr)
        return 1

    dataset_paths = build_dataset_paths(data_dir, args.datasets)
    if not dataset_paths:
        print("[error] no datasets resolved for audit", file=sys.stderr)
        return 1

    dataset_results: List[Dict[str, Any]] = []
    errors: List[str] = []

    for dataset_path in dataset_paths:
        if not dataset_path.exists():
            errors.append(f"dataset_missing:{dataset_path}")
            dataset_results.append(
                {
                    "dataset": dataset_path.name,
                    "dataset_path": str(dataset_path),
                    "error": "dataset_missing",
                }
            )
            continue

        run_result = run_backtest_json(
            exe_path=exe_path,
            dataset_path=dataset_path,
            require_higher_tf_companions=bool(args.require_higher_tf_companions),
        )
        payload = run_result.get("payload")
        if not isinstance(payload, dict):
            errors.append(f"backtest_failed:{dataset_path.name}")
            dataset_results.append(run_result)
            continue

        strategyless_diag = summarize_strategyless_diag(payload)
        runtime_distribution = summarize_runtime_trade_distribution(payload)
        dataset_results.append(
            {
                "dataset": dataset_path.name,
                "dataset_path": str(dataset_path),
                "total_trades": safe_int(payload.get("total_trades", 0)),
                "profit_factor": safe_float(payload.get("profit_factor", 0.0)),
                "expectancy_krw": safe_float(payload.get("expectancy_krw", 0.0)),
                "strategyless_diag": strategyless_diag,
                "runtime_trade_distribution": runtime_distribution,
            }
        )

    shadow_report = load_json(shadow_report_path) or {}
    shadow_comparison = shadow_report.get("comparison", {}) if isinstance(shadow_report, dict) else {}
    shadow_snapshot = {
        "path": str(shadow_report_path),
        "exists": shadow_report_path.exists(),
        "status": str(shadow_report.get("status", "")) if isinstance(shadow_report, dict) else "",
        "shadow_pass": bool(shadow_report.get("shadow_pass", False)) if isinstance(shadow_report, dict) else False,
        "decision_mismatch_count": safe_int(shadow_comparison.get("decision_mismatch_count", 0))
        if isinstance(shadow_comparison, dict)
        else 0,
        "mismatch_count": safe_int(shadow_comparison.get("mismatch_count", 0))
        if isinstance(shadow_comparison, dict)
        else 0,
        "compared_decision_count": safe_int(shadow_comparison.get("compared_decision_count", 0))
        if isinstance(shadow_comparison, dict)
        else 0,
    }

    live_decision_snapshot = summarize_policy_decision_log(live_decision_log_path)
    backtest_decision_snapshot = summarize_policy_decision_log(backtest_decision_log_path)
    live_execution_snapshot = summarize_execution_log(live_execution_log_path)
    backtest_execution_snapshot = summarize_execution_log(backtest_execution_log_path)
    live_exit_reason_snapshot = summarize_live_exit_reasons(live_runtime_log_path)

    entry_side_shadow_parity_pass = (
        shadow_snapshot["status"] == "pass"
        and shadow_snapshot["mismatch_count"] == 0
        and shadow_snapshot["compared_decision_count"] > 0
    )
    exit_side_parity_evidence_ready = (
        bool(live_execution_snapshot.get("exists", False))
        and bool(backtest_execution_snapshot.get("exists", False))
        and safe_int(live_execution_snapshot.get("event_count", 0)) > 0
        and safe_int(backtest_execution_snapshot.get("event_count", 0)) > 0
    )
    live_buy_count = safe_int((live_execution_snapshot.get("events_by_side", {}) or {}).get("BUY", 0))
    live_sell_count = safe_int((live_execution_snapshot.get("events_by_side", {}) or {}).get("SELL", 0))
    backtest_buy_count = safe_int((backtest_execution_snapshot.get("events_by_side", {}) or {}).get("BUY", 0))
    backtest_sell_count = safe_int((backtest_execution_snapshot.get("events_by_side", {}) or {}).get("SELL", 0))
    full_sell_lifecycle_evidence_ready = (
        exit_side_parity_evidence_ready
        and live_buy_count > 0
        and live_sell_count > 0
    )
    live_exit_reason_observed = safe_int(live_exit_reason_snapshot.get("matched_exit_count", 0)) > 0
    side_comparison_ready = (
        exit_side_parity_evidence_ready
        and live_buy_count > 0
        and backtest_buy_count > 0
    )

    aggregate = aggregate_dataset_results(dataset_results)
    report = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "inputs": {
            "exe_path": str(exe_path),
            "data_dir": str(data_dir),
            "datasets": [str(path) for path in dataset_paths],
            "require_higher_tf_companions": bool(args.require_higher_tf_companions),
            "shadow_report_json": str(shadow_report_path),
            "live_decision_log_jsonl": str(live_decision_log_path),
            "backtest_decision_log_jsonl": str(backtest_decision_log_path),
            "live_execution_log_jsonl": str(live_execution_log_path),
            "backtest_execution_log_jsonl": str(backtest_execution_log_path),
        },
        "dataset_results": dataset_results,
        "aggregate": aggregate,
        "shadow_snapshot": shadow_snapshot,
        "policy_decision_log_snapshot": {
            "live": live_decision_snapshot,
            "backtest": backtest_decision_snapshot,
        },
        "execution_log_snapshot": {
            "live": live_execution_snapshot,
            "backtest": backtest_execution_snapshot,
        },
        "execution_lifecycle_comparison": {
            "live_side_counts": {
                "BUY": live_buy_count,
                "SELL": live_sell_count,
            },
            "backtest_side_counts": {
                "BUY": backtest_buy_count,
                "SELL": backtest_sell_count,
            },
            "live_has_buy_sell": (live_buy_count > 0 and live_sell_count > 0),
            "backtest_has_buy_sell": (backtest_buy_count > 0 and backtest_sell_count > 0),
            "side_comparison_ready": side_comparison_ready,
        },
        "live_exit_reason_snapshot": live_exit_reason_snapshot,
        "conclusions": {
            "entry_side_shadow_parity_pass": entry_side_shadow_parity_pass,
            "exit_side_parity_evidence_ready": exit_side_parity_evidence_ready,
            "full_sell_lifecycle_evidence_ready": full_sell_lifecycle_evidence_ready,
            "exit_event_side_comparison_ready": side_comparison_ready,
            "live_exit_reason_observed": live_exit_reason_observed,
            "risk_exit_signal_rate_high": aggregate.get("risk_exit_signal_rate", 0.0),
            "observed_runtime_backtest_eod_ratio": aggregate.get("runtime_trade_backtest_eod_ratio", 0.0),
            "required_next_evidence": (
                "collect_live_execution_updates_jsonl_for_same_bundle_and_candle_window"
                if not exit_side_parity_evidence_ready
                else (
                    "collect_live_sell_lifecycle_events_for_exit_side_validation"
                    if not full_sell_lifecycle_evidence_ready
                    else (
                        "prime_backtest_execution_updates_for_side_distribution_comparison"
                        if not side_comparison_ready
                        else (
                            "capture_live_exit_reasons_for_reason_label_comparison"
                            if not live_exit_reason_observed
                            else "ready_for_exit_event_parity_comparison"
                        )
                    )
                )
            ),
        },
        "errors": errors,
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[audit] wrote {output_path}")
    print(
        "[audit] entry_side_shadow_parity_pass="
        f"{str(report['conclusions']['entry_side_shadow_parity_pass']).lower()} "
        "exit_side_parity_evidence_ready="
        f"{str(report['conclusions']['exit_side_parity_evidence_ready']).lower()}"
    )
    print(
        "[audit] aggregate_risk_exit_signal_rate="
        f"{report['aggregate']['risk_exit_signal_rate']:.6f} "
        "runtime_backtest_eod_ratio="
        f"{report['aggregate']['runtime_trade_backtest_eod_ratio']:.6f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
