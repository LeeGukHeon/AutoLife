#!/usr/bin/env python3
import argparse
import csv
import json
import pathlib
import subprocess
import sys
from copy import deepcopy
from typing import Any, Dict, List


def resolve_or_throw(path_value: str, label: str) -> pathlib.Path:
    p = pathlib.Path(path_value)
    if not p.is_absolute():
        p = (pathlib.Path.cwd() / p).resolve()
    if not p.exists():
        raise FileNotFoundError(f"{label} not found: {path_value}")
    return p


def ensure_parent_directory(path_value: pathlib.Path) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)


def set_or_add_property(obj: Dict[str, Any], name: str, value: Any) -> None:
    obj[name] = value


def ensure_strategy_node(cfg: Dict[str, Any], strategy_name: str) -> None:
    if "strategies" not in cfg or not isinstance(cfg["strategies"], dict):
        cfg["strategies"] = {}
    if strategy_name not in cfg["strategies"] or not isinstance(cfg["strategies"][strategy_name], dict):
        cfg["strategies"][strategy_name] = {}


def apply_candidate_combo_to_config(cfg: Dict[str, Any], combo: Dict[str, Any]) -> None:
    if "trading" not in cfg or not isinstance(cfg["trading"], dict):
        cfg["trading"] = {}
    t = cfg["trading"]
    for k in (
        "max_new_orders_per_scan",
        "min_expected_edge_pct",
        "min_reward_risk",
        "min_rr_weak_signal",
        "min_rr_strong_signal",
        "min_strategy_trades_for_ev",
        "min_strategy_expectancy_krw",
        "min_strategy_profit_factor",
        "avoid_high_volatility",
        "avoid_trending_down",
    ):
        set_or_add_property(t, k, combo[k])

    for strategy in ("scalping", "momentum", "breakout", "mean_reversion"):
        ensure_strategy_node(cfg, strategy)
    cfg["strategies"]["scalping"]["min_signal_strength"] = combo["scalping_min_signal_strength"]
    cfg["strategies"]["momentum"]["min_signal_strength"] = combo["momentum_min_signal_strength"]
    cfg["strategies"]["breakout"]["min_signal_strength"] = combo["breakout_min_signal_strength"]
    cfg["strategies"]["mean_reversion"]["min_signal_strength"] = combo["mean_reversion_min_signal_strength"]


def has_higher_tf_companions(primary_path: pathlib.Path) -> bool:
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


def get_dataset_list(dirs: List[pathlib.Path], only_real_data: bool, require_higher_tf: bool) -> List[pathlib.Path]:
    all_items: List[pathlib.Path] = []
    for dir_path in dirs:
        if not dir_path.exists():
            continue
        is_real = "backtest_real" in str(dir_path).lower()
        if only_real_data and not is_real:
            continue
        for f in sorted(dir_path.glob("*.csv"), key=lambda x: x.name.lower()):
            if is_real and "_1m_" not in f.name.lower():
                continue
            if only_real_data and (not is_real):
                continue
            if require_higher_tf and is_real and not has_higher_tf_companions(f):
                continue
            all_items.append(f.resolve())
    return sorted(set(all_items), key=lambda x: str(x).lower())


def new_combo_variant(base: Dict[str, Any], combo_id: str, description: str, overrides: Dict[str, Any]) -> Dict[str, Any]:
    clone = deepcopy(base)
    clone["combo_id"] = combo_id
    clone["description"] = description
    clone.update(overrides)
    return clone


def build_combo_specs(scenario_mode: str, include_legacy: bool, max_scenarios: int) -> List[Dict[str, Any]]:
    legacy = [
        {
            "combo_id": "baseline_current",
            "description": "Current baseline in build config.",
            "max_new_orders_per_scan": 2,
            "min_expected_edge_pct": 0.0010,
            "min_reward_risk": 1.20,
            "min_rr_weak_signal": 1.80,
            "min_rr_strong_signal": 1.20,
            "min_strategy_trades_for_ev": 30,
            "min_strategy_expectancy_krw": -2.0,
            "min_strategy_profit_factor": 0.95,
            "avoid_high_volatility": True,
            "avoid_trending_down": True,
            "scalping_min_signal_strength": 0.70,
            "momentum_min_signal_strength": 0.72,
            "breakout_min_signal_strength": 0.40,
            "mean_reversion_min_signal_strength": 0.40,
        }
    ]
    if scenario_mode == "legacy_only":
        combos = legacy
    else:
        base_balanced = legacy[0]
        generated: List[Dict[str, Any]] = []
        if scenario_mode in ("diverse_light", "diverse_wide"):
            edge_grid = [0.0006, 0.0008, 0.0010, 0.0012, 0.0014, 0.0016] if scenario_mode == "diverse_wide" else [0.0008, 0.0010, 0.0012, 0.0014]
            rr_grid = [1.05, 1.15, 1.25, 1.35] if scenario_mode == "diverse_wide" else [1.10, 1.20, 1.30]
            scalp_grid = [0.62, 0.66, 0.70, 0.74] if scenario_mode == "diverse_wide" else [0.64, 0.68, 0.72]
            mom_grid = [0.60, 0.64, 0.68, 0.72, 0.76] if scenario_mode == "diverse_wide" else [0.62, 0.68, 0.74]
            breakout_grid = [0.35, 0.40, 0.45] if scenario_mode == "diverse_wide" else [0.36, 0.42]
            mrev_grid = [0.35, 0.40, 0.45] if scenario_mode == "diverse_wide" else [0.36, 0.42]
            i = 0
            for edge in edge_grid:
                for rr in rr_grid:
                    weak = round(min(2.20, rr + 0.45), 2)
                    strong = round(max(0.80, rr - 0.10), 2)
                    ev_trades = 35 if rr >= 1.30 else (25 if rr >= 1.20 else 18)
                    ev_expect = 0.0 if edge >= 0.0014 else (-1.0 if edge >= 0.0010 else -3.0)
                    ev_pf = 1.00 if rr >= 1.30 else (0.95 if rr >= 1.20 else 0.90)
                    generated.append(
                        new_combo_variant(
                            base_balanced,
                            f"scenario_{scenario_mode}_{i:03d}",
                            f"Auto-generated {scenario_mode} scenario",
                            {
                                "max_new_orders_per_scan": 2 if rr >= 1.25 else 3,
                                "min_expected_edge_pct": edge,
                                "min_reward_risk": rr,
                                "min_rr_weak_signal": weak,
                                "min_rr_strong_signal": strong,
                                "min_strategy_trades_for_ev": ev_trades,
                                "min_strategy_expectancy_krw": ev_expect,
                                "min_strategy_profit_factor": ev_pf,
                                "avoid_high_volatility": edge >= 0.0010,
                                "avoid_trending_down": rr >= 1.20,
                                "scalping_min_signal_strength": scalp_grid[i % len(scalp_grid)],
                                "momentum_min_signal_strength": mom_grid[i % len(mom_grid)],
                                "breakout_min_signal_strength": breakout_grid[i % len(breakout_grid)],
                                "mean_reversion_min_signal_strength": mrev_grid[i % len(mrev_grid)],
                            },
                        )
                    )
                    i += 1
        combos = (legacy + generated) if include_legacy else generated
    if max_scenarios > 0 and len(combos) > max_scenarios:
        combos = combos[:max_scenarios]
    if not combos:
        raise RuntimeError("No tuning combos selected. Check --scenario-mode/--max-scenarios.")
    return combos


def select_evenly_spaced_datasets(datasets: List[pathlib.Path], limit: int) -> List[pathlib.Path]:
    if limit <= 0 or len(datasets) <= limit:
        return datasets
    if limit == 1:
        return [datasets[len(datasets) // 2]]
    step = (len(datasets) - 1) / float(limit - 1)
    indices = sorted({int(round(i * step)) for i in range(limit)})
    if len(indices) < limit:
        existing = set(indices)
        for i in range(len(datasets)):
            if i in existing:
                continue
            indices.append(i)
            if len(indices) >= limit:
                break
        indices = sorted(indices[:limit])
    return [datasets[i] for i in indices]


def compute_combo_objective(
    avg_profit_factor: float,
    avg_expectancy_krw: float,
    profitable_ratio: float,
    avg_total_trades: float,
    avg_win_rate_pct: float,
    min_avg_trades: float,
    min_profitable_ratio: float,
    min_avg_win_rate_pct: float,
    min_expectancy_krw: float,
) -> float:
    penalty = 0.0
    if avg_total_trades < min_avg_trades:
        penalty += 6000.0 + ((min_avg_trades - avg_total_trades) * 800.0)
    if profitable_ratio < min_profitable_ratio:
        penalty += 6000.0 + ((min_profitable_ratio - profitable_ratio) * 9000.0)
    if avg_win_rate_pct < min_avg_win_rate_pct:
        penalty += 4000.0 + ((min_avg_win_rate_pct - avg_win_rate_pct) * 180.0)
    if avg_expectancy_krw < min_expectancy_krw:
        penalty += 6000.0 + ((min_expectancy_krw - avg_expectancy_krw) * 120.0)
    if avg_profit_factor < 1.0:
        penalty += (1.0 - avg_profit_factor) * 2500.0

    if penalty > 0.0:
        # Keep all infeasible combos below feasible ones while preserving ordering.
        return round(-penalty + (avg_profit_factor * 10.0), 6)

    score = 0.0
    score += (avg_expectancy_krw * 25.0)
    score += (profitable_ratio * 4000.0)
    score += (avg_win_rate_pct * 40.0)
    score += ((avg_profit_factor - 1.0) * 300.0)
    score += (min(avg_total_trades, 30.0) * 40.0)
    return round(score, 6)


def evaluate_combo(
    matrix_script: pathlib.Path,
    build_config: pathlib.Path,
    original_build_raw: str,
    combo: Dict[str, Any],
    datasets: List[pathlib.Path],
    output_dir: pathlib.Path,
    stage_name: str,
    profile_ids: List[str],
) -> Dict[str, Any]:
    cfg = json.loads(original_build_raw)
    apply_candidate_combo_to_config(cfg, combo)
    build_config.write_text(json.dumps(cfg, ensure_ascii=False, indent=4), encoding="utf-8", newline="\n")

    suffix = f"{combo['combo_id']}_{stage_name}"
    matrix_csv_rel = output_dir / f"profitability_matrix_{suffix}.csv"
    profile_csv_rel = output_dir / f"profitability_profile_summary_{suffix}.csv"
    report_json_rel = output_dir / f"profitability_gate_report_{suffix}.json"

    cmd = [
        sys.executable,
        str(matrix_script),
        "--dataset-names",
        *[str(x) for x in datasets],
        "--profile-ids",
        *profile_ids,
        "--exclude-low-trade-runs-for-gate",
        "--min-trades-per-run-for-gate",
        "1",
        "--output-csv",
        str(matrix_csv_rel),
        "--output-profile-csv",
        str(profile_csv_rel),
        "--output-json",
        str(report_json_rel),
    ]
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"run_profitability_matrix.py failed for combo={combo['combo_id']} stage={stage_name}")

    report = json.loads(report_json_rel.read_text(encoding="utf-8-sig"))
    target_profile = "core_full" if "core_full" in profile_ids else profile_ids[0]
    summary = next((x for x in report.get("profile_summaries", []) if x.get("profile_id") == target_profile), None)
    if summary is None:
        raise RuntimeError(f"{target_profile} profile summary not found for combo={combo['combo_id']} stage={stage_name}")

    return {
        "combo_id": combo["combo_id"],
        "description": combo["description"],
        "stage": stage_name,
        "target_profile": target_profile,
        "overall_gate_pass": bool(report.get("overall_gate_pass", False)),
        "profile_gate_pass": bool(report.get("profile_gate_pass", False)),
        "runs_used_for_gate": int(summary.get("runs_used_for_gate", 0)),
        "excluded_low_trade_runs": int(summary.get("excluded_low_trade_runs", 0)),
        "avg_profit_factor": float(summary.get("avg_profit_factor", 0.0)),
        "avg_expectancy_krw": float(summary.get("avg_expectancy_krw", 0.0)),
        "avg_total_trades": float(summary.get("avg_total_trades", 0.0)),
        "avg_win_rate_pct": float(summary.get("avg_win_rate_pct", 0.0)),
        "profitable_ratio": float(summary.get("profitable_ratio", 0.0)),
        "gate_profit_factor_pass": bool(summary.get("gate_profit_factor_pass", False)),
        "gate_trades_pass": bool(summary.get("gate_trades_pass", False)),
        "gate_profitable_ratio_pass": bool(summary.get("gate_profitable_ratio_pass", False)),
        "gate_expectancy_pass": bool(summary.get("gate_expectancy_pass", False)),
        "report_json": str(report_json_rel.resolve()),
        "profile_csv": str(profile_csv_rel.resolve()),
        "matrix_csv": str(matrix_csv_rel.resolve()),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--matrix-script", "-MatrixScript", default=r".\scripts\run_profitability_matrix.py")
    parser.add_argument("--build-config-path", "-BuildConfigPath", default=r".\build\Release\config\config.json")
    parser.add_argument("--data-dir", "-DataDir", default=r".\data\backtest")
    parser.add_argument("--curated-data-dir", "-CuratedDataDir", default=r".\data\backtest_curated")
    parser.add_argument("--extra-data-dirs", "-ExtraDataDirs", nargs="*", default=[r".\data\backtest_real"])
    parser.add_argument("--output-dir", "-OutputDir", default=r".\build\Release\logs")
    parser.add_argument("--summary-csv", "-SummaryCsv", default=r".\build\Release\logs\candidate_trade_density_tuning_summary.csv")
    parser.add_argument("--summary-json", "-SummaryJson", default=r".\build\Release\logs\candidate_trade_density_tuning_summary.json")
    parser.add_argument("--scenario-mode", "-ScenarioMode", choices=["legacy_only", "diverse_light", "diverse_wide", "quality_focus"], default="legacy_only")
    parser.add_argument("--max-scenarios", "-MaxScenarios", type=int, default=0)
    parser.add_argument("--include-legacy-scenarios", "-IncludeLegacyScenarios", action="store_true")
    parser.add_argument("--real-data-only", "-RealDataOnly", action="store_true")
    parser.add_argument("--require-higher-tf-companions", "-RequireHigherTfCompanions", action="store_true")
    parser.add_argument("--screen-dataset-limit", "-ScreenDatasetLimit", type=int, default=8)
    parser.add_argument("--screen-top-k", "-ScreenTopK", type=int, default=6)
    parser.add_argument("--screen-profile-ids", "-ScreenProfileIds", nargs="*", default=["core_full"])
    parser.add_argument("--final-profile-ids", "-FinalProfileIds", nargs="*", default=["core_full"])
    parser.add_argument("--objective-min-avg-trades", "-ObjectiveMinAvgTrades", type=float, default=8.0)
    parser.add_argument("--objective-min-profitable-ratio", "-ObjectiveMinProfitableRatio", type=float, default=0.50)
    parser.add_argument("--objective-min-avg-win-rate-pct", "-ObjectiveMinAvgWinRatePct", type=float, default=48.0)
    parser.add_argument("--objective-min-expectancy-krw", "-ObjectiveMinExpectancyKrw", type=float, default=0.0)
    args = parser.parse_args(argv)

    matrix_script = resolve_or_throw(args.matrix_script, "Matrix script")
    build_config = resolve_or_throw(args.build_config_path, "Build config")
    output_dir = pathlib.Path(args.output_dir).resolve()
    summary_csv = pathlib.Path(args.summary_csv).resolve()
    summary_json = pathlib.Path(args.summary_json).resolve()
    ensure_parent_directory(summary_csv)
    ensure_parent_directory(summary_json)
    output_dir.mkdir(parents=True, exist_ok=True)

    scan_dirs = [pathlib.Path(args.data_dir), pathlib.Path(args.curated_data_dir)]
    scan_dirs.extend(pathlib.Path(x) for x in args.extra_data_dirs if x and x.strip())
    scan_dirs = [p.resolve() for p in scan_dirs]
    datasets = get_dataset_list(scan_dirs, args.real_data_only, args.require_higher_tf_companions)
    if not datasets:
        raise RuntimeError("No datasets found under DataDir/CuratedDataDir/ExtraDataDirs with current filters.")

    print(
        f"[TuneCandidate] dataset_mode={'realdata_only' if args.real_data_only else 'mixed'}, "
        f"require_higher_tf={bool(args.require_higher_tf_companions)}, dataset_count={len(datasets)}"
    )

    combo_specs = build_combo_specs(args.scenario_mode, args.include_legacy_scenarios, args.max_scenarios)
    print(f"[TuneCandidate] scenario_mode={args.scenario_mode}, combo_count={len(combo_specs)}")

    original_build_raw = build_config.read_text(encoding="utf-8-sig")
    screen_profile_ids = [str(x).strip() for x in args.screen_profile_ids if str(x).strip()]
    final_profile_ids = [str(x).strip() for x in args.final_profile_ids if str(x).strip()]
    if not screen_profile_ids:
        screen_profile_ids = ["core_full"]
    if not final_profile_ids:
        final_profile_ids = ["core_full"]

    screen_datasets = select_evenly_spaced_datasets(datasets, int(args.screen_dataset_limit))
    do_screening = int(args.screen_dataset_limit) > 0 and len(screen_datasets) < len(datasets)
    print(
        f"[TuneCandidate] screening={'on' if do_screening else 'off'}, "
        f"screen_dataset_count={len(screen_datasets)}, final_dataset_count={len(datasets)}"
    )

    screen_rows: List[Dict[str, Any]] = []
    rows: List[Dict[str, Any]] = []
    try:
        for combo in combo_specs:
            print(f"[TuneCandidate][Screen] Running combo: {combo['combo_id']}")
            screen_row = evaluate_combo(
                matrix_script=matrix_script,
                build_config=build_config,
                original_build_raw=original_build_raw,
                combo=combo,
                datasets=screen_datasets if do_screening else datasets,
                output_dir=output_dir,
                stage_name="screen",
                profile_ids=screen_profile_ids,
            )
            screen_objective = compute_combo_objective(
                avg_profit_factor=float(screen_row.get("avg_profit_factor", 0.0)),
                avg_expectancy_krw=float(screen_row.get("avg_expectancy_krw", 0.0)),
                profitable_ratio=float(screen_row.get("profitable_ratio", 0.0)),
                avg_total_trades=float(screen_row.get("avg_total_trades", 0.0)),
                avg_win_rate_pct=float(screen_row.get("avg_win_rate_pct", 0.0)),
                min_avg_trades=float(args.objective_min_avg_trades),
                min_profitable_ratio=float(args.objective_min_profitable_ratio),
                min_avg_win_rate_pct=float(args.objective_min_avg_win_rate_pct),
                min_expectancy_krw=float(args.objective_min_expectancy_krw),
            )
            screen_row["objective_score"] = screen_objective
            screen_row["constraint_pass"] = (
                float(screen_row.get("avg_total_trades", 0.0)) >= float(args.objective_min_avg_trades)
                and float(screen_row.get("profitable_ratio", 0.0)) >= float(args.objective_min_profitable_ratio)
                and float(screen_row.get("avg_win_rate_pct", 0.0)) >= float(args.objective_min_avg_win_rate_pct)
                and float(screen_row.get("avg_expectancy_krw", 0.0)) >= float(args.objective_min_expectancy_krw)
            )
            screen_rows.append(screen_row)

        selected_combo_ids = [x["combo_id"] for x in combo_specs]
        if do_screening:
            screen_sorted = sorted(
                screen_rows,
                key=lambda x: (
                    bool(x.get("constraint_pass", False)),
                    float(x.get("objective_score", 0.0)),
                    float(x.get("avg_expectancy_krw", 0.0)),
                    float(x.get("avg_win_rate_pct", 0.0)),
                    float(x.get("profitable_ratio", 0.0)),
                    float(x.get("avg_total_trades", 0.0)),
                ),
                reverse=True,
            )
            selected_combo_ids = [x["combo_id"] for x in screen_sorted[: max(1, int(args.screen_top_k))]]
            print(f"[TuneCandidate] screened_top_k={len(selected_combo_ids)}")

        screen_map = {str(x["combo_id"]): x for x in screen_rows}
        combos_for_final = [x for x in combo_specs if x["combo_id"] in set(selected_combo_ids)]
        for combo in combos_for_final:
            print(f"[TuneCandidate][Final] Running combo: {combo['combo_id']}")
            final_row = evaluate_combo(
                matrix_script=matrix_script,
                build_config=build_config,
                original_build_raw=original_build_raw,
                combo=combo,
                datasets=datasets,
                output_dir=output_dir,
                stage_name="final",
                profile_ids=final_profile_ids,
            )
            final_objective = compute_combo_objective(
                avg_profit_factor=float(final_row.get("avg_profit_factor", 0.0)),
                avg_expectancy_krw=float(final_row.get("avg_expectancy_krw", 0.0)),
                profitable_ratio=float(final_row.get("profitable_ratio", 0.0)),
                avg_total_trades=float(final_row.get("avg_total_trades", 0.0)),
                avg_win_rate_pct=float(final_row.get("avg_win_rate_pct", 0.0)),
                min_avg_trades=float(args.objective_min_avg_trades),
                min_profitable_ratio=float(args.objective_min_profitable_ratio),
                min_avg_win_rate_pct=float(args.objective_min_avg_win_rate_pct),
                min_expectancy_krw=float(args.objective_min_expectancy_krw),
            )
            final_row["objective_score"] = final_objective
            linked_screen = screen_map.get(str(combo["combo_id"]), {})
            final_row["screen_objective_score"] = float(linked_screen.get("objective_score", 0.0))
            final_row["screen_avg_total_trades"] = float(linked_screen.get("avg_total_trades", 0.0))
            final_row["screen_profitable_ratio"] = float(linked_screen.get("profitable_ratio", 0.0))
            final_row["screen_avg_win_rate_pct"] = float(linked_screen.get("avg_win_rate_pct", 0.0))
            rows.append(final_row)
    finally:
        build_config.write_text(original_build_raw, encoding="utf-8", newline="\n")

    if not rows:
        raise RuntimeError("No tuning rows generated.")

    sorted_rows = sorted(
        rows,
        key=lambda x: (
            float(x.get("objective_score", 0.0)),
            float(x.get("avg_expectancy_krw", 0.0)),
            float(x.get("avg_win_rate_pct", 0.0)),
            float(x.get("profitable_ratio", 0.0)),
            float(x.get("avg_total_trades", 0.0)),
            float(x.get("avg_profit_factor", 0.0)),
        ),
        reverse=True,
    )
    with summary_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(sorted_rows[0].keys()))
        writer.writeheader()
        writer.writerows(sorted_rows)

    report_out = {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "dataset_mode": "realdata_only" if args.real_data_only else "mixed",
        "require_higher_tf_companions": bool(args.require_higher_tf_companions),
        "dataset_dirs": [str(x) for x in scan_dirs],
        "dataset_count": len(datasets),
        "datasets": [str(x) for x in datasets],
        "screening": {
            "enabled": bool(do_screening),
            "screen_dataset_limit": int(args.screen_dataset_limit),
            "screen_dataset_count": len(screen_datasets),
            "screen_top_k": int(args.screen_top_k),
            "screen_profile_ids": screen_profile_ids,
            "final_profile_ids": final_profile_ids,
            "objective_min_avg_trades": float(args.objective_min_avg_trades),
            "objective_min_profitable_ratio": float(args.objective_min_profitable_ratio),
            "objective_min_avg_win_rate_pct": float(args.objective_min_avg_win_rate_pct),
            "objective_min_expectancy_krw": float(args.objective_min_expectancy_krw),
        },
        "combos": combo_specs,
        "screen_summary": screen_rows,
        "summary": sorted_rows,
    }
    ensure_parent_directory(summary_json)
    summary_json.write_text(json.dumps(report_out, ensure_ascii=False, indent=4), encoding="utf-8", newline="\n")

    print("[TuneCandidate] Completed")
    print(f"summary_csv={summary_csv}")
    print(f"summary_json={summary_json}")
    print(f"best_combo={sorted_rows[0]['combo_id']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
