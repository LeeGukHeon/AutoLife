#!/usr/bin/env python3
import argparse
import json
from datetime import datetime, timezone
from typing import Any, Dict

from _script_common import (
    dump_json,
    parse_last_json_line,
    read_nonempty_lines,
    resolve_repo_path,
    run_command,
    tail_strings,
)


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--test-exe-path",
        "-TestExePath",
        default=r".\build\Release\AutoLifeV2ShadowParityTest.exe",
    )
    parser.add_argument(
        "--output-json",
        "-OutputJson",
        default=r".\build\Release\logs\v2_shadow_parity_report.json",
    )
    parser.add_argument(
        "--max-selection-symmetric-diff",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--max-taxonomy-mismatch-count",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--max-execution-schema-mismatch-count",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--check-runtime-shadow",
        "-CheckRuntimeShadow",
        action="store_true",
    )
    parser.add_argument(
        "--check-runtime-live-shadow",
        "-CheckRuntimeLiveShadow",
        action="store_true",
    )
    parser.add_argument(
        "--runtime-shadow-jsonl-path",
        "-RuntimeShadowJsonlPath",
        default=r".\build\Release\logs\v2_shadow_policy_parity_backtest.jsonl",
    )
    parser.add_argument(
        "--runtime-shadow-live-jsonl-path",
        "-RuntimeShadowLiveJsonlPath",
        default=r".\build\Release\logs\v2_shadow_policy_parity_live.jsonl",
    )
    parser.add_argument(
        "--max-runtime-selection-symmetric-diff",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--max-runtime-taxonomy-mismatch-count",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--max-runtime-dropped-count-diff",
        type=int,
        default=0,
    )
    parser.add_argument("--strict", "-Strict", action="store_true")
    return parser.parse_args(argv)


def _as_bool(obj: Dict[str, Any], key: str) -> bool:
    return bool(obj.get(key, False))


def _as_int(obj: Dict[str, Any], key: str) -> int:
    try:
        return int(obj.get(key, 0) or 0)
    except Exception:
        return 0


def evaluate_runtime_shadow_jsonl(path_value) -> Dict[str, Any]:
    report: Dict[str, Any] = {
        "path": str(path_value),
        "exists": path_value.exists(),
        "entry_count": 0,
        "parse_errors": 0,
        "failed_entries": 0,
        "max_selection_symmetric_diff_count": 0,
        "max_taxonomy_mismatch_count": 0,
        "max_dropped_count_diff": 0,
        "sample_tail": [],
    }
    if not path_value.exists():
        return report

    sample_tail = []
    for line in read_nonempty_lines(path_value):
        sample_tail.append(line)
        if len(sample_tail) > 10:
            sample_tail = sample_tail[-10:]
        try:
            row = json.loads(line)
        except Exception:
            report["parse_errors"] += 1
            continue
        if not isinstance(row, dict):
            report["parse_errors"] += 1
            continue

        report["entry_count"] += 1
        if not bool(row.get("shadow_pass", False)):
            report["failed_entries"] += 1

        metrics = dict(row.get("metrics") or {})
        report["max_selection_symmetric_diff_count"] = max(
            int(report["max_selection_symmetric_diff_count"]),
            _as_int(metrics, "selection_symmetric_diff_count"),
        )
        report["max_taxonomy_mismatch_count"] = max(
            int(report["max_taxonomy_mismatch_count"]),
            _as_int(metrics, "taxonomy_mismatch_count"),
        )
        report["max_dropped_count_diff"] = max(
            int(report["max_dropped_count_diff"]),
            _as_int(metrics, "dropped_count_diff"),
        )

    report["sample_tail"] = sample_tail
    return report


def main(argv=None) -> int:
    args = parse_args(argv)
    test_exe_path = resolve_repo_path(args.test_exe_path)
    runtime_shadow_jsonl_path = resolve_repo_path(args.runtime_shadow_jsonl_path)
    runtime_shadow_live_jsonl_path = resolve_repo_path(args.runtime_shadow_live_jsonl_path)
    output_path = resolve_repo_path(args.output_json)

    result = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "inputs": {
            "test_exe_path": str(test_exe_path),
            "runtime_shadow_jsonl_path": str(runtime_shadow_jsonl_path),
            "runtime_shadow_live_jsonl_path": str(runtime_shadow_live_jsonl_path),
        },
        "thresholds": {
            "max_selection_symmetric_diff": int(args.max_selection_symmetric_diff),
            "max_taxonomy_mismatch_count": int(args.max_taxonomy_mismatch_count),
            "max_execution_schema_mismatch_count": int(args.max_execution_schema_mismatch_count),
            "check_runtime_shadow": bool(args.check_runtime_shadow),
            "check_runtime_live_shadow": bool(args.check_runtime_live_shadow),
            "max_runtime_selection_symmetric_diff": int(args.max_runtime_selection_symmetric_diff),
            "max_runtime_taxonomy_mismatch_count": int(args.max_runtime_taxonomy_mismatch_count),
            "max_runtime_dropped_count_diff": int(args.max_runtime_dropped_count_diff),
            "strict": bool(args.strict),
        },
        "checks": {},
        "metrics": {},
        "runtime_shadow": None,
        "runtime_shadow_live": None,
        "stdout_tail": [],
        "stderr_tail": [],
        "raw_shadow_report": None,
        "warnings": [],
        "errors": [],
    }

    if not test_exe_path.exists():
        result["errors"].append("shadow_parity_test_exe_missing")
        dump_json(output_path, result)
        print(f"[V2ShadowParity] FAILED - see {output_path}")
        return 1

    command = [str(test_exe_path)]
    cmd = run_command(command)

    result["checks"]["test_exit_zero"] = cmd.exit_code == 0
    result["stdout_tail"] = tail_strings(cmd.stdout.splitlines(), 20)
    result["stderr_tail"] = tail_strings(cmd.stderr.splitlines(), 20)

    shadow_report = parse_last_json_line(cmd.stdout)
    result["raw_shadow_report"] = shadow_report
    result["checks"]["shadow_report_available"] = isinstance(shadow_report, dict)

    if not isinstance(shadow_report, dict):
        result["errors"].append("shadow_report_missing_or_invalid")
    else:
        checks = dict(shadow_report.get("checks") or {})
        metrics = dict(shadow_report.get("metrics") or {})
        result["checks"]["policy_selected_set_equal"] = _as_bool(checks, "policy_selected_set_equal")
        result["checks"]["policy_dropped_count_equal"] = _as_bool(checks, "policy_dropped_count_equal")
        result["checks"]["rejection_taxonomy_equal"] = _as_bool(checks, "rejection_taxonomy_equal")
        result["checks"]["execution_schema_compatible"] = _as_bool(checks, "execution_schema_compatible")
        result["checks"]["execution_side_valid"] = _as_bool(checks, "execution_side_valid")
        result["checks"]["execution_status_valid"] = _as_bool(checks, "execution_status_valid")

        selection_symmetric_diff_count = _as_int(metrics, "selection_symmetric_diff_count")
        taxonomy_mismatch_count = _as_int(metrics, "taxonomy_mismatch_count")
        execution_schema_mismatch_count = _as_int(metrics, "execution_schema_mismatch_count")
        result["metrics"] = {
            "selection_symmetric_diff_count": selection_symmetric_diff_count,
            "taxonomy_mismatch_count": taxonomy_mismatch_count,
            "execution_schema_mismatch_count": execution_schema_mismatch_count,
            "core_selected_count": _as_int(metrics, "core_selected_count"),
            "v2_selected_count": _as_int(metrics, "v2_selected_count"),
            "core_dropped_count": _as_int(metrics, "core_dropped_count"),
            "v2_dropped_count": _as_int(metrics, "v2_dropped_count"),
        }

        if selection_symmetric_diff_count > int(args.max_selection_symmetric_diff):
            result["errors"].append("selection_symmetric_diff_threshold_exceeded")
        if taxonomy_mismatch_count > int(args.max_taxonomy_mismatch_count):
            result["errors"].append("taxonomy_mismatch_threshold_exceeded")
        if execution_schema_mismatch_count > int(args.max_execution_schema_mismatch_count):
            result["errors"].append("execution_schema_mismatch_threshold_exceeded")

        if not result["checks"]["policy_selected_set_equal"]:
            result["errors"].append("policy_selected_set_mismatch")
        if not result["checks"]["policy_dropped_count_equal"]:
            result["errors"].append("policy_dropped_count_mismatch")
        if not result["checks"]["rejection_taxonomy_equal"]:
            result["errors"].append("rejection_taxonomy_mismatch")
        if not result["checks"]["execution_schema_compatible"]:
            result["errors"].append("execution_schema_incompatible")
        if not result["checks"]["execution_side_valid"]:
            result["errors"].append("execution_side_invalid")
        if not result["checks"]["execution_status_valid"]:
            result["errors"].append("execution_status_invalid")

    if not result["checks"].get("test_exit_zero", False):
        result["errors"].append(f"shadow_parity_test_failed_exit_{cmd.exit_code}")

    if args.check_runtime_shadow:
        runtime_shadow = evaluate_runtime_shadow_jsonl(runtime_shadow_jsonl_path)
        result["runtime_shadow"] = runtime_shadow

        runtime_exists = bool(runtime_shadow.get("exists", False))
        runtime_entries = int(runtime_shadow.get("entry_count", 0) or 0)
        runtime_parse_errors = int(runtime_shadow.get("parse_errors", 0) or 0)
        runtime_failed_entries = int(runtime_shadow.get("failed_entries", 0) or 0)
        runtime_max_selection_diff = int(runtime_shadow.get("max_selection_symmetric_diff_count", 0) or 0)
        runtime_max_taxonomy_mismatch = int(runtime_shadow.get("max_taxonomy_mismatch_count", 0) or 0)
        runtime_max_dropped_diff = int(runtime_shadow.get("max_dropped_count_diff", 0) or 0)

        result["checks"]["runtime_shadow_log_available"] = runtime_exists
        result["checks"]["runtime_shadow_entries_available"] = runtime_entries > 0
        result["checks"]["runtime_shadow_parse_error_free"] = runtime_parse_errors == 0
        result["checks"]["runtime_shadow_all_entries_passed"] = runtime_failed_entries == 0

        result["metrics"]["runtime_entry_count"] = runtime_entries
        result["metrics"]["runtime_failed_entries"] = runtime_failed_entries
        result["metrics"]["runtime_max_selection_symmetric_diff_count"] = runtime_max_selection_diff
        result["metrics"]["runtime_max_taxonomy_mismatch_count"] = runtime_max_taxonomy_mismatch
        result["metrics"]["runtime_max_dropped_count_diff"] = runtime_max_dropped_diff

        if not runtime_exists:
            result["warnings"].append("runtime_shadow_log_missing")
        if runtime_exists and runtime_entries <= 0:
            result["warnings"].append("runtime_shadow_log_empty")
        if runtime_parse_errors > 0:
            result["errors"].append("runtime_shadow_log_parse_error")
        if runtime_failed_entries > 0:
            result["errors"].append("runtime_shadow_entry_failed")
        if runtime_max_selection_diff > int(args.max_runtime_selection_symmetric_diff):
            result["errors"].append("runtime_selection_symmetric_diff_threshold_exceeded")
        if runtime_max_taxonomy_mismatch > int(args.max_runtime_taxonomy_mismatch_count):
            result["errors"].append("runtime_taxonomy_mismatch_threshold_exceeded")
        if runtime_max_dropped_diff > int(args.max_runtime_dropped_count_diff):
            result["errors"].append("runtime_dropped_count_diff_threshold_exceeded")

    if args.check_runtime_live_shadow:
        runtime_shadow_live = evaluate_runtime_shadow_jsonl(runtime_shadow_live_jsonl_path)
        result["runtime_shadow_live"] = runtime_shadow_live

        runtime_live_exists = bool(runtime_shadow_live.get("exists", False))
        runtime_live_entries = int(runtime_shadow_live.get("entry_count", 0) or 0)
        runtime_live_parse_errors = int(runtime_shadow_live.get("parse_errors", 0) or 0)
        runtime_live_failed_entries = int(runtime_shadow_live.get("failed_entries", 0) or 0)
        runtime_live_max_selection_diff = int(runtime_shadow_live.get("max_selection_symmetric_diff_count", 0) or 0)
        runtime_live_max_taxonomy_mismatch = int(runtime_shadow_live.get("max_taxonomy_mismatch_count", 0) or 0)
        runtime_live_max_dropped_diff = int(runtime_shadow_live.get("max_dropped_count_diff", 0) or 0)

        result["checks"]["runtime_live_shadow_log_available"] = runtime_live_exists
        result["checks"]["runtime_live_shadow_entries_available"] = runtime_live_entries > 0
        result["checks"]["runtime_live_shadow_parse_error_free"] = runtime_live_parse_errors == 0
        result["checks"]["runtime_live_shadow_all_entries_passed"] = runtime_live_failed_entries == 0

        result["metrics"]["runtime_live_entry_count"] = runtime_live_entries
        result["metrics"]["runtime_live_failed_entries"] = runtime_live_failed_entries
        result["metrics"]["runtime_live_max_selection_symmetric_diff_count"] = runtime_live_max_selection_diff
        result["metrics"]["runtime_live_max_taxonomy_mismatch_count"] = runtime_live_max_taxonomy_mismatch
        result["metrics"]["runtime_live_max_dropped_count_diff"] = runtime_live_max_dropped_diff

        if not runtime_live_exists:
            result["warnings"].append("runtime_live_shadow_log_missing")
        if runtime_live_exists and runtime_live_entries <= 0:
            result["warnings"].append("runtime_live_shadow_log_empty")
        if runtime_live_parse_errors > 0:
            result["errors"].append("runtime_live_shadow_log_parse_error")
        if runtime_live_failed_entries > 0:
            result["errors"].append("runtime_live_shadow_entry_failed")
        if runtime_live_max_selection_diff > int(args.max_runtime_selection_symmetric_diff):
            result["errors"].append("runtime_live_selection_symmetric_diff_threshold_exceeded")
        if runtime_live_max_taxonomy_mismatch > int(args.max_runtime_taxonomy_mismatch_count):
            result["errors"].append("runtime_live_taxonomy_mismatch_threshold_exceeded")
        if runtime_live_max_dropped_diff > int(args.max_runtime_dropped_count_diff):
            result["errors"].append("runtime_live_dropped_count_diff_threshold_exceeded")

    if args.strict:
        if not result["checks"].get("shadow_report_available", False):
            result["errors"].append("strict_failed_shadow_report_missing")
        if not result["checks"].get("test_exit_zero", False):
            result["errors"].append("strict_failed_test_exit_nonzero")
        if args.check_runtime_shadow:
            if not result["checks"].get("runtime_shadow_log_available", False):
                result["errors"].append("strict_failed_runtime_shadow_log_missing")
            if not result["checks"].get("runtime_shadow_entries_available", False):
                result["errors"].append("strict_failed_runtime_shadow_log_empty")
            if not result["checks"].get("runtime_shadow_parse_error_free", False):
                result["errors"].append("strict_failed_runtime_shadow_parse_error")
            if not result["checks"].get("runtime_shadow_all_entries_passed", False):
                result["errors"].append("strict_failed_runtime_shadow_entry_failed")
        if args.check_runtime_live_shadow:
            if not result["checks"].get("runtime_live_shadow_log_available", False):
                result["errors"].append("strict_failed_runtime_live_shadow_log_missing")
            if not result["checks"].get("runtime_live_shadow_entries_available", False):
                result["errors"].append("strict_failed_runtime_live_shadow_log_empty")
            if not result["checks"].get("runtime_live_shadow_parse_error_free", False):
                result["errors"].append("strict_failed_runtime_live_shadow_parse_error")
            if not result["checks"].get("runtime_live_shadow_all_entries_passed", False):
                result["errors"].append("strict_failed_runtime_live_shadow_entry_failed")

    dump_json(output_path, result)

    if result["errors"]:
        print(f"[V2ShadowParity] FAILED - see {output_path}")
        return 1
    if result["warnings"]:
        print(f"[V2ShadowParity] PASSED with warnings - see {output_path}")
        return 0
    print(f"[V2ShadowParity] PASSED - see {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
