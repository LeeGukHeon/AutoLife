#!/usr/bin/env python3
"""Compare execution event distributions between live-paper and backtest logs.

This script is analysis-only. It summarizes side/event/status/source/strategy/market
counts and reports distribution-distance metrics for reproducible parity reviews.
"""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter
from datetime import datetime, timezone
from typing import Any, Dict, Iterable, List, Set

from _script_common import dump_json, read_nonempty_lines, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--live-execution-updates-path",
        default="build/Release/logs/execution_updates_live.jsonl",
    )
    parser.add_argument(
        "--backtest-execution-updates-path",
        default="build/Release/logs/execution_updates_backtest.jsonl",
    )
    parser.add_argument(
        "--live-source-filter",
        default="live,live_paper",
        help="Comma-separated allowed source values for live events. Empty means no filter.",
    )
    parser.add_argument(
        "--backtest-source-filter",
        default="backtest",
        help="Comma-separated allowed source values for backtest events. Empty means no filter.",
    )
    parser.add_argument(
        "--top-n",
        type=int,
        default=20,
        help="Top N rows to keep for strategy/market delta tables.",
    )
    parser.add_argument(
        "--output-json",
        default="build/Release/logs/execution_event_distribution_comparison_20260224.json",
    )
    return parser.parse_args(argv)


def parse_source_filter(raw: str) -> Set[str]:
    values = [token.strip() for token in str(raw).split(",")]
    return {v for v in values if v}


def safe_str(value: Any) -> str:
    return str(value).strip()


def load_events(path_value, allowed_sources: Set[str]) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "path": str(path_value),
        "exists": path_value.exists(),
        "raw_line_count": 0,
        "parsed_line_count": 0,
        "parse_error_count": 0,
        "filtered_out_count": 0,
        "events": [],
    }
    if not path_value.exists():
        return result

    events: List[Dict[str, Any]] = []
    for line in read_nonempty_lines(path_value):
        result["raw_line_count"] += 1
        try:
            row = json.loads(line)
        except Exception:
            result["parse_error_count"] += 1
            continue
        if not isinstance(row, dict):
            result["parse_error_count"] += 1
            continue
        result["parsed_line_count"] += 1
        source = safe_str(row.get("source", ""))
        if allowed_sources and source not in allowed_sources:
            result["filtered_out_count"] += 1
            continue
        events.append(row)

    result["events"] = events
    return result


def summarize_events(events: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
    side_counts: Counter = Counter()
    event_counts: Counter = Counter()
    status_counts: Counter = Counter()
    source_counts: Counter = Counter()
    strategy_counts: Counter = Counter()
    market_counts: Counter = Counter()
    side_event_counts: Counter = Counter()
    unique_orders: Set[str] = set()
    terminal_count = 0

    for row in events:
        side = safe_str(row.get("side", "")) or "unknown"
        event = safe_str(row.get("event", "")) or "unknown"
        status = safe_str(row.get("status", "")) or "unknown"
        source = safe_str(row.get("source", "")) or "unknown"
        strategy = safe_str(row.get("strategy_name", "")) or "unknown"
        market = safe_str(row.get("market", "")) or "unknown"
        order_id = safe_str(row.get("order_id", "")) or "unknown"

        side_counts[side] += 1
        event_counts[event] += 1
        status_counts[status] += 1
        source_counts[source] += 1
        strategy_counts[strategy] += 1
        market_counts[market] += 1
        side_event_counts[f"{side}|{event}"] += 1
        unique_orders.add(order_id)

        if bool(row.get("terminal", False)):
            terminal_count += 1

    total_events = sum(side_counts.values())
    buy_count = side_counts.get("BUY", 0)
    sell_count = side_counts.get("SELL", 0)
    has_buy_sell = buy_count > 0 and sell_count > 0

    return {
        "total_events": total_events,
        "terminal_event_count": terminal_count,
        "unique_order_count": len(unique_orders),
        "side_counts": dict(side_counts),
        "event_counts": dict(event_counts),
        "status_counts": dict(status_counts),
        "source_counts": dict(source_counts),
        "strategy_counts": dict(strategy_counts),
        "market_counts": dict(market_counts),
        "side_event_counts": dict(side_event_counts),
        "has_buy_sell": has_buy_sell,
    }


def _distribution_from_counter(counter: Dict[str, int], keys: Iterable[str]) -> List[float]:
    vec: List[float] = []
    total = float(sum(max(0, int(counter.get(key, 0))) for key in keys))
    if total <= 0.0:
        return [0.0 for _ in keys]
    for key in keys:
        vec.append(float(max(0, int(counter.get(key, 0)))) / total)
    return vec


def _safe_kl_divergence(p: List[float], q: List[float], eps: float = 1e-12) -> float:
    value = 0.0
    for pi, qi in zip(p, q):
        if pi <= 0.0:
            continue
        value += pi * math.log(pi / max(eps, qi), 2.0)
    return value


def js_divergence(counter_a: Dict[str, int], counter_b: Dict[str, int]) -> float:
    keys = sorted(set(counter_a.keys()) | set(counter_b.keys()))
    if not keys:
        return 0.0
    p = _distribution_from_counter(counter_a, keys)
    q = _distribution_from_counter(counter_b, keys)
    m = [(pi + qi) / 2.0 for pi, qi in zip(p, q)]
    return (_safe_kl_divergence(p, m) + _safe_kl_divergence(q, m)) / 2.0


def total_variation_distance(counter_a: Dict[str, int], counter_b: Dict[str, int]) -> float:
    keys = sorted(set(counter_a.keys()) | set(counter_b.keys()))
    if not keys:
        return 0.0
    p = _distribution_from_counter(counter_a, keys)
    q = _distribution_from_counter(counter_b, keys)
    return 0.5 * sum(abs(pi - qi) for pi, qi in zip(p, q))


def build_delta_table(
    counter_live: Dict[str, int],
    counter_backtest: Dict[str, int],
    top_n: int,
) -> List[Dict[str, Any]]:
    keys = sorted(set(counter_live.keys()) | set(counter_backtest.keys()))
    live_total = float(sum(counter_live.values()))
    backtest_total = float(sum(counter_backtest.values()))
    rows: List[Dict[str, Any]] = []
    for key in keys:
        live_count = int(counter_live.get(key, 0))
        backtest_count = int(counter_backtest.get(key, 0))
        rows.append(
            {
                "key": key,
                "live_count": live_count,
                "backtest_count": backtest_count,
                "delta_count": live_count - backtest_count,
                "live_ratio": (live_count / live_total) if live_total > 0.0 else 0.0,
                "backtest_ratio": (backtest_count / backtest_total) if backtest_total > 0.0 else 0.0,
            }
        )
    rows.sort(key=lambda x: (abs(int(x["delta_count"])), abs(float(x["live_ratio"]) - float(x["backtest_ratio"]))), reverse=True)
    if top_n > 0:
        return rows[:top_n]
    return rows


def main(argv=None) -> int:
    args = parse_args(argv)
    live_path = resolve_repo_path(args.live_execution_updates_path)
    backtest_path = resolve_repo_path(args.backtest_execution_updates_path)
    output_path = resolve_repo_path(args.output_json)
    live_filter = parse_source_filter(args.live_source_filter)
    backtest_filter = parse_source_filter(args.backtest_source_filter)
    top_n = max(1, int(args.top_n))

    live_loaded = load_events(live_path, live_filter)
    backtest_loaded = load_events(backtest_path, backtest_filter)
    live_summary = summarize_events(live_loaded["events"])
    backtest_summary = summarize_events(backtest_loaded["events"])

    side_js = js_divergence(live_summary["side_counts"], backtest_summary["side_counts"])
    event_js = js_divergence(live_summary["event_counts"], backtest_summary["event_counts"])
    status_js = js_divergence(live_summary["status_counts"], backtest_summary["status_counts"])
    side_tvd = total_variation_distance(live_summary["side_counts"], backtest_summary["side_counts"])
    event_tvd = total_variation_distance(live_summary["event_counts"], backtest_summary["event_counts"])
    status_tvd = total_variation_distance(live_summary["status_counts"], backtest_summary["status_counts"])

    both_nonempty = (
        live_summary["total_events"] > 0 and backtest_summary["total_events"] > 0
    )
    both_have_buy_sell = (
        bool(live_summary["has_buy_sell"]) and bool(backtest_summary["has_buy_sell"])
    )
    comparison_ready = both_nonempty and both_have_buy_sell

    report: Dict[str, Any] = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "inputs": {
            "live_execution_updates_path": str(live_path),
            "backtest_execution_updates_path": str(backtest_path),
            "live_source_filter": sorted(live_filter),
            "backtest_source_filter": sorted(backtest_filter),
            "top_n": top_n,
        },
        "live": {
            "load": {k: v for k, v in live_loaded.items() if k != "events"},
            "summary": live_summary,
        },
        "backtest": {
            "load": {k: v for k, v in backtest_loaded.items() if k != "events"},
            "summary": backtest_summary,
        },
        "comparison": {
            "ready_flags": {
                "both_nonempty": both_nonempty,
                "both_have_buy_sell": both_have_buy_sell,
                "comparison_ready": comparison_ready,
            },
            "distance_metrics": {
                "side_js_divergence": side_js,
                "event_js_divergence": event_js,
                "status_js_divergence": status_js,
                "side_total_variation_distance": side_tvd,
                "event_total_variation_distance": event_tvd,
                "status_total_variation_distance": status_tvd,
            },
            "delta_tables": {
                "side": build_delta_table(
                    live_summary["side_counts"],
                    backtest_summary["side_counts"],
                    top_n=top_n,
                ),
                "event": build_delta_table(
                    live_summary["event_counts"],
                    backtest_summary["event_counts"],
                    top_n=top_n,
                ),
                "status": build_delta_table(
                    live_summary["status_counts"],
                    backtest_summary["status_counts"],
                    top_n=top_n,
                ),
                "strategy": build_delta_table(
                    live_summary["strategy_counts"],
                    backtest_summary["strategy_counts"],
                    top_n=top_n,
                ),
                "market": build_delta_table(
                    live_summary["market_counts"],
                    backtest_summary["market_counts"],
                    top_n=top_n,
                ),
                "side_event": build_delta_table(
                    live_summary["side_event_counts"],
                    backtest_summary["side_event_counts"],
                    top_n=top_n,
                ),
            },
            "next_step_hint": (
                "ready_for_exit_event_parity_comparison"
                if comparison_ready
                else "collect_or_prime_missing_side_events_before_exit_comparison"
            ),
        },
    }
    dump_json(output_path, report)
    print(f"[ExecutionEventCompare] wrote {output_path}")
    print(
        "[ExecutionEventCompare] comparison_ready="
        f"{str(report['comparison']['ready_flags']['comparison_ready']).lower()} "
        "next_step_hint="
        f"{report['comparison']['next_step_hint']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
