#!/usr/bin/env python3
"""Compare occupancy and re-entry drift between two trade-profile snapshots."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from datetime import UTC, datetime
from pathlib import Path
from statistics import median
from typing import Any, Dict, Iterable, List, Tuple

RUNTIME_ARCHETYPE = "PROBABILISTIC_PRIMARY_RUNTIME"
RESCUE_TOKEN = "CORE_RESCUE"


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


def _parse_csv_set(raw: str) -> List[str]:
    return [token.strip() for token in str(raw).split(",") if token.strip()]


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze occupancy/re-entry drift between baseline and candidate trade-profile dumps."
    )
    parser.add_argument("--baseline-profile-json", required=True)
    parser.add_argument("--candidate-profile-json", required=True)
    parser.add_argument("--focus-markets", default="", help="Optional comma-separated market list.")
    parser.add_argument("--focus-days", default="", help="Optional comma-separated UTC day list.")
    parser.add_argument("--output-json", required=True)
    return parser.parse_args()


def _load_trade_rows(path: Path) -> List[Dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    rows = payload.get("trade_rows", [])
    if not isinstance(rows, list):
        return []

    normalized: List[Dict[str, Any]] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        entry_time = _to_int(row.get("entry_time", 0))
        exit_time = _to_int(row.get("exit_time", entry_time))
        normalized.append(
            {
                "dataset": str(row.get("dataset", "")).strip(),
                "day_utc": str(row.get("day_utc", "")).strip(),
                "market": str(row.get("market", "")).strip(),
                "cell": str(row.get("cell", "")).strip(),
                "archetype": str(row.get("entry_archetype", "")).strip(),
                "entry_time": entry_time,
                "exit_time": exit_time,
                "holding_minutes": _to_float(row.get("holding_minutes", 0.0)),
                "profit_loss_krw": _to_float(row.get("profit_loss_krw", 0.0)),
                "exit_reason": str(row.get("exit_reason", "")).strip(),
            }
        )
    return normalized


def _filter_rows(
    rows: Iterable[Dict[str, Any]],
    focus_markets: List[str],
    focus_days: List[str],
) -> List[Dict[str, Any]]:
    focus_market_set = set(focus_markets)
    focus_day_set = set(focus_days)
    out: List[Dict[str, Any]] = []
    for row in rows:
        if focus_market_set and row["market"] not in focus_market_set:
            continue
        if focus_day_set and row["day_utc"] not in focus_day_set:
            continue
        out.append(row)
    out.sort(key=lambda item: (item["market"], item["entry_time"], item["exit_time"], item["cell"]))
    return out


def _gap_bucket(gap_minutes: float) -> str:
    if gap_minutes < 0.0:
        return "overlap_or_clock_skew(<0m)"
    if gap_minutes <= 60.0:
        return "immediate_reentry(0-60m)"
    if gap_minutes <= 360.0:
        return "short_reentry(60-360m)"
    if gap_minutes <= 1440.0:
        return "same_day_reentry(6-24h)"
    return "late_reentry(>24h)"


def _pct(value: float, total: float) -> float:
    if total <= 0.0:
        return 0.0
    return value / total


def _round_map(counter: Counter[str]) -> Dict[str, int]:
    return {k: int(v) for k, v in sorted(counter.items(), key=lambda item: (-item[1], item[0]))}


def _analyze_side(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    holding_values = [float(item["holding_minutes"]) for item in rows]
    pnl_values = [float(item["profit_loss_krw"]) for item in rows]
    holding_total = float(sum(holding_values))
    pnl_total = float(sum(pnl_values))

    archetype_counts: Counter[str] = Counter()
    archetype_holding: defaultdict[str, float] = defaultdict(float)
    archetype_profit: defaultdict[str, float] = defaultdict(float)
    cell_counts: Counter[str] = Counter()
    exit_reason_counts: Counter[str] = Counter()
    day_trade_counts: Counter[str] = Counter()
    day_profit: defaultdict[str, float] = defaultdict(float)
    day_holding: defaultdict[str, float] = defaultdict(float)
    transition_counts: Counter[Tuple[str, str]] = Counter()
    transition_profit: defaultdict[Tuple[str, str], float] = defaultdict(float)
    gap_buckets: Counter[str] = Counter()
    runtime_to_rescue_count = 0
    runtime_to_rescue_profit = 0.0

    by_market: defaultdict[str, List[Dict[str, Any]]] = defaultdict(list)
    for row in rows:
        by_market[row["market"]].append(row)
        archetype = row["archetype"] or "UNKNOWN"
        cell = row["cell"] or "UNKNOWN|UNKNOWN"
        exit_reason = row["exit_reason"] or "UNKNOWN"
        day = row["day_utc"] or "UNKNOWN"
        archetype_counts[archetype] += 1
        archetype_holding[archetype] += float(row["holding_minutes"])
        archetype_profit[archetype] += float(row["profit_loss_krw"])
        cell_counts[cell] += 1
        exit_reason_counts[exit_reason] += 1
        day_trade_counts[day] += 1
        day_profit[day] += float(row["profit_loss_krw"])
        day_holding[day] += float(row["holding_minutes"])

    for market, market_rows in by_market.items():
        market_rows.sort(key=lambda item: (item["entry_time"], item["exit_time"], item["cell"]))
        for idx in range(1, len(market_rows)):
            prev = market_rows[idx - 1]
            curr = market_rows[idx]
            prev_cell = prev["cell"] or "UNKNOWN|UNKNOWN"
            curr_cell = curr["cell"] or "UNKNOWN|UNKNOWN"
            key = (prev_cell, curr_cell)
            transition_counts[key] += 1
            transition_profit[key] += float(curr["profit_loss_krw"])

            gap_minutes = (float(curr["entry_time"]) - float(prev["exit_time"])) / 60000.0
            gap_buckets[_gap_bucket(gap_minutes)] += 1

            prev_archetype = prev["archetype"]
            curr_archetype = curr["archetype"]
            if prev_archetype == RUNTIME_ARCHETYPE and RESCUE_TOKEN in curr_archetype:
                runtime_to_rescue_count += 1
                runtime_to_rescue_profit += float(curr["profit_loss_krw"])

    runtime_trade_count = int(archetype_counts.get(RUNTIME_ARCHETYPE, 0))
    rescue_trade_count = int(
        sum(count for archetype, count in archetype_counts.items() if RESCUE_TOKEN in archetype)
    )
    runtime_holding = float(archetype_holding.get(RUNTIME_ARCHETYPE, 0.0))
    rescue_holding = float(
        sum(value for archetype, value in archetype_holding.items() if RESCUE_TOKEN in archetype)
    )

    transition_rows: List[Dict[str, Any]] = []
    for (prev_cell, curr_cell), count in transition_counts.items():
        transition_rows.append(
            {
                "from_cell": prev_cell,
                "to_cell": curr_cell,
                "count": int(count),
                "to_trade_profit_sum_krw": round(float(transition_profit[(prev_cell, curr_cell)]), 6),
            }
        )
    transition_rows.sort(key=lambda item: (-item["count"], item["from_cell"], item["to_cell"]))

    day_rows: List[Dict[str, Any]] = []
    for day in sorted(day_trade_counts.keys()):
        day_rows.append(
            {
                "day_utc": day,
                "trade_count": int(day_trade_counts[day]),
                "profit_sum_krw": round(float(day_profit[day]), 6),
                "holding_minutes_sum": round(float(day_holding[day]), 6),
            }
        )

    return {
        "trade_count": int(len(rows)),
        "profit_sum_krw": round(pnl_total, 6),
        "holding_minutes_sum": round(holding_total, 6),
        "holding_minutes_avg": round(_pct(holding_total, float(max(1, len(rows)))), 6),
        "holding_minutes_median": round(float(median(holding_values)) if holding_values else 0.0, 6),
        "holding_minutes_max": round(max(holding_values) if holding_values else 0.0, 6),
        "long_hold_ge_1d_count": int(sum(1 for value in holding_values if value >= 1440.0)),
        "long_hold_ge_3d_count": int(sum(1 for value in holding_values if value >= 4320.0)),
        "runtime_trade_count": runtime_trade_count,
        "rescue_trade_count": rescue_trade_count,
        "runtime_trade_share": round(_pct(float(runtime_trade_count), float(max(1, len(rows)))), 6),
        "rescue_trade_share": round(_pct(float(rescue_trade_count), float(max(1, len(rows)))), 6),
        "runtime_holding_share": round(_pct(runtime_holding, holding_total), 6),
        "rescue_holding_share": round(_pct(rescue_holding, holding_total), 6),
        "runtime_to_rescue_transition_count": int(runtime_to_rescue_count),
        "runtime_to_rescue_transition_profit_sum_krw": round(runtime_to_rescue_profit, 6),
        "archetype_counts": _round_map(archetype_counts),
        "cell_counts": _round_map(cell_counts),
        "exit_reason_counts": _round_map(exit_reason_counts),
        "gap_bucket_counts": _round_map(gap_buckets),
        "transition_top": transition_rows[:30],
        "day_summaries": day_rows,
    }


def _transition_delta(
    baseline: Dict[str, Any],
    candidate: Dict[str, Any],
) -> List[Dict[str, Any]]:
    base_map: Dict[Tuple[str, str], Dict[str, Any]] = {}
    cand_map: Dict[Tuple[str, str], Dict[str, Any]] = {}
    for row in baseline.get("transition_top", []):
        key = (str(row.get("from_cell", "")), str(row.get("to_cell", "")))
        base_map[key] = row
    for row in candidate.get("transition_top", []):
        key = (str(row.get("from_cell", "")), str(row.get("to_cell", "")))
        cand_map[key] = row

    all_keys = set(base_map.keys()) | set(cand_map.keys())
    rows: List[Dict[str, Any]] = []
    for key in all_keys:
        b = base_map.get(key, {})
        c = cand_map.get(key, {})
        base_count = _to_int(b.get("count", 0))
        cand_count = _to_int(c.get("count", 0))
        rows.append(
            {
                "from_cell": key[0],
                "to_cell": key[1],
                "baseline_count": base_count,
                "candidate_count": cand_count,
                "count_delta": cand_count - base_count,
            }
        )
    rows.sort(key=lambda item: (-abs(_to_int(item["count_delta"])), -item["candidate_count"], item["from_cell"]))
    return rows[:30]


def _build_findings(baseline: Dict[str, Any], candidate: Dict[str, Any]) -> List[str]:
    findings: List[str] = []
    profit_delta = _to_float(candidate.get("profit_sum_krw")) - _to_float(baseline.get("profit_sum_krw"))
    rescue_share_delta = _to_float(candidate.get("rescue_trade_share")) - _to_float(
        baseline.get("rescue_trade_share")
    )
    runtime_to_rescue_delta = _to_int(candidate.get("runtime_to_rescue_transition_count")) - _to_int(
        baseline.get("runtime_to_rescue_transition_count")
    )

    if runtime_to_rescue_delta > 0:
        findings.append(
            "runtime->rescue transition frequency increased in candidate; this is the primary occupancy-drift signature."
        )
    if rescue_share_delta > 0.0:
        findings.append(
            "rescue trade share increased in candidate, indicating stronger post-runtime rescue occupancy."
        )
    if profit_delta < 0.0 and rescue_share_delta > 0.0:
        findings.append(
            "profit deteriorated while rescue occupancy expanded; direct guard retries should be deferred until transition trigger context is isolated."
        )
    if not findings:
        findings.append("no strong occupancy-drift signature was detected in the filtered slice.")
    return findings


def main() -> int:
    args = _parse_args()
    baseline_path = Path(args.baseline_profile_json).resolve()
    candidate_path = Path(args.candidate_profile_json).resolve()
    out_path = Path(args.output_json).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    focus_markets = _parse_csv_set(args.focus_markets)
    focus_days = _parse_csv_set(args.focus_days)

    baseline_rows = _filter_rows(_load_trade_rows(baseline_path), focus_markets, focus_days)
    candidate_rows = _filter_rows(_load_trade_rows(candidate_path), focus_markets, focus_days)

    baseline_metrics = _analyze_side(baseline_rows)
    candidate_metrics = _analyze_side(candidate_rows)

    delta = {
        "trade_count_delta": _to_int(candidate_metrics.get("trade_count")) - _to_int(
            baseline_metrics.get("trade_count")
        ),
        "profit_sum_delta_krw": round(
            _to_float(candidate_metrics.get("profit_sum_krw"))
            - _to_float(baseline_metrics.get("profit_sum_krw")),
            6,
        ),
        "holding_minutes_sum_delta": round(
            _to_float(candidate_metrics.get("holding_minutes_sum"))
            - _to_float(baseline_metrics.get("holding_minutes_sum")),
            6,
        ),
        "runtime_to_rescue_transition_count_delta": _to_int(
            candidate_metrics.get("runtime_to_rescue_transition_count")
        )
        - _to_int(baseline_metrics.get("runtime_to_rescue_transition_count")),
        "rescue_trade_share_delta": round(
            _to_float(candidate_metrics.get("rescue_trade_share"))
            - _to_float(baseline_metrics.get("rescue_trade_share")),
            6,
        ),
        "rescue_holding_share_delta": round(
            _to_float(candidate_metrics.get("rescue_holding_share"))
            - _to_float(baseline_metrics.get("rescue_holding_share")),
            6,
        ),
    }

    result = {
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "inputs": {
            "baseline_profile_json": str(baseline_path),
            "candidate_profile_json": str(candidate_path),
            "focus_markets": focus_markets,
            "focus_days": focus_days,
        },
        "baseline": baseline_metrics,
        "candidate": candidate_metrics,
        "delta": delta,
        "transition_deltas_top": _transition_delta(baseline_metrics, candidate_metrics),
        "findings": _build_findings(baseline_metrics, candidate_metrics),
    }

    out_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")
    print(f"[OccupancyDrift] wrote {out_path}")
    print(
        "[OccupancyDrift] "
        f"trade_delta={delta['trade_count_delta']} profit_delta={delta['profit_sum_delta_krw']} "
        f"runtime_to_rescue_delta={delta['runtime_to_rescue_transition_count_delta']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
