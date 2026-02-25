#!/usr/bin/env python3
"""Prefilter guard candidates by hit-signature equivalence against rejected probes."""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Sequence, Set, Tuple


def _to_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def _to_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return default


def _normalize_clause_set(clauses: Sequence[str]) -> Tuple[str, ...]:
    return tuple(sorted(str(clause).strip() for clause in clauses if str(clause).strip()))


def _parse_builtin_rejected(raw: str) -> List[Tuple[str, ...]]:
    tokens = [token.strip() for token in str(raw).split(";") if token.strip()]
    result: List[Tuple[str, ...]] = []
    for token in tokens:
        clauses = [part.strip() for part in token.split("&&") if part.strip()]
        normalized = _normalize_clause_set(clauses)
        if normalized:
            result.append(normalized)
    return result


def _jaccard(a: Set[int], b: Set[int]) -> float:
    if not a and not b:
        return 1.0
    if not a or not b:
        return 0.0
    inter = len(a & b)
    union = len(a | b)
    return (inter / union) if union > 0 else 0.0


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Filter candidates that are clause-duplicate or hit-signature-equivalent to rejected probes."
    )
    parser.add_argument("--candidate-json", required=True)
    parser.add_argument(
        "--candidate-key",
        default="recommended_zero_win_damage_top",
        help="Top-level key containing candidate list.",
    )
    parser.add_argument(
        "--builtin-rejected",
        default="cal <= 0.406871;cal <= 0.406871 && ev >= -0.00027",
        help="Semicolon-separated rejected clause sets. Use '&&' between clauses in a set.",
    )
    parser.add_argument(
        "--rejected-signature-json",
        default="",
        help="Optional JSON file with prior rejected signatures list.",
    )
    parser.add_argument(
        "--jaccard-threshold",
        type=float,
        default=0.80,
        help="Exclude when candidate blocked_indices Jaccard >= threshold against rejected signature.",
    )
    parser.add_argument("--top-n", type=int, default=30)
    parser.add_argument("--output-json", required=True)
    return parser


def _load_rejected_signature_rows(path: Path) -> List[Dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(payload, list):
        rows = [row for row in payload if isinstance(row, dict)]
        return rows
    if isinstance(payload, dict):
        for key in ("rejected_signatures", "rows", "items"):
            value = payload.get(key)
            if isinstance(value, list):
                return [row for row in value if isinstance(row, dict)]
    return []


def main() -> int:
    args = _build_arg_parser().parse_args()
    candidate_path = Path(args.candidate_json).resolve()
    output_path = Path(args.output_json).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    payload = json.loads(candidate_path.read_text(encoding="utf-8"))
    rows = payload.get(args.candidate_key, [])
    if not isinstance(rows, list):
        rows = []

    builtin_rejected = _parse_builtin_rejected(args.builtin_rejected)
    rejected_signature_rows: List[Dict[str, Any]] = []
    if str(args.rejected_signature_json).strip():
        rejected_path = Path(args.rejected_signature_json).resolve()
        if rejected_path.exists():
            rejected_signature_rows = _load_rejected_signature_rows(rejected_path)

    rejected_clause_sets: Set[Tuple[str, ...]] = set(builtin_rejected)
    rejected_signatures: List[Set[int]] = []

    # harvest exact rows from candidate set when clause matches rejected builtins
    for row in rows:
        if not isinstance(row, dict):
            continue
        clause_set = _normalize_clause_set(row.get("clauses", []))
        if clause_set in rejected_clause_sets:
            rejected_signatures.append(set(_to_int(v) for v in row.get("blocked_indices", [])))

    # add explicit rejected signature rows if provided
    for row in rejected_signature_rows:
        clauses = _normalize_clause_set(row.get("clauses", []))
        if clauses:
            rejected_clause_sets.add(clauses)
        rejected_signatures.append(set(_to_int(v) for v in row.get("blocked_indices", [])))

    selected: List[Dict[str, Any]] = []
    excluded: List[Dict[str, Any]] = []
    threshold = float(args.jaccard_threshold)

    for row in rows:
        if not isinstance(row, dict):
            continue
        clause_set = _normalize_clause_set(row.get("clauses", []))
        blocked = set(_to_int(v) for v in row.get("blocked_indices", []))

        exclude_reason = ""
        max_overlap = 0.0
        if clause_set in rejected_clause_sets:
            exclude_reason = "exact_clause_match_rejected"
        else:
            for rej in rejected_signatures:
                overlap = _jaccard(blocked, rej)
                if overlap > max_overlap:
                    max_overlap = overlap
                if overlap >= threshold:
                    exclude_reason = f"signature_overlap_ge_{threshold:.2f}"
                    break

        row_out = dict(row)
        row_out["clauses"] = list(clause_set)
        row_out["signature_overlap_max"] = round(max_overlap, 6)

        if exclude_reason:
            row_out["excluded_reason"] = exclude_reason
            excluded.append(row_out)
        else:
            selected.append(row_out)

    selected.sort(
        key=lambda row: (
            _to_int(row.get("blocked_win_count", 0)),
            -_to_float(row.get("blocked_net_improvement_krw", 0.0)),
            -_to_int(row.get("blocked_loss_count", 0)),
            _to_int(row.get("clause_count", 99)),
            row.get("clauses", []),
        )
    )
    excluded.sort(
        key=lambda row: (
            row.get("excluded_reason", ""),
            -_to_float(row.get("signature_overlap_max", 0.0)),
            -_to_float(row.get("blocked_net_improvement_krw", 0.0)),
        )
    )

    result = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "inputs": {
            "candidate_json": str(candidate_path),
            "candidate_key": str(args.candidate_key),
            "builtin_rejected": [list(item) for item in sorted(rejected_clause_sets)],
            "jaccard_threshold": threshold,
            "top_n": int(args.top_n),
        },
        "summary": {
            "candidate_count": len(rows),
            "selected_count": len(selected),
            "excluded_count": len(excluded),
        },
        "selected_top": selected[: max(1, int(args.top_n))],
        "excluded_top": excluded[: max(1, int(args.top_n))],
    }

    with output_path.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(result, fp, ensure_ascii=False, indent=2)

    print(f"[V21Prefilter] wrote {output_path}")
    print(
        "[V21Prefilter] "
        f"selected={result['summary']['selected_count']} "
        f"excluded={result['summary']['excluded_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
