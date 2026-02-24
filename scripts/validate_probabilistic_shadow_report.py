#!/usr/bin/env python3
import argparse
from datetime import datetime, timezone
from typing import Any, Dict, List

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate probabilistic shadow-run report schema and decision-log parity evidence (fail-closed)."
    )
    parser.add_argument(
        "--shadow-report-json",
        default=r".\build\Release\logs\probabilistic_shadow_report_latest.json",
    )
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("auto", "v1", "v2"),
        default="auto",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\probabilistic_shadow_report_validation.json",
    )
    parser.add_argument("--strict", action="store_true")
    return parser.parse_args(argv)


def to_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    return str(value).strip().lower() in ("1", "true", "yes", "y", "on")


def extract_gate_profile_name(payload: Dict[str, Any]) -> str:
    raw = payload.get("gate_profile", "")
    if isinstance(raw, dict):
        return str(raw.get("name", "")).strip()
    return str(raw).strip()


def infer_pipeline_from_shadow(shadow_payload: Dict[str, Any]) -> str:
    explicit = str(shadow_payload.get("pipeline_version", "")).strip().lower()
    if explicit in ("v1", "v2"):
        return explicit
    runtime_bundle_version = str(shadow_payload.get("runtime_bundle_version", "")).strip().lower()
    if "v2" in runtime_bundle_version:
        return "v2"
    gate_profile = extract_gate_profile_name(shadow_payload).strip().lower()
    if gate_profile == "v2_strict":
        return "v2"
    return "v1"


def shadow_report_pass(shadow_payload: Dict[str, Any]) -> bool:
    status = str(shadow_payload.get("status", "")).strip().lower()
    if status != "pass":
        return False
    errors = shadow_payload.get("errors", [])
    if isinstance(errors, list) and len(errors) > 0:
        return False
    checks = shadow_payload.get("checks", {})
    if isinstance(checks, dict):
        if "distinct_log_paths" in checks and not to_bool(checks.get("distinct_log_paths")):
            return False
    if "overall_gate_pass" in shadow_payload and not to_bool(shadow_payload.get("overall_gate_pass")):
        return False
    if "shadow_pass" in shadow_payload and not to_bool(shadow_payload.get("shadow_pass")):
        return False
    return True


def read_int_nested(payload: Dict[str, Any], keys: List[str]) -> int:
    cur: Any = payload
    for key in keys:
        if not isinstance(cur, dict):
            return 0
        cur = cur.get(key)
    try:
        return int(float(cur))
    except Exception:
        return 0


def evaluate(args: argparse.Namespace) -> Dict[str, Any]:
    shadow_report_path = resolve_repo_path(args.shadow_report_json)
    output_path = resolve_repo_path(args.output_json)
    requested_pipeline = str(args.pipeline_version).strip().lower()

    shadow_payload = load_json_or_none(shadow_report_path)
    checks: Dict[str, Dict[str, Any]] = {}
    errors: List[str] = []

    payload_ok = isinstance(shadow_payload, dict)
    checks["shadow_report_json_valid"] = {
        "pass": bool(payload_ok),
        "path": str(shadow_report_path),
    }
    if not payload_ok:
        errors.append("shadow_report_missing_or_invalid")
        out = {
            "generated_at_utc": utc_now_iso(),
            "status": "fail",
            "shadow_report_ok": False,
            "pipeline_version": "v1" if requested_pipeline == "auto" else requested_pipeline,
            "checks": checks,
            "errors": errors,
            "inputs": {
                "shadow_report_json": str(shadow_report_path),
                "requested_pipeline_version": requested_pipeline,
            },
            "artifacts": {
                "output_json": str(output_path),
            },
        }
        dump_json(output_path, out)
        return out

    assert isinstance(shadow_payload, dict)
    resolved_pipeline = infer_pipeline_from_shadow(shadow_payload)
    pipeline_requested_ok = requested_pipeline == "auto" or requested_pipeline == resolved_pipeline
    checks["pipeline_consistency"] = {
        "pass": bool(pipeline_requested_ok),
        "requested_pipeline": requested_pipeline,
        "resolved_pipeline": resolved_pipeline,
    }
    if not pipeline_requested_ok:
        errors.append("shadow_report_pipeline_mismatch")

    shadow_status_ok = shadow_report_pass(shadow_payload)
    checks["shadow_status_pass"] = {
        "pass": bool(shadow_status_ok),
        "status": str(shadow_payload.get("status", "")),
        "overall_gate_pass": bool(shadow_payload.get("overall_gate_pass", False)),
        "shadow_pass": bool(shadow_payload.get("shadow_pass", False)),
    }
    if not shadow_status_ok:
        errors.append("shadow_report_status_not_pass")

    gate_profile = extract_gate_profile_name(shadow_payload)
    gate_profile_ok = (
        resolved_pipeline != "v2"
        or not gate_profile
        or gate_profile.strip().lower() == "v2_strict"
    )
    checks["gate_profile_consistency"] = {
        "pass": bool(gate_profile_ok),
        "pipeline_version": resolved_pipeline,
        "gate_profile": gate_profile,
    }
    if not gate_profile_ok:
        errors.append("shadow_report_gate_profile_mismatch")

    checks_node = shadow_payload.get("checks", {})
    if not isinstance(checks_node, dict):
        checks_node = {}
    evidence_keys = ("decision_log_comparison_pass", "same_bundle", "same_candles")
    evidence_present = all(key in checks_node for key in evidence_keys)
    evidence_all_true = all(to_bool(checks_node.get(key)) for key in evidence_keys) if evidence_present else False
    checks["decision_log_parity_evidence"] = {
        "pass": bool(evidence_present and evidence_all_true),
        "required_keys": list(evidence_keys),
        "present": bool(evidence_present),
        "values": {key: bool(to_bool(checks_node.get(key))) for key in evidence_keys},
    }
    if not (evidence_present and evidence_all_true):
        errors.append("shadow_report_missing_or_failed_decision_parity_evidence")

    compared_decision_count = max(
        0,
        read_int_nested(shadow_payload, ["metadata", "compared_decision_count"]),
        read_int_nested(shadow_payload, ["comparison", "compared_decision_count"]),
        read_int_nested(shadow_payload, ["compared_decision_count"]),
    )
    mismatch_count = max(
        0,
        read_int_nested(shadow_payload, ["metadata", "mismatch_count"]),
        read_int_nested(shadow_payload, ["comparison", "mismatch_count"]),
        read_int_nested(shadow_payload, ["mismatch_count"]),
    )
    count_check_ok = compared_decision_count > 0 and mismatch_count == 0
    checks["decision_log_comparison_counts"] = {
        "pass": bool(count_check_ok),
        "compared_decision_count": int(compared_decision_count),
        "mismatch_count": int(mismatch_count),
    }
    if not count_check_ok:
        errors.append("shadow_report_comparison_counts_invalid")

    status = "pass" if len(errors) == 0 else "fail"
    out = {
        "generated_at_utc": utc_now_iso(),
        "status": status,
        "shadow_report_ok": bool(status == "pass"),
        "pipeline_version": resolved_pipeline,
        "checks": checks,
        "errors": errors,
        "inputs": {
            "shadow_report_json": str(shadow_report_path),
            "requested_pipeline_version": requested_pipeline,
        },
        "artifacts": {
            "output_json": str(output_path),
        },
    }
    dump_json(output_path, out)
    return out


def main(argv=None) -> int:
    args = parse_args(argv)
    out = evaluate(args)
    print("[ValidateProbabilisticShadowReport] completed", flush=True)
    print(f"status={out.get('status', 'fail')}", flush=True)
    print(f"pipeline_version={out.get('pipeline_version', '')}", flush=True)
    print(f"errors={len(out.get('errors', []) or [])}", flush=True)
    print(f"output={out.get('artifacts', {}).get('output_json', '')}", flush=True)
    if bool(args.strict) and str(out.get("status", "fail")).lower() != "pass":
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
