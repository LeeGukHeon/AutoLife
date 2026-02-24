#!/usr/bin/env python3
"""Compare trade transition chains between two backtest JSON/text reports."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


def _try_read_text(path: Path, encoding: str) -> Optional[str]:
    try:
        return path.read_text(encoding=encoding)
    except Exception:
        return None


def _load_text(path: Path) -> str:
    for enc in ("utf-8", "utf-16", "utf-16-le", "cp949", "latin-1"):
        text = _try_read_text(path, enc)
        if text is not None:
            return text
    raise RuntimeError(f"failed to read report text: {path}")


def _extract_json_payload(raw: str) -> Dict[str, Any]:
    marker = '{"avg_fee_krw"'
    start = raw.find(marker)
    if start < 0:
        start = raw.find("{")
    if start < 0:
        raise RuntimeError("json payload start not found")
    tail = raw[start:].strip()
    end = tail.rfind("}")
    if end < 0:
        raise RuntimeError("json payload end not found")
    payload = tail[: end + 1]
    return json.loads(payload)


def _load_backtest_payload(path: Path) -> Dict[str, Any]:
    if path.suffix.lower() == ".json":
        with path.open("r", encoding="utf-8") as fp:
            return json.load(fp)
    raw = _load_text(path)
    return _extract_json_payload(raw)


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


def _ts_day_utc(ts_ms: int) -> str:
    return datetime.fromtimestamp(max(0, ts_ms) / 1000.0, tz=UTC).strftime("%Y-%m-%d")


@dataclass
class TradeRow:
    idx: int
    entry_archetype: str
    regime: str
    exit_reason: str
    profit_loss_krw: float
    entry_time: int
    exit_time: int
    holding_minutes: float

    @property
    def entry_day_utc(self) -> str:
        return _ts_day_utc(self.entry_time)

    @property
    def exit_day_utc(self) -> str:
        return _ts_day_utc(self.exit_time)


def _normalize_trade_rows(payload: Dict[str, Any]) -> List[TradeRow]:
    rows: List[TradeRow] = []
    raw_rows = payload.get("trade_history_samples", [])
    if not isinstance(raw_rows, list):
        return rows
    for idx, raw in enumerate(raw_rows):
        if not isinstance(raw, dict):
            continue
        rows.append(
            TradeRow(
                idx=idx,
                entry_archetype=str(raw.get("entry_archetype", "")).strip() or "UNSPECIFIED",
                regime=str(raw.get("regime", "")).strip() or "UNKNOWN",
                exit_reason=str(raw.get("exit_reason", "")).strip() or "UNKNOWN",
                profit_loss_krw=_to_float(raw.get("profit_loss_krw", 0.0)),
                entry_time=_to_int(raw.get("entry_time", 0)),
                exit_time=_to_int(raw.get("exit_time", 0)),
                holding_minutes=_to_float(raw.get("holding_minutes", 0.0)),
            )
        )
    rows.sort(key=lambda x: (x.entry_time, x.exit_time, x.idx))
    return rows


def _build_transition_rows(rows: List[TradeRow]) -> List[Dict[str, Any]]:
    transitions: List[Dict[str, Any]] = []
    for prev_row, next_row in zip(rows, rows[1:]):
        gap_minutes = _to_float(next_row.entry_time - prev_row.exit_time) / 60000.0
        transitions.append(
            {
                "from_archetype": prev_row.entry_archetype,
                "to_archetype": next_row.entry_archetype,
                "from_regime": prev_row.regime,
                "to_regime": next_row.regime,
                "from_exit_reason": prev_row.exit_reason,
                "from_profit_loss_krw": prev_row.profit_loss_krw,
                "to_profit_loss_krw": next_row.profit_loss_krw,
                "from_exit_day_utc": prev_row.exit_day_utc,
                "to_entry_day_utc": next_row.entry_day_utc,
                "gap_minutes": gap_minutes,
            }
        )
    return transitions


def _transition_summary(transitions: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    bucket: Dict[Tuple[str, str], Dict[str, Any]] = {}
    for row in transitions:
        key = (row["from_archetype"], row["to_archetype"])
        slot = bucket.setdefault(
            key,
            {
                "from_archetype": row["from_archetype"],
                "to_archetype": row["to_archetype"],
                "count": 0,
                "avg_gap_minutes": 0.0,
                "min_gap_minutes": None,
                "max_gap_minutes": None,
            },
        )
        slot["count"] += 1
        gap = _to_float(row.get("gap_minutes", 0.0))
        slot["avg_gap_minutes"] += gap
        slot["min_gap_minutes"] = gap if slot["min_gap_minutes"] is None else min(slot["min_gap_minutes"], gap)
        slot["max_gap_minutes"] = gap if slot["max_gap_minutes"] is None else max(slot["max_gap_minutes"], gap)

    out: List[Dict[str, Any]] = []
    for _, slot in bucket.items():
        count = max(1, _to_int(slot["count"], 1))
        out.append(
            {
                "from_archetype": slot["from_archetype"],
                "to_archetype": slot["to_archetype"],
                "count": _to_int(slot["count"], 0),
                "avg_gap_minutes": round(_to_float(slot["avg_gap_minutes"]) / count, 6),
                "min_gap_minutes": round(_to_float(slot["min_gap_minutes"]), 6),
                "max_gap_minutes": round(_to_float(slot["max_gap_minutes"]), 6),
            }
        )
    out.sort(key=lambda x: (-_to_int(x["count"]), x["from_archetype"], x["to_archetype"]))
    return out


def _daily_archetype_rows(rows: List[TradeRow]) -> List[Dict[str, Any]]:
    bucket: Dict[Tuple[str, str], Dict[str, Any]] = {}
    for row in rows:
        key = (row.entry_day_utc, row.entry_archetype)
        slot = bucket.setdefault(
            key,
            {
                "day_utc": row.entry_day_utc,
                "entry_archetype": row.entry_archetype,
                "trade_count": 0,
                "profit_sum_krw": 0.0,
                "loss_trade_count": 0,
            },
        )
        slot["trade_count"] += 1
        slot["profit_sum_krw"] += row.profit_loss_krw
        if row.profit_loss_krw < 0.0:
            slot["loss_trade_count"] += 1
    out = list(bucket.values())
    for row in out:
        row["profit_sum_krw"] = round(_to_float(row["profit_sum_krw"]), 6)
    out.sort(key=lambda x: (x["day_utc"], x["entry_archetype"]))
    return out


def _summarize_anchor_post_exit_chain(
    rows: List[TradeRow],
    anchor_archetype: str,
    chain_window_minutes: float,
    anchor_exit_reasons: Optional[set] = None,
    focus_days: Optional[set] = None,
) -> Dict[str, Any]:
    anchor_exit_reasons = anchor_exit_reasons or set()
    focus_days = focus_days or set()
    window_ms = int(max(0.0, float(chain_window_minutes)) * 60.0 * 1000.0)

    anchors: List[TradeRow] = []
    for row in rows:
        if row.entry_archetype != anchor_archetype:
            continue
        if anchor_exit_reasons and row.exit_reason not in anchor_exit_reasons:
            continue
        if focus_days and row.exit_day_utc not in focus_days:
            continue
        anchors.append(row)

    chain_details: List[Dict[str, Any]] = []
    chain_archetype_bucket: Dict[str, Dict[str, float]] = defaultdict(lambda: {"trade_count": 0.0, "profit_sum_krw": 0.0})
    chain_regime_bucket: Dict[str, Dict[str, float]] = defaultdict(lambda: {"trade_count": 0.0, "profit_sum_krw": 0.0})
    chain_regime_archetype_bucket: Dict[str, Dict[str, float]] = defaultdict(
        lambda: {"trade_count": 0.0, "profit_sum_krw": 0.0}
    )
    chain_total_trades = 0
    chain_total_profit_sum = 0.0

    for anchor in anchors:
        chain_start = anchor.exit_time
        chain_end = chain_start + window_ms if window_ms > 0 else chain_start
        chain_trades: List[TradeRow] = []
        for row in rows:
            if row.entry_time < chain_start:
                continue
            if window_ms > 0 and row.entry_time >= chain_end:
                continue
            if row.idx == anchor.idx:
                continue
            chain_trades.append(row)

        chain_trades.sort(key=lambda x: (x.entry_time, x.exit_time, x.idx))
        chain_profit_sum = sum(_to_float(item.profit_loss_krw) for item in chain_trades)
        chain_total_profit_sum += chain_profit_sum
        chain_total_trades += len(chain_trades)

        for item in chain_trades:
            arch_slot = chain_archetype_bucket[item.entry_archetype]
            arch_slot["trade_count"] += 1.0
            arch_slot["profit_sum_krw"] += _to_float(item.profit_loss_krw)

            reg_slot = chain_regime_bucket[item.regime]
            reg_slot["trade_count"] += 1.0
            reg_slot["profit_sum_krw"] += _to_float(item.profit_loss_krw)

            reg_arch_key = f"{item.regime}|{item.entry_archetype}"
            reg_arch_slot = chain_regime_archetype_bucket[reg_arch_key]
            reg_arch_slot["trade_count"] += 1.0
            reg_arch_slot["profit_sum_krw"] += _to_float(item.profit_loss_krw)

        chain_details.append(
            {
                "anchor_idx": anchor.idx,
                "anchor_entry_day_utc": anchor.entry_day_utc,
                "anchor_exit_day_utc": anchor.exit_day_utc,
                "anchor_regime": anchor.regime,
                "anchor_exit_reason": anchor.exit_reason,
                "anchor_profit_loss_krw": round(_to_float(anchor.profit_loss_krw), 6),
                "chain_trade_count": len(chain_trades),
                "chain_profit_sum_krw": round(chain_profit_sum, 6),
                "chain_top_archetypes": [
                    {"entry_archetype": name, "count": count}
                    for name, count in Counter([item.entry_archetype for item in chain_trades]).most_common(5)
                ],
                "chain_top_regimes": [
                    {"regime": name, "count": count}
                    for name, count in Counter([item.regime for item in chain_trades]).most_common(5)
                ],
            }
        )

    def _bucket_to_rows(bucket: Dict[str, Dict[str, float]], key_name: str) -> List[Dict[str, Any]]:
        rows_out: List[Dict[str, Any]] = []
        for name, slot in bucket.items():
            rows_out.append(
                {
                    key_name: name,
                    "trade_count": int(_to_float(slot.get("trade_count", 0.0))),
                    "profit_sum_krw": round(_to_float(slot.get("profit_sum_krw", 0.0)), 6),
                }
            )
        rows_out.sort(key=lambda x: (_to_float(x["profit_sum_krw"]), -_to_int(x["trade_count"]), x[key_name]))
        return rows_out

    chain_details.sort(key=lambda row: (_to_float(row["chain_profit_sum_krw"]), -_to_int(row["chain_trade_count"])))
    summary = {
        "anchor_archetype": anchor_archetype,
        "chain_window_minutes": round(float(chain_window_minutes), 6),
        "anchor_count": len(anchors),
        "chain_total_trades": chain_total_trades,
        "chain_total_profit_sum_krw": round(chain_total_profit_sum, 6),
        "avg_chain_trades_per_anchor": round((chain_total_trades / len(anchors)) if anchors else 0.0, 6),
        "avg_chain_profit_per_anchor_krw": round((chain_total_profit_sum / len(anchors)) if anchors else 0.0, 6),
        "chain_by_archetype": _bucket_to_rows(chain_archetype_bucket, "entry_archetype"),
        "chain_by_regime": _bucket_to_rows(chain_regime_bucket, "regime"),
        "chain_by_regime_archetype": _bucket_to_rows(chain_regime_archetype_bucket, "regime_archetype"),
        "anchor_chain_details": chain_details[:25],
    }
    return summary


def _daily_archetype_map(rows: Iterable[Dict[str, Any]]) -> Dict[Tuple[str, str], Dict[str, Any]]:
    out: Dict[Tuple[str, str], Dict[str, Any]] = {}
    for row in rows:
        key = (str(row.get("day_utc", "")), str(row.get("entry_archetype", "")))
        out[key] = row
    return out


def _daily_delta_rows(
    baseline_rows: List[Dict[str, Any]],
    candidate_rows: List[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    base_map = _daily_archetype_map(baseline_rows)
    cand_map = _daily_archetype_map(candidate_rows)
    keys = sorted(set(base_map.keys()) | set(cand_map.keys()))
    out: List[Dict[str, Any]] = []
    for key in keys:
        base = base_map.get(key, {})
        cand = cand_map.get(key, {})
        out.append(
            {
                "day_utc": key[0],
                "entry_archetype": key[1],
                "baseline_trade_count": _to_int(base.get("trade_count", 0)),
                "candidate_trade_count": _to_int(cand.get("trade_count", 0)),
                "trade_delta": _to_int(cand.get("trade_count", 0)) - _to_int(base.get("trade_count", 0)),
                "baseline_profit_sum_krw": round(_to_float(base.get("profit_sum_krw", 0.0)), 6),
                "candidate_profit_sum_krw": round(_to_float(cand.get("profit_sum_krw", 0.0)), 6),
                "profit_delta_krw": round(
                    _to_float(cand.get("profit_sum_krw", 0.0)) - _to_float(base.get("profit_sum_krw", 0.0)),
                    6,
                ),
            }
        )
    out.sort(key=lambda x: (x["day_utc"], x["entry_archetype"]))
    return out


def _build_report_summary(payload: Dict[str, Any], trades: List[TradeRow]) -> Dict[str, Any]:
    total_trades = _to_int(payload.get("total_trades", 0))
    sample_count = len(trades)
    coverage = (float(sample_count) / float(total_trades)) if total_trades > 0 else 0.0
    archetype_counter = Counter([row.entry_archetype for row in trades])
    reason_counter = Counter([row.exit_reason for row in trades])
    return {
        "total_trades": total_trades,
        "sample_count": sample_count,
        "sample_coverage_ratio": round(coverage, 6),
        "profit_factor": round(_to_float(payload.get("profit_factor", 0.0)), 6),
        "expectancy_krw": round(_to_float(payload.get("expectancy_krw", 0.0)), 6),
        "top_archetypes": [
            {"entry_archetype": name, "count": count}
            for name, count in archetype_counter.most_common(8)
        ],
        "top_exit_reasons": [
            {"exit_reason": name, "count": count}
            for name, count in reason_counter.most_common(8)
        ],
    }


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare trade transition chains from backtest reports.")
    parser.add_argument("--baseline-report", required=True)
    parser.add_argument("--candidate-report", required=True)
    parser.add_argument("--focus-days", default="", help="Optional comma-separated UTC days (YYYY-MM-DD).")
    parser.add_argument("--anchor-archetype", default="PROBABILISTIC_PRIMARY_RUNTIME")
    parser.add_argument(
        "--anchor-exit-reasons",
        default="",
        help="Optional comma-separated anchor exit reasons.",
    )
    parser.add_argument(
        "--chain-window-minutes",
        type=float,
        default=1440.0,
        help="Post-exit chain window minutes (default: 1440 = 24h).",
    )
    parser.add_argument("--output-json", required=True)
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    baseline_path = Path(args.baseline_report).resolve()
    candidate_path = Path(args.candidate_report).resolve()
    output_path = Path(args.output_json).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    focus_days = {day.strip() for day in str(args.focus_days).split(",") if day.strip()}
    anchor_exit_reasons = {
        token.strip()
        for token in str(args.anchor_exit_reasons).split(",")
        if token.strip()
    }

    baseline_payload = _load_backtest_payload(baseline_path)
    candidate_payload = _load_backtest_payload(candidate_path)
    baseline_trades = _normalize_trade_rows(baseline_payload)
    candidate_trades = _normalize_trade_rows(candidate_payload)

    baseline_transitions = _build_transition_rows(baseline_trades)
    candidate_transitions = _build_transition_rows(candidate_trades)
    baseline_daily = _daily_archetype_rows(baseline_trades)
    candidate_daily = _daily_archetype_rows(candidate_trades)
    daily_delta = _daily_delta_rows(baseline_daily, candidate_daily)
    if focus_days:
        focused_daily_delta = [row for row in daily_delta if row["day_utc"] in focus_days]
    else:
        focused_daily_delta = list(daily_delta)

    baseline_post_exit_chain = _summarize_anchor_post_exit_chain(
        baseline_trades,
        anchor_archetype=str(args.anchor_archetype).strip() or "PROBABILISTIC_PRIMARY_RUNTIME",
        chain_window_minutes=float(args.chain_window_minutes),
        anchor_exit_reasons=anchor_exit_reasons,
        focus_days=focus_days,
    )
    candidate_post_exit_chain = _summarize_anchor_post_exit_chain(
        candidate_trades,
        anchor_archetype=str(args.anchor_archetype).strip() or "PROBABILISTIC_PRIMARY_RUNTIME",
        chain_window_minutes=float(args.chain_window_minutes),
        anchor_exit_reasons=anchor_exit_reasons,
        focus_days=focus_days,
    )

    runtime_to_rescue_rows = [
        row
        for row in candidate_transitions
        if row["from_archetype"] == "PROBABILISTIC_PRIMARY_RUNTIME"
        and row["to_archetype"] == "CORE_RESCUE_SHOULD_ENTER"
    ]
    runtime_to_rescue_rows.sort(key=lambda x: (_to_float(x["gap_minutes"]), x["to_entry_day_utc"]))

    summary = {
        "baseline": _build_report_summary(baseline_payload, baseline_trades),
        "candidate": _build_report_summary(candidate_payload, candidate_trades),
    }
    summary["delta"] = {
        "total_trades_delta": summary["candidate"]["total_trades"] - summary["baseline"]["total_trades"],
        "profit_factor_delta": round(
            _to_float(summary["candidate"]["profit_factor"]) - _to_float(summary["baseline"]["profit_factor"]), 6
        ),
        "expectancy_krw_delta": round(
            _to_float(summary["candidate"]["expectancy_krw"]) - _to_float(summary["baseline"]["expectancy_krw"]), 6
        ),
        "runtime_to_rescue_transition_count_candidate": len(runtime_to_rescue_rows),
        "post_exit_chain_total_trades_delta": _to_int(candidate_post_exit_chain.get("chain_total_trades", 0))
        - _to_int(baseline_post_exit_chain.get("chain_total_trades", 0)),
        "post_exit_chain_total_profit_delta_krw": round(
            _to_float(candidate_post_exit_chain.get("chain_total_profit_sum_krw", 0.0))
            - _to_float(baseline_post_exit_chain.get("chain_total_profit_sum_krw", 0.0)),
            6,
        ),
    }

    result: Dict[str, Any] = {
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "inputs": {
            "baseline_report": str(baseline_path),
            "candidate_report": str(candidate_path),
            "focus_days": sorted(focus_days),
            "anchor_archetype": str(args.anchor_archetype).strip() or "PROBABILISTIC_PRIMARY_RUNTIME",
            "anchor_exit_reasons": sorted(anchor_exit_reasons),
            "chain_window_minutes": float(args.chain_window_minutes),
        },
        "summary": summary,
        "baseline_transition_summary_top": _transition_summary(baseline_transitions)[:15],
        "candidate_transition_summary_top": _transition_summary(candidate_transitions)[:15],
        "candidate_runtime_to_rescue_transitions": runtime_to_rescue_rows[:20],
        "baseline_post_exit_chain": baseline_post_exit_chain,
        "candidate_post_exit_chain": candidate_post_exit_chain,
        "daily_archetype_delta": focused_daily_delta,
        "daily_archetype_delta_top_negative": sorted(
            focused_daily_delta,
            key=lambda row: (_to_float(row["profit_delta_krw"]), -_to_int(row["trade_delta"]), row["day_utc"]),
        )[:20],
    }
    with output_path.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(result, fp, ensure_ascii=False, indent=2)

    print(f"[TransitionChain] wrote {output_path}")
    print(
        "[TransitionChain] "
        f"total_trades_delta={result['summary']['delta']['total_trades_delta']} "
        f"runtime_to_rescue={result['summary']['delta']['runtime_to_rescue_transition_count_candidate']} "
        f"post_exit_chain_trades_delta={result['summary']['delta']['post_exit_chain_total_trades_delta']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
