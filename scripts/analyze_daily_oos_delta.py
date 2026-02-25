#!/usr/bin/env python3
"""Compare two daily OOS reports and extract profit/trade/archetype shift deltas."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Tuple


@dataclass
class DayRow:
    dataset: str
    day_utc: str
    total_profit: float
    total_trades: int
    evaluated: bool
    profitable: bool
    nonpositive_profit: bool
    cell_map: Dict[str, Dict[str, float]]


def _load_json(path: Path) -> Dict:
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def _safe_float(value, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return default


def _safe_int(value, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def _index_daily_rows(report: Dict) -> Dict[Tuple[str, str], DayRow]:
    indexed: Dict[Tuple[str, str], DayRow] = {}
    for row in report.get("daily_results", []):
        dataset = str(row.get("dataset", "")).strip()
        day_utc = str(row.get("day_utc", "")).strip()
        if not dataset or not day_utc:
            continue
        cell_map: Dict[str, Dict[str, float]] = {}
        for cell in row.get("cell_breakdown", []):
            cell_name = str(cell.get("cell", "")).strip()
            if not cell_name:
                continue
            cell_map[cell_name] = {
                "trade_count": _safe_int(cell.get("trade_count", 0)),
                "profit_sum": _safe_float(cell.get("profit_sum", 0.0)),
            }
        key = (dataset, day_utc)
        indexed[key] = DayRow(
            dataset=dataset,
            day_utc=day_utc,
            total_profit=_safe_float(row.get("total_profit", 0.0)),
            total_trades=_safe_int(row.get("total_trades", 0)),
            evaluated=bool(row.get("evaluated", False)),
            profitable=bool(row.get("profitable", False)),
            nonpositive_profit=bool(row.get("nonpositive_profit", False)),
            cell_map=cell_map,
        )
    return indexed


def _empty_row(dataset: str, day_utc: str) -> DayRow:
    return DayRow(
        dataset=dataset,
        day_utc=day_utc,
        total_profit=0.0,
        total_trades=0,
        evaluated=False,
        profitable=False,
        nonpositive_profit=False,
        cell_map={},
    )


def _extract_gate_aggregates(report: Dict) -> Dict[str, float]:
    aggregates = report.get("aggregates", {})
    if not isinstance(aggregates, dict):
        aggregates = {}
    return {
        "evaluated_day_count": float(_safe_int(aggregates.get("evaluated_day_count", 0))),
        "nonpositive_day_count": float(_safe_int(aggregates.get("nonpositive_day_count", 0))),
        "total_profit_sum": float(_safe_float(aggregates.get("total_profit_sum", 0.0))),
    }


def _summarize_evaluated_scope(day_delta_rows: List[Dict]) -> Dict:
    union_rows = [row for row in day_delta_rows if bool(row.get("baseline_evaluated")) or bool(row.get("candidate_evaluated"))]
    intersection_rows = [row for row in union_rows if bool(row.get("baseline_evaluated")) and bool(row.get("candidate_evaluated"))]

    baseline_profit_sum_union = sum(
        _safe_float(row.get("baseline_profit", 0.0)) for row in union_rows if bool(row.get("baseline_evaluated"))
    )
    candidate_profit_sum_union = sum(
        _safe_float(row.get("candidate_profit", 0.0)) for row in union_rows if bool(row.get("candidate_evaluated"))
    )
    baseline_nonpositive_union = sum(
        1 for row in union_rows if bool(row.get("baseline_evaluated")) and bool(row.get("baseline_nonpositive"))
    )
    candidate_nonpositive_union = sum(
        1 for row in union_rows if bool(row.get("candidate_evaluated")) and bool(row.get("candidate_nonpositive"))
    )

    baseline_profit_sum_intersection = sum(_safe_float(row.get("baseline_profit", 0.0)) for row in intersection_rows)
    candidate_profit_sum_intersection = sum(_safe_float(row.get("candidate_profit", 0.0)) for row in intersection_rows)
    baseline_nonpositive_intersection = sum(1 for row in intersection_rows if bool(row.get("baseline_nonpositive")))
    candidate_nonpositive_intersection = sum(1 for row in intersection_rows if bool(row.get("candidate_nonpositive")))

    return {
        "union_day_count": len(union_rows),
        "intersection_day_count": len(intersection_rows),
        "baseline_profit_sum_union": baseline_profit_sum_union,
        "candidate_profit_sum_union": candidate_profit_sum_union,
        "profit_sum_delta_union": candidate_profit_sum_union - baseline_profit_sum_union,
        "baseline_nonpositive_day_count_union": baseline_nonpositive_union,
        "candidate_nonpositive_day_count_union": candidate_nonpositive_union,
        "nonpositive_day_count_delta_union": candidate_nonpositive_union - baseline_nonpositive_union,
        "baseline_profit_sum_intersection": baseline_profit_sum_intersection,
        "candidate_profit_sum_intersection": candidate_profit_sum_intersection,
        "profit_sum_delta_intersection": candidate_profit_sum_intersection - baseline_profit_sum_intersection,
        "baseline_nonpositive_day_count_intersection": baseline_nonpositive_intersection,
        "candidate_nonpositive_day_count_intersection": candidate_nonpositive_intersection,
        "nonpositive_day_count_delta_intersection": candidate_nonpositive_intersection - baseline_nonpositive_intersection,
    }


def _archetype_aggregate(cell_map: Dict[str, Dict[str, float]]) -> Dict[str, Dict[str, float]]:
    aggregated: Dict[str, Dict[str, float]] = {}
    for cell_name, stats in cell_map.items():
        parts = cell_name.split("|", 1)
        archetype = parts[1] if len(parts) == 2 else cell_name
        bucket = aggregated.setdefault(archetype, {"trade_count": 0.0, "profit_sum": 0.0})
        bucket["trade_count"] += _safe_float(stats.get("trade_count", 0.0))
        bucket["profit_sum"] += _safe_float(stats.get("profit_sum", 0.0))
    return aggregated


def _dominant_archetype(cell_map: Dict[str, Dict[str, float]]) -> str:
    agg = _archetype_aggregate(cell_map)
    if not agg:
        return ""
    ranked = sorted(
        agg.items(),
        key=lambda kv: (
            -_safe_float(kv[1].get("trade_count", 0.0)),
            -abs(_safe_float(kv[1].get("profit_sum", 0.0))),
            kv[0],
        ),
    )
    return ranked[0][0]


def _top_cell_deltas(
    baseline_cell_map: Dict[str, Dict[str, float]],
    candidate_cell_map: Dict[str, Dict[str, float]],
    limit: int = 5,
) -> List[Dict]:
    rows: List[Dict] = []
    keys = sorted(set(baseline_cell_map.keys()) | set(candidate_cell_map.keys()))
    for key in keys:
        b = baseline_cell_map.get(key, {})
        c = candidate_cell_map.get(key, {})
        b_profit = _safe_float(b.get("profit_sum", 0.0))
        c_profit = _safe_float(c.get("profit_sum", 0.0))
        b_trades = _safe_int(b.get("trade_count", 0))
        c_trades = _safe_int(c.get("trade_count", 0))
        rows.append(
            {
                "cell": key,
                "baseline_profit_sum": b_profit,
                "candidate_profit_sum": c_profit,
                "profit_delta": c_profit - b_profit,
                "baseline_trade_count": b_trades,
                "candidate_trade_count": c_trades,
                "trade_delta": c_trades - b_trades,
            }
        )
    rows.sort(key=lambda r: (-abs(_safe_float(r["profit_delta"])), r["cell"]))
    return rows[: max(1, int(limit))]


def _parse_csv_set(raw: str) -> set:
    values = set()
    for token in str(raw).split(","):
        value = token.strip()
        if value:
            values.add(value)
    return values


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Compare two daily OOS stability reports and summarize distortion deltas."
    )
    parser.add_argument("--baseline-json", required=True, help="Baseline daily OOS report JSON path.")
    parser.add_argument("--candidate-json", required=True, help="Candidate/probe daily OOS report JSON path.")
    parser.add_argument("--focus-datasets", default="", help="Optional comma-separated dataset names.")
    parser.add_argument("--focus-days", default="", help="Optional comma-separated UTC day labels (YYYY-MM-DD).")
    parser.add_argument("--top-k", type=int, default=10, help="Row count for top negative delta summaries.")
    parser.add_argument("--output-json", required=True, help="Output JSON path.")
    return parser


def main() -> int:
    args = _build_arg_parser().parse_args()
    baseline_path = Path(args.baseline_json).resolve()
    candidate_path = Path(args.candidate_json).resolve()
    output_path = Path(args.output_json).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    baseline_report = _load_json(baseline_path)
    candidate_report = _load_json(candidate_path)
    baseline_rows = _index_daily_rows(baseline_report)
    candidate_rows = _index_daily_rows(candidate_report)
    baseline_gate = _extract_gate_aggregates(baseline_report)
    candidate_gate = _extract_gate_aggregates(candidate_report)

    focus_datasets = _parse_csv_set(args.focus_datasets)
    focus_days = _parse_csv_set(args.focus_days)
    top_k = max(1, int(args.top_k))

    all_keys = sorted(set(baseline_rows.keys()) | set(candidate_rows.keys()))
    day_delta_rows: List[Dict] = []
    for dataset, day_utc in all_keys:
        baseline = baseline_rows.get((dataset, day_utc), _empty_row(dataset, day_utc))
        candidate = candidate_rows.get((dataset, day_utc), _empty_row(dataset, day_utc))

        dominant_baseline = _dominant_archetype(baseline.cell_map)
        dominant_candidate = _dominant_archetype(candidate.cell_map)
        row = {
            "dataset": dataset,
            "day_utc": day_utc,
            "baseline_profit": baseline.total_profit,
            "candidate_profit": candidate.total_profit,
            "profit_delta": candidate.total_profit - baseline.total_profit,
            "baseline_trades": baseline.total_trades,
            "candidate_trades": candidate.total_trades,
            "trade_delta": candidate.total_trades - baseline.total_trades,
            "baseline_evaluated": baseline.evaluated,
            "candidate_evaluated": candidate.evaluated,
            "baseline_nonpositive": baseline.nonpositive_profit,
            "candidate_nonpositive": candidate.nonpositive_profit,
            "dominant_archetype_baseline": dominant_baseline,
            "dominant_archetype_candidate": dominant_candidate,
            "dominant_archetype_switched": bool(dominant_baseline != dominant_candidate),
            "top_cell_profit_deltas": _top_cell_deltas(
                baseline_cell_map=baseline.cell_map,
                candidate_cell_map=candidate.cell_map,
                limit=5,
            ),
        }
        day_delta_rows.append(row)

    if focus_datasets or focus_days:
        focus_rows = [
            row
            for row in day_delta_rows
            if (not focus_datasets or row["dataset"] in focus_datasets)
            and (not focus_days or row["day_utc"] in focus_days)
        ]
    else:
        focus_rows = list(day_delta_rows)

    focus_rows_sorted = sorted(focus_rows, key=lambda r: (_safe_float(r["profit_delta"]), r["dataset"], r["day_utc"]))
    top_negative_profit_deltas = focus_rows_sorted[:top_k]
    negative_trade_expansion_days = [
        row for row in focus_rows_sorted if _safe_float(row["profit_delta"]) < 0.0 and _safe_int(row["trade_delta"]) > 0
    ][:top_k]
    archetype_switch_negative_days = [
        row
        for row in focus_rows_sorted
        if bool(row["dominant_archetype_switched"]) and _safe_float(row["profit_delta"]) < 0.0
    ][:top_k]

    baseline_profit_sum = sum(_safe_float(row["baseline_profit"]) for row in day_delta_rows)
    candidate_profit_sum = sum(_safe_float(row["candidate_profit"]) for row in day_delta_rows)
    baseline_nonpositive_days = sum(1 for row in day_delta_rows if bool(row["baseline_nonpositive"]))
    candidate_nonpositive_days = sum(1 for row in day_delta_rows if bool(row["candidate_nonpositive"]))
    focus_baseline_profit_sum = sum(_safe_float(row["baseline_profit"]) for row in focus_rows)
    focus_candidate_profit_sum = sum(_safe_float(row["candidate_profit"]) for row in focus_rows)
    focus_baseline_nonpositive_days = sum(1 for row in focus_rows if bool(row["baseline_nonpositive"]))
    focus_candidate_nonpositive_days = sum(1 for row in focus_rows if bool(row["candidate_nonpositive"]))
    evaluated_row_alignment = _summarize_evaluated_scope(day_delta_rows)
    gate_aligned_profit_sum_delta = _safe_float(candidate_gate.get("total_profit_sum", 0.0)) - _safe_float(
        baseline_gate.get("total_profit_sum", 0.0)
    )
    gate_aligned_nonpositive_day_count_delta = _safe_int(candidate_gate.get("nonpositive_day_count", 0)) - _safe_int(
        baseline_gate.get("nonpositive_day_count", 0)
    )

    result = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "inputs": {
            "baseline_json": str(baseline_path),
            "candidate_json": str(candidate_path),
            "focus_datasets": sorted(focus_datasets),
            "focus_days": sorted(focus_days),
            "top_k": top_k,
        },
        "summary": {
            "summary_basis": "all_day_rows_union",
            "day_count": len(day_delta_rows),
            "baseline_profit_sum": baseline_profit_sum,
            "candidate_profit_sum": candidate_profit_sum,
            "profit_sum_delta": candidate_profit_sum - baseline_profit_sum,
            "baseline_nonpositive_day_count": baseline_nonpositive_days,
            "candidate_nonpositive_day_count": candidate_nonpositive_days,
            "nonpositive_day_count_delta": candidate_nonpositive_days - baseline_nonpositive_days,
            "negative_trade_expansion_count": len(
                [row for row in day_delta_rows if _safe_float(row["profit_delta"]) < 0.0 and _safe_int(row["trade_delta"]) > 0]
            ),
            "dominant_archetype_switch_count": len(
                [row for row in day_delta_rows if bool(row["dominant_archetype_switched"])]
            ),
            "focus_day_count": len(focus_rows),
            "focus_profit_sum_delta": focus_candidate_profit_sum - focus_baseline_profit_sum,
            "focus_nonpositive_day_count_delta": focus_candidate_nonpositive_days - focus_baseline_nonpositive_days,
            "gate_aligned": {
                "baseline_evaluated_day_count": _safe_int(baseline_gate.get("evaluated_day_count", 0)),
                "candidate_evaluated_day_count": _safe_int(candidate_gate.get("evaluated_day_count", 0)),
                "baseline_nonpositive_day_count": _safe_int(baseline_gate.get("nonpositive_day_count", 0)),
                "candidate_nonpositive_day_count": _safe_int(candidate_gate.get("nonpositive_day_count", 0)),
                "nonpositive_day_count_delta": gate_aligned_nonpositive_day_count_delta,
                "baseline_total_profit_sum": _safe_float(baseline_gate.get("total_profit_sum", 0.0)),
                "candidate_total_profit_sum": _safe_float(candidate_gate.get("total_profit_sum", 0.0)),
                "profit_sum_delta": gate_aligned_profit_sum_delta,
            },
            "evaluated_row_alignment": evaluated_row_alignment,
        },
        "top_negative_profit_deltas": top_negative_profit_deltas,
        "negative_trade_expansion_days": negative_trade_expansion_days,
        "archetype_switch_negative_days": archetype_switch_negative_days,
        "all_day_deltas": day_delta_rows,
    }
    with output_path.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(result, fp, ensure_ascii=False, indent=2)

    print(f"[DailyOOSDelta] wrote {output_path}")
    print(
        "[DailyOOSDelta] "
        f"profit_sum_delta={result['summary']['profit_sum_delta']:.6f} "
        f"nonpositive_day_count_delta={result['summary']['nonpositive_day_count_delta']} "
        f"gate_profit_sum_delta={result['summary']['gate_aligned']['profit_sum_delta']:.6f} "
        f"gate_nonpositive_day_count_delta={result['summary']['gate_aligned']['nonpositive_day_count_delta']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
