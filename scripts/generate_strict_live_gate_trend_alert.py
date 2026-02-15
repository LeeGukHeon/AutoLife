#!/usr/bin/env python3
import argparse
import csv
import json
import os
from datetime import datetime, timedelta, timezone

from _script_common import resolve_repo_path


def parse_args(argv=None):
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--execution-parity-report-path", "-ExecutionParityReportPath", default="build/Release/logs/execution_parity_report.json")
    parser.add_argument("--operational-readiness-report-path", "-OperationalReadinessReportPath", default="build/Release/logs/operational_readiness_report.json")
    parser.add_argument("--history-path", "-HistoryPath", default="build/Release/logs/strict_live_gate_history.jsonl")
    parser.add_argument("--daily-summary-json-path", "-DailySummaryJsonPath", default="build/Release/logs/strict_live_gate_daily_summary.json")
    parser.add_argument("--weekly-summary-json-path", "-WeeklySummaryJsonPath", default="build/Release/logs/strict_live_gate_weekly_summary.json")
    parser.add_argument("--daily-summary-csv-path", "-DailySummaryCsvPath", default="build/Release/logs/strict_live_gate_daily_summary.csv")
    parser.add_argument("--weekly-summary-csv-path", "-WeeklySummaryCsvPath", default="build/Release/logs/strict_live_gate_weekly_summary.csv")
    parser.add_argument("--alert-output-json-path", "-AlertOutputJsonPath", default="build/Release/logs/strict_live_gate_alert_report.json")
    parser.add_argument("--threshold-tuning-output-json-path", "-ThresholdTuningOutputJsonPath", default="build/Release/logs/strict_live_gate_threshold_tuning_report.json")
    parser.add_argument("--action-response-output-json-path", "-ActionResponseOutputJsonPath", default="build/Release/logs/strict_live_gate_action_response_report.json")
    parser.add_argument("--gate-profile", "-GateProfile", default="strict_live")
    parser.add_argument("--consecutive-failure-threshold", "-ConsecutiveFailureThreshold", type=int, default=2)
    parser.add_argument("--warning-ratio-threshold", "-WarningRatioThreshold", type=float, default=0.30)
    parser.add_argument("--warning-ratio-lookback-days", "-WarningRatioLookbackDays", type=int, default=7)
    parser.add_argument("--warning-ratio-min-samples", "-WarningRatioMinSamples", type=int, default=3)
    parser.add_argument("--apply-tuned-thresholds", "-ApplyTunedThresholds", action="store_true")
    parser.add_argument("--action-execution-policy", "-ActionExecutionPolicy", choices=["report-only", "safe-auto-execute"], default="report-only")
    parser.add_argument("--enable-action-feedback-loop", "-EnableActionFeedbackLoop", action="store_true")
    parser.add_argument("--fail-on-critical-alert", "-FailOnCriticalAlert", action="store_true")
    args, _ = parser.parse_known_args(argv)
    return args


def load_json_or_none(path):
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def ensure_parent(path):
    path.parent.mkdir(parents=True, exist_ok=True)


def to_bool(value):
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    return str(value).strip().lower() in ("1", "true", "yes", "y", "on")


def export_csv(path, rows):
    with path.open("w", encoding="utf-8", newline="") as fh:
        if not rows:
            fh.write("")
            return
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main(argv=None) -> int:
    args = parse_args(argv)
    parity_path = resolve_repo_path(args.execution_parity_report_path)
    operational_path = resolve_repo_path(args.operational_readiness_report_path)
    history_path = resolve_repo_path(args.history_path)
    daily_json_path = resolve_repo_path(args.daily_summary_json_path)
    weekly_json_path = resolve_repo_path(args.weekly_summary_json_path)
    daily_csv_path = resolve_repo_path(args.daily_summary_csv_path)
    weekly_csv_path = resolve_repo_path(args.weekly_summary_csv_path)
    alert_json_path = resolve_repo_path(args.alert_output_json_path)
    tuning_json_path = resolve_repo_path(args.threshold_tuning_output_json_path)
    action_json_path = resolve_repo_path(args.action_response_output_json_path)

    for p in [history_path, daily_json_path, weekly_json_path, daily_csv_path, weekly_csv_path, alert_json_path, tuning_json_path, action_json_path]:
        ensure_parent(p)

    operational = load_json_or_none(operational_path)
    parity = load_json_or_none(parity_path)
    prev_action = load_json_or_none(action_json_path)

    now_utc = datetime.now(tz=timezone.utc)
    run_utc = now_utc
    if isinstance(operational, dict) and operational.get("generated_at"):
        try:
            run_utc = datetime.fromisoformat(str(operational["generated_at"]).replace("Z", "+00:00")).astimezone(timezone.utc)
        except Exception:
            pass

    operational_errors = list((operational or {}).get("errors") or [])
    parity_errors = list((parity or {}).get("errors") or [])
    operational_warnings = list((operational or {}).get("warnings") or [])
    parity_warnings = list((parity or {}).get("warnings") or [])

    operational_passed = bool(operational is not None and len(operational_errors) == 0)
    parity_passed = bool(parity is not None and len(parity_errors) == 0)
    strict_gate_passed = operational_passed and parity_passed
    warning_present = (len(operational_warnings) + len(parity_warnings)) > 0

    record = {
        "schema_version": 1,
        "recorded_at": now_utc.isoformat(),
        "run_timestamp_utc": run_utc.isoformat(),
        "run_date_utc": run_utc.strftime("%Y-%m-%d"),
        "week_start_date_utc": (run_utc - timedelta(days=run_utc.weekday())).strftime("%Y-%m-%d"),
        "gate_profile": args.gate_profile,
        "checks": {
            "operational_report_available": operational is not None,
            "execution_parity_report_available": parity is not None,
            "operational_passed": operational_passed,
            "execution_parity_passed": parity_passed,
            "strict_gate_passed": strict_gate_passed,
        },
        "metrics": {
            "operational_error_count": len(operational_errors),
            "parity_error_count": len(parity_errors),
            "warning_count": len(operational_warnings) + len(parity_warnings),
            "warning_present": warning_present,
        },
        "warnings": ["warning_signals_present"] if warning_present else [],
        "errors": [] if strict_gate_passed else ["strict_live_gate_failed"],
        "ci": {
            "github_run_id": os.environ.get("GITHUB_RUN_ID"),
            "github_run_number": os.environ.get("GITHUB_RUN_NUMBER"),
            "github_workflow": os.environ.get("GITHUB_WORKFLOW"),
            "github_job": os.environ.get("GITHUB_JOB"),
            "github_sha": os.environ.get("GITHUB_SHA"),
        },
    }

    history_records = []
    parse_errors = 0
    if history_path.exists():
        for line in history_path.read_text(encoding="utf-8", errors="ignore").splitlines():
            if not line.strip():
                continue
            try:
                history_records.append(json.loads(line))
            except Exception:
                parse_errors += 1
    history_records.append(record)
    with history_path.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(record, ensure_ascii=False) + "\n")

    daily = {}
    weekly = {}
    for r in history_records:
        d = str(r.get("run_date_utc", ""))
        w = str(r.get("week_start_date_utc", ""))
        d_entry = daily.setdefault(d, {"date_utc": d, "total_runs": 0, "strict_pass_runs": 0, "strict_fail_runs": 0, "warning_runs": 0, "warning_ratio": 0.0, "operational_error_events": 0, "parity_error_events": 0})
        w_entry = weekly.setdefault(w, {"week_start_date_utc": w, "total_runs": 0, "strict_pass_runs": 0, "strict_fail_runs": 0, "warning_runs": 0, "warning_ratio": 0.0, "operational_error_events": 0, "parity_error_events": 0})
        strict_pass = to_bool(((r.get("checks") or {}).get("strict_gate_passed")))
        warn = to_bool(((r.get("metrics") or {}).get("warning_present")))
        op_err = int(((r.get("metrics") or {}).get("operational_error_count") or 0))
        pa_err = int(((r.get("metrics") or {}).get("parity_error_count") or 0))
        for e in (d_entry, w_entry):
            e["total_runs"] += 1
            e["strict_pass_runs"] += (1 if strict_pass else 0)
            e["strict_fail_runs"] += (0 if strict_pass else 1)
            e["warning_runs"] += (1 if warn else 0)
            e["operational_error_events"] += op_err
            e["parity_error_events"] += pa_err
    for container in (daily, weekly):
        for item in container.values():
            item["warning_ratio"] = round(item["warning_runs"] / float(item["total_runs"]), 4) if item["total_runs"] > 0 else 0.0

    daily_rows = [daily[k] for k in sorted(daily.keys()) if k]
    weekly_rows = [weekly[k] for k in sorted(weekly.keys()) if k]
    export_csv(daily_csv_path, daily_rows)
    export_csv(weekly_csv_path, weekly_rows)

    daily_json_path.write_text(json.dumps({"generated_at": now_utc.isoformat(), "history_path": str(history_path), "history_record_count": len(history_records), "history_parse_errors_ignored": parse_errors, "latest_record": record, "summaries": daily_rows}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    weekly_json_path.write_text(json.dumps({"generated_at": now_utc.isoformat(), "history_path": str(history_path), "history_record_count": len(history_records), "history_parse_errors_ignored": parse_errors, "latest_record": record, "summaries": weekly_rows}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    sorted_latest = sorted(history_records, key=lambda x: str(x.get("run_timestamp_utc", "")), reverse=True)
    consecutive_failures = 0
    for r in sorted_latest:
        if to_bool(((r.get("checks") or {}).get("strict_gate_passed"))):
            break
        consecutive_failures += 1

    lookback_start = now_utc - timedelta(days=abs(int(args.warning_ratio_lookback_days)))
    lookback = [r for r in history_records if str(r.get("run_timestamp_utc", "")) >= lookback_start.isoformat()]
    lookback_total = len(lookback)
    lookback_warning_runs = len([r for r in lookback if to_bool(((r.get("metrics") or {}).get("warning_present")))])
    lookback_warning_ratio = round(lookback_warning_runs / float(lookback_total), 4) if lookback_total > 0 else 0.0

    consecutive_triggered = consecutive_failures >= int(args.consecutive_failure_threshold)
    warning_ratio_triggered = lookback_total >= int(args.warning_ratio_min_samples) and lookback_warning_ratio > float(args.warning_ratio_threshold)
    overall_status = "critical" if consecutive_triggered else ("warning" if warning_ratio_triggered else "ok")
    should_fail_gate = overall_status == "critical"

    threshold_payload = {
        "generated_at": now_utc.isoformat(),
        "history_path": str(history_path),
        "baseline_thresholds": {
            "consecutive_failure_threshold": int(args.consecutive_failure_threshold),
            "warning_ratio_threshold": float(args.warning_ratio_threshold),
            "warning_ratio_lookback_days": abs(int(args.warning_ratio_lookback_days)),
            "warning_ratio_min_samples": int(args.warning_ratio_min_samples),
        },
        "recommended_thresholds": {
            "consecutive_failure_threshold": int(args.consecutive_failure_threshold),
            "warning_ratio_threshold": float(args.warning_ratio_threshold),
        },
        "applied_thresholds": {
            "apply_tuned_thresholds_requested": bool(args.apply_tuned_thresholds),
            "tuned_thresholds_applied": bool(args.apply_tuned_thresholds),
            "consecutive_failure_threshold": int(args.consecutive_failure_threshold),
            "warning_ratio_threshold": float(args.warning_ratio_threshold),
        },
        "feedback_loop": {
            "enabled": bool(args.enable_action_feedback_loop),
            "previous_feedback_available": bool(isinstance(prev_action, dict)),
        },
    }
    tuning_json_path.write_text(json.dumps(threshold_payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    alert_payload = {
        "generated_at": now_utc.isoformat(),
        "thresholds": {
            "consecutive_failure_threshold": int(args.consecutive_failure_threshold),
            "warning_ratio_threshold": float(args.warning_ratio_threshold),
            "warning_ratio_lookback_days": abs(int(args.warning_ratio_lookback_days)),
            "warning_ratio_min_samples": int(args.warning_ratio_min_samples),
            "tuned_thresholds_applied": bool(args.apply_tuned_thresholds),
        },
        "current": {
            "latest_run_timestamp_utc": run_utc.isoformat(),
            "latest_run_strict_gate_passed": strict_gate_passed,
            "consecutive_strict_failures": consecutive_failures,
            "lookback_start_utc": lookback_start.isoformat(),
            "lookback_total_runs": lookback_total,
            "lookback_warning_runs": lookback_warning_runs,
            "lookback_warning_ratio": lookback_warning_ratio,
        },
        "alerts": [
            {"id": "consecutive_strict_failures", "severity": "critical" if consecutive_triggered else "info", "triggered": consecutive_triggered, "value": consecutive_failures, "threshold": int(args.consecutive_failure_threshold), "message": "Consecutive strict gate failures reached threshold."},
            {"id": "warning_ratio_high", "severity": "warning" if warning_ratio_triggered else "info", "triggered": warning_ratio_triggered, "value": lookback_warning_ratio, "threshold": float(args.warning_ratio_threshold), "message": "Warning ratio in lookback window exceeded threshold."},
        ],
        "overall_status": overall_status,
        "should_fail_gate": should_fail_gate,
    }
    alert_json_path.write_text(json.dumps(alert_payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    policies = []
    if not strict_gate_passed:
        policies.append({"id": "strict_gate_failed", "severity": "critical", "triggered": True, "evidence": {"operational_errors": operational_errors, "parity_errors": parity_errors}, "automatic_actions": ["Mark strict live gate as failed."], "manual_actions": ["Run strict gate diagnostics and rerun gate."], "command_hints": ["python scripts/run_ci_operational_gate.py -IncludeBacktest -RunLiveProbe -StrictExecutionParity"], "escalation": "ops_oncall", "execution_boundary": {"mode": "report-only", "auto_execute_eligible": False, "auto_execute_enabled": False, "resume_requires_manual_approval": True, "resume_approval_scope": "strict_live_schedule_resume", "reason": "report_only_required_for_risk_level"}})
    elif warning_ratio_triggered:
        mode = "safe-auto-execute" if args.action_execution_policy == "safe-auto-execute" else "report-only"
        policies.append({"id": "warning_ratio_high", "severity": "warning", "triggered": True, "evidence": {"warning_ratio": lookback_warning_ratio}, "automatic_actions": ["Publish warning-ratio trend alert."], "manual_actions": ["Review warning categories and monitor."], "command_hints": [], "escalation": "ops_maintainer", "execution_boundary": {"mode": mode, "auto_execute_eligible": True, "auto_execute_enabled": mode == "safe-auto-execute", "resume_requires_manual_approval": False, "resume_approval_scope": "none", "reason": "safe_auto_execute_enabled_by_policy" if mode == "safe-auto-execute" else "safe_auto_execute_available_but_not_selected"}})
    else:
        policies.append({"id": "strict_live_gate_healthy", "severity": "info", "triggered": True, "evidence": {"strict_gate_passed": strict_gate_passed}, "automatic_actions": ["Publish healthy status."], "manual_actions": ["No remediation required."], "command_hints": [], "escalation": "none", "execution_boundary": {"mode": "safe-auto-execute" if args.action_execution_policy == "safe-auto-execute" else "report-only", "auto_execute_eligible": True, "auto_execute_enabled": args.action_execution_policy == "safe-auto-execute", "resume_requires_manual_approval": False, "resume_approval_scope": "none", "reason": "safe_auto_execute_enabled_by_policy" if args.action_execution_policy == "safe-auto-execute" else "safe_auto_execute_available_but_not_selected"}})

    requires_manual = any(p["severity"] == "critical" for p in policies)
    action_status = "manual_intervention_required" if requires_manual else ("monitor" if warning_ratio_triggered else "ok")
    recommended_commands = []
    for p in policies:
        for c in p.get("command_hints", []):
            if c and c not in recommended_commands:
                recommended_commands.append(c)

    action_payload = {
        "generated_at": now_utc.isoformat(),
        "gate_profile": args.gate_profile,
        "action_execution_policy": args.action_execution_policy,
        "overall_status": action_status,
        "requires_manual_intervention": requires_manual,
        "latest_run": {
            "run_timestamp_utc": run_utc.isoformat(),
            "strict_gate_passed": strict_gate_passed,
            "operational_passed": operational_passed,
            "execution_parity_passed": parity_passed,
        },
        "threshold_context": {
            "consecutive_failure_threshold": int(args.consecutive_failure_threshold),
            "warning_ratio_threshold": float(args.warning_ratio_threshold),
            "tuned_thresholds_applied": bool(args.apply_tuned_thresholds),
            "threshold_tuning_report": str(tuning_json_path),
        },
        "policy_boundary_summary": {
            "report_only_policy_count": len([p for p in policies if p["execution_boundary"]["mode"] == "report-only"]),
            "safe_auto_execute_policy_count": len([p for p in policies if p["execution_boundary"]["mode"] == "safe-auto-execute"]),
            "manual_approval_required_policy_count": len([p for p in policies if p["execution_boundary"]["resume_requires_manual_approval"]]),
            "safe_auto_execute_policy_ids": [p["id"] for p in policies if p["execution_boundary"]["auto_execute_enabled"]],
        },
        "manual_approval": {
            "approval_required_for_resume": requires_manual,
            "approval_scope": "strict_live_schedule_resume" if requires_manual else "none",
            "approval_reason": "critical_policy_detected" if requires_manual else "no_critical_policy",
            "github_environment": {
                "required": requires_manual,
                "name": "strict-live-resume" if requires_manual else "none",
                "required_reviewers_expected": requires_manual,
            },
            "required_evidence_paths": [str(action_json_path), str(alert_json_path), str(tuning_json_path), str(operational_path), str(parity_path)],
            "resume_checklist": [
                "Pause strict live schedule while critical policy is open.",
                "Execute recommended_commands and confirm strict gate pass on rerun.",
                "Record approval decision and approver in ops log.",
                "Resume strict live schedule only after approval record is completed.",
            ],
        },
        "feedback_for_next_tuning": {
            "schema_version": 1,
            "source_run_timestamp_utc": run_utc.isoformat(),
            "stabilization_signal": "stable",
            "signal_reason": "no_stabilization_adjustment_needed",
            "signal_source": "run_level_indicator",
            "false_positive_indicator": warning_ratio_triggered and strict_gate_passed and (not requires_manual),
            "false_negative_indicator": (not strict_gate_passed) and (not warning_ratio_triggered) and (not consecutive_triggered),
        },
        "policies": policies,
        "recommended_commands": recommended_commands,
    }
    action_json_path.write_text(json.dumps(action_payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    if args.fail_on_critical_alert and should_fail_gate:
        print(f"[StrictLiveTrendAlert] FAILED (critical alert) - see {alert_json_path}")
        return 1
    print("[StrictLiveTrendAlert] PASSED - trend, tuning, and action reports generated under build/Release/logs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
