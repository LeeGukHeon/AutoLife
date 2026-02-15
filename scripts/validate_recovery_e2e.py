#!/usr/bin/env python3
import argparse
from datetime import datetime, timezone

import validate_recovery_state
from _script_common import (
    dump_json,
    find_latest_log,
    load_json_or_none,
    read_nonempty_lines,
    resolve_repo_path,
    tail_strings,
)


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshot-path", "-SnapshotPath", default="build/Release/state/snapshot_state.json")
    parser.add_argument("--legacy-state-path", "-LegacyStatePath", default="build/Release/state/state.json")
    parser.add_argument("--journal-path", "-JournalPath", default="build/Release/state/event_journal.jsonl")
    parser.add_argument("--log-dir", "-LogDir", default="build/Release/logs")
    parser.add_argument("--log-path", "-LogPath", default="")
    parser.add_argument("--output-json", "-OutputJson", default="build/Release/logs/recovery_e2e_report.json")
    parser.add_argument(
        "--state-validation-json",
        "-StateValidationJson",
        default="build/Release/logs/recovery_state_validation.json",
    )
    parser.add_argument("--strict-log-check", "-StrictLogCheck", action="store_true")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    snapshot_path = resolve_repo_path(args.snapshot_path)
    legacy_path = resolve_repo_path(args.legacy_state_path)
    journal_path = resolve_repo_path(args.journal_path)
    log_dir = resolve_repo_path(args.log_dir)
    output_path = resolve_repo_path(args.output_json)
    state_validation_path = resolve_repo_path(args.state_validation_json)

    state_exit = validate_recovery_state.main_with_paths(
        snapshot_path,
        legacy_path,
        journal_path,
        state_validation_path,
    )

    state_validation = load_json_or_none(state_validation_path)

    log_path = resolve_repo_path(args.log_path) if args.log_path.strip() else None
    if log_path is None:
        log_path = find_latest_log(log_dir)
    log_exists = bool(log_path and log_path.exists())

    log_matches = {
        "snapshot_loaded": [],
        "replay_applied": [],
        "replay_noop": [],
        "reconcile_completed": [],
    }
    if log_exists:
        for line in read_nonempty_lines(log_path):
            if "State snapshot loaded:" in line:
                log_matches["snapshot_loaded"].append(line)
            if "State restore: journal replay applied" in line:
                log_matches["replay_applied"].append(line)
            if "State restore: no replay events applied" in line:
                log_matches["replay_noop"].append(line)
            if "Account state synchronization completed" in line:
                log_matches["reconcile_completed"].append(line)

    replayable_count = 0
    if isinstance(state_validation, dict):
        metrics = state_validation.get("metrics")
        if isinstance(metrics, dict):
            replayable_count = int(metrics.get("replayable_event_count", 0))

    checks = {
        "state_validation_passed": state_exit == 0,
        "log_available": log_exists,
        "log_has_snapshot_loaded": len(log_matches["snapshot_loaded"]) > 0,
        "log_has_replay_evidence": len(log_matches["replay_applied"]) > 0 or len(log_matches["replay_noop"]) > 0,
        "log_has_reconcile_completed": len(log_matches["reconcile_completed"]) > 0,
    }

    warnings = []
    if not checks["log_available"]:
        warnings.append("log_not_found")
    if checks["log_available"] and not checks["log_has_snapshot_loaded"]:
        warnings.append("snapshot_load_log_missing")
    if checks["log_available"] and not checks["log_has_replay_evidence"]:
        warnings.append("replay_log_missing")
    if checks["log_available"] and not checks["log_has_reconcile_completed"]:
        warnings.append("reconcile_log_missing")
    if replayable_count > 0 and checks["log_available"] and not checks["log_has_replay_evidence"]:
        warnings.append("replayable_events_without_replay_log")

    errors = []
    if not checks["state_validation_passed"]:
        errors.append("state_validation_failed")
    if args.strict_log_check:
        if not checks["log_available"]:
            errors.append("strict_log_check_failed_no_log")
        elif not (checks["log_has_snapshot_loaded"] and checks["log_has_replay_evidence"] and checks["log_has_reconcile_completed"]):
            errors.append("strict_log_check_failed_incomplete_markers")

    report = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "inputs": {
            "snapshot_path": str(snapshot_path),
            "legacy_state_path": str(legacy_path),
            "journal_path": str(journal_path),
            "log_path": str(log_path) if log_path else "",
            "state_validation_json": str(state_validation_path),
        },
        "checks": checks,
        "metrics": {
            "replayable_event_count": replayable_count,
            "snapshot_loaded_log_count": len(log_matches["snapshot_loaded"]),
            "replay_applied_log_count": len(log_matches["replay_applied"]),
            "replay_noop_log_count": len(log_matches["replay_noop"]),
            "reconcile_completed_log_count": len(log_matches["reconcile_completed"]),
        },
        "evidence": {
            "snapshot_loaded": tail_strings(log_matches["snapshot_loaded"], 3),
            "replay_applied": tail_strings(log_matches["replay_applied"], 3),
            "replay_noop": tail_strings(log_matches["replay_noop"], 3),
            "reconcile_completed": tail_strings(log_matches["reconcile_completed"], 3),
        },
        "warnings": warnings,
        "errors": errors,
    }
    dump_json(output_path, report)

    if errors:
        print(f"[RecoveryE2E] FAILED - see {output_path}")
        return 1
    if warnings:
        print(f"[RecoveryE2E] PASSED with warnings - see {output_path}")
    else:
        print(f"[RecoveryE2E] PASSED - see {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
