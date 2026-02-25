#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import sys
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

from _script_common import dump_json, ensure_parent_directory, load_json_or_none, resolve_repo_path

DEFAULT_FEATURE_DIR = r".\data\model_input\probabilistic_features_v2_draft_latest"
DEFAULT_FEATURE_CONTRACT = r".\config\model\probabilistic_feature_contract_v2.json"
DEFAULT_RUNTIME_BUNDLE = r".\config\model\probabilistic_runtime_bundle_v2.json"


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run minimal mandatory checks after Codex context refresh. "
            "Checks are selected by touched areas (feature/model/runtime)."
        )
    )
    parser.add_argument("--python-exe", default=sys.executable)
    parser.add_argument(
        "--touched-areas",
        default="feature,model,runtime",
        help="Comma-separated areas: feature,model,runtime",
    )
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("auto", "v2"),
        default="auto",
    )
    parser.add_argument("--skip-missing", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\codex_context_refresh_checks.json",
    )

    parser.add_argument("--feature-dataset-manifest-json", default="")
    parser.add_argument("--feature-contract-json", default="")
    parser.add_argument(
        "--feature-output-json",
        default=r".\build\Release\logs\context_refresh_feature_validation.json",
    )

    parser.add_argument("--runtime-bundle-json", default="")
    parser.add_argument("--train-summary-json", default="")
    parser.add_argument("--split-manifest-json", default="")
    parser.add_argument(
        "--parity-output-json",
        default=r".\build\Release\logs\context_refresh_runtime_bundle_parity.json",
    )

    parser.add_argument("--verification-exe-path", default=r".\build\Release\AutoLifeTrading.exe")
    parser.add_argument("--verification-config-path", default=r".\build\Release\config\config.json")
    parser.add_argument("--verification-source-config-path", default=r".\config\config.json")
    parser.add_argument("--verification-data-dir", default=r".\data\backtest_real")
    parser.add_argument(
        "--verification-datasets",
        default="",
        help="Comma-separated dataset file names or paths.",
    )
    parser.add_argument(
        "--verification-baseline-report-path",
        default=r".\build\Release\logs\verification_report_baseline_current.json",
    )
    parser.add_argument(
        "--verification-output-json",
        default=r".\build\Release\logs\context_refresh_verification.json",
    )
    return parser.parse_args(argv)


def parse_csv_tokens(raw: str) -> List[str]:
    out: List[str] = []
    seen = set()
    for token in str(raw or "").split(","):
        value = token.strip()
        if not value or value in seen:
            continue
        seen.add(value)
        out.append(value)
    return out


def parse_touched_areas(raw: str) -> List[str]:
    valid = {"feature", "model", "runtime"}
    out: List[str] = []
    for token in parse_csv_tokens(raw):
        lowered = token.strip().lower()
        if lowered in valid and lowered not in out:
            out.append(lowered)
    return out


def infer_pipeline_version(args: argparse.Namespace) -> str:
    requested = str(args.pipeline_version).strip().lower()
    if requested == "v2":
        return requested
    if requested not in ("", "auto"):
        raise RuntimeError(f"Unsupported pipeline version: {requested}")
    bundle_path_raw = str(args.runtime_bundle_json).strip()
    if bundle_path_raw:
        bundle_path = resolve_repo_path(bundle_path_raw)
        if bundle_path.exists():
            try:
                import json
                payload = json.loads(bundle_path.read_text(encoding="utf-8-sig"))
                if isinstance(payload, dict):
                    version = str(payload.get("version", "")).strip().lower()
                    pipeline = str(payload.get("pipeline_version", "")).strip().lower()
                    if pipeline == "v2":
                        return pipeline
                    if "v2" in version:
                        return "v2"
            except Exception:
                pass
    return "v2"


def latest_file(glob_pattern: str) -> Optional[pathlib.Path]:
    root = resolve_repo_path(r".")
    items = [x for x in root.glob(glob_pattern) if x.is_file()]
    if not items:
        return None
    return max(items, key=lambda p: p.stat().st_mtime)


def resolve_feature_inputs(args: argparse.Namespace, pipeline_version: str) -> Dict[str, pathlib.Path]:
    _ = pipeline_version
    feature_dir = resolve_repo_path(DEFAULT_FEATURE_DIR)
    manifest = (
        resolve_repo_path(args.feature_dataset_manifest_json)
        if str(args.feature_dataset_manifest_json).strip()
        else (feature_dir / "feature_dataset_manifest.json")
    )
    contract = (
        resolve_repo_path(args.feature_contract_json)
        if str(args.feature_contract_json).strip()
        else resolve_repo_path(DEFAULT_FEATURE_CONTRACT)
    )
    output = resolve_repo_path(args.feature_output_json)
    return {"manifest": manifest, "contract": contract, "output": output}


def resolve_parity_inputs(args: argparse.Namespace, pipeline_version: str) -> Dict[str, pathlib.Path]:
    _ = pipeline_version
    feature_dir = resolve_repo_path(DEFAULT_FEATURE_DIR)
    runtime_bundle = (
        resolve_repo_path(args.runtime_bundle_json)
        if str(args.runtime_bundle_json).strip()
        else resolve_repo_path(DEFAULT_RUNTIME_BUNDLE)
    )
    train_summary = (
        resolve_repo_path(args.train_summary_json)
        if str(args.train_summary_json).strip()
        else (
            latest_file(r"build/Release/logs/probabilistic_model_train_summary_global_*.json")
            or resolve_repo_path(r".\build\Release\logs\probabilistic_model_train_summary_global_latest.json")
        )
    )
    split_manifest = (
        resolve_repo_path(args.split_manifest_json)
        if str(args.split_manifest_json).strip()
        else (feature_dir / "probabilistic_split_manifest_v2_draft.json")
    )
    output = resolve_repo_path(args.parity_output_json)
    return {
        "runtime_bundle": runtime_bundle,
        "train_summary": train_summary,
        "split_manifest": split_manifest,
        "output": output,
    }


def resolve_verification_inputs(args: argparse.Namespace) -> Dict[str, Any]:
    datasets = parse_csv_tokens(str(args.verification_datasets))
    if not datasets:
        data_dir = resolve_repo_path(args.verification_data_dir)
        if data_dir.exists():
            discovered = sorted([x.name for x in data_dir.glob("*.csv")])
            datasets = discovered[:2]
    return {
        "exe_path": resolve_repo_path(args.verification_exe_path),
        "config_path": resolve_repo_path(args.verification_config_path),
        "source_config_path": resolve_repo_path(args.verification_source_config_path),
        "data_dir": resolve_repo_path(args.verification_data_dir),
        "datasets": datasets,
        "baseline_report_path": resolve_repo_path(args.verification_baseline_report_path),
        "output": resolve_repo_path(args.verification_output_json),
    }


def check_required_files(
    required: List[Tuple[str, pathlib.Path]],
    *,
    skip_missing: bool,
) -> Tuple[bool, List[str]]:
    missing: List[str] = []
    for key, path_value in required:
        if not path_value.exists():
            missing.append(f"{key}:{path_value}")
    if missing and not skip_missing:
        return False, missing
    return True, missing


def run_step(step_name: str, cmd: List[str], dry_run: bool) -> Dict[str, Any]:
    started = utc_now_iso()
    if dry_run:
        return {
            "name": step_name,
            "started_at_utc": started,
            "finished_at_utc": started,
            "ok": True,
            "returncode": 0,
            "dry_run": True,
            "cmd": cmd,
        }
    proc = subprocess.run(cmd)
    ended = utc_now_iso()
    return {
        "name": step_name,
        "started_at_utc": started,
        "finished_at_utc": ended,
        "ok": int(proc.returncode) == 0,
        "returncode": int(proc.returncode),
        "dry_run": False,
        "cmd": cmd,
    }


def to_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    return str(value).strip().lower() in ("1", "true", "yes", "y", "on")


def evaluate_gate_output(step_name: str, output_json_path: pathlib.Path) -> Tuple[bool, str]:
    payload = load_json_or_none(output_json_path)
    if not isinstance(payload, dict):
        return False, "missing_or_invalid_output_json"

    if step_name == "validate_features":
        status = str(payload.get("status", "")).strip().lower()
        return (status == "pass"), f"feature_validation_status={status or 'missing'}"

    if step_name == "validate_bundle_parity":
        status = str(payload.get("status", "")).strip().lower()
        return (status == "pass"), f"parity_status={status or 'missing'}"

    if step_name == "run_verification":
        overall = to_bool(payload.get("overall_gate_pass", False))
        return overall, f"verification_overall_gate_pass={overall}"

    return False, "unknown_step_gate_rule"


def evaluate(args: argparse.Namespace) -> Dict[str, Any]:
    touched = parse_touched_areas(args.touched_areas)
    pipeline_version = infer_pipeline_version(args)
    output_path = resolve_repo_path(args.output_json)

    errors: List[str] = []
    warnings: List[str] = []
    steps: List[Dict[str, Any]] = []

    py = str(args.python_exe)
    repo_scripts = resolve_repo_path(r".\scripts")

    if "feature" in touched:
        feature = resolve_feature_inputs(args, pipeline_version)
        ok_required, missing = check_required_files(
            [
                ("feature_dataset_manifest_json", feature["manifest"]),
                ("feature_contract_json", feature["contract"]),
            ],
            skip_missing=bool(args.skip_missing),
        )
        if not ok_required and missing:
            errors.extend([f"missing_required:{x}" for x in missing])
        elif missing:
            warnings.extend([f"skipped_feature_validation_missing:{x}" for x in missing])
            steps.append(
                {
                    "name": "validate_features",
                    "ok": True,
                    "returncode": 0,
                    "skipped": True,
                    "missing": missing,
                }
            )
        else:
            ensure_parent_directory(feature["output"])
            cmd = [
                py,
                str(repo_scripts / "validate_probabilistic_feature_dataset.py"),
                "--dataset-manifest-json",
                str(feature["manifest"]),
                "--contract-json",
                str(feature["contract"]),
                "--pipeline-version",
                str(pipeline_version),
                "--output-json",
                str(feature["output"]),
                "--strict",
            ]
            step = run_step("validate_features", cmd, bool(args.dry_run))
            if step.get("ok", False) and not bool(step.get("dry_run", False)):
                gate_ok, gate_note = evaluate_gate_output("validate_features", feature["output"])
                step["gate_output_ok"] = bool(gate_ok)
                step["gate_output_note"] = str(gate_note)
                if not gate_ok:
                    step["ok"] = False
                    step["returncode"] = int(step.get("returncode", 0) or 2)
                    errors.append("gate_output_failed:validate_features")
            steps.append(step)

    if "model" in touched:
        parity = resolve_parity_inputs(args, pipeline_version)
        ok_required, missing = check_required_files(
            [
                ("runtime_bundle_json", parity["runtime_bundle"]),
                ("train_summary_json", parity["train_summary"]),
                ("split_manifest_json", parity["split_manifest"]),
            ],
            skip_missing=bool(args.skip_missing),
        )
        if not ok_required and missing:
            errors.extend([f"missing_required:{x}" for x in missing])
        elif missing:
            warnings.extend([f"skipped_parity_missing:{x}" for x in missing])
            steps.append(
                {
                    "name": "validate_bundle_parity",
                    "ok": True,
                    "returncode": 0,
                    "skipped": True,
                    "missing": missing,
                }
            )
        else:
            ensure_parent_directory(parity["output"])
            cmd = [
                py,
                str(repo_scripts / "validate_runtime_bundle_parity.py"),
                "--runtime-bundle-json",
                str(parity["runtime_bundle"]),
                "--train-summary-json",
                str(parity["train_summary"]),
                "--split-manifest-json",
                str(parity["split_manifest"]),
                "--output-json",
                str(parity["output"]),
            ]
            step = run_step("validate_bundle_parity", cmd, bool(args.dry_run))
            if step.get("ok", False) and not bool(step.get("dry_run", False)):
                gate_ok, gate_note = evaluate_gate_output("validate_bundle_parity", parity["output"])
                step["gate_output_ok"] = bool(gate_ok)
                step["gate_output_note"] = str(gate_note)
                if not gate_ok:
                    step["ok"] = False
                    step["returncode"] = int(step.get("returncode", 0) or 2)
                    errors.append("gate_output_failed:validate_bundle_parity")
            steps.append(step)

    if "runtime" in touched:
        verification = resolve_verification_inputs(args)
        datasets = list(verification["datasets"])
        required = [
            ("verification_exe_path", verification["exe_path"]),
            ("verification_config_path", verification["config_path"]),
            ("verification_source_config_path", verification["source_config_path"]),
            ("verification_data_dir", verification["data_dir"]),
        ]
        ok_required, missing = check_required_files(required, skip_missing=bool(args.skip_missing))
        if len(datasets) == 0:
            if bool(args.skip_missing):
                warnings.append("skipped_verification_missing:verification_datasets")
                steps.append(
                    {
                        "name": "run_verification",
                        "ok": True,
                        "returncode": 0,
                        "skipped": True,
                        "missing": ["verification_datasets"],
                    }
                )
            else:
                errors.append("missing_required:verification_datasets")
        elif not ok_required and missing:
            errors.extend([f"missing_required:{x}" for x in missing])
        elif missing:
            warnings.extend([f"skipped_verification_missing:{x}" for x in missing])
            steps.append(
                {
                    "name": "run_verification",
                    "ok": True,
                    "returncode": 0,
                    "skipped": True,
                    "missing": missing,
                }
            )
        else:
            ensure_parent_directory(verification["output"])
            cmd = [
                py,
                str(repo_scripts / "run_verification.py"),
                "--exe-path",
                str(verification["exe_path"]),
                "--config-path",
                str(verification["config_path"]),
                "--source-config-path",
                str(verification["source_config_path"]),
                "--data-dir",
                str(verification["data_dir"]),
                "--output-json",
                str(verification["output"]),
                "--baseline-report-path",
                str(verification["baseline_report_path"]),
                "--pipeline-version",
                str(pipeline_version),
                "--dataset-names",
                *datasets,
            ]
            step = run_step("run_verification", cmd, bool(args.dry_run))
            if step.get("ok", False) and not bool(step.get("dry_run", False)):
                gate_ok, gate_note = evaluate_gate_output("run_verification", verification["output"])
                step["gate_output_ok"] = bool(gate_ok)
                step["gate_output_note"] = str(gate_note)
                if not gate_ok:
                    step["ok"] = False
                    step["returncode"] = int(step.get("returncode", 0) or 2)
                    errors.append("gate_output_failed:run_verification")
            steps.append(step)

    step_failures = [x for x in steps if not bool(x.get("ok", False))]
    status = "pass" if not errors and not step_failures else "fail"
    out = {
        "generated_at_utc": utc_now_iso(),
        "status": status,
        "pipeline_version": pipeline_version,
        "dry_run": bool(args.dry_run),
        "skip_missing": bool(args.skip_missing),
        "touched_areas": touched,
        "errors": errors,
        "warnings": warnings,
        "steps": steps,
        "inputs": {
            "touched_areas": str(args.touched_areas),
            "pipeline_version_requested": str(args.pipeline_version),
            "output_json": str(output_path),
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
    print("[CodexContextRefreshChecks] completed", flush=True)
    print(f"status={out.get('status', 'fail')}", flush=True)
    print(f"pipeline_version={out.get('pipeline_version', 'v2')}", flush=True)
    print(f"steps={len(out.get('steps', []) or [])}", flush=True)
    print(f"errors={len(out.get('errors', []) or [])}", flush=True)
    print(f"output={out.get('artifacts', {}).get('output_json', '')}", flush=True)
    return 0 if str(out.get("status", "fail")).lower() == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
