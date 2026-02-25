#!/usr/bin/env python3
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
        "-LiveExecutionUpdatesPath",
        default="build/Release/logs/execution_updates_live.jsonl",
    )
    parser.add_argument(
        "--backtest-execution-updates-path",
        "-BacktestExecutionUpdatesPath",
        default="build/Release/logs/execution_updates_backtest.jsonl",
    )
    parser.add_argument("--output-json", "-OutputJson", default="build/Release/logs/execution_parity_report.json")
    parser.add_argument("--strict", "-Strict", action="store_true")
    parser.add_argument(
        "--strict-normalized-parity",
        action="store_true",
        help="Fail when normalized side/event/status distribution drifts beyond thresholds.",
    )
    parser.add_argument("--normalized-side-tvd-warn-threshold", type=float, default=0.35)
    parser.add_argument("--normalized-event-tvd-warn-threshold", type=float, default=0.45)
    parser.add_argument("--normalized-status-tvd-warn-threshold", type=float, default=0.45)
    parser.add_argument("--top-normalized-deltas", type=int, default=20)
    return parser.parse_args(argv)


def safe_token(value: Any, fallback: str = "UNKNOWN", upper: bool = True) -> str:
    token = str(value).strip()
    if not token:
        token = fallback
    return token.upper() if upper else token


def counter_to_dict(counter: Counter) -> Dict[str, int]:
    return {str(k): int(v) for k, v in sorted(counter.items(), key=lambda kv: str(kv[0]))}


def distribution_from_counter(counter: Dict[str, int], keys: Iterable[str]) -> List[float]:
    key_list = list(keys)
    total = float(sum(max(0, int(counter.get(key, 0))) for key in key_list))
    if total <= 0.0:
        return [0.0 for _ in key_list]
    return [float(max(0, int(counter.get(key, 0)))) / total for key in key_list]


def total_variation_distance(counter_a: Dict[str, int], counter_b: Dict[str, int]) -> float:
    keys = sorted(set(counter_a.keys()) | set(counter_b.keys()))
    if not keys:
        return 0.0
    p = distribution_from_counter(counter_a, keys)
    q = distribution_from_counter(counter_b, keys)
    return 0.5 * sum(abs(pi - qi) for pi, qi in zip(p, q))


def js_divergence(counter_a: Dict[str, int], counter_b: Dict[str, int]) -> float:
    keys = sorted(set(counter_a.keys()) | set(counter_b.keys()))
    if not keys:
        return 0.0
    p = distribution_from_counter(counter_a, keys)
    q = distribution_from_counter(counter_b, keys)
    m = [(pi + qi) / 2.0 for pi, qi in zip(p, q)]
    value = 0.0
    for left, right in [(p, m), (q, m)]:
        kl = 0.0
        for l, r in zip(left, right):
            if l <= 0.0:
                continue
            kl += l * math.log(l / max(1e-12, r), 2.0)
        value += kl
    return value / 2.0


def build_delta_table(counter_live: Dict[str, int], counter_backtest: Dict[str, int], top_n: int) -> List[Dict[str, Any]]:
    keys = sorted(set(counter_live.keys()) | set(counter_backtest.keys()))
    live_total = float(sum(max(0, int(v)) for v in counter_live.values()))
    backtest_total = float(sum(max(0, int(v)) for v in counter_backtest.values()))
    rows: List[Dict[str, Any]] = []
    for key in keys:
        live_count = max(0, int(counter_live.get(key, 0)))
        backtest_count = max(0, int(counter_backtest.get(key, 0)))
        rows.append(
            {
                "key": str(key),
                "live_count": live_count,
                "backtest_count": backtest_count,
                "delta_count": live_count - backtest_count,
                "live_ratio": (live_count / live_total) if live_total > 0.0 else 0.0,
                "backtest_ratio": (backtest_count / backtest_total) if backtest_total > 0.0 else 0.0,
            }
        )
    rows.sort(
        key=lambda x: (
            abs(int(x["delta_count"])),
            abs(float(x["live_ratio"]) - float(x["backtest_ratio"])),
        ),
        reverse=True,
    )
    return rows[: max(1, int(top_n))]


def read_execution_updates(
    path_value,
    expected_keys: List[str],
    allowed_sides: List[str],
    allowed_statuses: List[str],
) -> Dict[str, Any]:
    exists = path_value.exists()
    report: Dict[str, Any] = {
        "path": str(path_value),
        "exists": exists,
        "total_lines": 0,
        "parsed_lines": 0,
        "parse_errors": 0,
        "missing_keys": [],
        "unexpected_keys": [],
        "invalid_side_values": [],
        "invalid_status_values": [],
        "key_signature": "",
        "schema_valid": False,
        "sample": None,
        "normalized_summary": {
            "total_events": 0,
            "terminal_event_count": 0,
            "side_counts": {},
            "event_counts": {},
            "status_counts": {},
            "strategy_counts": {},
            "market_counts": {},
            "side_event_status_counts": {},
        },
    }
    if not exists:
        return report

    all_keys: Set[str] = set()
    missing_keys: Set[str] = set()
    unexpected_keys: Set[str] = set()
    invalid_sides: Set[str] = set()
    invalid_statuses: Set[str] = set()
    side_counts: Counter = Counter()
    event_counts: Counter = Counter()
    status_counts: Counter = Counter()
    strategy_counts: Counter = Counter()
    market_counts: Counter = Counter()
    side_event_status_counts: Counter = Counter()
    terminal_event_count = 0

    for line in read_nonempty_lines(path_value):
        report["total_lines"] += 1
        try:
            row = json.loads(line)
        except Exception:
            report["parse_errors"] += 1
            continue
        if not isinstance(row, dict):
            report["parse_errors"] += 1
            continue

        report["parsed_lines"] += 1
        if report["sample"] is None:
            report["sample"] = row

        props = list(row.keys())
        for prop in props:
            all_keys.add(str(prop))

        for expected in expected_keys:
            if expected not in row:
                missing_keys.add(expected)
        for prop in props:
            if prop not in expected_keys:
                unexpected_keys.add(str(prop))

        if "side" in row:
            side_value = str(row.get("side", ""))
            if side_value not in allowed_sides:
                invalid_sides.add(side_value)
        if "status" in row:
            status_value = str(row.get("status", ""))
            if status_value not in allowed_statuses:
                invalid_statuses.add(status_value)

        side_token = safe_token(row.get("side", ""), fallback="UNKNOWN", upper=True)
        event_token = safe_token(row.get("event", ""), fallback="UNKNOWN", upper=True)
        status_token = safe_token(row.get("status", ""), fallback="UNKNOWN", upper=True)
        strategy_token = safe_token(row.get("strategy_name", ""), fallback="unknown", upper=False)
        market_token = safe_token(row.get("market", ""), fallback="UNKNOWN", upper=True)
        side_event_status_token = f"{side_token}|{event_token}|{status_token}"
        side_counts[side_token] += 1
        event_counts[event_token] += 1
        status_counts[status_token] += 1
        strategy_counts[strategy_token] += 1
        market_counts[market_token] += 1
        side_event_status_counts[side_event_status_token] += 1
        if bool(row.get("terminal", False)):
            terminal_event_count += 1

    sorted_keys = sorted(all_keys)
    report["key_signature"] = ",".join(sorted_keys)
    report["missing_keys"] = sorted(missing_keys)
    report["unexpected_keys"] = sorted(unexpected_keys)
    report["invalid_side_values"] = sorted(invalid_sides)
    report["invalid_status_values"] = sorted(invalid_statuses)
    report["schema_valid"] = (
        report["parse_errors"] == 0
        and report["parsed_lines"] > 0
        and len(report["missing_keys"]) == 0
        and len(report["unexpected_keys"]) == 0
        and len(report["invalid_side_values"]) == 0
        and len(report["invalid_status_values"]) == 0
    )
    report["normalized_summary"] = {
        "total_events": int(sum(side_counts.values())),
        "terminal_event_count": int(terminal_event_count),
        "side_counts": counter_to_dict(side_counts),
        "event_counts": counter_to_dict(event_counts),
        "status_counts": counter_to_dict(status_counts),
        "strategy_counts": counter_to_dict(strategy_counts),
        "market_counts": counter_to_dict(market_counts),
        "side_event_status_counts": counter_to_dict(side_event_status_counts),
    }
    return report


def main(argv=None) -> int:
    args = parse_args(argv)
    live_path = resolve_repo_path(args.live_execution_updates_path)
    backtest_path = resolve_repo_path(args.backtest_execution_updates_path)
    output_path = resolve_repo_path(args.output_json)

    expected_keys = [
        "ts_ms",
        "source",
        "event",
        "order_id",
        "market",
        "side",
        "status",
        "filled_volume",
        "order_volume",
        "avg_price",
        "strategy_name",
        "terminal",
    ]
    allowed_sides = ["BUY", "SELL"]
    allowed_statuses = ["PENDING", "SUBMITTED", "FILLED", "PARTIALLY_FILLED", "CANCELLED", "REJECTED"]

    live = read_execution_updates(live_path, expected_keys, allowed_sides, allowed_statuses)
    backtest = read_execution_updates(backtest_path, expected_keys, allowed_sides, allowed_statuses)

    checks = {
        "live_file_available": bool(live["exists"]),
        "backtest_file_available": bool(backtest["exists"]),
        "live_rows_available": int(live["parsed_lines"]) > 0,
        "backtest_rows_available": int(backtest["parsed_lines"]) > 0,
        "live_schema_valid": bool(live["schema_valid"]),
        "backtest_schema_valid": bool(backtest["schema_valid"]),
        "both_have_rows": int(live["parsed_lines"]) > 0 and int(backtest["parsed_lines"]) > 0,
        "schema_compatible": False,
    }
    if checks["both_have_rows"]:
        checks["schema_compatible"] = live["key_signature"] == backtest["key_signature"]

    live_norm = live.get("normalized_summary", {}) if isinstance(live, dict) else {}
    backtest_norm = backtest.get("normalized_summary", {}) if isinstance(backtest, dict) else {}
    live_side_counts = live_norm.get("side_counts", {}) if isinstance(live_norm, dict) else {}
    backtest_side_counts = backtest_norm.get("side_counts", {}) if isinstance(backtest_norm, dict) else {}
    live_event_counts = live_norm.get("event_counts", {}) if isinstance(live_norm, dict) else {}
    backtest_event_counts = backtest_norm.get("event_counts", {}) if isinstance(backtest_norm, dict) else {}
    live_status_counts = live_norm.get("status_counts", {}) if isinstance(live_norm, dict) else {}
    backtest_status_counts = backtest_norm.get("status_counts", {}) if isinstance(backtest_norm, dict) else {}
    live_side_event_status_counts = (
        live_norm.get("side_event_status_counts", {}) if isinstance(live_norm, dict) else {}
    )
    backtest_side_event_status_counts = (
        backtest_norm.get("side_event_status_counts", {}) if isinstance(backtest_norm, dict) else {}
    )

    side_tvd = total_variation_distance(live_side_counts, backtest_side_counts)
    event_tvd = total_variation_distance(live_event_counts, backtest_event_counts)
    status_tvd = total_variation_distance(live_status_counts, backtest_status_counts)
    side_js = js_divergence(live_side_counts, backtest_side_counts)
    event_js = js_divergence(live_event_counts, backtest_event_counts)
    status_js = js_divergence(live_status_counts, backtest_status_counts)

    normalized_comparison = {
        "ready": bool(checks["both_have_rows"]),
        "overlap": {
            "side": sorted(set(live_side_counts.keys()) & set(backtest_side_counts.keys())),
            "event": sorted(set(live_event_counts.keys()) & set(backtest_event_counts.keys())),
            "status": sorted(set(live_status_counts.keys()) & set(backtest_status_counts.keys())),
        },
        "distance_metrics": {
            "side_total_variation_distance": side_tvd,
            "event_total_variation_distance": event_tvd,
            "status_total_variation_distance": status_tvd,
            "side_js_divergence": side_js,
            "event_js_divergence": event_js,
            "status_js_divergence": status_js,
        },
        "top_side_event_status_deltas": build_delta_table(
            live_side_event_status_counts,
            backtest_side_event_status_counts,
            top_n=max(1, int(args.top_normalized_deltas)),
        ),
    }

    warnings = []
    errors = []

    if not checks["live_file_available"]:
        warnings.append("live_execution_updates_missing")
    if not checks["backtest_file_available"]:
        warnings.append("backtest_execution_updates_missing")
    if checks["live_file_available"] and not checks["live_rows_available"]:
        warnings.append("live_execution_updates_empty")
    if checks["backtest_file_available"] and not checks["backtest_rows_available"]:
        warnings.append("backtest_execution_updates_empty")

    if int(live["parse_errors"]) > 0:
        errors.append("live_execution_updates_parse_error")
    if int(backtest["parse_errors"]) > 0:
        errors.append("backtest_execution_updates_parse_error")
    if checks["live_rows_available"] and not checks["live_schema_valid"]:
        errors.append("live_execution_updates_schema_invalid")
    if checks["backtest_rows_available"] and not checks["backtest_schema_valid"]:
        errors.append("backtest_execution_updates_schema_invalid")
    if checks["both_have_rows"] and not checks["schema_compatible"]:
        errors.append("execution_update_schema_mismatch")

    side_tvd_warn_threshold = max(0.0, float(args.normalized_side_tvd_warn_threshold))
    event_tvd_warn_threshold = max(0.0, float(args.normalized_event_tvd_warn_threshold))
    status_tvd_warn_threshold = max(0.0, float(args.normalized_status_tvd_warn_threshold))

    if checks["both_have_rows"] and side_tvd > side_tvd_warn_threshold:
        warnings.append("normalized_side_distribution_drift_high")
    if checks["both_have_rows"] and event_tvd > event_tvd_warn_threshold:
        warnings.append("normalized_event_distribution_drift_high")
    if checks["both_have_rows"] and status_tvd > status_tvd_warn_threshold:
        warnings.append("normalized_status_distribution_drift_high")

    if args.strict:
        if not checks["live_file_available"]:
            errors.append("strict_failed_live_execution_updates_missing")
        if not checks["backtest_file_available"]:
            errors.append("strict_failed_backtest_execution_updates_missing")
        if not checks["live_rows_available"]:
            errors.append("strict_failed_live_execution_updates_empty")
        if not checks["backtest_rows_available"]:
            errors.append("strict_failed_backtest_execution_updates_empty")
        if not checks["schema_compatible"]:
            errors.append("strict_failed_execution_schema_mismatch")

    if args.strict_normalized_parity and checks["both_have_rows"]:
        if side_tvd > side_tvd_warn_threshold:
            errors.append("strict_failed_normalized_side_distribution_drift")
        if event_tvd > event_tvd_warn_threshold:
            errors.append("strict_failed_normalized_event_distribution_drift")
        if status_tvd > status_tvd_warn_threshold:
            errors.append("strict_failed_normalized_status_distribution_drift")

    parity_boundary = {
        "must_match_core_surface": [
            "decision event/status/side schema",
            "BUY/SELL lifecycle surface coverage",
            "normalized side/event/status distribution stability",
        ],
        "allowed_runtime_differences": [
            "ts_ms wall-clock timestamps",
            "source label (live/live_paper vs backtest)",
            "order_id format",
            "exchange retry/latency side effects",
        ],
        "notes": "Parity review should prioritize decision surface equivalence and treat runtime transport differences as expected variance.",
    }

    report = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "inputs": {
            "live_execution_updates": str(live_path),
            "backtest_execution_updates": str(backtest_path),
        },
        "parity_boundary": parity_boundary,
        "expected_schema": {
            "keys": expected_keys,
            "side_values": allowed_sides,
            "status_values": allowed_statuses,
        },
        "checks": checks,
        "normalized_comparison": normalized_comparison,
        "live": live,
        "backtest": backtest,
        "warnings": warnings,
        "errors": errors,
    }
    dump_json(output_path, report)

    if errors:
        print(f"[ExecutionParity] FAILED - see {output_path}")
        return 1
    if warnings:
        print(f"[ExecutionParity] PASSED with warnings - see {output_path}")
    else:
        print(f"[ExecutionParity] PASSED - see {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
