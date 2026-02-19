#!/usr/bin/env python3
import argparse
from datetime import datetime, timezone

import validate_execution_parity
import validate_readiness
import validate_recovery_e2e
import validate_replay_reconcile_diff
from _script_common import dump_json, load_json_or_none, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--include-backtest", "-IncludeBacktest", action="store_true")
    parser.add_argument("--strict-log-check", "-StrictLogCheck", action="store_true")
    parser.add_argument("--no-strict-log-check", "-NoStrictLogCheck", action="store_true")
    parser.add_argument("--strict-execution-parity", "-StrictExecutionParity", action="store_true")
    parser.add_argument("--snapshot-path", "-SnapshotPath", default="build/Release/state/snapshot_state.json")
    parser.add_argument("--legacy-state-path", "-LegacyStatePath", default="build/Release/state/state.json")
    parser.add_argument("--journal-path", "-JournalPath", default="build/Release/state/event_journal.jsonl")
    parser.add_argument("--log-dir", "-LogDir", default="build/Release/logs")
    parser.add_argument("--recovery-output-json", "-RecoveryOutputJson", default="build/Release/logs/recovery_e2e_report_strict.json")
    parser.add_argument(
        "--recovery-state-validation-json",
        "-RecoveryStateValidationJson",
        default="build/Release/logs/recovery_state_validation_strict.json",
    )
    parser.add_argument(
        "--replay-reconcile-output-json",
        "-ReplayReconcileOutputJson",
        default="build/Release/logs/replay_reconcile_diff_report.json",
    )
    parser.add_argument(
        "--execution-parity-output-json",
        "-ExecutionParityOutputJson",
        default="build/Release/logs/execution_parity_report.json",
    )
    parser.add_argument("--backtest-output-json", "-BacktestOutputJson", default="build/Release/logs/readiness_report.json")
    parser.add_argument("--output-json", "-OutputJson", default="build/Release/logs/operational_readiness_report.json")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.strict_log_check and args.no_strict_log_check:
        raise RuntimeError("Invalid option combination: -StrictLogCheck and -NoStrictLogCheck cannot be used together.")

    output_path = resolve_repo_path(args.output_json)
    recovery_output_path = resolve_repo_path(args.recovery_output_json)
    recovery_state_validation_path = resolve_repo_path(args.recovery_state_validation_json)
    replay_reconcile_output_path = resolve_repo_path(args.replay_reconcile_output_json)
    execution_parity_output_path = resolve_repo_path(args.execution_parity_output_json)
    backtest_output_path = resolve_repo_path(args.backtest_output_json)

    strict_log_check_enabled = True
    if args.no_strict_log_check:
        strict_log_check_enabled = False
    if args.strict_log_check:
        strict_log_check_enabled = True

    recovery_argv = [
        "-SnapshotPath",
        str(resolve_repo_path(args.snapshot_path)),
        "-LegacyStatePath",
        str(resolve_repo_path(args.legacy_state_path)),
        "-JournalPath",
        str(resolve_repo_path(args.journal_path)),
        "-LogDir",
        str(resolve_repo_path(args.log_dir)),
        "-OutputJson",
        str(recovery_output_path),
        "-StateValidationJson",
        str(recovery_state_validation_path),
    ]
    if strict_log_check_enabled:
        recovery_argv.append("-StrictLogCheck")
    recovery_exit = validate_recovery_e2e.main(recovery_argv)

    replay_argv = [
        "-StateValidationJson",
        str(recovery_state_validation_path),
        "-RecoveryE2EJson",
        str(recovery_output_path),
        "-LogDir",
        str(resolve_repo_path(args.log_dir)),
        "-OutputJson",
        str(replay_reconcile_output_path),
    ]
    if strict_log_check_enabled:
        replay_argv.append("-Strict")
    replay_exit = validate_replay_reconcile_diff.main(replay_argv)

    parity_argv = ["-OutputJson", str(execution_parity_output_path)]
    if args.strict_execution_parity:
        parity_argv.append("-Strict")
    parity_exit = validate_execution_parity.main(parity_argv)

    backtest_exit = None
    if args.include_backtest:
        backtest_argv = ["-OutputJson", str(backtest_output_path)]
        backtest_exit = validate_readiness.main(backtest_argv)

    recovery_report = load_json_or_none(recovery_output_path)
    recovery_state_report = load_json_or_none(recovery_state_validation_path)
    replay_report = load_json_or_none(replay_reconcile_output_path)
    parity_report = load_json_or_none(execution_parity_output_path)
    backtest_report = load_json_or_none(backtest_output_path) if args.include_backtest else None

    errors = []
    if recovery_exit != 0:
        errors.append("recovery_validation_failed")
    if replay_exit != 0:
        errors.append("replay_reconcile_validation_failed")
    if parity_exit != 0:
        errors.append("execution_parity_validation_failed")
    if args.include_backtest and backtest_exit != 0:
        errors.append("backtest_readiness_failed")

    report = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "options": {
            "include_backtest": bool(args.include_backtest),
            "strict_log_check": bool(strict_log_check_enabled),
            "strict_execution_parity": bool(args.strict_execution_parity),
        },
        "checks": {
            "recovery_e2e_passed": recovery_exit == 0,
            "replay_reconcile_diff_passed": replay_exit == 0,
            "execution_parity_passed": parity_exit == 0,
            "backtest_readiness_executed": bool(args.include_backtest),
            "backtest_readiness_passed": (backtest_exit == 0) if args.include_backtest else None,
        },
        "artifacts": {
            "recovery_report": str(recovery_output_path),
            "recovery_state_validation_report": str(recovery_state_validation_path),
            "replay_reconcile_report": str(replay_reconcile_output_path),
            "execution_parity_report": str(execution_parity_output_path),
            "backtest_readiness_report": str(backtest_output_path) if args.include_backtest else None,
        },
        "recovery": recovery_report,
        "recovery_state_validation": recovery_state_report,
        "replay_reconcile": replay_report,
        "execution_parity": parity_report,
        "backtest": backtest_report,
        "errors": errors,
    }
    dump_json(output_path, report)

    if errors:
        print(f"[OperationalReadiness] FAILED - see {output_path}")
        return 1
    print(f"[OperationalReadiness] PASSED - see {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
