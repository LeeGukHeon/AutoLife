#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import sys
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

from _script_common import dump_json, ensure_parent_directory, resolve_repo_path

DEFAULT_FEATURE_DIR = r".\data\model_input\probabilistic_features_v1_latest"
DEFAULT_FEATURE_DIR_V2_DRAFT = r".\data\model_input\probabilistic_features_v2_draft_latest"
DEFAULT_RUNTIME_BUNDLE_JSON = r".\config\model\probabilistic_runtime_bundle_v1.json"
DEFAULT_RUNTIME_BUNDLE_JSON_V2_DRAFT = r".\config\model\probabilistic_runtime_bundle_v2.json"
DEFAULT_FEATURE_CONTRACT_JSON = r".\config\model\probabilistic_feature_contract_v1.json"
DEFAULT_FEATURE_CONTRACT_JSON_V2_DRAFT = r".\config\model\probabilistic_feature_contract_v2.json"


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Hybrid probabilistic refresh cycle: incremental fetch + deterministic rebuild + "
            "global retrain + runtime bundle export."
        )
    )
    parser.add_argument("--run-tag", default="")
    parser.add_argument("--python-exe", default=sys.executable)

    parser.add_argument("--markets-major", default="KRW-BTC,KRW-ETH,KRW-XRP,KRW-SOL,KRW-DOGE")
    parser.add_argument("--markets-alt", default="KRW-ETC,KRW-BCH,KRW-XLM,KRW-QTUM")
    parser.add_argument("--timeframes", default="1,5,15,60,240")
    parser.add_argument("--history-start-utc", default="2017-10-24T00:00:00Z")
    parser.add_argument("--history-end-utc", default="")
    parser.add_argument("--max-jobs", type=int, default=0)

    parser.add_argument("--incremental-update", dest="incremental_update", action="store_true", default=True)
    parser.add_argument("--full-refresh", dest="incremental_update", action="store_false")
    parser.add_argument("--incremental-overlap-bars", type=int, default=3)

    parser.add_argument("--min-free-gb", type=float, default=40.0)
    parser.add_argument("--max-output-gb", type=float, default=180.0)
    parser.add_argument("--disk-budget-policy", choices=("halt", "skip"), default="halt")

    parser.add_argument("--sleep-ms-between-jobs", type=int, default=350)
    parser.add_argument("--sleep-ms-per-request", type=int, default=120)

    parser.add_argument("--backtest-dir", default=r".\data\backtest_probabilistic")
    parser.add_argument("--feature-dir", default=DEFAULT_FEATURE_DIR)
    parser.add_argument("--runtime-bundle-json", default=DEFAULT_RUNTIME_BUNDLE_JSON)
    parser.add_argument("--train-max-datasets", type=int, default=0)
    parser.add_argument(
        "--universe-file",
        default="",
        help="Optional dynamic universe JSON for scope-aware 1m fetch/build behavior.",
    )
    parser.add_argument(
        "--enable-purged-walk-forward",
        action="store_true",
        help="Enable EXT-51 purge/embargo split mode in split manifest generation.",
    )
    parser.add_argument("--h1-bars", type=int, default=1)
    parser.add_argument("--h5-bars", type=int, default=5)
    parser.add_argument("--purge-bars", type=int, default=-1)
    parser.add_argument("--embargo-bars", type=int, default=-1)
    parser.add_argument("--enable-conditional-cost-model", action="store_true")
    parser.add_argument("--cost-fee-floor-bps", type=float, default=6.0)
    parser.add_argument("--cost-volatility-weight", type=float, default=3.0)
    parser.add_argument("--cost-range-weight", type=float, default=1.5)
    parser.add_argument("--cost-liquidity-weight", type=float, default=2.5)
    parser.add_argument("--cost-volatility-norm-bps", type=float, default=50.0)
    parser.add_argument("--cost-range-norm-bps", type=float, default=80.0)
    parser.add_argument("--cost-liquidity-ref-ratio", type=float, default=1.0)
    parser.add_argument("--cost-liquidity-penalty-cap", type=float, default=8.0)
    parser.add_argument("--cost-cap-bps", type=float, default=200.0)
    parser.add_argument("--sample-mode", "--sample_mode", choices=("time", "dollar", "volatility"), default="time")
    parser.add_argument("--sample-threshold", "--sample_threshold", type=float, default=0.0)
    parser.add_argument("--sample-lookback-minutes", "--sample_lookback_minutes", type=int, default=60)
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("v1", "v2"),
        default="v1",
        help="MODE switch for build/train/export path. default=v1 baseline.",
    )
    parser.add_argument("--ensemble-k", type=int, default=1)
    parser.add_argument("--ensemble-seed-step", type=int, default=1000)
    parser.add_argument("--run-verification", action="store_true")
    parser.add_argument("--verification-exe-path", default=r".\build\Release\AutoLifeTrading.exe")
    parser.add_argument("--verification-config-path", default=r".\build\Release\config\config.json")
    parser.add_argument("--verification-source-config-path", default=r".\config\config.json")
    parser.add_argument("--verification-data-dir", default=r".\data\backtest_real")
    parser.add_argument(
        "--verification-datasets",
        default="",
        help="Comma-separated dataset file names or paths for run_verification.py (required with --run-verification).",
    )
    parser.add_argument(
        "--verification-baseline-report-path",
        default=r".\build\Release\logs\verification_report_baseline_current.json",
    )
    parser.add_argument("--evaluate-promotion-readiness", action="store_true")
    parser.add_argument(
        "--promotion-target-stage",
        choices=("prelive", "live_enable"),
        default="prelive",
    )
    parser.add_argument(
        "--promotion-shadow-report-json",
        default="",
        help="Required for promotion target live_enable.",
    )
    parser.add_argument(
        "--generate-shadow-report",
        action="store_true",
        help="Generate shadow report from live/backtest decision logs before validation/readiness.",
    )
    parser.add_argument(
        "--shadow-report-output-json",
        default="",
        help="Optional explicit output path for generated shadow report.",
    )
    parser.add_argument(
        "--shadow-live-decision-log-jsonl",
        default=r".\build\Release\logs\policy_decisions.jsonl",
    )
    parser.add_argument(
        "--shadow-backtest-decision-log-jsonl",
        default=r".\build\Release\logs\policy_decisions_backtest.jsonl",
    )
    parser.add_argument(
        "--shadow-runtime-bundle-json",
        default="",
        help="Optional runtime bundle path for shadow report generation. Empty uses cycle runtime bundle.",
    )
    parser.add_argument(
        "--validate-shadow-report",
        action="store_true",
        help="Validate shadow report with strict schema/parity evidence before promotion readiness.",
    )
    parser.add_argument(
        "--shadow-validation-output-json",
        default="",
        help="Optional output path for shadow report validation summary.",
    )
    parser.add_argument(
        "--promotion-runtime-config-json",
        default=r".\build\Release\config\config.json",
    )
    parser.add_argument(
        "--promotion-output-json",
        default="",
        help="Optional explicit output path. Empty -> run-tag based log path.",
    )
    return parser.parse_args(argv)


def normalize_markets(major_raw: str, alt_raw: str) -> str:
    ordered: List[str] = []
    seen = set()
    for token in (str(major_raw or "") + "," + str(alt_raw or "")).split(","):
        market = token.strip().upper()
        if not market or market in seen:
            continue
        seen.add(market)
        ordered.append(market)
    return ",".join(ordered)


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


def run_step(name: str, cmd: List[str]) -> Dict[str, Any]:
    started = utc_now_iso()
    print(f"[HybridCycle] step={name} started={started}", flush=True)
    print(f"[HybridCycle] cmd={' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd)
    ended = utc_now_iso()
    result = {
        "name": name,
        "started_at_utc": started,
        "finished_at_utc": ended,
        "returncode": int(proc.returncode),
        "ok": bool(int(proc.returncode) == 0),
        "cmd": cmd,
    }
    print(f"[HybridCycle] step={name} returncode={proc.returncode}", flush=True)
    return result


def main(argv=None) -> int:
    args = parse_args(argv)
    if str(args.sample_mode) in ("dollar", "volatility") and float(args.sample_threshold) <= 0.0:
        raise ValueError("--sample-threshold must be > 0 when --sample-mode is dollar or volatility")
    if (bool(args.generate_shadow_report) or bool(args.validate_shadow_report)) and not bool(args.evaluate_promotion_readiness):
        raise ValueError("--generate-shadow-report/--validate-shadow-report require --evaluate-promotion-readiness")
    if bool(args.evaluate_promotion_readiness) and not bool(args.run_verification):
        raise ValueError("--evaluate-promotion-readiness requires --run-verification")
    if (
        str(args.promotion_target_stage).strip().lower() == "live_enable" and
        not str(args.promotion_shadow_report_json).strip() and
        not bool(args.generate_shadow_report)
    ):
        raise ValueError(
            "--promotion-shadow-report-json is required when --promotion-target-stage live_enable "
            "(or enable --generate-shadow-report)"
        )
    pipeline_version = str(args.pipeline_version).strip().lower()
    run_tag = str(args.run_tag).strip() or datetime.now().strftime("%Y%m%d_%H%M%S")
    py = str(args.python_exe)
    selected_markets = normalize_markets(args.markets_major, args.markets_alt)

    log_dir = resolve_repo_path(r".\build\Release\logs")
    model_root = resolve_repo_path(r".\build\Release\models")
    log_dir.mkdir(parents=True, exist_ok=True)
    model_root.mkdir(parents=True, exist_ok=True)

    backtest_dir = resolve_repo_path(args.backtest_dir)
    feature_dir = resolve_repo_path(args.feature_dir)
    runtime_bundle_json = resolve_repo_path(args.runtime_bundle_json)
    if pipeline_version == "v2":
        if feature_dir == resolve_repo_path(DEFAULT_FEATURE_DIR):
            feature_dir = resolve_repo_path(DEFAULT_FEATURE_DIR_V2_DRAFT)
        if runtime_bundle_json == resolve_repo_path(DEFAULT_RUNTIME_BUNDLE_JSON):
            runtime_bundle_json = resolve_repo_path(DEFAULT_RUNTIME_BUNDLE_JSON_V2_DRAFT)
    universe_file = resolve_repo_path(args.universe_file) if str(args.universe_file).strip() else None
    if universe_file is not None and not universe_file.exists():
        raise FileNotFoundError(f"universe file not found: {universe_file}")
    backtest_dir.mkdir(parents=True, exist_ok=True)
    feature_dir.mkdir(parents=True, exist_ok=True)
    ensure_parent_directory(runtime_bundle_json)

    fetch_manifest_json = backtest_dir / "probabilistic_bundle_manifest.json"
    fetch_summary_json = log_dir / f"probabilistic_bundle_summary_{run_tag}.json"
    feature_manifest_json = feature_dir / "feature_dataset_manifest.json"
    feature_build_summary_json = log_dir / f"probabilistic_feature_build_summary_{run_tag}.json"
    feature_validation_json = log_dir / f"probabilistic_feature_validation_summary_{run_tag}.json"
    split_manifest_json = feature_dir / (
        "probabilistic_split_manifest_v1.json"
        if pipeline_version == "v1"
        else "probabilistic_split_manifest_v2_draft.json"
    )
    baseline_json = log_dir / f"probabilistic_baseline_summary_{run_tag}.json"
    train_summary_json = log_dir / f"probabilistic_model_train_summary_global_{run_tag}.json"
    train_model_dir = model_root / f"probabilistic_pattern_global_{run_tag}"
    parity_json = log_dir / f"probabilistic_runtime_bundle_parity_{run_tag}.json"
    cycle_summary_json = log_dir / f"probabilistic_hybrid_cycle_summary_{run_tag}.json"

    steps: List[Dict[str, Any]] = []

    fetch_cmd = [
        py,
        str(resolve_repo_path(r".\scripts\fetch_probabilistic_training_bundle.py")),
        "--markets-major",
        str(args.markets_major),
        "--markets-alt",
        str(args.markets_alt),
        "--timeframes",
        str(args.timeframes),
        "--history-start-utc",
        str(args.history_start_utc),
        "--output-dir",
        str(backtest_dir),
        "--manifest-json",
        str(fetch_manifest_json),
        "--summary-json",
        str(fetch_summary_json),
        "--sleep-ms-between-jobs",
        str(int(args.sleep_ms_between_jobs)),
        "--sleep-ms-per-request",
        str(int(args.sleep_ms_per_request)),
        "--min-free-gb",
        str(float(args.min_free_gb)),
        "--max-output-gb",
        str(float(args.max_output_gb)),
        "--disk-budget-policy",
        str(args.disk_budget_policy),
        "--incremental-overlap-bars",
        str(int(args.incremental_overlap_bars)),
    ]
    if str(args.history_end_utc).strip():
        fetch_cmd.extend(["--history-end-utc", str(args.history_end_utc).strip()])
    if bool(args.incremental_update):
        fetch_cmd.append("--incremental-update")
    if int(args.max_jobs) > 0:
        fetch_cmd.extend(["--max-jobs", str(int(args.max_jobs))])
    if universe_file is not None:
        fetch_cmd.extend(["--universe-file", str(universe_file)])
    steps.append(run_step("fetch_bundle", fetch_cmd))
    if not steps[-1]["ok"]:
        dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
        return int(steps[-1]["returncode"] or 2)

    build_cmd = [
        py,
        str(resolve_repo_path(r".\scripts\build_probabilistic_feature_dataset.py")),
        "--input-dir",
        str(backtest_dir),
        "--output-dir",
        str(feature_dir),
        "--summary-json",
        str(feature_build_summary_json),
        "--manifest-json",
        str(feature_manifest_json),
        "--markets",
        str(selected_markets),
        "--sample-mode",
        str(args.sample_mode),
        "--sample-threshold",
        str(float(args.sample_threshold)),
        "--sample-lookback-minutes",
        str(int(args.sample_lookback_minutes)),
        "--pipeline-version",
        str(pipeline_version),
    ]
    if universe_file is not None:
        build_cmd.extend(["--universe-file", str(universe_file)])
    if bool(args.enable_conditional_cost_model):
        build_cmd.extend(
            [
                "--enable-conditional-cost-model",
                "--cost-fee-floor-bps",
                str(float(args.cost_fee_floor_bps)),
                "--cost-volatility-weight",
                str(float(args.cost_volatility_weight)),
                "--cost-range-weight",
                str(float(args.cost_range_weight)),
                "--cost-liquidity-weight",
                str(float(args.cost_liquidity_weight)),
                "--cost-volatility-norm-bps",
                str(float(args.cost_volatility_norm_bps)),
                "--cost-range-norm-bps",
                str(float(args.cost_range_norm_bps)),
                "--cost-liquidity-ref-ratio",
                str(float(args.cost_liquidity_ref_ratio)),
                "--cost-liquidity-penalty-cap",
                str(float(args.cost_liquidity_penalty_cap)),
                "--cost-cap-bps",
                str(float(args.cost_cap_bps)),
            ]
        )
    steps.append(run_step("build_features", build_cmd))
    if not steps[-1]["ok"]:
        dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
        return int(steps[-1]["returncode"] or 2)

    validate_cmd = [
        py,
        str(resolve_repo_path(r".\scripts\validate_probabilistic_feature_dataset.py")),
        "--dataset-manifest-json",
        str(feature_manifest_json),
        "--contract-json",
        str(
            resolve_repo_path(
                DEFAULT_FEATURE_CONTRACT_JSON
                if pipeline_version == "v1"
                else DEFAULT_FEATURE_CONTRACT_JSON_V2_DRAFT
            )
        ),
        "--pipeline-version",
        str(pipeline_version),
        "--output-json",
        str(feature_validation_json),
        "--strict",
    ]
    steps.append(run_step("validate_features", validate_cmd))
    if not steps[-1]["ok"]:
        dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
        return int(steps[-1]["returncode"] or 2)

    split_cmd = [
        py,
        str(resolve_repo_path(r".\scripts\generate_probabilistic_split_manifest.py")),
        "--input-manifest-json",
        str(feature_manifest_json),
        "--output-split-manifest-json",
        str(split_manifest_json),
        "--dataset-kind",
        "feature",
    ]
    if bool(args.enable_purged_walk_forward):
        split_cmd.extend(
            [
                "--enable-purged-walk-forward",
                "--h1-bars",
                str(int(args.h1_bars)),
                "--h5-bars",
                str(int(args.h5_bars)),
                "--purge-bars",
                str(int(args.purge_bars)),
                "--embargo-bars",
                str(int(args.embargo_bars)),
            ]
        )
    steps.append(run_step("generate_split_manifest", split_cmd))
    if not steps[-1]["ok"]:
        dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
        return int(steps[-1]["returncode"] or 2)

    baseline_cmd = [
        py,
        str(resolve_repo_path(r".\scripts\generate_probabilistic_baseline.py")),
        "--split-manifest-json",
        str(split_manifest_json),
        "--output-json",
        str(baseline_json),
    ]
    steps.append(run_step("generate_baseline", baseline_cmd))
    if not steps[-1]["ok"]:
        dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
        return int(steps[-1]["returncode"] or 2)

    train_cmd = [
        py,
        str(resolve_repo_path(r".\scripts\train_probabilistic_pattern_model_global.py")),
        "--split-manifest-json",
        str(split_manifest_json),
        "--baseline-json",
        str(baseline_json),
        "--output-json",
        str(train_summary_json),
        "--model-dir",
        str(train_model_dir),
        "--pipeline-version",
        str(pipeline_version),
    ]
    if int(args.ensemble_k) > 1:
        train_cmd.extend(["--ensemble-k", str(int(args.ensemble_k))])
        train_cmd.extend(["--ensemble-seed-step", str(int(args.ensemble_seed_step))])
    if int(args.train_max_datasets) > 0:
        train_cmd.extend(["--max-datasets", str(int(args.train_max_datasets))])
    steps.append(run_step("train_global_model", train_cmd))
    if not steps[-1]["ok"]:
        dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
        return int(steps[-1]["returncode"] or 2)

    export_cmd = [
        py,
        str(resolve_repo_path(r".\scripts\export_probabilistic_runtime_bundle.py")),
        "--train-summary-json",
        str(train_summary_json),
        "--output-json",
        str(runtime_bundle_json),
        "--export-mode",
        "global_only",
        "--pipeline-version",
        str(pipeline_version),
    ]
    steps.append(run_step("export_runtime_bundle", export_cmd))
    if not steps[-1]["ok"]:
        dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
        return int(steps[-1]["returncode"] or 2)

    parity_cmd = [
        py,
        str(resolve_repo_path(r".\scripts\validate_runtime_bundle_parity.py")),
        "--runtime-bundle-json",
        str(runtime_bundle_json),
        "--train-summary-json",
        str(train_summary_json),
        "--split-manifest-json",
        str(split_manifest_json),
        "--output-json",
        str(parity_json),
    ]
    steps.append(run_step("validate_bundle_parity", parity_cmd))
    if not steps[-1]["ok"]:
        dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
        return int(steps[-1]["returncode"] or 2)

    verification_report_json = log_dir / f"verification_report_{run_tag}.json"
    if bool(args.run_verification):
        verification_datasets = parse_csv_tokens(str(args.verification_datasets))
        if not verification_datasets:
            raise ValueError("--verification-datasets is required when --run-verification is enabled")
        verify_cmd = [
            py,
            str(resolve_repo_path(r".\scripts\run_verification.py")),
            "--exe-path",
            str(resolve_repo_path(args.verification_exe_path)),
            "--config-path",
            str(resolve_repo_path(args.verification_config_path)),
            "--source-config-path",
            str(resolve_repo_path(args.verification_source_config_path)),
            "--data-dir",
            str(resolve_repo_path(args.verification_data_dir)),
            "--output-json",
            str(verification_report_json),
            "--baseline-report-path",
            str(resolve_repo_path(args.verification_baseline_report_path)),
            "--pipeline-version",
            str(pipeline_version),
            "--dataset-names",
            *verification_datasets,
        ]
        steps.append(run_step("run_verification", verify_cmd))
        if not steps[-1]["ok"]:
            dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
            return int(steps[-1]["returncode"] or 2)

    promotion_readiness_json = (
        resolve_repo_path(args.promotion_output_json)
        if str(args.promotion_output_json).strip()
        else (log_dir / f"probabilistic_promotion_readiness_{run_tag}.json")
    )
    shadow_report_json: Optional[pathlib.Path] = (
        resolve_repo_path(args.promotion_shadow_report_json)
        if str(args.promotion_shadow_report_json).strip()
        else None
    )
    shadow_report_generated = False
    shadow_validation_json = pathlib.Path("")
    shadow_validation_enabled = False
    if bool(args.evaluate_promotion_readiness):
        should_generate_shadow_report = bool(args.generate_shadow_report)
        if str(args.promotion_target_stage).strip().lower() == "live_enable" and pipeline_version == "v2":
            should_generate_shadow_report = True
        if should_generate_shadow_report:
            if shadow_report_json is None:
                shadow_report_json = (
                    resolve_repo_path(args.shadow_report_output_json)
                    if str(args.shadow_report_output_json).strip()
                    else (log_dir / f"probabilistic_shadow_report_{run_tag}.json")
                )
            shadow_bundle_json = (
                resolve_repo_path(args.shadow_runtime_bundle_json)
                if str(args.shadow_runtime_bundle_json).strip()
                else runtime_bundle_json
            )
            shadow_generate_cmd = [
                py,
                str(resolve_repo_path(r".\scripts\generate_probabilistic_shadow_report.py")),
                "--live-decision-log-jsonl",
                str(resolve_repo_path(args.shadow_live_decision_log_jsonl)),
                "--backtest-decision-log-jsonl",
                str(resolve_repo_path(args.shadow_backtest_decision_log_jsonl)),
                "--runtime-bundle-json",
                str(shadow_bundle_json),
                "--pipeline-version",
                str(pipeline_version),
                "--output-json",
                str(shadow_report_json) if shadow_report_json is not None else "",
                "--strict",
            ]
            steps.append(run_step("generate_shadow_report", shadow_generate_cmd))
            if not steps[-1]["ok"]:
                dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
                return int(steps[-1]["returncode"] or 2)
            shadow_report_generated = True

        shadow_validation_json = (
            resolve_repo_path(args.shadow_validation_output_json)
            if str(args.shadow_validation_output_json).strip()
            else (log_dir / f"probabilistic_shadow_report_validation_{run_tag}.json")
        )
        should_validate_shadow_report = bool(args.validate_shadow_report)
        if str(args.promotion_target_stage).strip().lower() == "live_enable" and pipeline_version == "v2":
            should_validate_shadow_report = True
        shadow_validation_enabled = bool(should_validate_shadow_report)
        if should_validate_shadow_report and shadow_report_json is None:
            raise ValueError("--validate-shadow-report requires --promotion-shadow-report-json or --generate-shadow-report")
        if should_validate_shadow_report:
            shadow_validate_cmd = [
                py,
                str(resolve_repo_path(r".\scripts\validate_probabilistic_shadow_report.py")),
                "--shadow-report-json",
                str(shadow_report_json) if shadow_report_json is not None else "",
                "--pipeline-version",
                str(pipeline_version),
                "--output-json",
                str(shadow_validation_json),
                "--strict",
            ]
            steps.append(run_step("validate_shadow_report", shadow_validate_cmd))
            if not steps[-1]["ok"]:
                dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
                return int(steps[-1]["returncode"] or 2)

        promotion_cmd = [
            py,
            str(resolve_repo_path(r".\scripts\evaluate_probabilistic_promotion_readiness.py")),
            "--feature-validation-json",
            str(feature_validation_json),
            "--parity-json",
            str(parity_json),
            "--verification-json",
            str(verification_report_json),
            "--runtime-config-json",
            str(resolve_repo_path(args.promotion_runtime_config_json)),
            "--target-stage",
            str(args.promotion_target_stage),
            "--pipeline-version",
            str(pipeline_version),
            "--output-json",
            str(promotion_readiness_json),
        ]
        if should_validate_shadow_report:
            promotion_cmd.extend(["--shadow-validation-json", str(shadow_validation_json)])
        if shadow_report_json is not None:
            promotion_cmd.extend(["--shadow-report-json", str(shadow_report_json)])
        steps.append(run_step("evaluate_promotion_readiness", promotion_cmd))
        if not steps[-1]["ok"]:
            dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
            return int(steps[-1]["returncode"] or 2)

    status = "pass"

    summary = {
        "generated_at_utc": utc_now_iso(),
        "run_tag": run_tag,
        "status": status,
        "incremental_update": bool(args.incremental_update),
        "enable_purged_walk_forward": bool(args.enable_purged_walk_forward),
        "enable_conditional_cost_model": bool(args.enable_conditional_cost_model),
        "sample_mode": str(args.sample_mode),
        "sample_threshold": float(args.sample_threshold),
        "sample_lookback_minutes": int(args.sample_lookback_minutes),
        "ensemble_k": int(args.ensemble_k),
        "run_verification": bool(args.run_verification),
        "evaluate_promotion_readiness": bool(args.evaluate_promotion_readiness),
        "promotion_target_stage": str(args.promotion_target_stage),
        "paths": {
            "backtest_dir": str(backtest_dir),
            "feature_dir": str(feature_dir),
            "runtime_bundle_json": str(runtime_bundle_json),
            "universe_file": str(universe_file) if universe_file is not None else "",
            "split_manifest_json": str(split_manifest_json),
            "train_summary_json": str(train_summary_json),
            "parity_json": str(parity_json),
            "verification_report_json": str(verification_report_json) if bool(args.run_verification) else "",
            "shadow_report_json": (
                str(shadow_report_json)
                if bool(args.evaluate_promotion_readiness) and shadow_report_json is not None and (
                    shadow_report_generated or str(args.promotion_shadow_report_json).strip()
                )
                else ""
            ),
            "shadow_validation_json": (
                str(shadow_validation_json)
                if bool(args.evaluate_promotion_readiness) and shadow_validation_enabled
                else ""
            ),
            "promotion_readiness_json": str(promotion_readiness_json) if bool(args.evaluate_promotion_readiness) else "",
        },
        "steps": steps,
    }
    if pipeline_version != "v1":
        summary["pipeline_version"] = str(pipeline_version)
    dump_json(cycle_summary_json, summary)
    print(f"[HybridCycle] summary={cycle_summary_json}", flush=True)
    return 0 if status == "pass" else int(steps[-1]["returncode"] or 2)


if __name__ == "__main__":
    raise SystemExit(main())
