#!/usr/bin/env python3
import argparse
import pathlib
from datetime import datetime, timezone
from typing import Any, Dict

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Fail-closed promotion readiness evaluator for probabilistic pipeline "
            "(validation/parity/verification/shadow/live-config checks)."
        )
    )
    parser.add_argument(
        "--feature-validation-json",
        default=r".\build\Release\logs\probabilistic_feature_validation_summary.json",
    )
    parser.add_argument(
        "--parity-json",
        default=r".\build\Release\logs\probabilistic_runtime_bundle_parity.json",
    )
    parser.add_argument(
        "--verification-json",
        default=r".\build\Release\logs\verification_report.json",
    )
    parser.add_argument(
        "--shadow-report-json",
        default="",
        help="Optional shadow report path. Required for --target-stage live_enable.",
    )
    parser.add_argument(
        "--runtime-config-json",
        default=r".\build\Release\config\config.json",
        help="Runtime config used to assert allow_live_orders=false before live enable.",
    )
    parser.add_argument(
        "--shadow-validation-json",
        default="",
        help="Optional shadow report validation summary produced by validate_probabilistic_shadow_report.py.",
    )
    parser.add_argument(
        "--target-stage",
        choices=("prelive", "live_enable"),
        default="prelive",
        help="prelive: evaluate core gates; live_enable: require shadow + live orders disabled.",
    )
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("auto", "v2"),
        default="auto",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\probabilistic_promotion_readiness.json",
    )
    return parser.parse_args(argv)


def to_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    return str(value).strip().lower() in ("1", "true", "yes", "y", "on")


def infer_pipeline_from_manifest_payload(payload: Dict[str, Any]) -> str:
    explicit = str(payload.get("pipeline_version", "")).strip().lower()
    if explicit == "v2":
        return explicit
    version = str(payload.get("version", "")).strip().lower()
    if "v2" in version:
        return "v2"
    return "v2"


def infer_pipeline_from_feature_validation(feature_validation: Dict[str, Any]) -> str:
    explicit = str(feature_validation.get("pipeline_version", "")).strip().lower()
    if explicit == "v2":
        return explicit
    manifest_path = str(feature_validation.get("dataset_manifest_json", "")).strip()
    if not manifest_path:
        return "v2"
    manifest_payload = load_json_or_none(pathlib.Path(manifest_path))
    if isinstance(manifest_payload, dict):
        return infer_pipeline_from_manifest_payload(manifest_payload)
    return "v2"


def infer_pipeline_from_parity(parity: Dict[str, Any]) -> str:
    explicit = str(parity.get("pipeline_version", "")).strip().lower()
    if explicit == "v2":
        return explicit
    bundle_version = str(parity.get("runtime_bundle_version", "")).strip().lower()
    if "v2" in bundle_version:
        return "v2"
    return "v2"


def infer_pipeline_from_verification(verification: Dict[str, Any]) -> str:
    explicit = str(verification.get("pipeline_version", "")).strip().lower()
    if explicit == "v2":
        return explicit
    return "v2"


def extract_gate_profile_name(payload: Dict[str, Any]) -> str:
    raw = payload.get("gate_profile", "")
    if isinstance(raw, dict):
        return str(raw.get("name", "")).strip()
    return str(raw).strip()


def infer_pipeline_from_shadow(shadow_payload: Dict[str, Any]) -> str:
    explicit = str(shadow_payload.get("pipeline_version", "")).strip().lower()
    if explicit == "v2":
        return explicit
    runtime_bundle_version = str(shadow_payload.get("runtime_bundle_version", "")).strip().lower()
    if "v2" in runtime_bundle_version:
        return "v2"
    gate_profile = extract_gate_profile_name(shadow_payload).strip().lower()
    if gate_profile == "v2_strict":
        return "v2"
    return "v2"


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


def read_allow_live_orders(runtime_config_payload: Dict[str, Any]) -> Any:
    trading = runtime_config_payload.get("trading", {})
    if not isinstance(trading, dict):
        return None
    if "allow_live_orders" not in trading:
        return None
    return trading.get("allow_live_orders")


def evaluate(args: argparse.Namespace) -> Dict[str, Any]:
    feature_validation_path = resolve_repo_path(args.feature_validation_json)
    parity_path = resolve_repo_path(args.parity_json)
    verification_path = resolve_repo_path(args.verification_json)
    runtime_config_path = resolve_repo_path(args.runtime_config_json)
    output_path = resolve_repo_path(args.output_json)
    shadow_report_path = resolve_repo_path(args.shadow_report_json) if str(args.shadow_report_json).strip() else None
    shadow_validation_path = (
        resolve_repo_path(args.shadow_validation_json) if str(args.shadow_validation_json).strip() else None
    )

    feature_validation = load_json_or_none(feature_validation_path)
    parity = load_json_or_none(parity_path)
    verification = load_json_or_none(verification_path)
    runtime_config = load_json_or_none(runtime_config_path)
    shadow_payload = load_json_or_none(shadow_report_path) if shadow_report_path is not None else None
    shadow_validation_payload = (
        load_json_or_none(shadow_validation_path) if shadow_validation_path is not None else None
    )

    checks: Dict[str, Dict[str, Any]] = {}
    errors = []

    fv_ok = isinstance(feature_validation, dict) and str(feature_validation.get("status", "")).strip().lower() == "pass"
    checks["gate1_feature_validation_pass"] = {
        "pass": bool(fv_ok),
        "path": str(feature_validation_path),
        "status": str(feature_validation.get("status", "")) if isinstance(feature_validation, dict) else "missing",
    }
    if not fv_ok:
        errors.append("gate1_feature_validation_failed_or_missing")
    fv_preflight_errors = []
    if isinstance(feature_validation, dict):
        raw_preflight_errors = feature_validation.get("preflight_errors", [])
        if isinstance(raw_preflight_errors, list):
            fv_preflight_errors = [str(x).strip() for x in raw_preflight_errors if str(x).strip()]
    fv_preflight_ok = len(fv_preflight_errors) == 0
    checks["gate1_feature_validation_preflight"] = {
        "pass": bool(fv_preflight_ok),
        "path": str(feature_validation_path),
        "preflight_error_count": len(fv_preflight_errors),
        "preflight_errors": fv_preflight_errors,
    }
    if not fv_preflight_ok:
        errors.append("gate1_feature_validation_preflight_failed")

    parity_ok = isinstance(parity, dict) and str(parity.get("status", "")).strip().lower() == "pass"
    checks["gate2_parity_pass"] = {
        "pass": bool(parity_ok),
        "path": str(parity_path),
        "status": str(parity.get("status", "")) if isinstance(parity, dict) else "missing",
    }
    if not parity_ok:
        errors.append("gate2_parity_failed_or_missing")

    verification_ok = isinstance(verification, dict) and to_bool(verification.get("overall_gate_pass", False))
    checks["gate3_verification_pass"] = {
        "pass": bool(verification_ok),
        "path": str(verification_path),
        "overall_gate_pass": bool(verification.get("overall_gate_pass", False)) if isinstance(verification, dict) else False,
    }
    if not verification_ok:
        errors.append("gate3_verification_failed_or_missing")

    feature_pipeline = infer_pipeline_from_feature_validation(feature_validation) if isinstance(feature_validation, dict) else "v2"
    parity_pipeline = infer_pipeline_from_parity(parity) if isinstance(parity, dict) else "v2"
    verification_pipeline = infer_pipeline_from_verification(verification) if isinstance(verification, dict) else "v2"
    requested_pipeline = str(args.pipeline_version).strip().lower()
    resolved_pipeline = parity_pipeline
    if requested_pipeline == "v2":
        resolved_pipeline = requested_pipeline

    pipeline_consistent = (
        feature_pipeline == parity_pipeline == verification_pipeline == resolved_pipeline
    )
    checks["pipeline_consistency"] = {
        "pass": bool(pipeline_consistent),
        "requested": requested_pipeline,
        "resolved": resolved_pipeline,
        "feature_pipeline": feature_pipeline,
        "parity_pipeline": parity_pipeline,
        "verification_pipeline": verification_pipeline,
    }
    if not pipeline_consistent:
        errors.append("pipeline_version_mismatch_across_gate_outputs")

    if resolved_pipeline == "v2":
        feature_gate_profile = str(feature_validation.get("gate_profile", "")) if isinstance(feature_validation, dict) else ""
        parity_gate_profile = str(parity.get("gate_profile", "")) if isinstance(parity, dict) else ""
        verification_gate_profile = ""
        if isinstance(verification, dict):
            gp = verification.get("gate_profile", {})
            if isinstance(gp, dict):
                verification_gate_profile = str(gp.get("name", ""))
        v2_profile_ok = (
            str(feature_gate_profile).strip().lower() == "v2_strict" and
            str(parity_gate_profile).strip().lower() == "v2_strict" and
            str(verification_gate_profile).strip().lower() == "v2_strict"
        )
        checks["v2_strict_gate_profiles"] = {
            "pass": bool(v2_profile_ok),
            "feature_gate_profile": feature_gate_profile,
            "parity_gate_profile": parity_gate_profile,
            "verification_gate_profile": verification_gate_profile,
        }
        if not v2_profile_ok:
            errors.append("v2_strict_gate_profile_missing_or_mismatch")

    target_stage = str(args.target_stage).strip().lower()
    require_shadow = target_stage == "live_enable"
    require_live_disabled = target_stage == "live_enable"

    shadow_validation_ok = True
    shadow_validation_status = "not_checked"
    shadow_validation_pipeline = ""
    if shadow_validation_path is not None:
        shadow_validation_ok = (
            isinstance(shadow_validation_payload, dict) and
            str(shadow_validation_payload.get("status", "")).strip().lower() == "pass"
        )
        shadow_validation_status = (
            str(shadow_validation_payload.get("status", "")).strip()
            if isinstance(shadow_validation_payload, dict)
            else "missing"
        )
        shadow_validation_pipeline = (
            str(shadow_validation_payload.get("pipeline_version", "")).strip().lower()
            if isinstance(shadow_validation_payload, dict)
            else ""
        )
    shadow_validation_pipeline_ok = (
        shadow_validation_path is None or
        (shadow_validation_pipeline in ("", resolved_pipeline))
    )
    checks["gate4_shadow_validation"] = {
        "pass": bool(shadow_validation_ok and shadow_validation_pipeline_ok) if require_shadow else True,
        "required": bool(require_shadow and shadow_validation_path is not None),
        "path": str(shadow_validation_path) if shadow_validation_path is not None else "",
        "status": shadow_validation_status,
        "resolved_pipeline": resolved_pipeline,
        "shadow_validation_pipeline": shadow_validation_pipeline,
    }
    if require_shadow and shadow_validation_path is not None and not bool(shadow_validation_ok):
        errors.append("gate4_shadow_validation_failed_or_missing")
    if require_shadow and shadow_validation_path is not None and not bool(shadow_validation_pipeline_ok):
        errors.append("gate4_shadow_validation_pipeline_mismatch")

    shadow_pipeline = infer_pipeline_from_shadow(shadow_payload) if isinstance(shadow_payload, dict) else ""
    shadow_pipeline_ok = bool(isinstance(shadow_payload, dict) and shadow_pipeline == resolved_pipeline)
    checks["gate4_shadow_pipeline_consistency"] = {
        "pass": bool(shadow_pipeline_ok) if require_shadow else bool(shadow_pipeline_ok or shadow_payload is None),
        "required": bool(require_shadow),
        "path": str(shadow_report_path) if shadow_report_path is not None else "",
        "resolved_pipeline": resolved_pipeline,
        "shadow_pipeline": shadow_pipeline,
    }
    if require_shadow and not shadow_pipeline_ok:
        errors.append("gate4_shadow_pipeline_mismatch")

    shadow_gate_profile_name = extract_gate_profile_name(shadow_payload) if isinstance(shadow_payload, dict) else ""
    shadow_v2_gate_profile_ok = bool(
        resolved_pipeline != "v2" or not shadow_gate_profile_name or shadow_gate_profile_name.strip().lower() == "v2_strict"
    )
    checks["gate4_shadow_gate_profile"] = {
        "pass": bool(shadow_v2_gate_profile_ok) if require_shadow else bool(shadow_v2_gate_profile_ok or shadow_payload is None),
        "required": bool(require_shadow and resolved_pipeline == "v2"),
        "path": str(shadow_report_path) if shadow_report_path is not None else "",
        "shadow_gate_profile": shadow_gate_profile_name,
    }
    if require_shadow and not shadow_v2_gate_profile_ok:
        errors.append("gate4_shadow_gate_profile_mismatch")

    if require_shadow:
        shadow_ok = (
            shadow_report_path is not None and
            isinstance(shadow_payload, dict) and
            shadow_report_pass(shadow_payload)
            and shadow_pipeline_ok
            and shadow_v2_gate_profile_ok
            and shadow_validation_ok
            and shadow_validation_pipeline_ok
        )
        checks["gate4_shadow_pass"] = {
            "pass": bool(shadow_ok),
            "required": True,
            "path": str(shadow_report_path) if shadow_report_path is not None else "",
            "status": str(shadow_payload.get("status", "")) if isinstance(shadow_payload, dict) else "missing",
        }
        if not shadow_ok:
            errors.append("gate4_shadow_failed_or_missing")
    else:
        shadow_info = (
            shadow_report_path is not None and
            isinstance(shadow_payload, dict) and
            shadow_report_pass(shadow_payload) and
            (shadow_pipeline == resolved_pipeline) and
            shadow_v2_gate_profile_ok
        )
        checks["gate4_shadow_pass"] = {
            "pass": True,
            "required": False,
            "path": str(shadow_report_path) if shadow_report_path is not None else "",
            "status": str(shadow_payload.get("status", "")) if isinstance(shadow_payload, dict) else "not_checked",
            "observed_shadow_pass": bool(shadow_info),
        }

    if require_live_disabled:
        allow_live_orders = read_allow_live_orders(runtime_config) if isinstance(runtime_config, dict) else None
        live_disabled_ok = (allow_live_orders is False)
        checks["gate5_live_orders_disabled_before_enable"] = {
            "pass": bool(live_disabled_ok),
            "required": True,
            "runtime_config_path": str(runtime_config_path),
            "allow_live_orders": allow_live_orders,
        }
        if not live_disabled_ok:
            errors.append("gate5_allow_live_orders_must_be_false_before_live_enable")
    else:
        checks["gate5_live_orders_disabled_before_enable"] = {
            "pass": True,
            "required": False,
            "runtime_config_path": str(runtime_config_path),
            "allow_live_orders": (
                read_allow_live_orders(runtime_config) if isinstance(runtime_config, dict) else None
            ),
        }

    all_pass = all(bool(item.get("pass", False)) for item in checks.values())
    status = "pass" if all_pass else "fail"
    output = {
        "generated_at_utc": utc_now_iso(),
        "status": status,
        "promotion_ready": bool(all_pass),
        "target_stage": target_stage,
        "pipeline_version": resolved_pipeline,
        "checks": checks,
        "errors": errors,
        "inputs": {
            "feature_validation_json": str(feature_validation_path),
            "parity_json": str(parity_path),
            "verification_json": str(verification_path),
            "shadow_report_json": str(shadow_report_path) if shadow_report_path is not None else "",
            "shadow_validation_json": str(shadow_validation_path) if shadow_validation_path is not None else "",
            "runtime_config_json": str(runtime_config_path),
        },
        "artifacts": {
            "output_json": str(output_path),
        },
        "recommended_next_step": (
            "staged_live_enable_allowed"
            if all_pass and target_stage == "live_enable"
            else ("shadow_run_required" if "gate4_shadow_failed_or_missing" in errors else "fix_failed_gates")
        ),
    }
    dump_json(output_path, output)
    return output


def main(argv=None) -> int:
    args = parse_args(argv)
    output = evaluate(args)
    print("[ProbabilisticPromotionReadiness] completed", flush=True)
    print(f"status={output.get('status', 'fail')}", flush=True)
    print(f"target_stage={output.get('target_stage', '')}", flush=True)
    print(f"pipeline_version={output.get('pipeline_version', '')}", flush=True)
    print(f"errors={len(output.get('errors', []) or [])}", flush=True)
    print(f"output={output.get('artifacts', {}).get('output_json', '')}", flush=True)
    return 0 if str(output.get("status", "fail")).lower() == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
