#!/usr/bin/env python3
import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Tuple

import run_realdata_candidate_loop


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--train-iterations", "-TrainIterations", type=int, default=2)

    parser.add_argument("--skip-fetch", "-SkipFetch", action="store_true", default=True)
    parser.add_argument("--run-fetch", dest="skip_fetch", action="store_false")

    parser.add_argument("--skip-tune", "-SkipTune", action="store_true", default=True)
    parser.add_argument("--run-tune", dest="skip_tune", action="store_false")

    parser.add_argument("--real-data-only", "-RealDataOnly", action="store_true", default=True)
    parser.add_argument(
        "--require-higher-tf-companions",
        "-RequireHigherTfCompanions",
        dest="require_higher_tf_companions",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--allow-missing-higher-tf-companions",
        dest="require_higher_tf_companions",
        action="store_false",
    )
    parser.add_argument(
        "--run-both-hostility-modes",
        "-RunBothHostilityModes",
        action="store_true",
        default=False,
        help="Deprecated compatibility flag. Strict/adaptive split is no longer used.",
    )
    parser.add_argument("--gate-min-avg-trades", "-GateMinAvgTrades", type=int, default=8)
    parser.add_argument("--matrix-max-workers", "-MatrixMaxWorkers", type=int, default=1)
    parser.add_argument("--matrix-backtest-retry-count", "-MatrixBacktestRetryCount", type=int, default=2)
    parser.add_argument(
        "--run-parity-invariant",
        "-RunParityInvariant",
        action="store_true",
        default=False,
        help="Run parity invariant report during each nested realdata loop stage (default: off for speed).",
    )
    parser.add_argument("--fail-on-parity-invariant", "-FailOnParityInvariant", action="store_true")
    parser.add_argument(
        "--enable-hostility-adaptive-thresholds",
        "-EnableHostilityAdaptiveThresholds",
        dest="enable_hostility_adaptive_thresholds",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-hostility-adaptive-thresholds",
        dest="enable_hostility_adaptive_thresholds",
        action="store_false",
    )

    parser.add_argument(
        "--clear-state-before-train",
        "-ClearStateBeforeTrain",
        action="store_true",
        default=True,
    )
    parser.add_argument("--state-dir", "-StateDir", default=r".\build\Release\state")
    parser.add_argument("--state-snapshot-root", "-StateSnapshotRoot", default=r".\build\Release\state_snapshots")
    parser.add_argument(
        "--gate-report-json",
        "-GateReportJson",
        default=r".\build\Release\logs\profitability_gate_report_realdata.json",
    )
    parser.add_argument(
        "--summary-json",
        "-SummaryJson",
        default=r".\build\Release\logs\candidate_train_eval_cycle_summary.json",
    )
    parser.add_argument(
        "--entry-rejection-output-root",
        "-EntryRejectionOutputRoot",
        default=r".\build\Release\logs\train_eval_entry_rejections",
    )
    parser.add_argument(
        "--live-signal-funnel-taxonomy-json",
        "-LiveSignalFunnelTaxonomyJson",
        default=r".\build\Release\logs\live_signal_funnel_taxonomy_report.json",
    )
    parser.add_argument(
        "--split-manifest-json",
        "-SplitManifestJson",
        default=r".\build\Release\logs\candidate_train_eval_split_manifest.json",
    )
    parser.add_argument(
        "--real-data-dir",
        "-RealDataDir",
        default=r".\data\backtest_real",
    )
    parser.add_argument("--train-market-ratio", "-TrainMarketRatio", type=float, default=0.6)
    parser.add_argument("--validation-market-ratio", "-ValidationMarketRatio", type=float, default=0.2)
    parser.add_argument(
        "--max-markets",
        "-MaxMarkets",
        type=int,
        default=0,
        help="Limit number of markets used for cycle (0 means all).",
    )

    parser.add_argument(
        "--run-learned-eval",
        "-RunLearnedEval",
        action="store_true",
        help="Run additional evaluation with adaptive state I/O enabled.",
    )

    parser.add_argument(
        "--enforce-walk-forward",
        "-EnforceWalkForward",
        dest="enforce_walk_forward",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-walk-forward",
        dest="enforce_walk_forward",
        action="store_false",
    )
    parser.add_argument("--walk-forward-script", "-WalkForwardScript", default=r".\scripts\walk_forward_validate.py")
    parser.add_argument("--walk-forward-exe-path", "-WalkForwardExePath", default=r".\build\Release\AutoLifeTrading.exe")
    parser.add_argument("--walk-forward-train-size", "-WalkForwardTrainSize", type=int, default=600)
    parser.add_argument("--walk-forward-test-size", "-WalkForwardTestSize", type=int, default=200)
    parser.add_argument("--walk-forward-step-size", "-WalkForwardStepSize", type=int, default=200)
    parser.add_argument("--walk-forward-min-train-trades", "-WalkForwardMinTrainTrades", type=int, default=30)
    parser.add_argument(
        "--walk-forward-run-all-tests",
        "-WalkForwardRunAllTests",
        type=lambda s: str(s).strip().lower() in ("1", "true", "yes", "y"),
        default=True,
    )
    parser.add_argument(
        "--walk-forward-max-datasets",
        "-WalkForwardMaxDatasets",
        type=int,
        default=0,
        help="Limit datasets for walk-forward stage (0 means all in split).",
    )
    parser.add_argument("--walk-forward-min-ready-ratio", "-WalkForwardMinReadyRatio", type=float, default=0.55)
    parser.add_argument("--walk-forward-min-datasets", "-WalkForwardMinDatasets", type=int, default=2)
    parser.add_argument(
        "--walk-forward-output-root",
        "-WalkForwardOutputRoot",
        default=r".\build\Release\logs\walk_forward_split",
    )
    parser.add_argument("--verification-lock-path", "-VerificationLockPath", default=r".\build\Release\logs\verification_run.lock")
    parser.add_argument("--verification-lock-timeout-sec", "-VerificationLockTimeoutSec", type=int, default=1800)
    parser.add_argument("--verification-lock-stale-sec", "-VerificationLockStaleSec", type=int, default=14400)

    return parser.parse_args(argv)


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def parse_reason_counts_json(value: Any) -> Dict[str, int]:
    payload: Any = {}
    if isinstance(value, dict):
        payload = value
    elif isinstance(value, str):
        text = value.strip()
        if text:
            try:
                payload = json.loads(text)
            except json.JSONDecodeError:
                payload = {}
    if not isinstance(payload, dict):
        return {}
    out: Dict[str, int] = {}
    for key, raw in payload.items():
        name = str(key)
        if not name:
            continue
        try:
            out[name] = int(raw or 0)
        except (TypeError, ValueError):
            continue
    return out


def parse_int_or_default(value: Any, default: int = 0) -> int:
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return int(default)


def read_entry_rejection_snapshot(summary_json_path: Path, profile_id: str = "core_full") -> Dict[str, Any]:
    if not summary_json_path.exists():
        return {
            "exists": False,
            "summary_json": str(summary_json_path),
            "overall_top_reason": "",
            "overall_top_count": 0,
            "profile_top_reason": "",
            "profile_top_count": 0,
        }
    payload = json.loads(summary_json_path.read_text(encoding="utf-8-sig"))
    overall_top = payload.get("overall_top_reasons") or []
    overall_top_reason = ""
    overall_top_count = 0
    if overall_top and isinstance(overall_top[0], dict):
        overall_top_reason = str(overall_top[0].get("reason", ""))
        overall_top_count = int(overall_top[0].get("count", 0) or 0)

    profile_top_reason = ""
    profile_top_count = 0
    profile_top = (payload.get("profile_top_reasons") or {}).get(profile_id) or []
    if profile_top and isinstance(profile_top[0], dict):
        profile_top_reason = str(profile_top[0].get("reason", ""))
        profile_top_count = int(profile_top[0].get("count", 0) or 0)

    return {
        "exists": True,
        "summary_json": str(summary_json_path),
        "overall_top_reason": overall_top_reason,
        "overall_top_count": overall_top_count,
        "profile_id": profile_id,
        "profile_top_reason": profile_top_reason,
        "profile_top_count": profile_top_count,
    }


def entry_rejection_paths_for_stage(output_root: Path, stage_name: str) -> Dict[str, Path]:
    safe_stage = stage_name.strip().replace(" ", "_")
    summary_json = output_root / f"{safe_stage}_summary.json"
    return {
        "summary_json": output_root / f"{safe_stage}_summary.json",
        "profile_csv": output_root / f"{safe_stage}_by_profile.csv",
        "dataset_csv": output_root / f"{safe_stage}_by_dataset.csv",
        "taxonomy_json": summary_json.with_name(f"{summary_json.stem}_taxonomy.json"),
    }


def read_strategy_rejection_taxonomy_snapshot(taxonomy_json_path: Path) -> Dict[str, Any]:
    if not taxonomy_json_path.exists():
        return {
            "exists": False,
            "taxonomy_json": str(taxonomy_json_path),
            "overall_top_group": "",
            "overall_top_group_count": 0,
            "taxonomy_coverage_ratio": 0.0,
            "unknown_reason_code_count": 0,
        }
    payload = json.loads(taxonomy_json_path.read_text(encoding="utf-8-sig"))
    group_counts = payload.get("group_counts") or {}
    top_group = ""
    top_group_count = 0
    if isinstance(group_counts, dict) and group_counts:
        group_items = sorted(
            ((str(k), int(v)) for k, v in group_counts.items()),
            key=lambda kv: (-kv[1], kv[0]),
        )
        if group_items:
            top_group = group_items[0][0]
            top_group_count = int(group_items[0][1])
    return {
        "exists": True,
        "taxonomy_json": str(taxonomy_json_path),
        "overall_top_group": top_group,
        "overall_top_group_count": top_group_count,
        "taxonomy_coverage_ratio": float(payload.get("taxonomy_coverage_ratio", 0.0) or 0.0),
        "unknown_reason_code_count": int(len(payload.get("unknown_reason_codes") or [])),
    }


def live_signal_funnel_path_for_stage(output_root: Path, stage_name: str) -> Path:
    safe_stage = stage_name.strip().replace(" ", "_")
    return output_root / f"{safe_stage}_live_signal_funnel.json"


def read_live_signal_funnel_snapshot(path_value: Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {
            "exists": False,
            "funnel_json": str(path_value),
            "scan_count": 0,
            "top_group": "",
            "top_group_count": 0,
            "total_rejections": 0,
            "signal_generation_share": 0.0,
            "manager_prefilter_share": 0.0,
            "position_state_share": 0.0,
        }

    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    group_counts_raw = payload.get("rejection_group_counts") or {}
    group_counts: Dict[str, int] = {}
    if isinstance(group_counts_raw, dict):
        for key, value in group_counts_raw.items():
            try:
                group_counts[str(key)] = int(value or 0)
            except (TypeError, ValueError):
                continue

    total_rejections = int(sum(max(0, v) for v in group_counts.values()))
    top_group = ""
    top_group_count = 0
    if group_counts:
        sorted_groups = sorted(group_counts.items(), key=lambda kv: (-kv[1], kv[0]))
        top_group = str(sorted_groups[0][0])
        top_group_count = int(sorted_groups[0][1])

    def _share(name: str) -> float:
        if total_rejections <= 0:
            return 0.0
        return float(max(0, group_counts.get(name, 0))) / float(total_rejections)

    return {
        "exists": True,
        "funnel_json": str(path_value),
        "scan_count": int(payload.get("scan_count", 0) or 0),
        "top_group": top_group,
        "top_group_count": top_group_count,
        "total_rejections": total_rejections,
        "signal_generation_share": float(_share("signal_generation")),
        "manager_prefilter_share": float(_share("manager_prefilter")),
        "position_state_share": float(_share("position_state")),
    }


def capture_live_signal_funnel_snapshot_for_stage(
    source_path: Path,
    output_root: Path,
    stage_name: str,
) -> Dict[str, Any]:
    stage_path = live_signal_funnel_path_for_stage(output_root, stage_name)
    if source_path.exists():
        stage_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, stage_path)
    return read_live_signal_funnel_snapshot(stage_path)


def clear_state_dir(state_dir: Path) -> int:
    state_dir.mkdir(parents=True, exist_ok=True)
    removed = 0
    for item in state_dir.glob("*"):
        if item.is_file():
            item.unlink(missing_ok=True)
            removed += 1
    return removed


def snapshot_state_dir(state_dir: Path, snapshot_root: Path) -> Path:
    snapshot_root.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    snapshot_dir = snapshot_root / f"state_snapshot_{stamp}"
    snapshot_dir.mkdir(parents=True, exist_ok=True)
    if state_dir.exists():
        for item in state_dir.glob("*"):
            if item.is_file():
                shutil.copy2(item, snapshot_dir / item.name)
    return snapshot_dir


def read_gate_snapshot(report_path: Path) -> Dict[str, Any]:
    report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    core_full = next((x for x in report.get("profile_summaries", []) if x.get("profile_id") == "core_full"), {})
    risk_gate_breakdown = parse_reason_counts_json(core_full.get("entry_risk_gate_breakdown_json"))
    risk_gate_component_breakdown = {
        k: int(v)
        for k, v in risk_gate_breakdown.items()
        if str(k)
        not in {
            "blocked_risk_gate_total",
            "blocked_risk_gate_entry_quality",
            "blocked_risk_gate_entry_quality_rr",
            "blocked_risk_gate_entry_quality_rr_adaptive",
            "blocked_risk_gate_entry_quality_edge",
            "blocked_risk_gate_entry_quality_edge_adaptive",
            "blocked_risk_gate_entry_quality_rr_edge",
            "blocked_risk_gate_entry_quality_rr_edge_adaptive",
        }
    }
    top_risk_gate_component_reason = ""
    top_risk_gate_component_count = 0
    if risk_gate_component_breakdown:
        top_risk_gate_component_reason, top_risk_gate_component_count = max(
            risk_gate_component_breakdown.items(),
            key=lambda kv: (kv[1], kv[0]),
        )
    return {
        "overall_gate_pass": bool(report.get("overall_gate_pass")),
        "core_full": {
            "avg_profit_factor": core_full.get("avg_profit_factor"),
            "avg_total_trades": core_full.get("avg_total_trades"),
            "avg_expectancy_krw": core_full.get("avg_expectancy_krw"),
            "profitable_ratio": core_full.get("profitable_ratio"),
            "top_entry_rejection_reason": str(core_full.get("top_entry_rejection_reason", "") or ""),
            "top_entry_rejection_count": parse_int_or_default(core_full.get("top_entry_rejection_count"), 0),
            "entry_rejection_total": parse_int_or_default(core_full.get("entry_rejection_total"), 0),
            "top_entry_risk_gate_reason": str(core_full.get("top_entry_risk_gate_reason", "") or ""),
            "top_entry_risk_gate_count": parse_int_or_default(core_full.get("top_entry_risk_gate_count"), 0),
            "top_entry_risk_gate_component_reason": str(
                core_full.get("top_entry_risk_gate_component_reason", top_risk_gate_component_reason) or ""
            ),
            "top_entry_risk_gate_component_count": parse_int_or_default(
                core_full.get("top_entry_risk_gate_component_count", top_risk_gate_component_count),
                top_risk_gate_component_count,
            ),
            "entry_risk_gate_breakdown": risk_gate_breakdown,
        },
    }


def read_gate_snapshot_bundle(report_path: Path) -> Dict[str, Any]:
    selected = read_gate_snapshot(report_path)
    return {
        "selected": selected,
        # Strict/adaptive pair outputs are deprecated.
        "strict": None,
        "adaptive": None,
    }


def has_higher_tf_companions(primary_path: Path) -> bool:
    stem = primary_path.stem.lower()
    if not stem.startswith("upbit_") or "_1m_" not in stem:
        return False
    pivot = stem.index("_1m_")
    if pivot <= 6:
        return False
    market_token = stem[6:pivot]
    for tf in ("5m", "60m", "240m"):
        if not list(primary_path.parent.glob(f"upbit_{market_token}_{tf}_*.csv")):
            return False
    return True


def list_real_primary_datasets(real_data_dir: Path, require_higher_tf_companions: bool) -> List[Path]:
    if not real_data_dir.exists():
        return []
    out: List[Path] = []
    for f in sorted(real_data_dir.glob("*.csv"), key=lambda x: x.name.lower()):
        name = f.name.lower()
        if not name.startswith("upbit_") or "_1m_" not in name:
            continue
        if require_higher_tf_companions and not has_higher_tf_companions(f):
            continue
        out.append(f.resolve())
    return sorted(set(out), key=lambda x: str(x).lower())


def market_token_from_dataset_path(dataset_path: Path) -> str:
    stem = dataset_path.stem.lower()
    if stem.startswith("upbit_") and "_1m_" in stem:
        pivot = stem.index("_1m_")
        return stem[6:pivot].upper()
    return dataset_path.stem.upper()


def split_market_tokens(market_tokens: List[str], train_ratio: float, validation_ratio: float) -> Dict[str, List[str]]:
    tokens = sorted(set(market_tokens), key=lambda x: x.lower())
    n = len(tokens)
    if n <= 0:
        return {"train": [], "validation": [], "holdout": []}
    if n == 1:
        return {"train": tokens[:], "validation": [], "holdout": []}
    if n == 2:
        return {"train": [tokens[0]], "validation": [tokens[1]], "holdout": []}

    train_n = int(round(n * max(0.1, min(0.9, float(train_ratio)))))
    val_n = int(round(n * max(0.05, min(0.8, float(validation_ratio)))))

    train_n = max(1, min(n - 2, train_n))
    val_n = max(1, min(n - train_n - 1, val_n))
    hold_n = n - train_n - val_n

    if hold_n <= 0:
        hold_n = 1
        if val_n > 1:
            val_n -= 1
        elif train_n > 1:
            train_n -= 1

    train = tokens[:train_n]
    validation = tokens[train_n : train_n + val_n]
    holdout = tokens[train_n + val_n :]
    return {"train": train, "validation": validation, "holdout": holdout}


def build_dataset_splits(
    datasets: List[Path],
    train_ratio: float,
    validation_ratio: float,
    max_markets: int,
) -> Dict[str, Any]:
    by_market: Dict[str, List[Path]] = {}
    for ds in datasets:
        by_market.setdefault(market_token_from_dataset_path(ds), []).append(ds)

    market_tokens = sorted(by_market.keys(), key=lambda x: x.lower())
    if int(max_markets) > 0:
        market_tokens = market_tokens[: int(max_markets)]
        by_market = {k: by_market[k] for k in market_tokens}

    split_markets = split_market_tokens(market_tokens, train_ratio=train_ratio, validation_ratio=validation_ratio)

    split_datasets: Dict[str, List[Path]] = {}
    for stage in ("train", "validation", "holdout"):
        rows: List[Path] = []
        for market in split_markets[stage]:
            rows.extend(by_market.get(market, []))
        split_datasets[stage] = sorted(set(rows), key=lambda x: str(x).lower())

    manifest = {
        "market_count_total": len(market_tokens),
        "train_markets": split_markets["train"],
        "validation_markets": split_markets["validation"],
        "holdout_markets": split_markets["holdout"],
        "train_dataset_count": len(split_datasets["train"]),
        "validation_dataset_count": len(split_datasets["validation"]),
        "holdout_dataset_count": len(split_datasets["holdout"]),
        "train_datasets": [str(x) for x in split_datasets["train"]],
        "validation_datasets": [str(x) for x in split_datasets["validation"]],
        "holdout_datasets": [str(x) for x in split_datasets["holdout"]],
    }
    return {"manifest": manifest, "datasets": split_datasets}


def build_loop_argv(
    args,
    enable_adaptive_state_io: bool,
    datasets: List[Path],
    skip_tune: bool,
    stage_name: str,
    entry_rejection_output_root: Path,
) -> List[str]:
    loop_argv: List[str] = []
    if bool(args.skip_fetch):
        loop_argv.append("--skip-fetch")
    if bool(skip_tune):
        loop_argv.append("--skip-tune")
    if bool(args.real_data_only):
        loop_argv.append("--real-data-only")
    if not bool(args.run_parity_invariant):
        loop_argv.append("--skip-parity-invariant")
    if bool(args.fail_on_parity_invariant):
        loop_argv.append("--fail-on-parity-invariant")
    if bool(args.require_higher_tf_companions):
        loop_argv.append("--require-higher-tf-companions")
    else:
        loop_argv.append("--allow-missing-higher-tf-companions")
    if bool(args.enable_hostility_adaptive_thresholds):
        loop_argv.append("--enable-hostility-adaptive-thresholds")
    else:
        loop_argv.append("--disable-hostility-adaptive-thresholds")
    if bool(enable_adaptive_state_io):
        loop_argv.append("--enable-adaptive-state-io")

    loop_argv.extend(["--real-data-dir", str(Path(args.real_data_dir).resolve())])
    loop_argv.extend(["--gate-min-avg-trades", str(max(1, int(args.gate_min_avg_trades)))])
    loop_argv.extend(["--matrix-max-workers", str(max(1, int(args.matrix_max_workers)))])
    loop_argv.extend(["--matrix-backtest-retry-count", str(max(1, int(args.matrix_backtest_retry_count)))])
    loop_argv.extend(["--verification-lock-path", str(Path(args.verification_lock_path).resolve())])
    loop_argv.extend(["--verification-lock-timeout-sec", str(max(1, int(args.verification_lock_timeout_sec)))])
    loop_argv.extend(["--verification-lock-stale-sec", str(max(10, int(args.verification_lock_stale_sec)))])

    rejection_paths = entry_rejection_paths_for_stage(entry_rejection_output_root, stage_name)
    rejection_paths["summary_json"].parent.mkdir(parents=True, exist_ok=True)
    loop_argv.extend(["--entry-rejection-output-json", str(rejection_paths["summary_json"])])
    loop_argv.extend(["--entry-rejection-profile-csv", str(rejection_paths["profile_csv"])])
    loop_argv.extend(["--entry-rejection-dataset-csv", str(rejection_paths["dataset_csv"])])
    loop_argv.extend(
        [
            "--strategy-rejection-taxonomy-output-json",
            str(rejection_paths["taxonomy_json"]),
        ]
    )

    if datasets:
        loop_argv.extend(["--dataset-names", *[str(x) for x in datasets]])
    return loop_argv


def run_loop_once(
    args,
    report_path: Path,
    enable_adaptive_state_io: bool,
    datasets: List[Path],
    stage_name: str,
    skip_tune: bool,
    entry_rejection_output_root: Path,
) -> Dict[str, Any]:
    if not datasets:
        raise RuntimeError(f"No datasets provided for stage: {stage_name}")
    rc = run_realdata_candidate_loop.main(
        build_loop_argv(
            args,
            enable_adaptive_state_io=enable_adaptive_state_io,
            datasets=datasets,
            skip_tune=skip_tune,
            stage_name=stage_name,
            entry_rejection_output_root=entry_rejection_output_root,
        )
    )
    if rc != 0:
        raise RuntimeError(f"run_realdata_candidate_loop failed stage={stage_name} (exit={rc})")
    if not report_path.exists():
        raise RuntimeError(f"Gate report not found: {report_path}")
    return read_gate_snapshot_bundle(report_path)


def mode_gate_pass(bundle: Dict[str, Any]) -> Tuple[bool, bool, bool]:
    strict = bundle.get("strict")
    adaptive = bundle.get("adaptive")
    if strict is not None and adaptive is not None:
        strict_pass = bool(strict.get("overall_gate_pass"))
        adaptive_pass = bool(adaptive.get("overall_gate_pass"))
        return strict_pass and adaptive_pass, strict_pass, adaptive_pass

    selected = bundle.get("selected") or {}
    selected_pass = bool(selected.get("overall_gate_pass"))
    return selected_pass, selected_pass, selected_pass


def run_walk_forward_batch(args, datasets: List[Path], stage_name: str) -> Dict[str, Any]:
    if not bool(args.enforce_walk_forward):
        return {
            "enforced": False,
            "stage": stage_name,
            "datasets_requested": len(datasets),
            "datasets_ran": 0,
            "ready_ratio": 0.0,
            "gate_pass": True,
            "details": [],
            "failures": [],
        }

    wf_script = Path(args.walk_forward_script).resolve()
    wf_exe = Path(args.walk_forward_exe_path).resolve()
    if not wf_script.exists():
        raise FileNotFoundError(f"Walk-forward script not found: {wf_script}")
    if not wf_exe.exists():
        raise FileNotFoundError(f"Walk-forward executable not found: {wf_exe}")

    selected = list(datasets)
    if int(args.walk_forward_max_datasets) > 0:
        selected = selected[: int(args.walk_forward_max_datasets)]

    output_root = Path(args.walk_forward_output_root).resolve()
    stage_dir = output_root / stage_name
    stage_dir.mkdir(parents=True, exist_ok=True)

    details: List[Dict[str, Any]] = []
    failures: List[Dict[str, Any]] = []
    for ds in selected:
        safe_name = ds.stem.replace(" ", "_")
        out_csv = stage_dir / f"wf_{safe_name}.csv"
        out_json = stage_dir / f"wf_{safe_name}.json"
        cmd = [
            sys.executable,
            str(wf_script),
            "--exe-path",
            str(wf_exe),
            "--input-csv",
            str(ds),
            "--train-size",
            str(max(50, int(args.walk_forward_train_size))),
            "--test-size",
            str(max(20, int(args.walk_forward_test_size))),
            "--step-size",
            str(max(20, int(args.walk_forward_step_size))),
            "--min-train-trades",
            str(max(1, int(args.walk_forward_min_train_trades))),
            "--run-all-tests",
            "true" if bool(args.walk_forward_run_all_tests) else "false",
            "--output-csv",
            str(out_csv),
            "--output-json",
            str(out_json),
        ]
        proc = subprocess.run(cmd)
        if proc.returncode != 0:
            failures.append({"dataset": str(ds), "exit_code": int(proc.returncode)})
            continue
        if not out_json.exists():
            failures.append({"dataset": str(ds), "exit_code": 0, "error": "missing_output_json"})
            continue
        report = json.loads(out_json.read_text(encoding="utf-8-sig"))
        details.append(
            {
                "dataset": str(ds),
                "is_ready_for_live_walkforward": bool(report.get("is_ready_for_live_walkforward", False)),
                "windows_oos_ran": int(report.get("windows_oos_ran", 0)),
                "oos_profitable_ratio": float(report.get("oos_profitable_ratio", 0.0)),
                "oos_total_profit": float(report.get("oos_total_profit", 0.0)),
                "oos_max_mdd_pct": float(report.get("oos_max_mdd_pct", 0.0)),
                "report_json": str(out_json),
                "report_csv": str(out_csv),
            }
        )

    ran_count = len(details)
    ready_count = sum(1 for x in details if bool(x.get("is_ready_for_live_walkforward", False)))
    ready_ratio = (ready_count / float(ran_count)) if ran_count > 0 else 0.0
    effective_min_datasets = min(max(1, int(args.walk_forward_min_datasets)), max(1, len(selected)))
    gate_pass = (
        ran_count >= effective_min_datasets
        and ready_ratio >= float(args.walk_forward_min_ready_ratio)
        and len(failures) == 0
    )

    return {
        "enforced": True,
        "stage": stage_name,
        "datasets_requested": len(selected),
        "datasets_ran": ran_count,
        "datasets_ready": ready_count,
        "ready_ratio": round(ready_ratio, 4),
        "min_ready_ratio": float(args.walk_forward_min_ready_ratio),
        "effective_min_datasets": effective_min_datasets,
        "gate_pass": bool(gate_pass),
        "details": details,
        "failures": failures,
    }


def build_live_signal_funnel_context(snapshot: Dict[str, Any] | None) -> Dict[str, Any]:
    payload = snapshot or {}
    signal_generation_share = float(payload.get("signal_generation_share", 0.0) or 0.0)
    manager_prefilter_share = float(payload.get("manager_prefilter_share", 0.0) or 0.0)
    position_state_share = float(payload.get("position_state_share", 0.0) or 0.0)
    total_rejections = int(payload.get("total_rejections", 0) or 0)
    scan_count = int(payload.get("scan_count", 0) or 0)
    no_trade_bias_active = bool(
        scan_count >= 3
        and total_rejections >= 20
        and position_state_share < 0.40
        and (
            signal_generation_share >= 0.55
            or (signal_generation_share + manager_prefilter_share) >= 0.75
        )
    )
    return {
        "exists": bool(payload.get("exists", False)),
        "funnel_json": str(payload.get("funnel_json", "")),
        "scan_count": scan_count,
        "top_group": str(payload.get("top_group", "")),
        "top_group_count": int(payload.get("top_group_count", 0) or 0),
        "total_rejections": total_rejections,
        "signal_generation_share": signal_generation_share,
        "manager_prefilter_share": manager_prefilter_share,
        "position_state_share": position_state_share,
        "no_trade_bias_active": no_trade_bias_active,
    }


def build_promotion_verdict(
    train_snapshot_bundle: Dict[str, Any] | None,
    validation_snapshot_bundle: Dict[str, Any],
    holdout_snapshot_bundle: Dict[str, Any] | None,
    wf_validation: Dict[str, Any],
    wf_holdout: Dict[str, Any] | None,
    validation_live_funnel_snapshot: Dict[str, Any] | None = None,
    holdout_live_funnel_snapshot: Dict[str, Any] | None = None,
) -> Dict[str, Any]:
    val_pass, val_strict_pass, val_adaptive_pass = mode_gate_pass(validation_snapshot_bundle)
    holdout_pass = True
    holdout_strict_pass = None
    holdout_adaptive_pass = None
    if holdout_snapshot_bundle is not None:
        holdout_pass, holdout_strict_pass, holdout_adaptive_pass = mode_gate_pass(holdout_snapshot_bundle)

    wf_validation_pass = bool(wf_validation.get("gate_pass", True))
    wf_holdout_pass = True
    if wf_holdout is not None:
        wf_holdout_pass = bool(wf_holdout.get("gate_pass", True))

    val_live_ctx = build_live_signal_funnel_context(validation_live_funnel_snapshot)
    holdout_live_ctx = (
        build_live_signal_funnel_context(holdout_live_funnel_snapshot)
        if holdout_live_funnel_snapshot is not None
        else None
    )

    def _core_metrics(bundle: Dict[str, Any] | None) -> Tuple[float, float]:
        selected = (bundle or {}).get("selected") or {}
        core_full = selected.get("core_full") or {}
        pf = float(core_full.get("avg_profit_factor", 0.0) or 0.0)
        exp = float(core_full.get("avg_expectancy_krw", 0.0) or 0.0)
        return pf, exp

    def _core_rejection_context(bundle: Dict[str, Any] | None) -> Dict[str, Any]:
        selected = (bundle or {}).get("selected") or {}
        core_full = selected.get("core_full") or {}
        return {
            "top_entry_rejection_reason": str(core_full.get("top_entry_rejection_reason", "") or ""),
            "top_entry_rejection_count": parse_int_or_default(core_full.get("top_entry_rejection_count"), 0),
            "entry_rejection_total": parse_int_or_default(core_full.get("entry_rejection_total"), 0),
            "top_entry_risk_gate_reason": str(core_full.get("top_entry_risk_gate_reason", "") or ""),
            "top_entry_risk_gate_count": parse_int_or_default(core_full.get("top_entry_risk_gate_count"), 0),
            "top_entry_risk_gate_component_reason": str(
                core_full.get("top_entry_risk_gate_component_reason", "") or ""
            ),
            "top_entry_risk_gate_component_count": parse_int_or_default(
                core_full.get("top_entry_risk_gate_component_count"),
                0,
            ),
            "entry_risk_gate_breakdown": core_full.get("entry_risk_gate_breakdown") or {},
        }

    train_pf, train_exp = _core_metrics(train_snapshot_bundle)
    val_pf, val_exp = _core_metrics(validation_snapshot_bundle)
    holdout_pf, holdout_exp = _core_metrics(holdout_snapshot_bundle)
    val_core_reject_ctx = _core_rejection_context(validation_snapshot_bundle)
    holdout_core_reject_ctx = _core_rejection_context(holdout_snapshot_bundle)

    # Overfit guard: reject promotions that look good on train but fail to
    # generalize to validation/holdout by a wide margin.
    generalization_checks: List[Dict[str, Any]] = []
    if train_snapshot_bundle is not None:
        generalization_checks.append(
            {
                "name": "validation_vs_train",
                "pass": bool((val_pf >= (train_pf - 0.08)) and (val_exp >= (train_exp - 3.0))),
                "validation_pf": val_pf,
                "validation_expectancy_krw": val_exp,
                "train_pf": train_pf,
                "train_expectancy_krw": train_exp,
                "max_pf_drop": 0.08,
                "max_expectancy_drop_krw": 3.0,
            }
        )
        if holdout_snapshot_bundle is not None:
            generalization_checks.append(
                {
                    "name": "holdout_vs_train",
                    "pass": bool((holdout_pf >= (train_pf - 0.10)) and (holdout_exp >= (train_exp - 3.5))),
                    "holdout_pf": holdout_pf,
                    "holdout_expectancy_krw": holdout_exp,
                    "train_pf": train_pf,
                    "train_expectancy_krw": train_exp,
                    "max_pf_drop": 0.10,
                    "max_expectancy_drop_krw": 3.5,
                }
            )
    if holdout_snapshot_bundle is not None:
        generalization_checks.append(
            {
                "name": "holdout_vs_validation",
                "pass": bool((holdout_pf >= (val_pf - 0.08)) and (holdout_exp >= (val_exp - 2.5))),
                "holdout_pf": holdout_pf,
                "holdout_expectancy_krw": holdout_exp,
                "validation_pf": val_pf,
                "validation_expectancy_krw": val_exp,
                "max_pf_drop": 0.08,
                "max_expectancy_drop_krw": 2.5,
            }
        )

    generalization_guard_pass = all(bool(x.get("pass", False)) for x in generalization_checks)
    base_promote = bool(val_pass and holdout_pass and wf_validation_pass and wf_holdout_pass)
    promote = bool(base_promote and generalization_guard_pass)
    if promote:
        recommendation = "promote_candidate"
    elif base_promote and (not generalization_guard_pass):
        recommendation = "hold_candidate_avoid_overfit_generalization_gap"
    elif (not val_pass) and (
        str(val_core_reject_ctx.get("top_entry_rejection_reason", "")).startswith("blocked_risk_gate")
        or str(val_core_reject_ctx.get("top_entry_rejection_reason", "")) == "blocked_second_stage_confirmation"
        or str(val_core_reject_ctx.get("top_entry_risk_gate_component_reason", "")).startswith("blocked_risk_gate")
        or str(val_core_reject_ctx.get("top_entry_risk_gate_component_reason", "")) == "blocked_second_stage_confirmation"
    ):
        risk_component = str(val_core_reject_ctx.get("top_entry_risk_gate_component_reason", ""))
        if risk_component.startswith("blocked_risk_gate_entry_quality"):
            if risk_component == "blocked_risk_gate_entry_quality_rr_base":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_baseline_floor"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_adaptive_history":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_adaptive_history_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_adaptive_regime":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_adaptive_regime_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_adaptive_mixed":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_adaptive_mixed_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_adaptive":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_adaptive_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_rr":
                recommendation = "hold_candidate_calibrate_risk_gate_entry_quality_rr"
            elif risk_component == "blocked_risk_gate_entry_quality_edge_base":
                recommendation = "hold_candidate_calibrate_risk_gate_edge_baseline_floor"
            elif risk_component == "blocked_risk_gate_entry_quality_edge_adaptive_history":
                recommendation = "hold_candidate_calibrate_risk_gate_edge_adaptive_history_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_edge_adaptive_regime":
                recommendation = "hold_candidate_calibrate_risk_gate_edge_adaptive_regime_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_edge_adaptive_mixed":
                recommendation = "hold_candidate_calibrate_risk_gate_edge_adaptive_mixed_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_edge_adaptive":
                recommendation = "hold_candidate_calibrate_risk_gate_edge_adaptive_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_edge":
                recommendation = "hold_candidate_calibrate_risk_gate_entry_quality_edge"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_edge_base":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_baseline_floor"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_edge_adaptive_history":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_adaptive_history_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_edge_adaptive_regime":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_adaptive_regime_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_edge_adaptive_mixed":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_adaptive_mixed_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_edge_adaptive":
                recommendation = "hold_candidate_calibrate_risk_gate_rr_adaptive_adders"
            elif risk_component == "blocked_risk_gate_entry_quality_rr_edge":
                recommendation = "hold_candidate_calibrate_risk_gate_entry_quality_rr_edge"
            elif risk_component == "blocked_risk_gate_entry_quality_invalid_levels":
                recommendation = "hold_candidate_fix_entry_quality_price_level_consistency"
            else:
                recommendation = "hold_candidate_calibrate_risk_gate_entry_quality_ownership"
        elif risk_component == "blocked_risk_gate_entry_quality":
            recommendation = "hold_candidate_calibrate_risk_gate_entry_quality_ownership"
        elif risk_component == "blocked_risk_gate_regime":
            recommendation = "hold_candidate_calibrate_risk_gate_regime_alignment"
        elif risk_component == "blocked_risk_gate_strategy_ev":
            recommendation = "hold_candidate_calibrate_risk_gate_strategy_ev_alignment"
        elif risk_component == "blocked_second_stage_confirmation":
            recommendation = "hold_candidate_calibrate_second_stage_confirmation_consistency"
        else:
            recommendation = "hold_candidate_investigate_risk_gate_breakdown"
    elif (not val_pass) and bool(val_live_ctx.get("no_trade_bias_active", False)):
        recommendation = "hold_candidate_improve_signal_generation_or_dataset_quality"
    elif val_pass and not holdout_pass:
        if holdout_live_ctx is not None and bool(holdout_live_ctx.get("no_trade_bias_active", False)):
            recommendation = "hold_candidate_improve_holdout_signal_quality"
        else:
            recommendation = "hold_candidate_improve_holdout_generalization"
    elif (not val_pass) and str(val_live_ctx.get("top_group", "")) == "manager_prefilter":
        recommendation = "hold_candidate_improve_prefilter_policy_or_feature_quality"
    elif (not val_pass) and str(val_live_ctx.get("top_group", "")) == "position_state":
        recommendation = "hold_candidate_improve_position_turnover_and_exit_management"
    elif val_pass and holdout_pass and (not wf_validation_pass or not wf_holdout_pass):
        recommendation = "hold_candidate_improve_walkforward_stability"
    else:
        recommendation = "hold_candidate_improve_holdout_generalization"
        if not val_pass:
            recommendation = "hold_candidate_improve_validation_bottlenecks"

    return {
        "validation_both_modes_gate_pass": bool(val_pass),
        "validation_strict_gate_pass": bool(val_strict_pass),
        "validation_adaptive_gate_pass": bool(val_adaptive_pass),
        "holdout_both_modes_gate_pass": bool(holdout_pass),
        "holdout_strict_gate_pass": holdout_strict_pass,
        "holdout_adaptive_gate_pass": holdout_adaptive_pass,
        "walk_forward_validation_gate_pass": bool(wf_validation_pass),
        "walk_forward_holdout_gate_pass": bool(wf_holdout_pass),
        "base_promotion_gate_pass": bool(base_promote),
        "generalization_guard_pass": bool(generalization_guard_pass),
        "generalization_checks": generalization_checks,
        "promotion_gate_pass": bool(promote),
        "recommendation": recommendation,
        "validation_core_rejection_context": val_core_reject_ctx,
        "holdout_core_rejection_context": holdout_core_reject_ctx,
        "validation_live_signal_funnel_context": val_live_ctx,
        "holdout_live_signal_funnel_context": holdout_live_ctx,
        "live_no_trade_bias_any": bool(
            val_live_ctx.get("no_trade_bias_active", False)
            or (holdout_live_ctx or {}).get("no_trade_bias_active", False)
        ),
    }


def main(argv=None) -> int:
    args = parse_args(argv)
    state_dir = Path(args.state_dir).resolve()
    snapshot_root = Path(args.state_snapshot_root).resolve()
    report_path = Path(args.gate_report_json).resolve()
    summary_path = Path(args.summary_json).resolve()
    split_manifest_path = Path(args.split_manifest_json).resolve()
    real_data_dir = Path(args.real_data_dir).resolve()
    entry_rejection_output_root = Path(args.entry_rejection_output_root).resolve()
    live_signal_funnel_source_path = Path(args.live_signal_funnel_taxonomy_json).resolve()

    summary_path.parent.mkdir(parents=True, exist_ok=True)
    entry_rejection_output_root.mkdir(parents=True, exist_ok=True)

    all_datasets = list_real_primary_datasets(real_data_dir, bool(args.require_higher_tf_companions))
    if len(all_datasets) < 2:
        raise RuntimeError(
            "Need at least 2 real primary datasets to split train/validation (current: "
            f"{len(all_datasets)})."
        )

    split_info = build_dataset_splits(
        datasets=all_datasets,
        train_ratio=float(args.train_market_ratio),
        validation_ratio=float(args.validation_market_ratio),
        max_markets=int(args.max_markets),
    )
    split_manifest = split_info["manifest"]
    split_datasets = split_info["datasets"]

    if not split_datasets["train"]:
        raise RuntimeError("Train split is empty. Adjust market split ratios.")
    if not split_datasets["validation"]:
        raise RuntimeError("Validation split is empty. Adjust market split ratios.")

    write_json(
        split_manifest_path,
        {
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "real_data_dir": str(real_data_dir),
            "require_higher_tf_companions": bool(args.require_higher_tf_companions),
            "train_market_ratio": float(args.train_market_ratio),
            "validation_market_ratio": float(args.validation_market_ratio),
            "max_markets": int(args.max_markets),
            **split_manifest,
        },
    )

    summary: Dict[str, Any] = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "train_iterations": int(args.train_iterations),
        "state_dir": str(state_dir),
        "split_manifest_json": str(split_manifest_path),
        "entry_rejection_output_root": str(entry_rejection_output_root),
        "live_signal_funnel_source_json": str(live_signal_funnel_source_path),
        "split": split_manifest,
        "stages": [],
    }

    if args.clear_state_before_train:
        removed = clear_state_dir(state_dir)
        summary["state_cleared_before_train"] = removed
        print(f"[TrainEval] Cleared state files before training: {removed}")

    latest_train_snapshot = None
    for i in range(1, max(1, int(args.train_iterations)) + 1):
        print(f"[TrainEval] Training iteration {i}/{args.train_iterations} (adaptive-state=ON)")
        snap = run_loop_once(
            args,
            report_path,
            enable_adaptive_state_io=True,
            datasets=split_datasets["train"],
            stage_name=f"train_{i}",
            skip_tune=bool(args.skip_tune),
            entry_rejection_output_root=entry_rejection_output_root,
        )
        latest_train_snapshot = snap
        entry_rejection_snapshot = read_entry_rejection_snapshot(
            entry_rejection_paths_for_stage(entry_rejection_output_root, f"train_{i}")["summary_json"]
        )
        entry_rejection_taxonomy_snapshot = read_strategy_rejection_taxonomy_snapshot(
            entry_rejection_paths_for_stage(entry_rejection_output_root, f"train_{i}")["taxonomy_json"]
        )
        live_signal_funnel_snapshot = capture_live_signal_funnel_snapshot_for_stage(
            source_path=live_signal_funnel_source_path,
            output_root=entry_rejection_output_root,
            stage_name=f"train_{i}",
        )
        summary["stages"].append(
            {
                "stage": f"train_{i}",
                "split": "train",
                "adaptive_state_io": True,
                "dataset_count": len(split_datasets["train"]),
                "skip_tune": bool(args.skip_tune),
                "snapshot": snap,
                "entry_rejection": entry_rejection_snapshot,
                "entry_rejection_taxonomy": entry_rejection_taxonomy_snapshot,
                "live_signal_funnel": live_signal_funnel_snapshot,
            }
        )

    snapshot_dir = snapshot_state_dir(state_dir, snapshot_root)
    summary["state_snapshot_dir"] = str(snapshot_dir)
    snapshot_files = sorted(p.name for p in snapshot_dir.glob("*") if p.is_file())
    summary["state_snapshot_files"] = snapshot_files
    print(f"[TrainEval] State snapshot saved: {snapshot_dir}")

    print("[TrainEval] Evaluation stage: validation split (adaptive-state=OFF, skip-tune=ON)")
    eval_validation_det = run_loop_once(
        args,
        report_path,
        enable_adaptive_state_io=False,
        datasets=split_datasets["validation"],
        stage_name="eval_validation_deterministic",
        skip_tune=True,
        entry_rejection_output_root=entry_rejection_output_root,
    )
    entry_rejection_validation_det = read_entry_rejection_snapshot(
        entry_rejection_paths_for_stage(entry_rejection_output_root, "eval_validation_deterministic")["summary_json"]
    )
    entry_rejection_validation_taxonomy_det = read_strategy_rejection_taxonomy_snapshot(
        entry_rejection_paths_for_stage(entry_rejection_output_root, "eval_validation_deterministic")["taxonomy_json"]
    )
    live_signal_funnel_validation_det = capture_live_signal_funnel_snapshot_for_stage(
        source_path=live_signal_funnel_source_path,
        output_root=entry_rejection_output_root,
        stage_name="eval_validation_deterministic",
    )
    summary["stages"].append(
        {
            "stage": "eval_validation_deterministic",
            "split": "validation",
            "adaptive_state_io": False,
            "dataset_count": len(split_datasets["validation"]),
            "skip_tune": True,
            "snapshot": eval_validation_det,
            "entry_rejection": entry_rejection_validation_det,
            "entry_rejection_taxonomy": entry_rejection_validation_taxonomy_det,
            "live_signal_funnel": live_signal_funnel_validation_det,
        }
    )

    eval_holdout_det = None
    live_signal_funnel_holdout_det = None
    if split_datasets["holdout"]:
        print("[TrainEval] Evaluation stage: holdout split (adaptive-state=OFF, skip-tune=ON)")
        eval_holdout_det = run_loop_once(
            args,
            report_path,
            enable_adaptive_state_io=False,
            datasets=split_datasets["holdout"],
            stage_name="eval_holdout_deterministic",
            skip_tune=True,
            entry_rejection_output_root=entry_rejection_output_root,
        )
        entry_rejection_holdout_det = read_entry_rejection_snapshot(
            entry_rejection_paths_for_stage(entry_rejection_output_root, "eval_holdout_deterministic")["summary_json"]
        )
        entry_rejection_holdout_taxonomy_det = read_strategy_rejection_taxonomy_snapshot(
            entry_rejection_paths_for_stage(entry_rejection_output_root, "eval_holdout_deterministic")["taxonomy_json"]
        )
        live_signal_funnel_holdout_det = capture_live_signal_funnel_snapshot_for_stage(
            source_path=live_signal_funnel_source_path,
            output_root=entry_rejection_output_root,
            stage_name="eval_holdout_deterministic",
        )
        summary["stages"].append(
            {
                "stage": "eval_holdout_deterministic",
                "split": "holdout",
                "adaptive_state_io": False,
                "dataset_count": len(split_datasets["holdout"]),
                "skip_tune": True,
                "snapshot": eval_holdout_det,
                "entry_rejection": entry_rejection_holdout_det,
                "entry_rejection_taxonomy": entry_rejection_holdout_taxonomy_det,
                "live_signal_funnel": live_signal_funnel_holdout_det,
            }
        )

    if args.run_learned_eval:
        print("[TrainEval] Evaluation stage: validation split (adaptive-state=ON, learned carry)")
        eval_validation_learned = run_loop_once(
            args,
            report_path,
            enable_adaptive_state_io=True,
            datasets=split_datasets["validation"],
            stage_name="eval_validation_learned",
            skip_tune=True,
            entry_rejection_output_root=entry_rejection_output_root,
        )
        entry_rejection_validation_learned = read_entry_rejection_snapshot(
            entry_rejection_paths_for_stage(entry_rejection_output_root, "eval_validation_learned")["summary_json"]
        )
        entry_rejection_validation_taxonomy_learned = read_strategy_rejection_taxonomy_snapshot(
            entry_rejection_paths_for_stage(entry_rejection_output_root, "eval_validation_learned")["taxonomy_json"]
        )
        live_signal_funnel_validation_learned = capture_live_signal_funnel_snapshot_for_stage(
            source_path=live_signal_funnel_source_path,
            output_root=entry_rejection_output_root,
            stage_name="eval_validation_learned",
        )
        summary["stages"].append(
            {
                "stage": "eval_validation_learned",
                "split": "validation",
                "adaptive_state_io": True,
                "dataset_count": len(split_datasets["validation"]),
                "skip_tune": True,
                "snapshot": eval_validation_learned,
                "entry_rejection": entry_rejection_validation_learned,
                "entry_rejection_taxonomy": entry_rejection_validation_taxonomy_learned,
                "live_signal_funnel": live_signal_funnel_validation_learned,
            }
        )

        if split_datasets["holdout"]:
            print("[TrainEval] Evaluation stage: holdout split (adaptive-state=ON, learned carry)")
            eval_holdout_learned = run_loop_once(
                args,
                report_path,
                enable_adaptive_state_io=True,
                datasets=split_datasets["holdout"],
                stage_name="eval_holdout_learned",
                skip_tune=True,
                entry_rejection_output_root=entry_rejection_output_root,
            )
            entry_rejection_holdout_learned = read_entry_rejection_snapshot(
                entry_rejection_paths_for_stage(entry_rejection_output_root, "eval_holdout_learned")["summary_json"]
            )
            entry_rejection_holdout_taxonomy_learned = read_strategy_rejection_taxonomy_snapshot(
                entry_rejection_paths_for_stage(entry_rejection_output_root, "eval_holdout_learned")["taxonomy_json"]
            )
            live_signal_funnel_holdout_learned = capture_live_signal_funnel_snapshot_for_stage(
                source_path=live_signal_funnel_source_path,
                output_root=entry_rejection_output_root,
                stage_name="eval_holdout_learned",
            )
            summary["stages"].append(
                {
                    "stage": "eval_holdout_learned",
                    "split": "holdout",
                    "adaptive_state_io": True,
                    "dataset_count": len(split_datasets["holdout"]),
                    "skip_tune": True,
                    "snapshot": eval_holdout_learned,
                    "entry_rejection": entry_rejection_holdout_learned,
                    "entry_rejection_taxonomy": entry_rejection_holdout_taxonomy_learned,
                    "live_signal_funnel": live_signal_funnel_holdout_learned,
                }
            )

    wf_validation = run_walk_forward_batch(args, split_datasets["validation"], "validation")
    wf_holdout = None
    if split_datasets["holdout"]:
        wf_holdout = run_walk_forward_batch(args, split_datasets["holdout"], "holdout")

    summary["walk_forward_validation"] = wf_validation
    summary["walk_forward_holdout"] = wf_holdout
    summary["promotion_verdict"] = build_promotion_verdict(
        train_snapshot_bundle=latest_train_snapshot,
        validation_snapshot_bundle=eval_validation_det,
        holdout_snapshot_bundle=eval_holdout_det,
        wf_validation=wf_validation,
        wf_holdout=wf_holdout,
        validation_live_funnel_snapshot=live_signal_funnel_validation_det,
        holdout_live_funnel_snapshot=live_signal_funnel_holdout_det,
    )

    summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("[TrainEval] Completed")
    print(f"split_manifest_json={split_manifest_path}")
    print(f"summary_json={summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
