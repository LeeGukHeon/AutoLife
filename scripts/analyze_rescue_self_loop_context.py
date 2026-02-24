#!/usr/bin/env python3
"""Analyze rescue self-loop transition context (baseline vs candidate)."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Dict, Iterable, List


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
        description="Compare rescue self-loop transition context between baseline and candidate trade profiles."
    )
    parser.add_argument("--baseline-profile-json", required=True)
    parser.add_argument("--candidate-profile-json", required=True)
    parser.add_argument("--focus-market", default="KRW-ETH")
    parser.add_argument("--focus-days", default="")
    parser.add_argument(
        "--target-cell",
        default="TRENDING_UP|CORE_RESCUE_SHOULD_ENTER",
        help="Cell used for self-loop detection.",
    )
    parser.add_argument("--output-json", required=True)
    return parser.parse_args()


def _load_rows(path: Path) -> List[Dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    rows = payload.get("trade_rows", [])
    if not isinstance(rows, list):
        return []
    out: List[Dict[str, Any]] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        out.append(
            {
                "dataset": str(row.get("dataset", "")).strip(),
                "day_utc": str(row.get("day_utc", "")).strip(),
                "market": str(row.get("market", "")).strip(),
                "cell": str(row.get("cell", "")).strip(),
                "entry_time": _to_int(row.get("entry_time", 0)),
                "exit_time": _to_int(row.get("exit_time", row.get("entry_time", 0))),
                "profit_loss_krw": _to_float(row.get("profit_loss_krw", 0.0)),
                "signal_strength": _to_float(row.get("signal_strength", 0.0)),
                "liquidity_score": _to_float(row.get("liquidity_score", 0.0)),
                "volatility": _to_float(row.get("volatility", 0.0)),
                "probabilistic_h5_calibrated": _to_float(row.get("probabilistic_h5_calibrated", 0.0)),
                "probabilistic_h5_margin": _to_float(row.get("probabilistic_h5_margin", 0.0)),
                "expected_value": _to_float(row.get("expected_value", 0.0)),
                "reward_risk_ratio": _to_float(row.get("reward_risk_ratio", 0.0)),
                "exit_reason": str(row.get("exit_reason", "")).strip(),
            }
        )
    return out


def _filter_rows(rows: Iterable[Dict[str, Any]], market: str, focus_days: List[str]) -> List[Dict[str, Any]]:
    day_set = set(focus_days)
    out: List[Dict[str, Any]] = []
    for row in rows:
        if market and row.get("market") != market:
            continue
        if day_set and row.get("day_utc") not in day_set:
            continue
        out.append(row)
    out.sort(key=lambda item: (item["entry_time"], item["exit_time"], item["cell"]))
    return out


def _quantile(sorted_values: List[float], q: float) -> float:
    if not sorted_values:
        return 0.0
    if q <= 0.0:
        return float(sorted_values[0])
    if q >= 1.0:
        return float(sorted_values[-1])
    idx = int(round((len(sorted_values) - 1) * q))
    return float(sorted_values[max(0, min(len(sorted_values) - 1, idx))])


def _summarize_series(values: List[float]) -> Dict[str, float]:
    sorted_values = sorted(float(v) for v in values)
    if not sorted_values:
        return {
            "count": 0,
            "min": 0.0,
            "p25": 0.0,
            "p50": 0.0,
            "p75": 0.0,
            "max": 0.0,
            "mean": 0.0,
        }
    total = float(sum(sorted_values))
    count = len(sorted_values)
    return {
        "count": int(count),
        "min": round(_quantile(sorted_values, 0.0), 6),
        "p25": round(_quantile(sorted_values, 0.25), 6),
        "p50": round(_quantile(sorted_values, 0.50), 6),
        "p75": round(_quantile(sorted_values, 0.75), 6),
        "max": round(_quantile(sorted_values, 1.0), 6),
        "mean": round(total / float(max(1, count)), 6),
    }


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


def _analyze_self_loop(rows: List[Dict[str, Any]], target_cell: str) -> Dict[str, Any]:
    loop_rows: List[Dict[str, Any]] = []
    gap_minutes_values: List[float] = []
    gap_buckets: Counter[str] = Counter()
    day_counts: Counter[str] = Counter()
    next_exit_reason_counts: Counter[str] = Counter()

    signal_strength_values: List[float] = []
    liquidity_values: List[float] = []
    volatility_values: List[float] = []
    cal_values: List[float] = []
    margin_values: List[float] = []
    ev_values: List[float] = []
    rr_values: List[float] = []
    pnl_values: List[float] = []

    for idx in range(1, len(rows)):
        prev = rows[idx - 1]
        curr = rows[idx]
        if prev.get("cell") != target_cell:
            continue
        if curr.get("cell") != target_cell:
            continue

        gap_minutes = (float(curr["entry_time"]) - float(prev["exit_time"])) / 60000.0
        gap_minutes_values.append(gap_minutes)
        gap_buckets[_gap_bucket(gap_minutes)] += 1
        day_counts[str(curr.get("day_utc", "UNKNOWN"))] += 1
        next_exit_reason_counts[str(curr.get("exit_reason", "UNKNOWN"))] += 1

        signal_strength_values.append(float(curr["signal_strength"]))
        liquidity_values.append(float(curr["liquidity_score"]))
        volatility_values.append(float(curr["volatility"]))
        cal_values.append(float(curr["probabilistic_h5_calibrated"]))
        margin_values.append(float(curr["probabilistic_h5_margin"]))
        ev_values.append(float(curr["expected_value"]))
        rr_values.append(float(curr["reward_risk_ratio"]))
        pnl_values.append(float(curr["profit_loss_krw"]))

        loop_rows.append(
            {
                "day_utc": curr["day_utc"],
                "prev_exit_time": prev["exit_time"],
                "curr_entry_time": curr["entry_time"],
                "reentry_gap_minutes": round(gap_minutes, 6),
                "curr_profit_loss_krw": round(float(curr["profit_loss_krw"]), 6),
                "curr_exit_reason": curr["exit_reason"],
                "signal_strength": round(float(curr["signal_strength"]), 6),
                "liquidity_score": round(float(curr["liquidity_score"]), 6),
                "volatility": round(float(curr["volatility"]), 6),
                "probabilistic_h5_calibrated": round(float(curr["probabilistic_h5_calibrated"]), 6),
                "probabilistic_h5_margin": round(float(curr["probabilistic_h5_margin"]), 6),
                "expected_value": round(float(curr["expected_value"]), 6),
                "reward_risk_ratio": round(float(curr["reward_risk_ratio"]), 6),
            }
        )

    loop_rows.sort(key=lambda item: (item["day_utc"], item["curr_entry_time"]))
    loop_count = len(loop_rows)
    negative_count = sum(1 for value in pnl_values if value <= 0.0)

    return {
        "self_loop_count": int(loop_count),
        "self_loop_profit_sum_krw": round(float(sum(pnl_values)), 6),
        "self_loop_negative_count": int(negative_count),
        "self_loop_negative_ratio": round(
            float(negative_count) / float(max(1, loop_count)),
            6,
        ),
        "reentry_gap_minutes_summary": _summarize_series(gap_minutes_values),
        "gap_bucket_counts": {
            key: int(value)
            for key, value in sorted(gap_buckets.items(), key=lambda item: (-item[1], item[0]))
        },
        "day_counts": {
            key: int(value) for key, value in sorted(day_counts.items(), key=lambda item: item[0])
        },
        "next_exit_reason_counts": {
            key: int(value)
            for key, value in sorted(next_exit_reason_counts.items(), key=lambda item: (-item[1], item[0]))
        },
        "feature_summary": {
            "signal_strength": _summarize_series(signal_strength_values),
            "liquidity_score": _summarize_series(liquidity_values),
            "volatility": _summarize_series(volatility_values),
            "probabilistic_h5_calibrated": _summarize_series(cal_values),
            "probabilistic_h5_margin": _summarize_series(margin_values),
            "expected_value": _summarize_series(ev_values),
            "reward_risk_ratio": _summarize_series(rr_values),
            "profit_loss_krw": _summarize_series(pnl_values),
        },
        "self_loop_samples": loop_rows[:40],
    }


def main() -> int:
    args = _parse_args()
    baseline_path = Path(args.baseline_profile_json).resolve()
    candidate_path = Path(args.candidate_profile_json).resolve()
    out_path = Path(args.output_json).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    focus_days = _parse_csv_set(args.focus_days)
    target_cell = str(args.target_cell).strip()

    baseline_rows = _filter_rows(_load_rows(baseline_path), str(args.focus_market).strip(), focus_days)
    candidate_rows = _filter_rows(_load_rows(candidate_path), str(args.focus_market).strip(), focus_days)

    baseline = _analyze_self_loop(baseline_rows, target_cell)
    candidate = _analyze_self_loop(candidate_rows, target_cell)

    delta = {
        "self_loop_count_delta": int(candidate["self_loop_count"]) - int(baseline["self_loop_count"]),
        "self_loop_profit_sum_delta_krw": round(
            _to_float(candidate["self_loop_profit_sum_krw"]) - _to_float(baseline["self_loop_profit_sum_krw"]),
            6,
        ),
        "self_loop_negative_ratio_delta": round(
            _to_float(candidate["self_loop_negative_ratio"]) - _to_float(baseline["self_loop_negative_ratio"]),
            6,
        ),
    }

    result = {
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "inputs": {
            "baseline_profile_json": str(baseline_path),
            "candidate_profile_json": str(candidate_path),
            "focus_market": str(args.focus_market).strip(),
            "focus_days": focus_days,
            "target_cell": target_cell,
        },
        "baseline": baseline,
        "candidate": candidate,
        "delta": delta,
    }

    out_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")
    print(f"[RescueSelfLoop] wrote {out_path}")
    print(
        "[RescueSelfLoop] "
        f"self_loop_delta={delta['self_loop_count_delta']} "
        f"profit_delta={delta['self_loop_profit_sum_delta_krw']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
