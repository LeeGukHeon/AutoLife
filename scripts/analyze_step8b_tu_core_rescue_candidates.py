#!/usr/bin/env python3
"""Find low-win-damage guard candidates from step8b TU|CORE_RESCUE entries."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Dict, Iterable, List, Sequence, Set, Tuple


DEFAULT_FEATURES: Tuple[str, ...] = (
    "signal_strength",
    "liquidity_score",
    "volatility",
    "cal",
    "margin",
    "ev",
    "rr",
)


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


@dataclass(frozen=True)
class Clause:
    feature: str
    op: str
    threshold: float

    def key(self) -> str:
        return f"{self.feature} {self.op} {self.threshold:.12g}"

    def matches(self, row: Dict[str, Any]) -> bool:
        value = _to_float(row.get(self.feature, 0.0))
        if self.op == "<=":
            return value <= self.threshold
        if self.op == ">=":
            return value >= self.threshold
        return False


def _parse_days(raw: str) -> Set[str]:
    return {token.strip() for token in str(raw).split(",") if token.strip()}


def _parse_features(raw: str) -> Tuple[str, ...]:
    values = [token.strip() for token in str(raw).split(",") if token.strip()]
    return tuple(values) if values else DEFAULT_FEATURES


def _load_rows(path: Path, focus_days: Set[str]) -> List[Dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, list):
        return []

    rows: List[Dict[str, Any]] = []
    for idx, item in enumerate(payload):
        if not isinstance(item, dict):
            continue
        day = str(item.get("day", "")).strip()
        if focus_days and day not in focus_days:
            continue
        pnl = _to_float(item.get("pnl", 0.0))
        row = dict(item)
        row["_idx"] = idx
        row["_day"] = day
        row["_pnl"] = pnl
        row["_is_loss"] = pnl < 0.0
        row["_is_win"] = pnl > 0.0
        rows.append(row)
    return rows


def _build_single_clauses(rows: Sequence[Dict[str, Any]], features: Sequence[str]) -> List[Clause]:
    clauses: List[Clause] = []
    for feature in features:
        values = sorted({_to_float(row.get(feature, 0.0)) for row in rows})
        for threshold in values:
            clauses.append(Clause(feature=feature, op="<=", threshold=float(threshold)))
            clauses.append(Clause(feature=feature, op=">=", threshold=float(threshold)))
    return clauses


def _blocked_index_set(rows: Sequence[Dict[str, Any]], clauses: Sequence[Clause]) -> Tuple[int, ...]:
    blocked: List[int] = []
    for i, row in enumerate(rows):
        matched = True
        for clause in clauses:
            if not clause.matches(row):
                matched = False
                break
        if matched:
            blocked.append(i)
    return tuple(blocked)


def _candidate_from_blocked(
    rows: Sequence[Dict[str, Any]],
    clauses: Sequence[Clause],
    blocked_indices: Sequence[int],
) -> Dict[str, Any]:
    blocked_rows = [rows[i] for i in blocked_indices]
    blocked_total = len(blocked_rows)
    blocked_loss = [row for row in blocked_rows if bool(row.get("_is_loss", False))]
    blocked_win = [row for row in blocked_rows if bool(row.get("_is_win", False))]

    blocked_loss_count = len(blocked_loss)
    blocked_win_count = len(blocked_win)
    blocked_loss_sum_abs = -sum(_to_float(row.get("_pnl", 0.0)) for row in blocked_loss)
    blocked_win_sum = sum(_to_float(row.get("_pnl", 0.0)) for row in blocked_win)
    blocked_net_improvement = blocked_loss_sum_abs - blocked_win_sum

    return {
        "clauses": [clause.key() for clause in clauses],
        "clause_count": len(clauses),
        "blocked_total_count": blocked_total,
        "blocked_loss_count": blocked_loss_count,
        "blocked_win_count": blocked_win_count,
        "blocked_loss_sum_abs_krw": round(blocked_loss_sum_abs, 6),
        "blocked_win_sum_krw": round(blocked_win_sum, 6),
        "blocked_net_improvement_krw": round(blocked_net_improvement, 6),
        "loss_precision": round((blocked_loss_count / blocked_total) if blocked_total > 0 else 0.0, 6),
        "blocked_days": sorted({str(row.get("_day", "")) for row in blocked_rows}),
        "blocked_indices": list(blocked_indices),
        "blocked_trade_samples": [
            {
                "day": str(row.get("_day", "")),
                "entry_time": _to_int(row.get("entry_time", 0)),
                "pnl": round(_to_float(row.get("_pnl", 0.0)), 6),
                "signal_strength": round(_to_float(row.get("signal_strength", 0.0)), 6),
                "liquidity_score": round(_to_float(row.get("liquidity_score", 0.0)), 6),
                "volatility": round(_to_float(row.get("volatility", 0.0)), 6),
                "cal": round(_to_float(row.get("cal", 0.0)), 6),
                "margin": round(_to_float(row.get("margin", 0.0)), 6),
                "ev": round(_to_float(row.get("ev", 0.0)), 6),
                "rr": round(_to_float(row.get("rr", 0.0)), 6),
                "label": "loss" if bool(row.get("_is_loss", False)) else ("win" if bool(row.get("_is_win", False)) else "flat"),
            }
            for row in blocked_rows[:8]
        ],
    }


def _rank_candidates(candidates: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
    ranked = list(candidates)
    ranked.sort(
        key=lambda row: (
            _to_int(row.get("blocked_win_count", 0)),
            -_to_float(row.get("blocked_net_improvement_krw", 0.0)),
            -_to_int(row.get("blocked_loss_count", 0)),
            _to_int(row.get("clause_count", 99)),
            row.get("clauses", []),
        )
    )
    return ranked


def _summarize_rows(rows: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
    rows_list = list(rows)
    losses = [row for row in rows_list if bool(row.get("_is_loss", False))]
    wins = [row for row in rows_list if bool(row.get("_is_win", False))]
    return {
        "row_count": len(rows_list),
        "loss_count": len(losses),
        "win_count": len(wins),
        "loss_sum_krw": round(sum(_to_float(row.get("_pnl", 0.0)) for row in losses), 6),
        "win_sum_krw": round(sum(_to_float(row.get("_pnl", 0.0)) for row in wins), 6),
        "days": sorted({str(row.get("_day", "")) for row in rows_list}),
    }


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze step8b TU|CORE_RESCUE entries and rank low-win-damage guard candidates."
    )
    parser.add_argument(
        "--input-json",
        default="build/Release/logs/step8b_btc_0216_0218_tu_core_rescue_entries_v1.json",
    )
    parser.add_argument(
        "--focus-days",
        default="2026-02-16,2026-02-17,2026-02-18",
    )
    parser.add_argument(
        "--features",
        default=",".join(DEFAULT_FEATURES),
    )
    parser.add_argument(
        "--single-top-limit",
        type=int,
        default=40,
        help="Max number of single-clause candidates retained before pairwise expansion.",
    )
    parser.add_argument(
        "--top-n",
        type=int,
        default=20,
    )
    parser.add_argument(
        "--output-json",
        default="build/Release/logs/step8b_tu_core_rescue_guard_candidates_20260225.json",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    input_path = Path(args.input_json).resolve()
    output_path = Path(args.output_json).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    focus_days = _parse_days(args.focus_days)
    features = _parse_features(args.features)
    rows = _load_rows(input_path, focus_days)
    if not rows:
        raise RuntimeError(f"no rows loaded from {input_path}")

    base_summary = _summarize_rows(rows)
    clauses = _build_single_clauses(rows, features)

    candidate_map: Dict[Tuple[int, ...], Dict[str, Any]] = {}
    single_candidates: List[Dict[str, Any]] = []

    for clause in clauses:
        blocked = _blocked_index_set(rows, [clause])
        if not blocked:
            continue
        candidate = _candidate_from_blocked(rows, [clause], blocked)
        if _to_int(candidate.get("blocked_loss_count", 0)) <= 0:
            continue
        signature = tuple(candidate.get("blocked_indices", []))
        prev = candidate_map.get(signature)
        if prev is None or _to_int(candidate.get("clause_count", 99)) < _to_int(prev.get("clause_count", 99)):
            candidate_map[signature] = candidate
        single_candidates.append(candidate)

    ranked_single = _rank_candidates(single_candidates)
    seed_for_pairwise = ranked_single[: max(1, int(args.single_top_limit))]

    for i in range(len(seed_for_pairwise)):
        left = seed_for_pairwise[i]
        for j in range(i + 1, len(seed_for_pairwise)):
            right = seed_for_pairwise[j]
            clause_specs = [str(v) for v in (left.get("clauses", []) + right.get("clauses", []))]
            clauses_pair: List[Clause] = []
            seen: Set[str] = set()
            for spec in clause_specs:
                token = spec.strip()
                if token in seen:
                    continue
                seen.add(token)
                parts = token.split(" ")
                if len(parts) != 3:
                    continue
                try:
                    threshold = float(parts[2])
                except Exception:
                    continue
                clauses_pair.append(Clause(feature=parts[0], op=parts[1], threshold=threshold))
            if len(clauses_pair) != 2:
                continue

            blocked = _blocked_index_set(rows, clauses_pair)
            if not blocked:
                continue
            candidate = _candidate_from_blocked(rows, clauses_pair, blocked)
            if _to_int(candidate.get("blocked_loss_count", 0)) <= 0:
                continue
            signature = tuple(candidate.get("blocked_indices", []))
            prev = candidate_map.get(signature)
            if prev is None or _to_int(candidate.get("clause_count", 99)) < _to_int(prev.get("clause_count", 99)):
                candidate_map[signature] = candidate

    all_candidates = list(candidate_map.values())
    ranked_all = _rank_candidates(all_candidates)
    zero_win_damage = [row for row in ranked_all if _to_int(row.get("blocked_win_count", 0)) == 0]
    one_win_damage = [row for row in ranked_all if _to_int(row.get("blocked_win_count", 0)) <= 1]

    result: Dict[str, Any] = {
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "inputs": {
            "input_json": str(input_path),
            "focus_days": sorted(focus_days),
            "features": list(features),
            "single_top_limit": int(args.single_top_limit),
            "top_n": int(args.top_n),
        },
        "dataset_summary": base_summary,
        "candidate_counts": {
            "single_candidates_raw": len(single_candidates),
            "deduped_all_candidates": len(all_candidates),
            "zero_win_damage_candidates": len(zero_win_damage),
            "leq_one_win_damage_candidates": len(one_win_damage),
        },
        "recommended_zero_win_damage_top": zero_win_damage[: max(1, int(args.top_n))],
        "recommended_leq_one_win_damage_top": one_win_damage[: max(1, int(args.top_n))],
        "ranked_all_top": ranked_all[: max(1, int(args.top_n))],
    }

    output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")
    print(f"[Step8bGuardCandidates] wrote {output_path}")
    if zero_win_damage:
        top = zero_win_damage[0]
    else:
        top = ranked_all[0] if ranked_all else {}
    print(
        "[Step8bGuardCandidates] "
        f"rows={base_summary['row_count']} "
        f"best_blocked_loss={top.get('blocked_loss_count', 0)} "
        f"best_blocked_win={top.get('blocked_win_count', 0)} "
        f"best_net_improvement_krw={top.get('blocked_net_improvement_krw', 0.0)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
