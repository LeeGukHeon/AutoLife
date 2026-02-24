#!/usr/bin/env python3
"""Analyze selected policy decision drift between baseline and candidate JSONL logs."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple


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


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare selected decision distributions between baseline and candidate policy JSONL logs."
    )
    parser.add_argument("--baseline-policy-jsonl", required=True)
    parser.add_argument("--candidate-policy-jsonl", required=True)
    parser.add_argument("--focus-market", default="")
    parser.add_argument("--output-json", required=True)
    return parser.parse_args()


def _flatten_selected_rows(path: Path, focus_market: str) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except Exception:
            continue
        ts = _to_int(payload.get("ts", 0))
        regime = str(payload.get("dominant_regime", "")).strip()
        decisions = payload.get("decisions", [])
        if not isinstance(decisions, list):
            continue
        for dec in decisions:
            if not isinstance(dec, dict):
                continue
            market = str(dec.get("market", "")).strip()
            if focus_market and market != focus_market:
                continue
            selected = bool(dec.get("selected", False))
            if not selected:
                continue
            rows.append(
                {
                    "ts": ts,
                    "dominant_regime": regime or "UNKNOWN",
                    "strategy": str(dec.get("strategy", "")).strip() or "UNKNOWN",
                    "market": market,
                    "reason": str(dec.get("reason", "")).strip() or "UNKNOWN",
                    "strength": _to_float(dec.get("strength", 0.0)),
                    "liquidity": _to_float(dec.get("liquidity", 0.0)),
                    "volatility": _to_float(dec.get("volatility", 0.0)),
                    "expected_value": _to_float(dec.get("expected_value", 0.0)),
                    "policy_score": _to_float(dec.get("policy_score", 0.0)),
                    "base_score": _to_float(dec.get("base_score", 0.0)),
                }
            )
    rows.sort(key=lambda item: (item["ts"], item["strategy"], item["dominant_regime"]))
    return rows


def _quantile(values: List[float], q: float) -> float:
    if not values:
        return 0.0
    xs = sorted(float(v) for v in values)
    idx = int(round((len(xs) - 1) * q))
    idx = max(0, min(len(xs) - 1, idx))
    return float(xs[idx])


def _series_summary(values: Iterable[float]) -> Dict[str, float]:
    xs = [float(v) for v in values]
    if not xs:
        return {
            "count": 0,
            "min": 0.0,
            "p25": 0.0,
            "p50": 0.0,
            "p75": 0.0,
            "max": 0.0,
            "mean": 0.0,
        }
    total = float(sum(xs))
    n = len(xs)
    return {
        "count": int(n),
        "min": round(_quantile(xs, 0.0), 6),
        "p25": round(_quantile(xs, 0.25), 6),
        "p50": round(_quantile(xs, 0.50), 6),
        "p75": round(_quantile(xs, 0.75), 6),
        "max": round(_quantile(xs, 1.0), 6),
        "mean": round(total / float(max(1, n)), 6),
    }


def _group_counts(rows: List[Dict[str, Any]]) -> Dict[str, int]:
    counts: Counter[str] = Counter()
    for row in rows:
        key = f"{row['dominant_regime']}|{row['strategy']}"
        counts[key] += 1
    return {k: int(v) for k, v in sorted(counts.items(), key=lambda item: (-item[1], item[0]))}


def _feature_summary(rows: List[Dict[str, Any]]) -> Dict[str, Dict[str, float]]:
    return {
        "strength": _series_summary([row["strength"] for row in rows]),
        "liquidity": _series_summary([row["liquidity"] for row in rows]),
        "volatility": _series_summary([row["volatility"] for row in rows]),
        "expected_value": _series_summary([row["expected_value"] for row in rows]),
        "policy_score": _series_summary([row["policy_score"] for row in rows]),
        "base_score": _series_summary([row["base_score"] for row in rows]),
    }


def _fingerprint(row: Dict[str, Any]) -> Tuple[Any, ...]:
    return (
        row["dominant_regime"],
        row["strategy"],
        round(float(row["strength"]), 6),
        round(float(row["expected_value"]), 6),
        round(float(row["liquidity"]), 3),
        round(float(row["volatility"]), 6),
        round(float(row["policy_score"]), 6),
    )


def _multiset_diff(
    baseline: List[Dict[str, Any]],
    candidate: List[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    baseline_counts: Counter[Tuple[Any, ...]] = Counter(_fingerprint(row) for row in baseline)
    candidate_counts: Counter[Tuple[Any, ...]] = Counter(_fingerprint(row) for row in candidate)

    rows: List[Dict[str, Any]] = []
    for fp, cand_count in candidate_counts.items():
        delta = int(cand_count) - int(baseline_counts.get(fp, 0))
        if delta <= 0:
            continue
        rows.append(
            {
                "dominant_regime": fp[0],
                "strategy": fp[1],
                "strength": fp[2],
                "expected_value": fp[3],
                "liquidity": fp[4],
                "volatility": fp[5],
                "policy_score": fp[6],
                "count_delta": delta,
            }
        )
    rows.sort(key=lambda item: (-item["count_delta"], item["dominant_regime"], item["strategy"]))
    return rows


def main() -> int:
    args = _parse_args()
    baseline_path = Path(args.baseline_policy_jsonl).resolve()
    candidate_path = Path(args.candidate_policy_jsonl).resolve()
    out_path = Path(args.output_json).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    focus_market = str(args.focus_market).strip()
    baseline_rows = _flatten_selected_rows(baseline_path, focus_market)
    candidate_rows = _flatten_selected_rows(candidate_path, focus_market)

    baseline_counts = _group_counts(baseline_rows)
    candidate_counts = _group_counts(candidate_rows)

    all_group_keys = set(baseline_counts.keys()) | set(candidate_counts.keys())
    group_deltas: List[Dict[str, Any]] = []
    for key in all_group_keys:
        group_deltas.append(
            {
                "group": key,
                "baseline_count": int(baseline_counts.get(key, 0)),
                "candidate_count": int(candidate_counts.get(key, 0)),
                "count_delta": int(candidate_counts.get(key, 0)) - int(baseline_counts.get(key, 0)),
            }
        )
    group_deltas.sort(key=lambda item: (-abs(item["count_delta"]), item["group"]))

    candidate_only_rows = _multiset_diff(baseline_rows, candidate_rows)

    result = {
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "inputs": {
            "baseline_policy_jsonl": str(baseline_path),
            "candidate_policy_jsonl": str(candidate_path),
            "focus_market": focus_market,
        },
        "baseline": {
            "selected_count": int(len(baseline_rows)),
            "group_counts": baseline_counts,
            "feature_summary": _feature_summary(baseline_rows),
        },
        "candidate": {
            "selected_count": int(len(candidate_rows)),
            "group_counts": candidate_counts,
            "feature_summary": _feature_summary(candidate_rows),
        },
        "delta": {
            "selected_count_delta": int(len(candidate_rows) - len(baseline_rows)),
            "group_deltas_top": group_deltas[:30],
            "candidate_only_patterns_top": candidate_only_rows[:40],
        },
    }

    out_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")
    print(f"[PolicySelectionDrift] wrote {out_path}")
    print(
        "[PolicySelectionDrift] "
        f"selected_delta={result['delta']['selected_count_delta']} "
        f"baseline={len(baseline_rows)} candidate={len(candidate_rows)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
