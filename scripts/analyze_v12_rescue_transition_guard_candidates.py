#!/usr/bin/env python3
"""Derive minimal guard/cooldown candidates for rescue self-loop transitions."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Dict, Iterable, List, Sequence, Set, Tuple


DEFAULT_FEATURES: Tuple[str, ...] = (
    "reentry_gap_minutes",
    "signal_strength",
    "expected_value",
    "probabilistic_h5_calibrated",
    "probabilistic_h5_margin",
    "reward_risk_ratio",
    "liquidity_score",
    "volatility",
    "prev_exit_was_stop_loss",
)

TARGET_CELL_DEFAULT = "TRENDING_UP|CORE_RESCUE_SHOULD_ENTER"


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


def _parse_csv_set(raw: str) -> Set[str]:
    return {token.strip() for token in str(raw).split(",") if token.strip()}


def _parse_features(raw: str) -> Tuple[str, ...]:
    values = [token.strip() for token in str(raw).split(",") if token.strip()]
    return tuple(values) if values else DEFAULT_FEATURES


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


def _load_trade_rows(
    profile_json: Path,
    market: str,
    focus_days: Set[str],
    target_cell: str,
    require_prev_same_cell: bool,
) -> List[Dict[str, Any]]:
    payload = json.loads(profile_json.read_text(encoding="utf-8"))
    rows_raw = payload.get("trade_rows", [])
    if not isinstance(rows_raw, list):
        rows_raw = []

    by_market: Dict[str, List[Dict[str, Any]]] = {}
    for row in rows_raw:
        if not isinstance(row, dict):
            continue
        mkt = str(row.get("market", "")).strip()
        if market and mkt != market:
            continue
        by_market.setdefault(mkt, []).append(row)

    rows: List[Dict[str, Any]] = []
    for _, market_rows in by_market.items():
        market_rows.sort(
            key=lambda item: (
                _to_int(item.get("entry_time", 0)),
                _to_int(item.get("exit_time", 0)),
                str(item.get("cell", "")),
            )
        )
        for idx in range(1, len(market_rows)):
            prev = market_rows[idx - 1]
            curr = market_rows[idx]
            curr_cell = str(curr.get("cell", "")).strip()
            if curr_cell != target_cell:
                continue
            curr_day = str(curr.get("day_utc", "")).strip()
            if focus_days and curr_day not in focus_days:
                continue
            prev_cell = str(prev.get("cell", "")).strip()
            if require_prev_same_cell and prev_cell != target_cell:
                continue

            prev_exit_reason = str(prev.get("exit_reason", "")).strip()
            gap_minutes = (
                (_to_float(curr.get("entry_time", 0.0)) - _to_float(prev.get("exit_time", 0.0))) / 60000.0
            )
            pnl = _to_float(curr.get("profit_loss_krw", 0.0))
            row = {
                "day_utc": curr_day,
                "market": str(curr.get("market", "")).strip(),
                "curr_cell": curr_cell,
                "prev_cell": prev_cell,
                "prev_exit_reason": prev_exit_reason,
                "prev_exit_was_stop_loss": 1.0 if prev_exit_reason == "StopLoss" else 0.0,
                "reentry_gap_minutes": gap_minutes,
                "signal_strength": _to_float(curr.get("signal_strength", 0.0)),
                "liquidity_score": _to_float(curr.get("liquidity_score", 0.0)),
                "volatility": _to_float(curr.get("volatility", 0.0)),
                "probabilistic_h5_calibrated": _to_float(curr.get("probabilistic_h5_calibrated", 0.0)),
                "probabilistic_h5_margin": _to_float(curr.get("probabilistic_h5_margin", 0.0)),
                "expected_value": _to_float(curr.get("expected_value", 0.0)),
                "reward_risk_ratio": _to_float(curr.get("reward_risk_ratio", 0.0)),
                "curr_exit_reason": str(curr.get("exit_reason", "")).strip(),
                "profit_loss_krw": pnl,
                "_is_loss": pnl < 0.0,
                "_is_win": pnl > 0.0,
                "_idx": len(rows),
            }
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
    for idx, row in enumerate(rows):
        if all(clause.matches(row) for clause in clauses):
            blocked.append(idx)
    return tuple(blocked)


def _candidate_from_blocked(
    rows: Sequence[Dict[str, Any]],
    clauses: Sequence[Clause],
    blocked_indices: Sequence[int],
) -> Dict[str, Any]:
    blocked_rows = [rows[i] for i in blocked_indices]
    blocked_loss = [row for row in blocked_rows if bool(row.get("_is_loss", False))]
    blocked_win = [row for row in blocked_rows if bool(row.get("_is_win", False))]

    blocked_loss_sum_abs = -sum(_to_float(row.get("profit_loss_krw", 0.0)) for row in blocked_loss)
    blocked_win_sum = sum(_to_float(row.get("profit_loss_krw", 0.0)) for row in blocked_win)
    blocked_total = len(blocked_rows)
    blocked_loss_count = len(blocked_loss)
    blocked_win_count = len(blocked_win)

    return {
        "clauses": [clause.key() for clause in clauses],
        "clause_count": len(clauses),
        "blocked_total_count": blocked_total,
        "blocked_loss_count": blocked_loss_count,
        "blocked_win_count": blocked_win_count,
        "blocked_loss_sum_abs_krw": round(blocked_loss_sum_abs, 6),
        "blocked_win_sum_krw": round(blocked_win_sum, 6),
        "blocked_net_improvement_krw": round(blocked_loss_sum_abs - blocked_win_sum, 6),
        "loss_precision": round((blocked_loss_count / blocked_total) if blocked_total > 0 else 0.0, 6),
        "blocked_days": sorted({str(row.get("day_utc", "")) for row in blocked_rows}),
        "blocked_prev_exit_reason_counts": {
            key: int(value)
            for key, value in sorted(
                _count_by(blocked_rows, "prev_exit_reason").items(),
                key=lambda item: (-item[1], item[0]),
            )
        },
        "blocked_curr_exit_reason_counts": {
            key: int(value)
            for key, value in sorted(
                _count_by(blocked_rows, "curr_exit_reason").items(),
                key=lambda item: (-item[1], item[0]),
            )
        },
        "blocked_trade_samples": [
            {
                "day_utc": str(row.get("day_utc", "")),
                "prev_exit_reason": str(row.get("prev_exit_reason", "")),
                "reentry_gap_minutes": round(_to_float(row.get("reentry_gap_minutes", 0.0)), 6),
                "pnl": round(_to_float(row.get("profit_loss_krw", 0.0)), 6),
                "signal_strength": round(_to_float(row.get("signal_strength", 0.0)), 6),
                "expected_value": round(_to_float(row.get("expected_value", 0.0)), 6),
                "probabilistic_h5_calibrated": round(_to_float(row.get("probabilistic_h5_calibrated", 0.0)), 6),
                "probabilistic_h5_margin": round(_to_float(row.get("probabilistic_h5_margin", 0.0)), 6),
                "reward_risk_ratio": round(_to_float(row.get("reward_risk_ratio", 0.0)), 6),
                "label": "loss"
                if bool(row.get("_is_loss", False))
                else ("win" if bool(row.get("_is_win", False)) else "flat"),
            }
            for row in blocked_rows[:10]
        ],
    }


def _count_by(rows: Sequence[Dict[str, Any]], key: str) -> Dict[str, int]:
    counts: Dict[str, int] = {}
    for row in rows:
        token = str(row.get(key, "")).strip() or "UNKNOWN"
        counts[token] = counts.get(token, 0) + 1
    return counts


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


def _summarize_rows(rows: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    loss_rows = [row for row in rows if bool(row.get("_is_loss", False))]
    win_rows = [row for row in rows if bool(row.get("_is_win", False))]
    return {
        "row_count": len(rows),
        "loss_count": len(loss_rows),
        "win_count": len(win_rows),
        "loss_sum_krw": round(sum(_to_float(row.get("profit_loss_krw", 0.0)) for row in loss_rows), 6),
        "win_sum_krw": round(sum(_to_float(row.get("profit_loss_krw", 0.0)) for row in win_rows), 6),
        "net_sum_krw": round(sum(_to_float(row.get("profit_loss_krw", 0.0)) for row in rows), 6),
        "days": sorted({str(row.get("day_utc", "")) for row in rows}),
        "prev_exit_reason_counts": {
            key: int(value)
            for key, value in sorted(_count_by(rows, "prev_exit_reason").items(), key=lambda item: (-item[1], item[0]))
        },
        "curr_exit_reason_counts": {
            key: int(value)
            for key, value in sorted(_count_by(rows, "curr_exit_reason").items(), key=lambda item: (-item[1], item[0]))
        },
    }


def _preset_v11_boundary_candidate(rows: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    clauses = [
        Clause(feature="prev_exit_was_stop_loss", op=">=", threshold=1.0),
        Clause(feature="reentry_gap_minutes", op="<=", threshold=360.0),
        Clause(feature="signal_strength", op="<=", threshold=0.475509),
        Clause(feature="expected_value", op="<=", threshold=-0.000179),
    ]
    blocked = _blocked_index_set(rows, clauses)
    return _candidate_from_blocked(rows, clauses, blocked)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze v12 minimal guard/cooldown candidates from rescue self-loop transitions."
    )
    parser.add_argument(
        "--profile-json",
        default="build/Release/logs/daily_oos_trade_profile_correctness_runtime_mapping_on_5set_0216_0219_fullslice_cfgaligned_v9.json",
    )
    parser.add_argument("--market", default="KRW-ETH")
    parser.add_argument("--focus-days", default="2026-02-16,2026-02-17,2026-02-18,2026-02-19")
    parser.add_argument("--target-cell", default=TARGET_CELL_DEFAULT)
    parser.add_argument(
        "--features",
        default=",".join(DEFAULT_FEATURES),
    )
    parser.add_argument(
        "--require-prev-same-cell",
        action="store_true",
        help="When set, only self-loop transitions (prev_cell==target_cell) are analyzed.",
    )
    parser.add_argument(
        "--single-top-limit",
        type=int,
        default=40,
    )
    parser.add_argument(
        "--top-n",
        type=int,
        default=20,
    )
    parser.add_argument(
        "--output-json",
        default="build/Release/logs/v12_rescue_transition_guard_candidates_eth_0216_0219.json",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    profile_path = Path(args.profile_json).resolve()
    output_path = Path(args.output_json).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    focus_days = _parse_csv_set(args.focus_days)
    features = _parse_features(args.features)
    rows = _load_trade_rows(
        profile_json=profile_path,
        market=str(args.market).strip(),
        focus_days=focus_days,
        target_cell=str(args.target_cell).strip(),
        require_prev_same_cell=bool(args.require_prev_same_cell),
    )
    if not rows:
        raise RuntimeError("no transition rows found for provided filters")

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
        signature = tuple(blocked)
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
            clause_specs = [str(token) for token in (left.get("clauses", []) + right.get("clauses", []))]
            clause_objs: List[Clause] = []
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
                clause_objs.append(Clause(feature=parts[0], op=parts[1], threshold=threshold))
            if len(clause_objs) != 2:
                continue
            blocked = _blocked_index_set(rows, clause_objs)
            if not blocked:
                continue
            candidate = _candidate_from_blocked(rows, clause_objs, blocked)
            if _to_int(candidate.get("blocked_loss_count", 0)) <= 0:
                continue
            signature = tuple(blocked)
            prev = candidate_map.get(signature)
            if prev is None or _to_int(candidate.get("clause_count", 99)) < _to_int(prev.get("clause_count", 99)):
                candidate_map[signature] = candidate

    preset_candidate = _preset_v11_boundary_candidate(rows)

    ranked_all = _rank_candidates(list(candidate_map.values()))
    zero_win = [row for row in ranked_all if _to_int(row.get("blocked_win_count", 0)) == 0]
    leq_one_win = [row for row in ranked_all if _to_int(row.get("blocked_win_count", 0)) <= 1]

    result = {
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "inputs": {
            "profile_json": str(profile_path),
            "market": str(args.market).strip(),
            "focus_days": sorted(focus_days),
            "target_cell": str(args.target_cell).strip(),
            "features": list(features),
            "require_prev_same_cell": bool(args.require_prev_same_cell),
            "single_top_limit": int(args.single_top_limit),
            "top_n": int(args.top_n),
        },
        "dataset_summary": base_summary,
        "candidate_counts": {
            "single_candidates_raw": len(single_candidates),
            "deduped_all_candidates": len(ranked_all),
            "zero_win_damage_candidates": len(zero_win),
            "leq_one_win_damage_candidates": len(leq_one_win),
        },
        "preset_v11_boundary_candidate": preset_candidate,
        "recommended_zero_win_damage_top": zero_win[: max(1, int(args.top_n))],
        "recommended_leq_one_win_damage_top": leq_one_win[: max(1, int(args.top_n))],
        "ranked_all_top": ranked_all[: max(1, int(args.top_n))],
    }

    output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")
    print(f"[V12GuardCandidates] wrote {output_path}")
    print(
        "[V12GuardCandidates] "
        f"rows={base_summary['row_count']} loss={base_summary['loss_count']} win={base_summary['win_count']} "
        f"candidates={len(ranked_all)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
