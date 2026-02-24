#!/usr/bin/env python3
"""Evaluate spillover lock constraints from v15 occupancy distortion report."""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Set


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return default


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def _parse_csv_set(raw: str) -> Set[str]:
    return {token.strip() for token in str(raw).split(",") if token.strip()}


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Evaluate v16 spillover lock constraints from occupancy distortion JSON."
    )
    parser.add_argument("--distortion-json", required=True)
    parser.add_argument(
        "--target-cells",
        default="TRENDING_UP|CORE_RESCUE_SHOULD_ENTER",
        help="Comma-separated intended target cells for the probe.",
    )
    parser.add_argument(
        "--hard-max-nontarget-positive-trade-delta",
        type=int,
        default=0,
        help="Hard upper bound for total non-target positive_trade_delta.",
    )
    parser.add_argument(
        "--hard-min-nontarget-adverse-profit-delta",
        type=float,
        default=0.0,
        help="Hard lower bound for non-target adverse_expansion_profit_delta sum.",
    )
    parser.add_argument(
        "--hard-max-nonpositive-day-count-delta",
        type=int,
        default=0,
        help="Hard upper bound for candidate-baseline nonpositive day count delta.",
    )
    parser.add_argument("--output-json", required=True)
    return parser


def main() -> int:
    args = _build_arg_parser().parse_args()
    distortion_path = Path(args.distortion_json).resolve()
    output_path = Path(args.output_json).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    payload = json.loads(distortion_path.read_text(encoding="utf-8"))
    target_cells = _parse_csv_set(args.target_cells)
    cell_rows = payload.get("all_cell_aggregates", [])
    if not isinstance(cell_rows, list):
        cell_rows = []

    target_rows: List[Dict[str, Any]] = []
    nontarget_rows: List[Dict[str, Any]] = []
    for row in cell_rows:
        if not isinstance(row, dict):
            continue
        cell = str(row.get("cell", "")).strip()
        if not cell:
            continue
        if cell in target_cells:
            target_rows.append(row)
        else:
            nontarget_rows.append(row)

    nontarget_positive_trade_delta_sum = sum(
        _safe_int(row.get("positive_trade_delta", 0)) for row in nontarget_rows
    )
    nontarget_adverse_profit_delta_sum = sum(
        _safe_float(row.get("adverse_expansion_profit_delta", 0.0)) for row in nontarget_rows
    )
    nontarget_adverse_rows = [
        {
            "cell": str(row.get("cell", "")),
            "regime": str(row.get("regime", "")),
            "entry_archetype": str(row.get("entry_archetype", "")),
            "positive_trade_delta": _safe_int(row.get("positive_trade_delta", 0)),
            "adverse_expansion_trade_delta": _safe_int(row.get("adverse_expansion_trade_delta", 0)),
            "adverse_expansion_profit_delta": round(
                _safe_float(row.get("adverse_expansion_profit_delta", 0.0)), 6
            ),
            "total_profit_delta": round(_safe_float(row.get("total_profit_delta", 0.0)), 6),
        }
        for row in nontarget_rows
        if _safe_int(row.get("positive_trade_delta", 0)) > 0
        and _safe_float(row.get("adverse_expansion_profit_delta", 0.0)) < 0.0
    ]
    nontarget_adverse_rows.sort(
        key=lambda row: (
            _safe_float(row.get("adverse_expansion_profit_delta", 0.0)),
            -_safe_int(row.get("adverse_expansion_trade_delta", 0)),
            row.get("cell", ""),
        )
    )

    summary = payload.get("summary", {})
    nonpositive_day_count_delta = _safe_int(summary.get("candidate_nonpositive_day_count", 0)) - _safe_int(
        summary.get("baseline_nonpositive_day_count", 0)
    )

    checks = {
        "nontarget_positive_trade_delta": {
            "pass": nontarget_positive_trade_delta_sum
            <= int(args.hard_max_nontarget_positive_trade_delta),
            "actual": nontarget_positive_trade_delta_sum,
            "threshold_max": int(args.hard_max_nontarget_positive_trade_delta),
        },
        "nontarget_adverse_profit_delta": {
            "pass": nontarget_adverse_profit_delta_sum
            >= float(args.hard_min_nontarget_adverse_profit_delta),
            "actual": round(nontarget_adverse_profit_delta_sum, 6),
            "threshold_min": float(args.hard_min_nontarget_adverse_profit_delta),
        },
        "nonpositive_day_count_delta": {
            "pass": nonpositive_day_count_delta <= int(args.hard_max_nonpositive_day_count_delta),
            "actual": nonpositive_day_count_delta,
            "threshold_max": int(args.hard_max_nonpositive_day_count_delta),
        },
    }
    fail_reasons = [name for name, check in checks.items() if not bool(check.get("pass", False))]
    status = "pass" if not fail_reasons else "fail"

    result = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": status,
        "fail_reasons": fail_reasons,
        "inputs": {
            "distortion_json": str(distortion_path),
            "target_cells": sorted(target_cells),
            "hard_max_nontarget_positive_trade_delta": int(args.hard_max_nontarget_positive_trade_delta),
            "hard_min_nontarget_adverse_profit_delta": float(args.hard_min_nontarget_adverse_profit_delta),
            "hard_max_nonpositive_day_count_delta": int(args.hard_max_nonpositive_day_count_delta),
        },
        "checks": checks,
        "target_cells_present": sorted(str(row.get("cell", "")) for row in target_rows),
        "nontarget_adverse_rows": nontarget_adverse_rows,
        "source_summary": summary,
    }

    with output_path.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(result, fp, ensure_ascii=False, indent=2)

    print(f"[V16SpilloverLock] wrote {output_path}")
    print(
        "[V16SpilloverLock] "
        f"status={status} "
        f"nontarget_positive_trade_delta={nontarget_positive_trade_delta_sum} "
        f"nonpositive_day_count_delta={nonpositive_day_count_delta}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
