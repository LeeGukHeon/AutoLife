#!/usr/bin/env python3
"""Collect target-day trade profiles from daily OOS report slices."""

from __future__ import annotations

import argparse
import json
import subprocess
from collections import defaultdict
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Set


def _to_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return default


def _to_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def _utc_day_ms(ts_ms: int) -> str:
    return datetime.fromtimestamp(max(0, ts_ms) / 1000.0, tz=UTC).strftime("%Y-%m-%d")


def _parse_csv_set(raw: str) -> Set[str]:
    return {token.strip() for token in str(raw).split(",") if token.strip()}


def _parse_last_json(stdout: str, stderr: str) -> Dict[str, Any]:
    lines: List[str] = []
    if stdout:
        lines.extend(stdout.splitlines())
    if stderr:
        lines.extend(stderr.splitlines())
    for line in reversed(lines):
        text = line.strip()
        if not (text.startswith("{") and text.endswith("}")):
            continue
        try:
            parsed = json.loads(text)
        except Exception:
            continue
        if isinstance(parsed, dict):
            return parsed
    raise RuntimeError("backtest json payload not found in command output")


def _run_backtest_json(exe_path: Path, slice_csv: Path) -> Dict[str, Any]:
    cmd = [str(exe_path), "--backtest", str(slice_csv), "--json"]
    completed = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        cwd=str(exe_path.parent),
    )
    if completed.returncode != 0:
        tail = "\n".join((completed.stderr or "").splitlines()[-20:])
        raise RuntimeError(f"backtest failed for {slice_csv} exit={completed.returncode}\n{tail}")
    return _parse_last_json(completed.stdout, completed.stderr)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Replay daily OOS slice CSVs and extract target-day trade profiles."
    )
    parser.add_argument(
        "--daily-report-json",
        required=True,
        help="Path to daily_oos_stability_report_*.json generated with --keep-temp.",
    )
    parser.add_argument(
        "--exe-path",
        default=r".\build\Release\AutoLifeTrading.exe",
    )
    parser.add_argument(
        "--target-days",
        default="",
        help="Optional comma-separated UTC days (YYYY-MM-DD).",
    )
    parser.add_argument(
        "--target-cells",
        default="",
        help="Optional comma-separated cells (e.g., TRENDING_UP|CORE_RESCUE_SHOULD_ENTER).",
    )
    parser.add_argument(
        "--target-regimes",
        default="",
        help="Optional comma-separated regimes.",
    )
    parser.add_argument(
        "--target-archetypes",
        default="",
        help="Optional comma-separated entry archetypes.",
    )
    parser.add_argument(
        "--only-evaluated",
        action="store_true",
        help="Include only daily rows where evaluated=true.",
    )
    parser.add_argument(
        "--only-negative-days",
        action="store_true",
        help="Include only daily rows where total_profit <= 0.",
    )
    parser.add_argument(
        "--output-json",
        required=True,
    )
    return parser.parse_args()


def _cell_key(trade: Dict[str, Any]) -> str:
    regime = str(trade.get("regime", "UNKNOWN")).strip() or "UNKNOWN"
    archetype = str(trade.get("entry_archetype", "UNSPECIFIED")).strip() or "UNSPECIFIED"
    return f"{regime}|{archetype}"


def _trade_profile_row(dataset: str, day_utc: str, trade: Dict[str, Any]) -> Dict[str, Any]:
    exit_time = _to_int(trade.get("exit_time", trade.get("entry_time", 0)))
    entry_time = _to_int(trade.get("entry_time", 0))
    pnl = _to_float(trade.get("profit_loss_krw", 0.0))
    return {
        "dataset": dataset,
        "day_utc": day_utc,
        "market": str(trade.get("market", "")).strip(),
        "cell": _cell_key(trade),
        "regime": str(trade.get("regime", "")).strip(),
        "entry_archetype": str(trade.get("entry_archetype", "")).strip(),
        "entry_time": entry_time,
        "exit_time": exit_time,
        "exit_day_utc": _utc_day_ms(exit_time),
        "holding_minutes": round(_to_float(trade.get("holding_minutes", 0.0)), 6),
        "profit_loss_krw": round(pnl, 6),
        "exit_reason": str(trade.get("exit_reason", "")).strip(),
        "signal_strength": round(_to_float(trade.get("signal_strength", 0.0)), 6),
        "liquidity_score": round(_to_float(trade.get("liquidity_score", 0.0)), 6),
        "volatility": round(_to_float(trade.get("volatility", 0.0)), 6),
        "probabilistic_h5_calibrated": round(_to_float(trade.get("probabilistic_h5_calibrated", 0.0)), 6),
        "probabilistic_h5_margin": round(_to_float(trade.get("probabilistic_h5_margin", 0.0)), 6),
        "expected_value": round(_to_float(trade.get("expected_value", 0.0)), 6),
        "reward_risk_ratio": round(_to_float(trade.get("reward_risk_ratio", 0.0)), 6),
    }


def main() -> int:
    args = _parse_args()
    report_path = Path(args.daily_report_json).resolve()
    exe_path = Path(args.exe_path).resolve()
    out_path = Path(args.output_json).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    payload = json.loads(report_path.read_text(encoding="utf-8"))
    rows = payload.get("daily_results", [])
    if not isinstance(rows, list):
        rows = []

    target_days = _parse_csv_set(args.target_days)
    target_cells = _parse_csv_set(args.target_cells)
    target_regimes = _parse_csv_set(args.target_regimes)
    target_archetypes = _parse_csv_set(args.target_archetypes)

    selected_rows: List[Dict[str, Any]] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        day_utc = str(row.get("day_utc", "")).strip()
        if target_days and day_utc not in target_days:
            continue
        if bool(args.only_evaluated) and not bool(row.get("evaluated", False)):
            continue
        if bool(args.only_negative_days) and _to_float(row.get("total_profit", 0.0)) > 0.0:
            continue
        selected_rows.append(row)

    trade_rows: List[Dict[str, Any]] = []
    row_errors: List[Dict[str, Any]] = []

    for row in selected_rows:
        dataset = str(row.get("dataset", "")).strip()
        day_utc = str(row.get("day_utc", "")).strip()
        slice_csv = Path(str(row.get("slice_csv", "")).strip())
        if not slice_csv.exists():
            row_errors.append(
                {
                    "dataset": dataset,
                    "day_utc": day_utc,
                    "error": "missing_slice_csv",
                    "slice_csv": str(slice_csv),
                }
            )
            continue
        try:
            backtest_payload = _run_backtest_json(exe_path, slice_csv)
        except Exception as exc:
            row_errors.append(
                {
                    "dataset": dataset,
                    "day_utc": day_utc,
                    "error": "backtest_failed",
                    "message": str(exc),
                    "slice_csv": str(slice_csv),
                }
            )
            continue

        trades = backtest_payload.get("trade_history_samples", [])
        if not isinstance(trades, list):
            trades = []
        for trade in trades:
            if not isinstance(trade, dict):
                continue
            exit_time = _to_int(trade.get("exit_time", trade.get("entry_time", 0)))
            trade_day = _utc_day_ms(exit_time)
            if trade_day != day_utc:
                continue
            trade_row = _trade_profile_row(dataset, day_utc, trade)
            if target_cells and trade_row["cell"] not in target_cells:
                continue
            if target_regimes and trade_row["regime"] not in target_regimes:
                continue
            if target_archetypes and trade_row["entry_archetype"] not in target_archetypes:
                continue
            trade_rows.append(trade_row)

    by_cell: Dict[str, Dict[str, Any]] = defaultdict(
        lambda: {"trade_count": 0, "profit_sum_krw": 0.0, "days": set(), "datasets": set()}
    )
    for row in trade_rows:
        cell = row["cell"]
        slot = by_cell[cell]
        slot["trade_count"] += 1
        slot["profit_sum_krw"] += _to_float(row["profit_loss_krw"], 0.0)
        slot["days"].add(str(row["day_utc"]))
        slot["datasets"].add(str(row["dataset"]))

    by_cell_rows: List[Dict[str, Any]] = []
    for cell, slot in by_cell.items():
        by_cell_rows.append(
            {
                "cell": cell,
                "trade_count": int(slot["trade_count"]),
                "profit_sum_krw": round(_to_float(slot["profit_sum_krw"]), 6),
                "days": sorted(slot["days"]),
                "datasets": sorted(slot["datasets"]),
            }
        )
    by_cell_rows.sort(key=lambda x: (_to_float(x["profit_sum_krw"]), -_to_int(x["trade_count"]), x["cell"]))

    result = {
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "inputs": {
            "daily_report_json": str(report_path),
            "exe_path": str(exe_path),
            "target_days": sorted(target_days),
            "target_cells": sorted(target_cells),
            "target_regimes": sorted(target_regimes),
            "target_archetypes": sorted(target_archetypes),
            "only_evaluated": bool(args.only_evaluated),
            "only_negative_days": bool(args.only_negative_days),
        },
        "selected_daily_rows": len(selected_rows),
        "row_errors": row_errors,
        "trade_row_count": len(trade_rows),
        "by_cell": by_cell_rows,
        "trade_rows": trade_rows,
    }
    out_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")
    print(f"[DailyOosTradeProfile] wrote {out_path}")
    print(
        "[DailyOosTradeProfile] "
        f"selected_rows={len(selected_rows)} trade_rows={len(trade_rows)} errors={len(row_errors)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
