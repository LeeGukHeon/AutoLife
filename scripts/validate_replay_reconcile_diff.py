#!/usr/bin/env python3
import argparse
import re
from datetime import datetime, timezone

from _script_common import dump_json, find_latest_log, load_json_or_none, read_nonempty_lines, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--state-validation-json",
        "-StateValidationJson",
        default="build/Release/logs/recovery_state_validation_strict.json",
    )
    parser.add_argument(
        "--recovery-e2e-json",
        "-RecoveryE2EJson",
        default="build/Release/logs/recovery_e2e_report_strict.json",
    )
    parser.add_argument("--log-dir", "-LogDir", default="build/Release/logs")
    parser.add_argument("--log-path", "-LogPath", default="")
    parser.add_argument("--output-json", "-OutputJson", default="build/Release/logs/replay_reconcile_diff_report.json")
    parser.add_argument("--strict", "-Strict", action="store_true")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    state_validation_path = resolve_repo_path(args.state_validation_json)
    recovery_e2e_path = resolve_repo_path(args.recovery_e2e_json)
    log_dir = resolve_repo_path(args.log_dir)
    output_path = resolve_repo_path(args.output_json)

    log_path = resolve_repo_path(args.log_path) if args.log_path.strip() else None
    if log_path is None:
        log_path = find_latest_log(log_dir)

    state_validation = load_json_or_none(state_validation_path)
    recovery_e2e = load_json_or_none(recovery_e2e_path)

    predicted_open = None
    if isinstance(state_validation, dict):
        metrics = state_validation.get("metrics")
        if isinstance(metrics, dict) and "predicted_open_positions_after_replay" in metrics:
            predicted_open = int(metrics.get("predicted_open_positions_after_replay", 0))

    log_exists = bool(log_path and log_path.exists())
    summary_line = None
    summary = None
    if log_exists:
        pattern = re.compile(
            r"Account state sync summary: wallet_markets=(\d+), reconcile_candidates=(\d+), restored_positions=(\d+), external_closes=(\d+)"
        )
        for line in read_nonempty_lines(log_path):
            if "Account state sync summary:" not in line:
                continue
            summary_line = line
        if summary_line:
            match = pattern.search(summary_line)
            if match:
                summary = {
                    "wallet_markets": int(match.group(1)),
                    "reconcile_candidates": int(match.group(2)),
                    "restored_positions": int(match.group(3)),
                    "external_closes": int(match.group(4)),
                }

    checks = {
        "state_validation_available": isinstance(state_validation, dict),
        "recovery_e2e_available": isinstance(recovery_e2e, dict),
        "log_available": log_exists,
        "sync_summary_available": summary is not None,
        "predicted_open_available": predicted_open is not None,
        "candidates_match_predicted": False,
        "partition_consistent": False,
    }

    metrics = {
        "predicted_open_positions_after_replay": predicted_open,
        "reconcile_candidates": summary["reconcile_candidates"] if summary else None,
        "restored_positions": summary["restored_positions"] if summary else None,
        "external_closes": summary["external_closes"] if summary else None,
        "wallet_markets": summary["wallet_markets"] if summary else None,
        "partition_delta": None,
    }

    if summary is not None and predicted_open is not None:
        checks["candidates_match_predicted"] = summary["reconcile_candidates"] == predicted_open
        partition_delta = (summary["restored_positions"] + summary["external_closes"]) - predicted_open
        checks["partition_consistent"] = partition_delta == 0
        metrics["partition_delta"] = partition_delta

    warnings = []
    if not checks["state_validation_available"]:
        warnings.append("state_validation_json_missing_or_invalid")
    if not checks["recovery_e2e_available"]:
        warnings.append("recovery_e2e_json_missing_or_invalid")
    if not checks["log_available"]:
        warnings.append("log_not_found")
    if checks["log_available"] and not checks["sync_summary_available"]:
        warnings.append("sync_summary_log_missing")
    if checks["sync_summary_available"] and not checks["predicted_open_available"]:
        warnings.append("predicted_open_positions_missing")
    if checks["sync_summary_available"] and checks["predicted_open_available"] and not checks["candidates_match_predicted"]:
        warnings.append("reconcile_candidates_mismatch")
    if checks["sync_summary_available"] and checks["predicted_open_available"] and not checks["partition_consistent"]:
        warnings.append("replay_reconcile_partition_mismatch")

    errors = []
    if args.strict:
        if not checks["log_available"]:
            errors.append("strict_failed_no_log")
        if not checks["sync_summary_available"]:
            errors.append("strict_failed_no_sync_summary")
        if not checks["predicted_open_available"]:
            errors.append("strict_failed_no_predicted_open_positions")
        if not checks["candidates_match_predicted"]:
            errors.append("strict_failed_candidates_mismatch")
        if not checks["partition_consistent"]:
            errors.append("strict_failed_partition_mismatch")

    replay_log_evidence = []
    replay_noop_evidence = []
    if isinstance(recovery_e2e, dict):
        evidence = recovery_e2e.get("evidence")
        if isinstance(evidence, dict):
            replay_applied = evidence.get("replay_applied")
            replay_noop = evidence.get("replay_noop")
            if isinstance(replay_applied, list):
                replay_log_evidence = [str(x) for x in replay_applied]
            if isinstance(replay_noop, list):
                replay_noop_evidence = [str(x) for x in replay_noop]

    report = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "inputs": {
            "state_validation_json": str(state_validation_path),
            "recovery_e2e_json": str(recovery_e2e_path),
            "log_path": str(log_path) if log_path else "",
        },
        "checks": checks,
        "metrics": metrics,
        "evidence": {
            "sync_summary_line": summary_line,
            "replay_log": replay_log_evidence,
            "replay_noop_log": replay_noop_evidence,
        },
        "warnings": warnings,
        "errors": errors,
    }
    dump_json(output_path, report)

    if errors:
        print(f"[ReplayReconcileDiff] FAILED - see {output_path}")
        return 1
    if warnings:
        print(f"[ReplayReconcileDiff] PASSED with warnings - see {output_path}")
    else:
        print(f"[ReplayReconcileDiff] PASSED - see {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
