#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import json
import math
import os
import pathlib
import subprocess
import sys
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


def load_json(path_value: pathlib.Path) -> Dict[str, Any]:
    with path_value.open("r", encoding="utf-8-sig") as f:
        return json.load(f)


def dump_json(path_value: pathlib.Path, payload: Any) -> None:
    ensure_parent_directory(path_value)
    with path_value.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(payload, f, ensure_ascii=False, indent=4)


def apply_profile_flags(cfg: Dict[str, Any], bridge: bool, policy: bool, risk: bool, execution: bool) -> None:
    trading = cfg.setdefault("trading", {})
    trading["enable_core_plane_bridge"] = bridge
    trading["enable_core_policy_plane"] = policy
    trading["enable_core_risk_plane"] = risk
    trading["enable_core_execution_plane"] = execution


def invoke_backtest_json(exe_file: pathlib.Path, dataset_path: pathlib.Path) -> Dict[str, Any]:
    proc = subprocess.run(
        [str(exe_file), "--backtest", str(dataset_path), "--json"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )
    lines: List[str] = []
    if proc.stdout:
        lines.extend(proc.stdout.splitlines())
    if proc.stderr:
        lines.extend(proc.stderr.splitlines())

    json_line = None
    for line in reversed(lines):
        t = line.strip()
        if t.startswith("{") and t.endswith("}"):
            json_line = t
            break

    if json_line is None:
        raise RuntimeError(
            f"Backtest JSON parsing failed: dataset={dataset_path}, exit={proc.returncode}"
        )
    return json.loads(json_line)


def run_profile_backtests(
    exe_file: pathlib.Path,
    profile: Dict[str, Any],
    datasets: List[pathlib.Path],
    exclude_low_trade_runs_for_gate: bool,
    min_trades_per_run_for_gate: int,
) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []

    max_workers = max(1, min(len(datasets), (os.cpu_count() or 4)))
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {pool.submit(invoke_backtest_json, exe_file, ds): ds for ds in datasets}
        for fut in concurrent.futures.as_completed(futures):
            ds = futures[fut]
            result = fut.result()

            profit = float(result.get("total_profit", 0.0))
            profit_factor = float(result.get("profit_factor", 0.0))
            expectancy = float(result.get("expectancy_krw", 0.0))
            run_max_drawdown_pct = round(float(result.get("max_drawdown", 0.0)) * 100.0, 4)
            total_trades = int(result.get("total_trades", 0))
            win_rate_pct = round(float(result.get("win_rate", 0.0)) * 100.0, 4)

            rows.append(
                {
                    "profile_id": str(profile["profile_id"]),
                    "profile_description": str(profile["description"]),
                    "dataset": ds.name,
                    "core_bridge_enabled": bool(profile["bridge"]),
                    "core_policy_enabled": bool(profile["policy"]),
                    "core_risk_enabled": bool(profile["risk"]),
                    "core_execution_enabled": bool(profile["execution"]),
                    "total_profit_krw": round(profit, 4),
                    "profit_factor": round(profit_factor, 4),
                    "expectancy_krw": round(expectancy, 4),
                    "max_drawdown_pct": run_max_drawdown_pct,
                    "total_trades": total_trades,
                    "win_rate_pct": win_rate_pct,
                    "profitable": profit > 0.0,
                    "gate_trade_eligible": (
                        total_trades >= min_trades_per_run_for_gate
                        if exclude_low_trade_runs_for_gate
                        else True
                    ),
                }
            )
    return rows


def safe_avg(values: List[float]) -> float:
    return sum(values) / float(len(values)) if values else 0.0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", default=r".\build\Release\AutoLifeTrading.exe")
    parser.add_argument("--config-path", default=r".\build\Release\config\config.json")
    parser.add_argument("--source-config-path", default=r".\config\config.json")
    parser.add_argument("--data-dir", default=r".\data\backtest")
    parser.add_argument(
        "--dataset-names",
        nargs="*",
        default=["simulation_2000.csv", "simulation_large.csv"],
    )
    parser.add_argument("--output-csv", default=r".\build\Release\logs\profitability_matrix.csv")
    parser.add_argument(
        "--output-profile-csv",
        default=r".\build\Release\logs\profitability_profile_summary.csv",
    )
    parser.add_argument("--output-json", default=r".\build\Release\logs\profitability_gate_report.json")
    parser.add_argument(
        "--profile-ids",
        nargs="*",
        default=["legacy_default", "core_bridge_only", "core_policy_risk", "core_full"],
    )
    parser.add_argument("--include-walk-forward", action="store_true")
    parser.add_argument("--walk-forward-script", default=r".\scripts\walk_forward_validate.py")
    parser.add_argument("--walk-forward-input", default=r".\data\backtest\simulation_2000.csv")
    parser.add_argument(
        "--walk-forward-output-json",
        default=r".\build\Release\logs\walk_forward_profitability_matrix.json",
    )
    parser.add_argument("--min-profit-factor", type=float, default=1.00)
    parser.add_argument("--min-expectancy-krw", type=float, default=0.0)
    parser.add_argument("--max-drawdown-pct", type=float, default=12.0)
    parser.add_argument("--min-profitable-ratio", type=float, default=0.55)
    parser.add_argument("--min-avg-win-rate-pct", type=float, default=0.0)
    parser.add_argument("--min-avg-trades", type=int, default=10)
    parser.add_argument("--exclude-low-trade-runs-for-gate", action="store_true")
    parser.add_argument("--min-trades-per-run-for-gate", type=int, default=5)
    parser.add_argument("--core-vs-legacy-min-profit-factor-delta", type=float, default=-0.05)
    parser.add_argument("--core-vs-legacy-min-expectancy-delta-krw", type=float, default=-5.0)
    parser.add_argument("--core-vs-legacy-min-total-profit-delta-krw", type=float, default=-10000.0)
    parser.add_argument("--fail-on-gate", action="store_true")
    args = parser.parse_args()

    resolved_exe_path = resolve_or_throw(args.exe_path, "Executable")
    resolved_source_config_path = resolve_or_throw(args.source_config_path, "Source config")
    resolved_data_dir = resolve_or_throw(args.data_dir, "Data directory")

    resolved_config_path = pathlib.Path(args.config_path)
    if not resolved_config_path.is_absolute():
        resolved_config_path = (pathlib.Path.cwd() / resolved_config_path).resolve()

    resolved_output_csv = pathlib.Path(args.output_csv).resolve()
    resolved_output_profile_csv = pathlib.Path(args.output_profile_csv).resolve()
    resolved_output_json = pathlib.Path(args.output_json).resolve()
    resolved_walk_forward_output_json = pathlib.Path(args.walk_forward_output_json).resolve()

    ensure_parent_directory(resolved_output_csv)
    ensure_parent_directory(resolved_output_profile_csv)
    ensure_parent_directory(resolved_output_json)
    ensure_parent_directory(resolved_walk_forward_output_json)

    if not resolved_config_path.exists():
        ensure_parent_directory(resolved_config_path)
        resolved_config_path.write_text(
            resolved_source_config_path.read_text(encoding="utf-8"),
            encoding="utf-8",
            newline="\n",
        )

    dataset_paths: List[pathlib.Path] = []
    for dataset_name in args.dataset_names:
        if not dataset_name or dataset_name.strip() == "":
            continue
        cand = pathlib.Path(dataset_name)
        if not cand.is_absolute():
            cand = (resolved_data_dir / cand).resolve()
        if not cand.exists():
            raise FileNotFoundError(f"Dataset not found: {cand}")
        dataset_paths.append(cand)
    if not dataset_paths:
        raise RuntimeError("No datasets configured. Set --dataset-names.")

    all_profile_specs = [
        {
            "profile_id": "legacy_default",
            "description": "All core plane flags disabled.",
            "bridge": False,
            "policy": False,
            "risk": False,
            "execution": False,
        },
        {
            "profile_id": "core_bridge_only",
            "description": "Core bridge enabled, policy/risk/execution planes disabled.",
            "bridge": True,
            "policy": False,
            "risk": False,
            "execution": False,
        },
        {
            "profile_id": "core_policy_risk",
            "description": "Core bridge + policy + risk enabled, execution plane disabled.",
            "bridge": True,
            "policy": True,
            "risk": True,
            "execution": False,
        },
        {
            "profile_id": "core_full",
            "description": "All core plane flags enabled.",
            "bridge": True,
            "policy": True,
            "risk": True,
            "execution": True,
        },
    ]
    requested_profile_ids = {str(x).strip() for x in args.profile_ids if str(x).strip()}
    profile_specs = [p for p in all_profile_specs if p["profile_id"] in requested_profile_ids]
    if not profile_specs:
        raise RuntimeError("No valid profiles selected. Check --profile-ids.")

    original_config_raw = resolved_config_path.read_text(encoding="utf-8-sig")
    rows: List[Dict[str, Any]] = []

    try:
        for profile in profile_specs:
            cfg = json.loads(original_config_raw)
            apply_profile_flags(
                cfg,
                bool(profile["bridge"]),
                bool(profile["policy"]),
                bool(profile["risk"]),
                bool(profile["execution"]),
            )
            dump_json(resolved_config_path, cfg)
            rows.extend(
                run_profile_backtests(
                    resolved_exe_path,
                    profile,
                    dataset_paths,
                    args.exclude_low_trade_runs_for_gate,
                    int(args.min_trades_per_run_for_gate),
                )
            )
    finally:
        resolved_config_path.write_text(original_config_raw, encoding="utf-8", newline="\n")

    if not rows:
        raise RuntimeError("No profitability rows generated.")

    sorted_rows = sorted(rows, key=lambda x: (x["profile_id"], x["dataset"]))
    with resolved_output_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(sorted_rows[0].keys()))
        writer.writeheader()
        writer.writerows(sorted_rows)

    profile_summaries: List[Dict[str, Any]] = []
    by_profile: Dict[str, List[Dict[str, Any]]] = {}
    for row in sorted_rows:
        by_profile.setdefault(str(row["profile_id"]), []).append(row)

    for profile_id in sorted(by_profile.keys()):
        items = by_profile[profile_id]
        gate_items = [r for r in items if r["gate_trade_eligible"]] if args.exclude_low_trade_runs_for_gate else items
        run_count = len(items)
        gate_run_count = len(gate_items)
        excluded_runs = run_count - gate_run_count
        profitable_count = sum(1 for r in gate_items if r["profitable"])
        profitable_ratio = round((profitable_count / float(gate_run_count)), 4) if gate_run_count > 0 else 0.0

        avg_profit_factor = round(safe_avg([float(r["profit_factor"]) for r in gate_items]), 4) if gate_items else 0.0
        avg_expectancy = round(safe_avg([float(r["expectancy_krw"]) for r in gate_items]), 4) if gate_items else 0.0
        avg_win_rate_pct = round(safe_avg([float(r["win_rate_pct"]) for r in gate_items]), 4) if gate_items else 0.0
        peak_drawdown = round(max((float(r["max_drawdown_pct"]) for r in gate_items), default=0.0), 4)
        avg_trades = round(safe_avg([float(r["total_trades"]) for r in gate_items]), 4) if gate_items else 0.0
        sum_profit = round(sum(float(r["total_profit_krw"]) for r in gate_items), 4) if gate_items else 0.0

        gate_sample_pass = gate_run_count > 0
        gate_profit_factor_pass = avg_profit_factor >= args.min_profit_factor
        gate_expectancy_pass = avg_expectancy >= args.min_expectancy_krw
        gate_drawdown_pass = peak_drawdown <= args.max_drawdown_pct
        gate_profitable_ratio_pass = profitable_ratio >= args.min_profitable_ratio
        gate_win_rate_pass = avg_win_rate_pct >= args.min_avg_win_rate_pct
        gate_trades_pass = avg_trades >= args.min_avg_trades
        gate_pass = (
            gate_sample_pass
            and gate_profit_factor_pass
            and gate_expectancy_pass
            and gate_drawdown_pass
            and gate_profitable_ratio_pass
            and gate_win_rate_pass
            and gate_trades_pass
        )

        profile_summaries.append(
            {
                "profile_id": profile_id,
                "runs": run_count,
                "runs_used_for_gate": gate_run_count,
                "excluded_low_trade_runs": excluded_runs,
                "profitable_runs": profitable_count,
                "profitable_ratio": profitable_ratio,
                "avg_profit_factor": avg_profit_factor,
                "avg_expectancy_krw": avg_expectancy,
                "avg_win_rate_pct": avg_win_rate_pct,
                "peak_max_drawdown_pct": peak_drawdown,
                "avg_total_trades": avg_trades,
                "total_profit_sum_krw": sum_profit,
                "gate_sample_pass": gate_sample_pass,
                "gate_profit_factor_pass": gate_profit_factor_pass,
                "gate_expectancy_pass": gate_expectancy_pass,
                "gate_drawdown_pass": gate_drawdown_pass,
                "gate_profitable_ratio_pass": gate_profitable_ratio_pass,
                "gate_win_rate_pass": gate_win_rate_pass,
                "gate_trades_pass": gate_trades_pass,
                "gate_pass": gate_pass,
            }
        )

    with resolved_output_profile_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(profile_summaries[0].keys()))
        writer.writeheader()
        writer.writerows(profile_summaries)

    legacy_summary = next((x for x in profile_summaries if x["profile_id"] == "legacy_default"), None)
    core_full_summary = next((x for x in profile_summaries if x["profile_id"] == "core_full"), None)
    core_vs_legacy: Dict[str, Any] = {
        "comparison_available": legacy_summary is not None and core_full_summary is not None,
        "baseline_profile": "legacy_default",
        "candidate_profile": "core_full",
    }
    if core_vs_legacy["comparison_available"]:
        delta_pf = round(float(core_full_summary["avg_profit_factor"]) - float(legacy_summary["avg_profit_factor"]), 4)
        delta_exp = round(float(core_full_summary["avg_expectancy_krw"]) - float(legacy_summary["avg_expectancy_krw"]), 4)
        delta_total = round(float(core_full_summary["total_profit_sum_krw"]) - float(legacy_summary["total_profit_sum_krw"]), 4)

        core_vs_legacy.update(
            {
                "delta_avg_profit_factor": delta_pf,
                "delta_avg_expectancy_krw": delta_exp,
                "delta_total_profit_sum_krw": delta_total,
                "min_delta_avg_profit_factor": args.core_vs_legacy_min_profit_factor_delta,
                "min_delta_avg_expectancy_krw": args.core_vs_legacy_min_expectancy_delta_krw,
                "min_delta_total_profit_sum_krw": args.core_vs_legacy_min_total_profit_delta_krw,
                "gate_profit_factor_delta_pass": delta_pf >= args.core_vs_legacy_min_profit_factor_delta,
                "gate_expectancy_delta_pass": delta_exp >= args.core_vs_legacy_min_expectancy_delta_krw,
                "gate_total_profit_delta_pass": delta_total >= args.core_vs_legacy_min_total_profit_delta_krw,
            }
        )
        core_vs_legacy["gate_pass"] = (
            core_vs_legacy["gate_profit_factor_delta_pass"]
            and core_vs_legacy["gate_expectancy_delta_pass"]
            and core_vs_legacy["gate_total_profit_delta_pass"]
        )
    else:
        core_vs_legacy["gate_pass"] = False

    walk_forward = None
    if args.include_walk_forward:
        resolved_walk_forward_script = resolve_or_throw(args.walk_forward_script, "Walk-forward script")
        resolved_walk_forward_input = resolve_or_throw(args.walk_forward_input, "Walk-forward input")
        cfg = json.loads(original_config_raw)
        apply_profile_flags(cfg, True, True, True, True)
        dump_json(resolved_config_path, cfg)
        try:
            proc = subprocess.run(
                [
                    sys.executable,
                    str(resolved_walk_forward_script),
                    "--exe-path",
                    str(resolved_exe_path),
                    "--input-csv",
                    str(resolved_walk_forward_input),
                    "--output-json",
                    str(resolved_walk_forward_output_json),
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="ignore",
            )
            if proc.returncode != 0:
                raise RuntimeError(f"Walk-forward failed (exit={proc.returncode})")
        finally:
            resolved_config_path.write_text(original_config_raw, encoding="utf-8", newline="\n")

        if resolved_walk_forward_output_json.exists():
            try:
                walk_forward = load_json(resolved_walk_forward_output_json)
            except Exception:
                walk_forward = None

    profile_gate_pass = all(bool(x["gate_pass"]) for x in profile_summaries)
    overall_gate_pass = profile_gate_pass and bool(core_vs_legacy.get("gate_pass", False))

    report = {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "inputs": {
            "exe_path": str(resolved_exe_path),
            "config_path": str(resolved_config_path),
            "source_config_path": str(resolved_source_config_path),
            "data_dir": str(resolved_data_dir),
            "datasets": [str(x) for x in dataset_paths],
        },
        "thresholds": {
            "min_profit_factor": args.min_profit_factor,
            "min_expectancy_krw": args.min_expectancy_krw,
            "max_drawdown_pct": args.max_drawdown_pct,
            "min_profitable_ratio": args.min_profitable_ratio,
            "min_avg_win_rate_pct": args.min_avg_win_rate_pct,
            "min_avg_trades": args.min_avg_trades,
            "exclude_low_trade_runs_for_gate": bool(args.exclude_low_trade_runs_for_gate),
            "min_trades_per_run_for_gate": args.min_trades_per_run_for_gate,
            "core_vs_legacy_min_profit_factor_delta": args.core_vs_legacy_min_profit_factor_delta,
            "core_vs_legacy_min_expectancy_delta_krw": args.core_vs_legacy_min_expectancy_delta_krw,
            "core_vs_legacy_min_total_profit_delta_krw": args.core_vs_legacy_min_total_profit_delta_krw,
        },
        "profile_gate_pass": profile_gate_pass,
        "core_vs_legacy": core_vs_legacy,
        "overall_gate_pass": overall_gate_pass,
        "profile_summaries": profile_summaries,
        "matrix_rows": sorted_rows,
        "walk_forward": walk_forward if args.include_walk_forward else None,
    }
    dump_json(resolved_output_json, report)

    print("[ProfitabilityMatrix] Completed")
    print(f"matrix_csv={resolved_output_csv}")
    print(f"profile_csv={resolved_output_profile_csv}")
    print(f"gate_report={resolved_output_json}")
    if args.include_walk_forward:
        print(f"walk_forward_report={resolved_walk_forward_output_json}")
    print(f"overall_gate_pass={overall_gate_pass}")

    if args.fail_on_gate and not overall_gate_pass:
        print("[ProfitabilityMatrix] FAILED (overall gate)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
