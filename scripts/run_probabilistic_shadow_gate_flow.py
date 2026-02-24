#!/usr/bin/env python3
import argparse
import json
import pathlib
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

import evaluate_probabilistic_promotion_readiness as promotion_readiness
import generate_probabilistic_shadow_report as shadow_generate
import validate_probabilistic_shadow_report as shadow_validate
from _script_common import dump_json, resolve_repo_path

DEFAULT_RUNTIME_BUNDLE_V1 = r".\config\model\probabilistic_runtime_bundle_v1.json"
DEFAULT_RUNTIME_BUNDLE_V2 = r".\config\model\probabilistic_runtime_bundle_v2.json"
DEFAULT_LIVE_DECISION_LOG_JSONL = r".\build\Release\logs\policy_decisions.jsonl"
DEFAULT_BACKTEST_DECISION_LOG_JSONL = r".\build\Release\logs\policy_decisions_backtest.jsonl"
DEFAULT_FEATURE_VALIDATION_JSON = r".\build\Release\logs\probabilistic_feature_validation_summary.json"
DEFAULT_PARITY_JSON = r".\build\Release\logs\probabilistic_runtime_bundle_parity.json"
DEFAULT_VERIFICATION_JSON = r".\build\Release\logs\verification_report.json"
DEFAULT_RUNTIME_CONFIG_JSON = r".\build\Release\config\config.json"


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run Gate4 shadow evidence flow end-to-end: generate shadow report, "
            "validate shadow report, and evaluate promotion readiness fail-closed."
        )
    )
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("auto", "v1", "v2"),
        default="auto",
    )
    parser.add_argument(
        "--target-stage",
        choices=("prelive", "live_enable"),
        default="live_enable",
    )
    parser.add_argument(
        "--runtime-bundle-json",
        default="",
    )
    parser.add_argument(
        "--live-decision-log-jsonl",
        default=DEFAULT_LIVE_DECISION_LOG_JSONL,
    )
    parser.add_argument(
        "--backtest-decision-log-jsonl",
        default=DEFAULT_BACKTEST_DECISION_LOG_JSONL,
    )
    parser.add_argument(
        "--feature-validation-json",
        default=DEFAULT_FEATURE_VALIDATION_JSON,
    )
    parser.add_argument(
        "--parity-json",
        default=DEFAULT_PARITY_JSON,
    )
    parser.add_argument(
        "--verification-json",
        default=DEFAULT_VERIFICATION_JSON,
    )
    parser.add_argument(
        "--runtime-config-json",
        default=DEFAULT_RUNTIME_CONFIG_JSON,
    )
    parser.add_argument(
        "--shadow-report-json",
        default=r".\build\Release\logs\probabilistic_shadow_report_latest.json",
    )
    parser.add_argument(
        "--shadow-validation-json",
        default=r".\build\Release\logs\probabilistic_shadow_report_validation_latest.json",
    )
    parser.add_argument(
        "--promotion-output-json",
        default=r".\build\Release\logs\probabilistic_promotion_readiness_latest.json",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\probabilistic_shadow_gate_flow_summary.json",
    )
    return parser.parse_args(argv)


def infer_pipeline_version(args: argparse.Namespace) -> str:
    requested = str(args.pipeline_version).strip().lower()
    if requested in ("v1", "v2"):
        return requested
    bundle_raw = str(args.runtime_bundle_json).strip()
    bundle_path = (
        resolve_repo_path(bundle_raw)
        if bundle_raw
        else resolve_repo_path(DEFAULT_RUNTIME_BUNDLE_V2 if resolve_repo_path(DEFAULT_RUNTIME_BUNDLE_V2).exists() else DEFAULT_RUNTIME_BUNDLE_V1)
    )
    if bundle_path.exists():
        try:
            payload = json.loads(bundle_path.read_text(encoding="utf-8-sig"))
            if isinstance(payload, dict):
                explicit = str(payload.get("pipeline_version", "")).strip().lower()
                if explicit in ("v1", "v2"):
                    return explicit
                version = str(payload.get("version", "")).strip().lower()
                if "v2" in version:
                    return "v2"
        except Exception:
            pass
    return "v1"


def check_required_inputs(required: Dict[str, str]) -> List[str]:
    missing: List[str] = []
    for key, raw_path in required.items():
        path_value = resolve_repo_path(raw_path)
        if not path_value.exists():
            missing.append(f"{key}:{path_value}")
    return missing


def latest_file_for_patterns(
    patterns: List[str],
    *,
    exclude_name_tokens: Optional[List[str]] = None,
) -> Optional[pathlib.Path]:
    root = resolve_repo_path(r".")
    candidates: List[pathlib.Path] = []
    excluded = [str(x).strip().lower() for x in (exclude_name_tokens or []) if str(x).strip()]
    for pattern in patterns:
        for item in root.glob(pattern):
            if not item.is_file():
                continue
            name_lc = item.name.lower()
            if any(token in name_lc for token in excluded):
                continue
            candidates.append(item)
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


def resolve_input_path(
    raw_path: str,
    *,
    default_path: str,
    fallback_patterns: List[str],
    exclude_name_tokens: Optional[List[str]] = None,
) -> Tuple[pathlib.Path, bool, str]:
    requested_path = resolve_repo_path(raw_path)
    if requested_path.exists():
        return requested_path, False, str(requested_path)

    is_default = str(raw_path).strip() == str(default_path).strip()
    if is_default and fallback_patterns:
        candidate = latest_file_for_patterns(
            fallback_patterns,
            exclude_name_tokens=exclude_name_tokens,
        )
        if candidate is not None and candidate.exists():
            return candidate, True, str(requested_path)
    return requested_path, False, str(requested_path)


def evaluate(args: argparse.Namespace) -> Dict[str, Any]:
    resolved_pipeline = infer_pipeline_version(args)
    runtime_bundle_json = (
        str(resolve_repo_path(args.runtime_bundle_json))
        if str(args.runtime_bundle_json).strip()
        else str(resolve_repo_path(DEFAULT_RUNTIME_BUNDLE_V2 if resolved_pipeline == "v2" else DEFAULT_RUNTIME_BUNDLE_V1))
    )
    shadow_report_json = str(resolve_repo_path(args.shadow_report_json))
    shadow_validation_json = str(resolve_repo_path(args.shadow_validation_json))
    promotion_output_json = str(resolve_repo_path(args.promotion_output_json))
    output_json = resolve_repo_path(args.output_json)

    resolved_live_log, auto_live_log, requested_live_log = resolve_input_path(
        str(args.live_decision_log_jsonl),
        default_path=DEFAULT_LIVE_DECISION_LOG_JSONL,
        fallback_patterns=[
            r"build/Release/logs/policy_decisions*.jsonl",
        ],
        exclude_name_tokens=["backtest"],
    )
    resolved_backtest_log, auto_backtest_log, requested_backtest_log = resolve_input_path(
        str(args.backtest_decision_log_jsonl),
        default_path=DEFAULT_BACKTEST_DECISION_LOG_JSONL,
        fallback_patterns=[
            r"build/Release/logs/policy_decisions_backtest*.jsonl",
        ],
    )
    resolved_feature_validation, auto_feature_validation, requested_feature_validation = resolve_input_path(
        str(args.feature_validation_json),
        default_path=DEFAULT_FEATURE_VALIDATION_JSON,
        fallback_patterns=[
            r"build/Release/logs/probabilistic_feature_validation_summary_*.json",
            r"build/Release/logs/context_refresh_feature_validation*.json",
        ],
    )
    resolved_parity, auto_parity, requested_parity = resolve_input_path(
        str(args.parity_json),
        default_path=DEFAULT_PARITY_JSON,
        fallback_patterns=[
            r"build/Release/logs/probabilistic_runtime_bundle_parity_*.json",
            r"build/Release/logs/context_refresh_runtime_bundle_parity*.json",
        ],
    )
    resolved_verification, auto_verification, requested_verification = resolve_input_path(
        str(args.verification_json),
        default_path=DEFAULT_VERIFICATION_JSON,
        fallback_patterns=[
            r"build/Release/logs/verification_report_*.json",
            r"build/Release/logs/context_refresh_verification*.json",
        ],
    )
    resolved_runtime_config, auto_runtime_config, requested_runtime_config = resolve_input_path(
        str(args.runtime_config_json),
        default_path=DEFAULT_RUNTIME_CONFIG_JSON,
        fallback_patterns=[
            r"build/Release/config/config.json",
            r"config/config.json",
        ],
    )

    required = {
        "live_decision_log_jsonl": str(resolved_live_log),
        "backtest_decision_log_jsonl": str(resolved_backtest_log),
        "runtime_bundle_json": runtime_bundle_json,
        "feature_validation_json": str(resolved_feature_validation),
        "parity_json": str(resolved_parity),
        "verification_json": str(resolved_verification),
        "runtime_config_json": str(resolved_runtime_config),
    }
    missing = check_required_inputs(required)
    if missing:
        auto_resolution = {
            "live_decision_log_jsonl": {
                "requested_path": requested_live_log,
                "resolved_path": str(resolved_live_log),
                "auto_resolved": bool(auto_live_log),
            },
            "backtest_decision_log_jsonl": {
                "requested_path": requested_backtest_log,
                "resolved_path": str(resolved_backtest_log),
                "auto_resolved": bool(auto_backtest_log),
            },
            "feature_validation_json": {
                "requested_path": requested_feature_validation,
                "resolved_path": str(resolved_feature_validation),
                "auto_resolved": bool(auto_feature_validation),
            },
            "parity_json": {
                "requested_path": requested_parity,
                "resolved_path": str(resolved_parity),
                "auto_resolved": bool(auto_parity),
            },
            "verification_json": {
                "requested_path": requested_verification,
                "resolved_path": str(resolved_verification),
                "auto_resolved": bool(auto_verification),
            },
            "runtime_config_json": {
                "requested_path": requested_runtime_config,
                "resolved_path": str(resolved_runtime_config),
                "auto_resolved": bool(auto_runtime_config),
            },
        }
        out = {
            "generated_at_utc": utc_now_iso(),
            "status": "fail",
            "pipeline_version": resolved_pipeline,
            "target_stage": str(args.target_stage),
            "steps": [],
            "errors": [f"missing_required:{x}" for x in missing],
            "input_resolution": auto_resolution,
            "artifacts": {
                "output_json": str(output_json),
            },
        }
        dump_json(output_json, out)
        return out

    steps: List[Dict[str, Any]] = []
    errors: List[str] = []

    generated = shadow_generate.evaluate(
        argparse.Namespace(
            live_decision_log_jsonl=str(resolved_live_log),
            backtest_decision_log_jsonl=str(resolved_backtest_log),
            runtime_bundle_json=runtime_bundle_json,
            live_runtime_bundle_json="",
            backtest_runtime_bundle_json="",
            pipeline_version=resolved_pipeline,
            output_json=shadow_report_json,
            strict=True,
        )
    )
    generate_ok = str(generated.get("status", "")).strip().lower() == "pass"
    steps.append(
        {
            "name": "generate_shadow_report",
            "ok": bool(generate_ok),
            "status": str(generated.get("status", "")),
            "output_json": shadow_report_json,
            "errors": list(generated.get("errors", []) or []),
        }
    )
    if not generate_ok:
        errors.append("generate_shadow_report_failed")

    validated = shadow_validate.evaluate(
        argparse.Namespace(
            shadow_report_json=shadow_report_json,
            pipeline_version=resolved_pipeline,
            output_json=shadow_validation_json,
            strict=True,
        )
    )
    validate_ok = str(validated.get("status", "")).strip().lower() == "pass"
    steps.append(
        {
            "name": "validate_shadow_report",
            "ok": bool(validate_ok),
            "status": str(validated.get("status", "")),
            "output_json": shadow_validation_json,
            "errors": list(validated.get("errors", []) or []),
        }
    )
    if not validate_ok:
        errors.append("validate_shadow_report_failed")

    promotion = promotion_readiness.evaluate(
        argparse.Namespace(
            feature_validation_json=str(resolved_feature_validation),
            parity_json=str(resolved_parity),
            verification_json=str(resolved_verification),
            shadow_report_json=shadow_report_json,
            runtime_config_json=str(resolved_runtime_config),
            shadow_validation_json=shadow_validation_json,
            target_stage=str(args.target_stage),
            pipeline_version=resolved_pipeline,
            output_json=promotion_output_json,
        )
    )
    promotion_ok = str(promotion.get("status", "")).strip().lower() == "pass"
    steps.append(
        {
            "name": "evaluate_promotion_readiness",
            "ok": bool(promotion_ok),
            "status": str(promotion.get("status", "")),
            "output_json": promotion_output_json,
            "errors": list(promotion.get("errors", []) or []),
        }
    )
    if not promotion_ok:
        errors.append("evaluate_promotion_readiness_failed")

    status = "pass" if len(errors) == 0 else "fail"
    out = {
        "generated_at_utc": utc_now_iso(),
        "status": status,
        "pipeline_version": resolved_pipeline,
        "target_stage": str(args.target_stage),
        "steps": steps,
        "errors": errors,
        "inputs": {
            "live_decision_log_jsonl": str(resolved_live_log),
            "backtest_decision_log_jsonl": str(resolved_backtest_log),
            "runtime_bundle_json": runtime_bundle_json,
            "feature_validation_json": str(resolved_feature_validation),
            "parity_json": str(resolved_parity),
            "verification_json": str(resolved_verification),
            "runtime_config_json": str(resolved_runtime_config),
        },
        "input_resolution": {
            "live_decision_log_jsonl": {
                "requested_path": requested_live_log,
                "resolved_path": str(resolved_live_log),
                "auto_resolved": bool(auto_live_log),
            },
            "backtest_decision_log_jsonl": {
                "requested_path": requested_backtest_log,
                "resolved_path": str(resolved_backtest_log),
                "auto_resolved": bool(auto_backtest_log),
            },
            "feature_validation_json": {
                "requested_path": requested_feature_validation,
                "resolved_path": str(resolved_feature_validation),
                "auto_resolved": bool(auto_feature_validation),
            },
            "parity_json": {
                "requested_path": requested_parity,
                "resolved_path": str(resolved_parity),
                "auto_resolved": bool(auto_parity),
            },
            "verification_json": {
                "requested_path": requested_verification,
                "resolved_path": str(resolved_verification),
                "auto_resolved": bool(auto_verification),
            },
            "runtime_config_json": {
                "requested_path": requested_runtime_config,
                "resolved_path": str(resolved_runtime_config),
                "auto_resolved": bool(auto_runtime_config),
            },
        },
        "artifacts": {
            "shadow_report_json": shadow_report_json,
            "shadow_validation_json": shadow_validation_json,
            "promotion_output_json": promotion_output_json,
            "output_json": str(output_json),
        },
    }
    dump_json(output_json, out)
    return out


def main(argv=None) -> int:
    args = parse_args(argv)
    out = evaluate(args)
    print("[ProbabilisticShadowGateFlow] completed", flush=True)
    print(f"status={out.get('status', 'fail')}", flush=True)
    print(f"pipeline_version={out.get('pipeline_version', '')}", flush=True)
    print(f"target_stage={out.get('target_stage', '')}", flush=True)
    print(f"steps={len(out.get('steps', []) or [])}", flush=True)
    print(f"errors={len(out.get('errors', []) or [])}", flush=True)
    print(f"output={out.get('artifacts', {}).get('output_json', '')}", flush=True)
    return 0 if str(out.get("status", "fail")).lower() == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
