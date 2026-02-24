#!/usr/bin/env python3
"""Aggregate occupancy distortion between two daily OOS reports."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple


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


@dataclass
class DayCell:
    trade_count: int
    profit_sum: float


@dataclass
class DayRow:
    dataset: str
    day_utc: str
    total_profit: float
    total_trades: int
    nonpositive: bool
    cell_map: Dict[str, DayCell]


def _load_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def _parse_cell(cell_name: str) -> Tuple[str, str]:
    parts = str(cell_name).split("|", 1)
    if len(parts) != 2:
        return str(cell_name), str(cell_name)
    return parts[0], parts[1]


def _index_report(report: Dict[str, Any]) -> Dict[Tuple[str, str], DayRow]:
    indexed: Dict[Tuple[str, str], DayRow] = {}
    for row in report.get("daily_results", []):
        if not isinstance(row, dict):
            continue
        dataset = str(row.get("dataset", "")).strip()
        day_utc = str(row.get("day_utc", "")).strip()
        if not dataset or not day_utc:
            continue
        cell_map: Dict[str, DayCell] = {}
        for cell in row.get("cell_breakdown", []):
            if not isinstance(cell, dict):
                continue
            cell_name = str(cell.get("cell", "")).strip()
            if not cell_name:
                continue
            cell_map[cell_name] = DayCell(
                trade_count=_safe_int(cell.get("trade_count", 0)),
                profit_sum=_safe_float(cell.get("profit_sum", 0.0)),
            )
        indexed[(dataset, day_utc)] = DayRow(
            dataset=dataset,
            day_utc=day_utc,
            total_profit=_safe_float(row.get("total_profit", 0.0)),
            total_trades=_safe_int(row.get("total_trades", 0)),
            nonpositive=bool(row.get("nonpositive_profit", False)),
            cell_map=cell_map,
        )
    return indexed


def _empty_day(dataset: str, day_utc: str) -> DayRow:
    return DayRow(
        dataset=dataset,
        day_utc=day_utc,
        total_profit=0.0,
        total_trades=0,
        nonpositive=False,
        cell_map={},
    )


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Aggregate occupancy distortion between baseline and candidate daily OOS reports."
    )
    parser.add_argument("--baseline-json", required=True)
    parser.add_argument("--candidate-json", required=True)
    parser.add_argument("--top-k", type=int, default=12)
    parser.add_argument("--output-json", required=True)
    return parser


def _sorted_rows(rows: Iterable[Dict[str, Any]], key: str, descending: bool = True) -> List[Dict[str, Any]]:
    ranked = list(rows)
    ranked.sort(key=lambda item: _safe_float(item.get(key, 0.0)), reverse=descending)
    return ranked


def main() -> int:
    args = _build_arg_parser().parse_args()
    baseline_path = Path(args.baseline_json).resolve()
    candidate_path = Path(args.candidate_json).resolve()
    output_path = Path(args.output_json).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    baseline_rows = _index_report(_load_json(baseline_path))
    candidate_rows = _index_report(_load_json(candidate_path))
    all_keys = sorted(set(baseline_rows.keys()) | set(candidate_rows.keys()))

    cell_agg: Dict[str, Dict[str, Any]] = defaultdict(
        lambda: {
            "regime": "",
            "entry_archetype": "",
            "day_count": 0,
            "datasets": set(),
            "total_trade_delta": 0,
            "positive_trade_delta": 0,
            "negative_trade_delta": 0,
            "total_profit_delta": 0.0,
            "adverse_expansion_count": 0,
            "adverse_expansion_trade_delta": 0,
            "adverse_expansion_profit_delta": 0.0,
            "favorable_expansion_count": 0,
            "favorable_expansion_trade_delta": 0,
            "favorable_expansion_profit_delta": 0.0,
            "max_abs_profit_delta": 0.0,
        }
    )

    regime_agg: Dict[str, Dict[str, Any]] = defaultdict(
        lambda: {
            "total_trade_delta": 0,
            "total_profit_delta": 0.0,
            "adverse_expansion_trade_delta": 0,
            "adverse_expansion_profit_delta": 0.0,
        }
    )

    adverse_day_cells: List[Dict[str, Any]] = []
    day_rows: List[Dict[str, Any]] = []

    for dataset, day_utc in all_keys:
        base = baseline_rows.get((dataset, day_utc), _empty_day(dataset, day_utc))
        cand = candidate_rows.get((dataset, day_utc), _empty_day(dataset, day_utc))

        day_rows.append(
            {
                "dataset": dataset,
                "day_utc": day_utc,
                "baseline_total_profit": base.total_profit,
                "candidate_total_profit": cand.total_profit,
                "total_profit_delta": round(cand.total_profit - base.total_profit, 6),
                "baseline_total_trades": base.total_trades,
                "candidate_total_trades": cand.total_trades,
                "total_trade_delta": cand.total_trades - base.total_trades,
                "baseline_nonpositive": base.nonpositive,
                "candidate_nonpositive": cand.nonpositive,
            }
        )

        cell_keys = sorted(set(base.cell_map.keys()) | set(cand.cell_map.keys()))
        for cell in cell_keys:
            b_cell = base.cell_map.get(cell, DayCell(0, 0.0))
            c_cell = cand.cell_map.get(cell, DayCell(0, 0.0))
            trade_delta = c_cell.trade_count - b_cell.trade_count
            profit_delta = c_cell.profit_sum - b_cell.profit_sum
            regime, archetype = _parse_cell(cell)

            slot = cell_agg[cell]
            slot["regime"] = regime
            slot["entry_archetype"] = archetype
            slot["day_count"] += 1
            slot["datasets"].add(dataset)
            slot["total_trade_delta"] += trade_delta
            slot["total_profit_delta"] += profit_delta
            slot["max_abs_profit_delta"] = max(slot["max_abs_profit_delta"], abs(profit_delta))
            if trade_delta > 0:
                slot["positive_trade_delta"] += trade_delta
            elif trade_delta < 0:
                slot["negative_trade_delta"] += trade_delta

            reg_slot = regime_agg[regime]
            reg_slot["total_trade_delta"] += trade_delta
            reg_slot["total_profit_delta"] += profit_delta

            if trade_delta > 0 and profit_delta < 0.0:
                slot["adverse_expansion_count"] += 1
                slot["adverse_expansion_trade_delta"] += trade_delta
                slot["adverse_expansion_profit_delta"] += profit_delta
                reg_slot["adverse_expansion_trade_delta"] += trade_delta
                reg_slot["adverse_expansion_profit_delta"] += profit_delta
                adverse_day_cells.append(
                    {
                        "dataset": dataset,
                        "day_utc": day_utc,
                        "cell": cell,
                        "regime": regime,
                        "entry_archetype": archetype,
                        "trade_delta": trade_delta,
                        "profit_delta": round(profit_delta, 6),
                        "baseline_trade_count": b_cell.trade_count,
                        "candidate_trade_count": c_cell.trade_count,
                        "baseline_profit_sum": round(b_cell.profit_sum, 6),
                        "candidate_profit_sum": round(c_cell.profit_sum, 6),
                    }
                )
            elif trade_delta > 0 and profit_delta > 0.0:
                slot["favorable_expansion_count"] += 1
                slot["favorable_expansion_trade_delta"] += trade_delta
                slot["favorable_expansion_profit_delta"] += profit_delta

    cell_rows: List[Dict[str, Any]] = []
    for cell_name, slot in cell_agg.items():
        row = dict(slot)
        row["cell"] = cell_name
        row["datasets"] = sorted(slot["datasets"])
        for key in (
            "total_profit_delta",
            "adverse_expansion_profit_delta",
            "favorable_expansion_profit_delta",
            "max_abs_profit_delta",
        ):
            row[key] = round(_safe_float(row[key]), 6)
        cell_rows.append(row)

    regime_rows: List[Dict[str, Any]] = []
    for regime, slot in regime_agg.items():
        row = {"regime": regime}
        row.update(slot)
        row["total_profit_delta"] = round(_safe_float(row["total_profit_delta"]), 6)
        row["adverse_expansion_profit_delta"] = round(_safe_float(row["adverse_expansion_profit_delta"]), 6)
        regime_rows.append(row)

    top_k = max(1, int(args.top_k))
    adverse_day_cells.sort(key=lambda r: (_safe_float(r["profit_delta"]), -_safe_int(r["trade_delta"])))
    top_adverse_cells = sorted(
        [
            row
            for row in cell_rows
            if _safe_int(row["adverse_expansion_trade_delta"]) > 0
        ],
        key=lambda r: (
            _safe_float(r["adverse_expansion_profit_delta"]),
            -_safe_int(r["adverse_expansion_trade_delta"]),
            r["cell"],
        ),
    )[:top_k]
    top_expansion_cells = _sorted_rows(cell_rows, key="positive_trade_delta", descending=True)[:top_k]
    top_regime_adverse = sorted(
        regime_rows,
        key=lambda r: (_safe_float(r["adverse_expansion_profit_delta"]), -_safe_int(r["adverse_expansion_trade_delta"])),
    )[:top_k]
    top_day_profit_deltas = sorted(day_rows, key=lambda r: _safe_float(r["total_profit_delta"]))[:top_k]

    summary = {
        "day_count": len(day_rows),
        "cell_count": len(cell_rows),
        "baseline_profit_sum": round(sum(_safe_float(r["baseline_total_profit"]) for r in day_rows), 6),
        "candidate_profit_sum": round(sum(_safe_float(r["candidate_total_profit"]) for r in day_rows), 6),
        "profit_sum_delta": round(sum(_safe_float(r["total_profit_delta"]) for r in day_rows), 6),
        "baseline_nonpositive_day_count": sum(1 for r in day_rows if bool(r["baseline_nonpositive"])),
        "candidate_nonpositive_day_count": sum(1 for r in day_rows if bool(r["candidate_nonpositive"])),
        "adverse_day_cell_count": len(adverse_day_cells),
    }

    result = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "inputs": {
            "baseline_json": str(baseline_path),
            "candidate_json": str(candidate_path),
            "top_k": top_k,
        },
        "summary": summary,
        "top_adverse_cells": top_adverse_cells,
        "top_expansion_cells": top_expansion_cells,
        "top_regime_adverse": top_regime_adverse,
        "top_adverse_day_cells": adverse_day_cells[:top_k],
        "top_day_profit_deltas": top_day_profit_deltas,
        "all_cell_aggregates": cell_rows,
        "all_regime_aggregates": regime_rows,
        "all_day_deltas": day_rows,
    }

    with output_path.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(result, fp, ensure_ascii=False, indent=2)

    print(f"[V15Distortion] wrote {output_path}")
    print(
        "[V15Distortion] "
        f"profit_sum_delta={summary['profit_sum_delta']:.6f} "
        f"adverse_day_cell_count={summary['adverse_day_cell_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
