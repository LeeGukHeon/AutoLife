#!/usr/bin/env python3
import argparse
import csv
import json
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import analyze_loss_contributors
import run_realdata_candidate_loop
import tune_candidate_gate_trade_density
from _script_common import resolve_repo_path, verification_lock


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--real-data-loop-script", "-RealDataLoopScript", default="./scripts/run_realdata_candidate_loop.py")
    parser.add_argument("--tune-script", "-TuneScript", default="./scripts/tune_candidate_gate_trade_density.py")
    parser.add_argument("--loss-analysis-script", "-LossAnalysisScript", default="./scripts/analyze_loss_contributors.py")
    parser.add_argument("--build-config-path", "-BuildConfigPath", default="./build/Release/config/config.json")
    parser.add_argument("--source-config-path", "-SourceConfigPath", default="./config/config.json")
    parser.add_argument(
        "--gate-report-json",
        "-GateReportJson",
        default="./build/Release/logs/profitability_gate_report_realdata.json",
    )
    parser.add_argument(
        "--tune-summary-json",
        "-TuneSummaryJson",
        default="./build/Release/logs/candidate_trade_density_tuning_summary.json",
    )
    parser.add_argument(
        "--iteration-csv",
        "-IterationCsv",
        default="./build/Release/logs/candidate_auto_improvement_iterations.csv",
    )
    parser.add_argument(
        "--summary-json",
        "-SummaryJson",
        default="./build/Release/logs/candidate_auto_improvement_summary.json",
    )
    parser.add_argument("--max-iterations", "-MaxIterations", type=int, default=4)
    parser.add_argument("--max-consecutive-no-improvement", "-MaxConsecutiveNoImprovement", type=int, default=2)
    parser.add_argument("--max-runtime-minutes", "-MaxRuntimeMinutes", type=int, default=120)
    parser.add_argument("--min-profit-factor", "-MinProfitFactor", type=float, default=1.00)
    parser.add_argument("--min-expectancy-krw", "-MinExpectancyKrw", type=float, default=0.0)
    parser.add_argument("--min-profitable-ratio", "-MinProfitableRatio", type=float, default=0.55)
    parser.add_argument("--min-avg-win-rate-pct", "-MinAvgWinRatePct", type=float, default=48.0)
    parser.add_argument("--min-avg-trades", "-MinAvgTrades", type=float, default=8.0)
    parser.add_argument("--improvement-epsilon", "-ImprovementEpsilon", type=float, default=0.05)
    parser.add_argument("--tune-scenario-mode", "-TuneScenarioMode", default="diverse_light")
    parser.add_argument("--tune-max-scenarios", "-TuneMaxScenarios", type=int, default=16)
    parser.add_argument("--tune-include-legacy-scenarios", "-TuneIncludeLegacyScenarios", action="store_true")
    parser.add_argument("--tune-screen-dataset-limit", "-TuneScreenDatasetLimit", type=int, default=8)
    parser.add_argument("--tune-screen-top-k", "-TuneScreenTopK", type=int, default=6)
    parser.add_argument("--tune-objective-min-avg-trades", "-TuneObjectiveMinAvgTrades", type=float, default=None)
    parser.add_argument("--tune-objective-min-profitable-ratio", "-TuneObjectiveMinProfitableRatio", type=float, default=None)
    parser.add_argument("--tune-objective-min-avg-win-rate-pct", "-TuneObjectiveMinAvgWinRatePct", type=float, default=None)
    parser.add_argument("--tune-objective-min-expectancy-krw", "-TuneObjectiveMinExpectancyKrw", type=float, default=None)
    parser.add_argument(
        "--tune-objective-mode",
        "-TuneObjectiveMode",
        choices=["balanced", "profitable_ratio_priority"],
        default="balanced",
    )
    parser.add_argument("--real-data-only", "-RealDataOnly", action="store_true")
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
    parser.add_argument("--fetch-each-iteration", "-FetchEachIteration", action="store_true")
    parser.add_argument("--skip-tune-phase", "-SkipTunePhase", action="store_true")
    parser.add_argument("--run-loss-analysis", "-RunLossAnalysis", action="store_true")
    parser.add_argument("--emit-strict-adaptive-pair", "-EmitStrictAdaptivePair", action="store_true")
    parser.add_argument(
        "--enable-adaptive-state-io",
        "-EnableAdaptiveStateIo",
        action="store_true",
        help="Enable adaptive strategy state I/O during matrix backtests inside loop.",
    )
    parser.add_argument("--sync-source-config", "-SyncSourceConfig", action="store_true")
    parser.add_argument("--matrix-max-workers", "-MatrixMaxWorkers", type=int, default=1)
    parser.add_argument("--matrix-backtest-retry-count", "-MatrixBacktestRetryCount", type=int, default=2)
    parser.add_argument("--verification-lock-path", "-VerificationLockPath", default="./build/Release/logs/verification_run.lock")
    parser.add_argument("--verification-lock-timeout-sec", "-VerificationLockTimeoutSec", type=int, default=1800)
    parser.add_argument("--verification-lock-stale-sec", "-VerificationLockStaleSec", type=int, default=14400)
    parser.add_argument(
        "--enable-hostility-adaptive-targets",
        "-EnableHostilityAdaptiveTargets",
        dest="enable_hostility_adaptive_targets",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-hostility-adaptive-targets",
        dest="enable_hostility_adaptive_targets",
        action="store_false",
    )
    return parser.parse_args(argv)


def compute_objective_score(
    avg_profit_factor: float,
    avg_expectancy_krw: float,
    profitable_ratio: float,
    avg_win_rate_pct: float,
    avg_total_trades: float,
    min_trades_floor: float,
    min_profitable_ratio: float,
    min_avg_win_rate_pct: float,
    min_expectancy_krw: float,
) -> float:
    penalty = 0.0
    if avg_total_trades < min_trades_floor:
        penalty += 6000.0 + ((min_trades_floor - avg_total_trades) * 800.0)
    if profitable_ratio < min_profitable_ratio:
        penalty += 6000.0 + ((min_profitable_ratio - profitable_ratio) * 9000.0)
    if avg_win_rate_pct < min_avg_win_rate_pct:
        penalty += 4000.0 + ((min_avg_win_rate_pct - avg_win_rate_pct) * 180.0)
    if avg_expectancy_krw < min_expectancy_krw:
        penalty += 6000.0 + ((min_expectancy_krw - avg_expectancy_krw) * 120.0)
    if avg_profit_factor < 1.0:
        penalty += (1.0 - avg_profit_factor) * 2500.0

    if penalty > 0.0:
        return round(-penalty + (avg_profit_factor * 10.0), 6)

    score = 0.0
    score += (avg_expectancy_krw * 25.0)
    score += (profitable_ratio * 4000.0)
    score += (avg_win_rate_pct * 40.0)
    score += ((avg_profit_factor - 1.0) * 300.0)
    score += (min(avg_total_trades, 30.0) * 40.0)
    return round(score, 6)


def target_satisfied(
    core_summary: Optional[Dict[str, Any]],
    min_pf: float,
    min_exp: float,
    min_ratio: float,
    min_trades: float,
    min_win_rate_pct: float,
) -> bool:
    if core_summary is None:
        return False
    return (
        float(core_summary.get("avg_profit_factor", 0.0)) >= min_pf
        and float(core_summary.get("avg_expectancy_krw", 0.0)) >= min_exp
        and float(core_summary.get("profitable_ratio", 0.0)) >= min_ratio
        and float(core_summary.get("avg_total_trades", 0.0)) >= min_trades
        and float(core_summary.get("avg_win_rate_pct", 0.0)) >= min_win_rate_pct
    )


def resolve_active_thresholds(
    report: Dict[str, Any],
    requested: Dict[str, float],
    use_effective: bool,
) -> Dict[str, Any]:
    thresholds = report.get("thresholds") or {}
    hostility_bundle = thresholds.get("hostility_adaptive") or {}
    effective = hostility_bundle.get("effective") or {}
    active = dict(requested)
    if use_effective and effective:
        active["min_profit_factor"] = float(effective.get("min_profit_factor", active["min_profit_factor"]))
        active["min_expectancy_krw"] = float(effective.get("min_expectancy_krw", active["min_expectancy_krw"]))
        active["min_profitable_ratio"] = float(effective.get("min_profitable_ratio", active["min_profitable_ratio"]))
        active["min_avg_win_rate_pct"] = float(effective.get("min_avg_win_rate_pct", active["min_avg_win_rate_pct"]))
        active["min_avg_trades"] = float(effective.get("min_avg_trades", active["min_avg_trades"]))
    return {
        "requested": requested,
        "active": active,
        "hostility": hostility_bundle.get("hostility") or {},
        "adaptive_applied": bool(use_effective and bool(effective)),
    }


def get_core_snapshot(
    report_path,
    min_profit_factor: float,
    min_trades_floor: float,
    min_profitable_ratio: float,
    min_avg_win_rate_pct: float,
    min_expectancy_krw: float,
    use_effective_targets: bool,
) -> Dict[str, Any]:
    report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    core = next((x for x in report.get("profile_summaries", []) if x.get("profile_id") == "core_full"), None)
    if core is None:
        raise RuntimeError(f"core_full summary not found: {report_path}")
    threshold_ctx = resolve_active_thresholds(
        report=report,
        requested={
            "min_profit_factor": float(min_profit_factor),
            "min_expectancy_krw": float(min_expectancy_krw),
            "min_profitable_ratio": float(min_profitable_ratio),
            "min_avg_win_rate_pct": float(min_avg_win_rate_pct),
            "min_avg_trades": float(min_trades_floor),
        },
        use_effective=bool(use_effective_targets),
    )
    active = threshold_ctx["active"]
    objective = compute_objective_score(
        float(core.get("avg_profit_factor", 0.0)),
        float(core.get("avg_expectancy_krw", 0.0)),
        float(core.get("profitable_ratio", 0.0)),
        float(core.get("avg_win_rate_pct", 0.0)),
        float(core.get("avg_total_trades", 0.0)),
        float(active["min_avg_trades"]),
        float(active["min_profitable_ratio"]),
        float(active["min_avg_win_rate_pct"]),
        float(active["min_expectancy_krw"]),
    )
    return {
        "report": report,
        "core": core,
        "objective_score": objective,
        "threshold_context": threshold_ctx,
        "active_thresholds": active,
        "overall_gate_pass": bool(report.get("overall_gate_pass", False)),
        "core_vs_legacy_gate_pass": bool((report.get("core_vs_legacy") or {}).get("gate_pass", False)),
    }


def apply_combo_to_config_object(config_obj: Dict[str, Any], combo: Dict[str, Any]) -> None:
    trading = config_obj.setdefault("trading", {})
    for key, cast in [
        ("max_new_orders_per_scan", int),
        ("min_expected_edge_pct", float),
        ("min_reward_risk", float),
        ("min_rr_weak_signal", float),
        ("min_rr_strong_signal", float),
        ("min_strategy_trades_for_ev", int),
        ("min_strategy_expectancy_krw", float),
        ("min_strategy_profit_factor", float),
        ("avoid_high_volatility", bool),
        ("avoid_trending_down", bool),
        ("hostility_ewma_alpha", float),
        ("hostility_hostile_threshold", float),
        ("hostility_severe_threshold", float),
        ("hostility_extreme_threshold", float),
        ("hostility_pause_scans", int),
        ("hostility_pause_scans_extreme", int),
        ("hostility_pause_recent_sample_min", int),
        ("hostility_pause_recent_expectancy_krw", float),
        ("hostility_pause_recent_win_rate", float),
        ("backtest_hostility_pause_candles", int),
        ("backtest_hostility_pause_candles_extreme", int),
    ]:
        if key in combo:
            trading[key] = cast(combo[key])

    strategies = config_obj.setdefault("strategies", {})
    for strategy_name in ("scalping", "momentum", "breakout", "mean_reversion"):
        strategies.setdefault(strategy_name, {})
    mapping = [
        ("scalping", "scalping_min_signal_strength"),
        ("momentum", "momentum_min_signal_strength"),
        ("breakout", "breakout_min_signal_strength"),
        ("mean_reversion", "mean_reversion_min_signal_strength"),
    ]
    for strategy_name, key in mapping:
        if key in combo:
            strategies[strategy_name]["min_signal_strength"] = float(combo[key])


def apply_combo_to_config_files(build_config_path, source_config_path, combo, sync_source):
    build_cfg = json.loads(build_config_path.read_text(encoding="utf-8-sig"))
    apply_combo_to_config_object(build_cfg, combo)
    build_config_path.write_text(json.dumps(build_cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if sync_source and source_config_path.exists():
        source_cfg = json.loads(source_config_path.read_text(encoding="utf-8-sig"))
        apply_combo_to_config_object(source_cfg, combo)
        source_config_path.write_text(json.dumps(source_cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def select_best_combo_from_tune_summary(
    tune_summary_path,
    min_trades_floor: float,
    min_profitable_ratio: float,
    min_avg_win_rate_pct: float,
    min_expectancy_krw: float,
) -> Dict[str, Any]:
    payload = json.loads(tune_summary_path.read_text(encoding="utf-8-sig"))
    combos = payload.get("combos")
    summary = payload.get("summary")
    if not isinstance(combos, list) or not isinstance(summary, list):
        raise RuntimeError(f"Invalid tune summary json: {tune_summary_path}")
    combo_by_id = {}
    for combo in combos:
        if isinstance(combo, dict):
            combo_by_id[str(combo.get("combo_id", ""))] = combo

    candidates = []
    for row in summary:
        if not isinstance(row, dict):
            continue
        combo_id = str(row.get("combo_id", ""))
        if combo_id not in combo_by_id:
            continue
        objective = compute_objective_score(
            float(row.get("avg_profit_factor", 0.0)),
            float(row.get("avg_expectancy_krw", 0.0)),
            float(row.get("profitable_ratio", 0.0)),
            float(row.get("avg_win_rate_pct", 0.0)),
            float(row.get("avg_total_trades", 0.0)),
            min_trades_floor,
            min_profitable_ratio,
            min_avg_win_rate_pct,
            min_expectancy_krw,
        )
        gate_bonus = 0.0
        if bool(row.get("overall_gate_pass", False)):
            gate_bonus += 300.0
        if bool(row.get("profile_gate_pass", False)):
            gate_bonus += 80.0
        if bool(row.get("gate_profit_factor_pass", False)):
            gate_bonus += 60.0
        if bool(row.get("gate_trades_pass", False)):
            gate_bonus += 40.0
        candidates.append(
            {
                "combo_id": combo_id,
                "combo": combo_by_id[combo_id],
                "objective_score": objective,
                "objective_with_gate_bonus": round(objective + gate_bonus, 6),
                "avg_profit_factor": float(row.get("avg_profit_factor", 0.0)),
                "avg_expectancy_krw": float(row.get("avg_expectancy_krw", 0.0)),
                "profitable_ratio": float(row.get("profitable_ratio", 0.0)),
                "avg_win_rate_pct": float(row.get("avg_win_rate_pct", 0.0)),
                "avg_total_trades": float(row.get("avg_total_trades", 0.0)),
                "overall_gate_pass": bool(row.get("overall_gate_pass", False)),
                "profile_gate_pass": bool(row.get("profile_gate_pass", False)),
                "report_json": str(row.get("report_json", "")),
            }
        )
    if not candidates:
        raise RuntimeError("No candidate combo rows from tuning summary.")
    candidates.sort(
        key=lambda x: (
            float(x["objective_with_gate_bonus"]),
            float(x["avg_expectancy_krw"]),
            float(x["avg_win_rate_pct"]),
            float(x["profitable_ratio"]),
            float(x["avg_profit_factor"]),
            float(x["avg_total_trades"]),
        ),
        reverse=True,
    )
    return candidates[0]


def main(argv=None) -> int:
    args = parse_args(argv)
    build_config_path = resolve_repo_path(args.build_config_path)
    source_config_path = resolve_repo_path(args.source_config_path)
    gate_report_path = resolve_repo_path(args.gate_report_json)
    tune_summary_path = resolve_repo_path(args.tune_summary_json)
    iteration_csv = resolve_repo_path(args.iteration_csv)
    summary_json = resolve_repo_path(args.summary_json)
    lock_path = resolve_repo_path(args.verification_lock_path)
    iteration_csv.parent.mkdir(parents=True, exist_ok=True)
    summary_json.parent.mkdir(parents=True, exist_ok=True)

    status = "running"
    reason = ""
    rows = []
    started_at = datetime.now(tz=timezone.utc)
    best_objective = float("-inf")
    best_snapshot: Optional[Dict[str, Any]] = None
    best_combo_id = ""
    last_applied_combo: Optional[Dict[str, Any]] = None
    last_applied_combo_id = ""
    consecutive_no_improve = 0
    tune_objective_min_avg_trades = (
        float(args.tune_objective_min_avg_trades)
        if args.tune_objective_min_avg_trades is not None
        else float(args.min_avg_trades)
    )
    tune_objective_min_profitable_ratio = (
        float(args.tune_objective_min_profitable_ratio)
        if args.tune_objective_min_profitable_ratio is not None
        else float(args.min_profitable_ratio)
    )
    tune_objective_min_avg_win_rate_pct = (
        float(args.tune_objective_min_avg_win_rate_pct)
        if args.tune_objective_min_avg_win_rate_pct is not None
        else float(args.min_avg_win_rate_pct)
    )
    tune_objective_min_expectancy_krw = (
        float(args.tune_objective_min_expectancy_krw)
        if args.tune_objective_min_expectancy_krw is not None
        else float(args.min_expectancy_krw)
    )

    with verification_lock(
        lock_path,
        timeout_sec=int(args.verification_lock_timeout_sec),
        stale_sec=int(args.verification_lock_stale_sec),
    ):
        for iteration in range(1, int(args.max_iterations) + 1):
            elapsed_minutes = (datetime.now(tz=timezone.utc) - started_at).total_seconds() / 60.0
            if elapsed_minutes >= float(args.max_runtime_minutes):
                status = "paused_runtime_limit"
                reason = "Max runtime exceeded before iteration start."
                break

            print(f"[AutoImprove] Iteration {iteration}/{args.max_iterations} - baseline matrix run")
            real_loop_argv = []
            if not args.fetch_each_iteration:
                real_loop_argv.append("--skip-fetch")
            real_loop_argv.append("--skip-tune")
            if args.real_data_only:
                real_loop_argv.append("--real-data-only")
            real_loop_argv.extend(["--gate-min-avg-trades", str(int(round(float(args.min_avg_trades))))])
            real_loop_argv.extend(["--matrix-max-workers", str(max(1, int(args.matrix_max_workers)))])
            real_loop_argv.extend(["--matrix-backtest-retry-count", str(max(1, int(args.matrix_backtest_retry_count)))])
            if args.enable_hostility_adaptive_targets:
                real_loop_argv.append("--enable-hostility-adaptive-thresholds")
            else:
                real_loop_argv.append("--disable-hostility-adaptive-thresholds")
            if args.require_higher_tf_companions:
                real_loop_argv.append("--require-higher-tf-companions")
            else:
                real_loop_argv.append("--allow-missing-higher-tf-companions")
            if args.emit_strict_adaptive_pair:
                real_loop_argv.append("--run-both-hostility-modes")
            if args.enable_adaptive_state_io:
                real_loop_argv.append("--enable-adaptive-state-io")
            rc = run_realdata_candidate_loop.main(real_loop_argv)
            if rc != 0:
                raise RuntimeError(f"Realdata candidate loop (baseline) failed (exit={rc})")
            if not gate_report_path.exists():
                raise RuntimeError(f"Gate report not found after baseline run: {gate_report_path}")
    
            snapshot = get_core_snapshot(
                gate_report_path,
                float(args.min_profit_factor),
                tune_objective_min_avg_trades,
                tune_objective_min_profitable_ratio,
                tune_objective_min_avg_win_rate_pct,
                tune_objective_min_expectancy_krw,
                bool(args.enable_hostility_adaptive_targets),
            )
            core = snapshot["core"]
            active_thresholds = snapshot["active_thresholds"]
            is_target = target_satisfied(
                core,
                float(active_thresholds["min_profit_factor"]),
                float(active_thresholds["min_expectancy_krw"]),
                float(active_thresholds["min_profitable_ratio"]),
                float(active_thresholds["min_avg_trades"]),
                float(active_thresholds["min_avg_win_rate_pct"]),
            )
            tune_min_avg_trades_iter = float(tune_objective_min_avg_trades)
            tune_min_profitable_ratio_iter = float(tune_objective_min_profitable_ratio)
            tune_min_avg_win_rate_iter = float(tune_objective_min_avg_win_rate_pct)
            tune_min_expectancy_iter = float(tune_objective_min_expectancy_krw)
            gate_min_avg_trades_iter = int(round(float(args.min_avg_trades)))
            if args.enable_hostility_adaptive_targets:
                tune_min_avg_trades_iter = min(tune_min_avg_trades_iter, float(active_thresholds["min_avg_trades"]))
                tune_min_profitable_ratio_iter = min(
                    tune_min_profitable_ratio_iter,
                    float(active_thresholds["min_profitable_ratio"]),
                )
                tune_min_avg_win_rate_iter = min(
                    tune_min_avg_win_rate_iter,
                    float(active_thresholds["min_avg_win_rate_pct"]),
                )
                tune_min_expectancy_iter = min(
                    tune_min_expectancy_iter,
                    float(active_thresholds["min_expectancy_krw"]),
                )
                gate_min_avg_trades_iter = int(round(min(float(args.min_avg_trades), float(active_thresholds["min_avg_trades"]))))
            gate_min_avg_trades_iter = max(1, gate_min_avg_trades_iter)
            rows.append(
                {
                    "iteration": iteration,
                    "phase": "baseline",
                    "selected_combo": "",
                    "post_apply_skipped_same_combo": False,
                    "overall_gate_pass": bool(snapshot["overall_gate_pass"]),
                    "core_vs_legacy_gate_pass": bool(snapshot["core_vs_legacy_gate_pass"]),
                    "core_full_gate_pass": bool(core.get("gate_pass", False)),
                    "avg_profit_factor": float(core.get("avg_profit_factor", 0.0)),
                    "avg_expectancy_krw": float(core.get("avg_expectancy_krw", 0.0)),
                    "avg_total_trades": float(core.get("avg_total_trades", 0.0)),
                    "avg_win_rate_pct": float(core.get("avg_win_rate_pct", 0.0)),
                    "profitable_ratio": float(core.get("profitable_ratio", 0.0)),
                    "objective_score": float(snapshot["objective_score"]),
                    "active_min_profit_factor": float(active_thresholds["min_profit_factor"]),
                    "active_min_expectancy_krw": float(active_thresholds["min_expectancy_krw"]),
                    "active_min_profitable_ratio": float(active_thresholds["min_profitable_ratio"]),
                    "active_min_avg_win_rate_pct": float(active_thresholds["min_avg_win_rate_pct"]),
                    "active_min_avg_trades": float(active_thresholds["min_avg_trades"]),
                    "target_satisfied": bool(is_target),
                    "timestamp": datetime.now(tz=timezone.utc).isoformat(),
                }
            )
    
            if float(snapshot["objective_score"]) > (best_objective + float(args.improvement_epsilon)):
                best_objective = float(snapshot["objective_score"])
                best_snapshot = snapshot
                best_combo_id = ""
                consecutive_no_improve = 0
            else:
                consecutive_no_improve += 1
    
            if is_target and bool(snapshot["overall_gate_pass"]):
                status = "success_gate_pass"
                reason = "Target metrics and overall gate passed on baseline run."
                break
    
            if args.skip_tune_phase:
                if consecutive_no_improve >= int(args.max_consecutive_no_improvement):
                    status = "paused_no_improvement"
                    reason = "No objective improvement within limit while tune phase skipped."
                    break
                continue
    
            elapsed_before_tune = (datetime.now(tz=timezone.utc) - started_at).total_seconds() / 60.0
            if elapsed_before_tune >= float(args.max_runtime_minutes):
                status = "paused_runtime_limit"
                reason = "Max runtime reached before tune phase."
                break
    
            print(f"[AutoImprove] Iteration {iteration}/{args.max_iterations} - tuning combos")
            tune_argv = [
                "--scenario-mode",
                str(args.tune_scenario_mode),
                "--max-scenarios",
                str(args.tune_max_scenarios),
                "--matrix-max-workers",
                str(max(1, int(args.matrix_max_workers))),
                "--matrix-backtest-retry-count",
                str(max(1, int(args.matrix_backtest_retry_count))),
                "--screen-dataset-limit",
                str(args.tune_screen_dataset_limit),
                "--screen-top-k",
                str(args.tune_screen_top_k),
                "--objective-min-avg-trades",
                str(tune_min_avg_trades_iter),
                "--gate-min-avg-trades",
                str(gate_min_avg_trades_iter),
                "--objective-min-profitable-ratio",
                str(tune_min_profitable_ratio_iter),
                "--objective-min-avg-win-rate-pct",
                str(tune_min_avg_win_rate_iter),
                "--objective-min-expectancy-krw",
                str(tune_min_expectancy_iter),
                "--objective-mode",
                str(args.tune_objective_mode),
            ]
            if args.tune_include_legacy_scenarios:
                tune_argv.append("--include-legacy-scenarios")
            if args.real_data_only:
                tune_argv.append("--real-data-only")
            if args.require_higher_tf_companions:
                tune_argv.append("--require-higher-tf-companions")
            else:
                tune_argv.append("--allow-missing-higher-tf-companions")
            if args.enable_hostility_adaptive_targets:
                tune_argv.append("--enable-hostility-adaptive-thresholds")
                tune_argv.append("--use-effective-thresholds-for-objective")
            else:
                tune_argv.append("--disable-hostility-adaptive-thresholds")
                tune_argv.append("--disable-effective-thresholds-for-objective")
            rc = tune_candidate_gate_trade_density.main(tune_argv)
            if rc != 0:
                raise RuntimeError(f"Candidate tuning failed (exit={rc})")
            if not tune_summary_path.exists():
                raise RuntimeError(f"Tune summary json not found: {tune_summary_path}")
    
            best_combo = select_best_combo_from_tune_summary(
                tune_summary_path,
                tune_min_avg_trades_iter,
                tune_min_profitable_ratio_iter,
                tune_min_avg_win_rate_iter,
                tune_min_expectancy_iter,
            )
            print(
                f"[AutoImprove] Iteration {iteration} selected_combo={best_combo['combo_id']} "
                f"objective={best_combo['objective_with_gate_bonus']}"
            )
            selected_combo_id = str(best_combo["combo_id"])
            selected_combo_payload = dict(best_combo["combo"])
            skip_post_apply_run = (
                selected_combo_id == last_applied_combo_id
                and last_applied_combo is not None
                and selected_combo_payload == last_applied_combo
            )
            if skip_post_apply_run:
                print(
                    f"[AutoImprove] Iteration {iteration} post-apply validation skipped "
                    f"(combo unchanged: {selected_combo_id})"
                )
                post_snapshot = snapshot
            else:
                apply_combo_to_config_files(
                    build_config_path,
                    source_config_path,
                    best_combo["combo"],
                    bool(args.sync_source_config),
                )
                last_applied_combo_id = selected_combo_id
                last_applied_combo = selected_combo_payload
    
                print(f"[AutoImprove] Iteration {iteration}/{args.max_iterations} - post-apply validation run")
                rc = run_realdata_candidate_loop.main(real_loop_argv)
                if rc != 0:
                    raise RuntimeError(f"Realdata candidate loop (post-apply) failed (exit={rc})")
                if not gate_report_path.exists():
                    raise RuntimeError(f"Gate report not found after post-apply run: {gate_report_path}")
    
                post_snapshot = get_core_snapshot(
                    gate_report_path,
                    float(args.min_profit_factor),
                    tune_objective_min_avg_trades,
                    tune_objective_min_profitable_ratio,
                    tune_objective_min_avg_win_rate_pct,
                    tune_objective_min_expectancy_krw,
                    bool(args.enable_hostility_adaptive_targets),
                )
            post_core = post_snapshot["core"]
            post_active_thresholds = post_snapshot["active_thresholds"]
            post_target = target_satisfied(
                post_core,
                float(post_active_thresholds["min_profit_factor"]),
                float(post_active_thresholds["min_expectancy_krw"]),
                float(post_active_thresholds["min_profitable_ratio"]),
                float(post_active_thresholds["min_avg_trades"]),
                float(post_active_thresholds["min_avg_win_rate_pct"]),
            )
            rows.append(
                {
                    "iteration": iteration,
                    "phase": "post_apply",
                    "selected_combo": selected_combo_id,
                    "post_apply_skipped_same_combo": bool(skip_post_apply_run),
                    "overall_gate_pass": bool(post_snapshot["overall_gate_pass"]),
                    "core_vs_legacy_gate_pass": bool(post_snapshot["core_vs_legacy_gate_pass"]),
                    "core_full_gate_pass": bool(post_core.get("gate_pass", False)),
                    "avg_profit_factor": float(post_core.get("avg_profit_factor", 0.0)),
                    "avg_expectancy_krw": float(post_core.get("avg_expectancy_krw", 0.0)),
                    "avg_total_trades": float(post_core.get("avg_total_trades", 0.0)),
                    "avg_win_rate_pct": float(post_core.get("avg_win_rate_pct", 0.0)),
                    "profitable_ratio": float(post_core.get("profitable_ratio", 0.0)),
                    "objective_score": float(post_snapshot["objective_score"]),
                    "active_min_profit_factor": float(post_active_thresholds["min_profit_factor"]),
                    "active_min_expectancy_krw": float(post_active_thresholds["min_expectancy_krw"]),
                    "active_min_profitable_ratio": float(post_active_thresholds["min_profitable_ratio"]),
                    "active_min_avg_win_rate_pct": float(post_active_thresholds["min_avg_win_rate_pct"]),
                    "active_min_avg_trades": float(post_active_thresholds["min_avg_trades"]),
                    "target_satisfied": bool(post_target),
                    "timestamp": datetime.now(tz=timezone.utc).isoformat(),
                }
            )
    
            if float(post_snapshot["objective_score"]) > (best_objective + float(args.improvement_epsilon)):
                best_objective = float(post_snapshot["objective_score"])
                best_snapshot = post_snapshot
                best_combo_id = str(best_combo["combo_id"])
                consecutive_no_improve = 0
            else:
                consecutive_no_improve += 1
    
            if args.run_loss_analysis:
                print(f"[AutoImprove] Iteration {iteration}/{args.max_iterations} - loss contributor analysis")
                rc = analyze_loss_contributors.main([])
                if rc != 0:
                    raise RuntimeError(f"Loss contributor analysis failed (exit={rc})")
    
            if post_target and bool(post_snapshot["overall_gate_pass"]):
                status = "success_gate_pass"
                reason = "Target metrics and overall gate passed on post-apply run."
                break
    
            if consecutive_no_improve >= int(args.max_consecutive_no_improvement):
                status = "paused_no_improvement"
                reason = "Objective score did not improve within configured consecutive limit."
                break
    
    if status == "running":
        if args.max_iterations > 0 and rows:
            status = "paused_max_iterations"
            reason = "Reached MaxIterations without full gate pass."
        else:
            status = "paused_no_data"
            reason = "No iteration rows produced."

    with iteration_csv.open("w", encoding="utf-8", newline="") as fh:
        if rows:
            writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        else:
            fh.write("")

    summary = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "status": status,
        "reason": reason,
        "started_at": started_at.isoformat(),
        "ended_at": datetime.now(tz=timezone.utc).isoformat(),
        "max_iterations": int(args.max_iterations),
        "max_runtime_minutes": int(args.max_runtime_minutes),
        "max_consecutive_no_improvement": int(args.max_consecutive_no_improvement),
        "tuning": {
            "scenario_mode": args.tune_scenario_mode,
            "max_scenarios": int(args.tune_max_scenarios),
            "include_legacy": bool(args.tune_include_legacy_scenarios),
            "screen_dataset_limit": int(args.tune_screen_dataset_limit),
            "screen_top_k": int(args.tune_screen_top_k),
            "objective_min_avg_trades": tune_objective_min_avg_trades,
            "objective_min_profitable_ratio": tune_objective_min_profitable_ratio,
            "objective_min_avg_win_rate_pct": tune_objective_min_avg_win_rate_pct,
            "objective_min_expectancy_krw": tune_objective_min_expectancy_krw,
        },
        "targets": {
            "min_profit_factor": float(args.min_profit_factor),
            "min_expectancy_krw": float(args.min_expectancy_krw),
            "min_profitable_ratio": float(args.min_profitable_ratio),
            "min_avg_win_rate_pct": float(args.min_avg_win_rate_pct),
            "min_avg_trades": float(args.min_avg_trades),
            "enable_hostility_adaptive_targets": bool(args.enable_hostility_adaptive_targets),
        },
        "best_objective_score": float(best_objective),
        "best_combo_id": best_combo_id,
        "best_snapshot": (
            {
                "overall_gate_pass": bool(best_snapshot["overall_gate_pass"]),
                "core_vs_legacy_gate_pass": bool(best_snapshot["core_vs_legacy_gate_pass"]),
                "avg_profit_factor": float(best_snapshot["core"].get("avg_profit_factor", 0.0)),
                "avg_expectancy_krw": float(best_snapshot["core"].get("avg_expectancy_krw", 0.0)),
                "avg_total_trades": float(best_snapshot["core"].get("avg_total_trades", 0.0)),
                "avg_win_rate_pct": float(best_snapshot["core"].get("avg_win_rate_pct", 0.0)),
                "profitable_ratio": float(best_snapshot["core"].get("profitable_ratio", 0.0)),
                "core_full_gate_pass": bool(best_snapshot["core"].get("gate_pass", False)),
                "active_thresholds": best_snapshot.get("active_thresholds"),
                "threshold_context": best_snapshot.get("threshold_context"),
            }
            if best_snapshot is not None
            else None
        ),
        "outputs": {
            "iteration_csv": str(iteration_csv),
            "summary_json": str(summary_json),
            "gate_report_json": str(gate_report_path),
            "tune_summary_json": str(tune_summary_path),
        },
        "iterations": rows,
    }
    summary_json.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("[AutoImprove] Completed")
    print(f"status={status}")
    print(f"reason={reason}")
    print(f"iteration_csv={iteration_csv}")
    print(f"summary_json={summary_json}")
    if best_snapshot is not None:
        print(f"best_objective={best_objective}")
        print(f"best_combo_id={best_combo_id}")
        print(f"best_avg_profit_factor={best_snapshot['core'].get('avg_profit_factor')}")
        print(f"best_avg_expectancy_krw={best_snapshot['core'].get('avg_expectancy_krw')}")
        print(f"best_avg_total_trades={best_snapshot['core'].get('avg_total_trades')}")
        print(f"best_avg_win_rate_pct={best_snapshot['core'].get('avg_win_rate_pct')}")
        print(f"best_profitable_ratio={best_snapshot['core'].get('profitable_ratio')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
