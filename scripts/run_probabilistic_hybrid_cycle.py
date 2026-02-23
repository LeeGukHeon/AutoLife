#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import sys
from datetime import datetime, timezone
from typing import Any, Dict, List

from _script_common import dump_json, ensure_parent_directory, resolve_repo_path


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
    parser.add_argument("--feature-dir", default=r".\data\model_input\probabilistic_features_v1_latest")
    parser.add_argument("--runtime-bundle-json", default=r".\config\model\probabilistic_runtime_bundle_v1.json")
    parser.add_argument("--train-max-datasets", type=int, default=0)
    parser.add_argument(
        "--universe-file",
        default="",
        help="Optional dynamic universe JSON for scope-aware 1m fetch/build behavior.",
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
    split_manifest_json = feature_dir / "probabilistic_split_manifest_v1.json"
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
    ]
    if universe_file is not None:
        build_cmd.extend(["--universe-file", str(universe_file)])
    steps.append(run_step("build_features", build_cmd))
    if not steps[-1]["ok"]:
        dump_json(cycle_summary_json, {"run_tag": run_tag, "status": "failed", "steps": steps})
        return int(steps[-1]["returncode"] or 2)

    validate_cmd = [
        py,
        str(resolve_repo_path(r".\scripts\validate_probabilistic_feature_dataset.py")),
        "--dataset-manifest-json",
        str(feature_manifest_json),
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
    ]
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
    status = "pass" if steps[-1]["ok"] else "failed"

    summary = {
        "generated_at_utc": utc_now_iso(),
        "run_tag": run_tag,
        "status": status,
        "incremental_update": bool(args.incremental_update),
        "paths": {
            "backtest_dir": str(backtest_dir),
            "feature_dir": str(feature_dir),
            "runtime_bundle_json": str(runtime_bundle_json),
            "universe_file": str(universe_file) if universe_file is not None else "",
            "split_manifest_json": str(split_manifest_json),
            "train_summary_json": str(train_summary_json),
            "parity_json": str(parity_json),
        },
        "steps": steps,
    }
    dump_json(cycle_summary_json, summary)
    print(f"[HybridCycle] summary={cycle_summary_json}", flush=True)
    return 0 if status == "pass" else int(steps[-1]["returncode"] or 2)


if __name__ == "__main__":
    raise SystemExit(main())
