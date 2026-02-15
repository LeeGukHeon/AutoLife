#!/usr/bin/env python3
import argparse
import json
from datetime import datetime, timezone
from typing import Any, Dict, List, Set

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
    return parser.parse_args(argv)


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
    }
    if not exists:
        return report

    all_keys: Set[str] = set()
    missing_keys: Set[str] = set()
    unexpected_keys: Set[str] = set()
    invalid_sides: Set[str] = set()
    invalid_statuses: Set[str] = set()

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

    report = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "inputs": {
            "live_execution_updates": str(live_path),
            "backtest_execution_updates": str(backtest_path),
        },
        "expected_schema": {
            "keys": expected_keys,
            "side_values": allowed_sides,
            "status_values": allowed_statuses,
        },
        "checks": checks,
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
