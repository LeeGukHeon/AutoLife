#!/usr/bin/env python3
import argparse
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List

import run_realdata_candidate_loop


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--train-iterations", "-TrainIterations", type=int, default=2)
    parser.add_argument("--skip-fetch", "-SkipFetch", action="store_true", default=True)
    parser.add_argument("--skip-tune", "-SkipTune", action="store_true", default=True)
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
    parser.add_argument("--run-both-hostility-modes", "-RunBothHostilityModes", action="store_true", default=True)
    parser.add_argument("--gate-min-avg-trades", "-GateMinAvgTrades", type=int, default=8)
    parser.add_argument("--matrix-max-workers", "-MatrixMaxWorkers", type=int, default=1)
    parser.add_argument("--matrix-backtest-retry-count", "-MatrixBacktestRetryCount", type=int, default=2)
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
        "--run-learned-eval",
        "-RunLearnedEval",
        action="store_true",
        help="Run additional evaluation with adaptive state I/O enabled.",
    )
    return parser.parse_args(argv)


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
    return {
        "overall_gate_pass": bool(report.get("overall_gate_pass")),
        "core_full": {
            "avg_profit_factor": core_full.get("avg_profit_factor"),
            "avg_total_trades": core_full.get("avg_total_trades"),
            "avg_expectancy_krw": core_full.get("avg_expectancy_krw"),
            "profitable_ratio": core_full.get("profitable_ratio"),
        },
    }


def derive_variant_path(base_path: Path, variant: str) -> Path:
    return base_path.with_name(f"{base_path.stem}_{variant}{base_path.suffix}")


def read_gate_snapshot_bundle(report_path: Path) -> Dict[str, Any]:
    selected = read_gate_snapshot(report_path)
    strict_path = derive_variant_path(report_path, "strict")
    adaptive_path = derive_variant_path(report_path, "adaptive")
    strict = read_gate_snapshot(strict_path) if strict_path.exists() else None
    adaptive = read_gate_snapshot(adaptive_path) if adaptive_path.exists() else None
    return {
        "selected": selected,
        "strict": strict,
        "adaptive": adaptive,
    }


def build_loop_argv(args, enable_adaptive_state_io: bool) -> List[str]:
    loop_argv: List[str] = []
    if args.skip_fetch:
        loop_argv.append("--skip-fetch")
    if args.skip_tune:
        loop_argv.append("--skip-tune")
    if args.real_data_only:
        loop_argv.append("--real-data-only")
    if args.require_higher_tf_companions:
        loop_argv.append("--require-higher-tf-companions")
    else:
        loop_argv.append("--allow-missing-higher-tf-companions")
    if args.run_both_hostility_modes:
        loop_argv.append("--run-both-hostility-modes")
    if args.enable_hostility_adaptive_thresholds:
        loop_argv.append("--enable-hostility-adaptive-thresholds")
    else:
        loop_argv.append("--disable-hostility-adaptive-thresholds")
    if enable_adaptive_state_io:
        loop_argv.append("--enable-adaptive-state-io")
    loop_argv.extend(["--gate-min-avg-trades", str(int(args.gate_min_avg_trades))])
    loop_argv.extend(["--matrix-max-workers", str(max(1, int(args.matrix_max_workers)))])
    loop_argv.extend(["--matrix-backtest-retry-count", str(max(1, int(args.matrix_backtest_retry_count)))])
    return loop_argv


def run_loop_once(args, report_path: Path, enable_adaptive_state_io: bool) -> Dict[str, Any]:
    rc = run_realdata_candidate_loop.main(build_loop_argv(args, enable_adaptive_state_io=enable_adaptive_state_io))
    if rc != 0:
        raise RuntimeError(f"run_realdata_candidate_loop failed (exit={rc})")
    if not report_path.exists():
        raise RuntimeError(f"Gate report not found: {report_path}")
    return read_gate_snapshot_bundle(report_path)


def build_promotion_verdict(eval_snapshot_bundle: Dict[str, Any]) -> Dict[str, Any]:
    strict = eval_snapshot_bundle.get("strict") or {}
    adaptive = eval_snapshot_bundle.get("adaptive") or {}
    strict_pass = bool(strict.get("overall_gate_pass")) if strict else False
    adaptive_pass = bool(adaptive.get("overall_gate_pass")) if adaptive else False
    both_pass = strict_pass and adaptive_pass

    if both_pass:
        recommendation = "promote_candidate"
    elif adaptive_pass and not strict_pass:
        recommendation = "hold_candidate_improve_strict_bottlenecks"
    else:
        recommendation = "hold_candidate_improve_both_modes"

    return {
        "strict_overall_gate_pass": strict_pass,
        "adaptive_overall_gate_pass": adaptive_pass,
        "both_modes_gate_pass": both_pass,
        "recommendation": recommendation,
    }


def main(argv=None) -> int:
    args = parse_args(argv)
    state_dir = Path(args.state_dir).resolve()
    snapshot_root = Path(args.state_snapshot_root).resolve()
    report_path = Path(args.gate_report_json).resolve()
    summary_path = Path(args.summary_json).resolve()
    summary_path.parent.mkdir(parents=True, exist_ok=True)

    summary: Dict[str, Any] = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "train_iterations": int(args.train_iterations),
        "state_dir": str(state_dir),
        "stages": [],
    }

    if args.clear_state_before_train:
        removed = clear_state_dir(state_dir)
        summary["state_cleared_before_train"] = removed
        print(f"[TrainEval] Cleared state files before training: {removed}")

    for i in range(1, max(1, int(args.train_iterations)) + 1):
        print(f"[TrainEval] Training iteration {i}/{args.train_iterations} (adaptive-state=ON)")
        snap = run_loop_once(args, report_path, enable_adaptive_state_io=True)
        summary["stages"].append({"stage": f"train_{i}", "adaptive_state_io": True, "snapshot": snap})

    snapshot_dir = snapshot_state_dir(state_dir, snapshot_root)
    summary["state_snapshot_dir"] = str(snapshot_dir)
    snapshot_files = sorted(p.name for p in snapshot_dir.glob("*") if p.is_file())
    summary["state_snapshot_files"] = snapshot_files
    print(f"[TrainEval] State snapshot saved: {snapshot_dir}")

    print("[TrainEval] Evaluation stage (adaptive-state=OFF, deterministic chain)")
    eval_det = run_loop_once(args, report_path, enable_adaptive_state_io=False)
    summary["stages"].append({"stage": "eval_deterministic", "adaptive_state_io": False, "snapshot": eval_det})
    summary["promotion_verdict"] = build_promotion_verdict(eval_det)

    if args.run_learned_eval:
        print("[TrainEval] Evaluation stage (adaptive-state=ON, learned carry mode)")
        eval_learned = run_loop_once(args, report_path, enable_adaptive_state_io=True)
        summary["stages"].append({"stage": "eval_learned", "adaptive_state_io": True, "snapshot": eval_learned})

    summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("[TrainEval] Completed")
    print(f"summary_json={summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
