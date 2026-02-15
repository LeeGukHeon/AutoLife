#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import os
from datetime import datetime, timezone
from typing import Any, Dict, List

from _script_common import dump_json, parse_last_json_line, resolve_repo_path, run_command


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="./build/Release/AutoLifeTrading.exe")
    parser.add_argument("--data-dir", "-DataDir", default="./data/backtest")
    parser.add_argument("--output-csv", "-OutputCsv", default="./build/Release/logs/backtest_matrix_summary.csv")
    parser.add_argument(
        "--output-strategy-csv",
        "-OutputStrategyCsv",
        default="./build/Release/logs/backtest_strategy_summary.csv",
    )
    parser.add_argument(
        "--output-profile-csv",
        "-OutputProfileCsv",
        default="./build/Release/logs/backtest_profile_summary.csv",
    )
    parser.add_argument("--output-json", "-OutputJson", default="./build/Release/logs/readiness_report.json")
    parser.add_argument("--recurse", "-Recurse", action="store_true")
    parser.add_argument("--min-trades", "-MinTrades", type=int, default=30)
    parser.add_argument("--include-strategy-matrix", "-IncludeStrategyMatrix", action="store_true")
    parser.add_argument("--max-workers", "-MaxWorkers", type=int, default=max(1, min(8, os.cpu_count() or 4)))
    return parser.parse_args(argv)


def run_profile_dataset(
    exe_path,
    dataset_path,
    profile_name: str,
    strategies_csv: str,
) -> Dict[str, Any]:
    cmd = [str(exe_path), "--backtest", str(dataset_path), "--json"]
    if strategies_csv:
        cmd.extend(["--strategies", strategies_csv])

    run = run_command(cmd)
    merged = run.stdout + "\n" + run.stderr
    parsed = parse_last_json_line(merged)

    final_balance = None
    total_profit = None
    mdd_pct = None
    total_trades = None
    winning_trades = None
    win_rate_pct = None
    strategy_rows: List[Dict[str, Any]] = []

    if parsed is not None:
        final_balance = float(parsed.get("final_balance", 0.0))
        total_profit = float(parsed.get("total_profit", 0.0))
        mdd_pct = float(parsed.get("max_drawdown", 0.0)) * 100.0
        total_trades = int(parsed.get("total_trades", 0))
        winning_trades = int(parsed.get("winning_trades", 0))
        if total_trades > 0:
            win_rate_pct = round((winning_trades / float(total_trades)) * 100.0, 2)

        summaries = parsed.get("strategy_summaries")
        if isinstance(summaries, list):
            for item in summaries:
                if not isinstance(item, dict):
                    continue
                strategy_rows.append(
                    {
                        "profile_name": profile_name,
                        "strategies_csv": strategies_csv,
                        "dataset_file": dataset_path.name,
                        "strategy_name": str(item.get("strategy_name", "")),
                        "total_trades": int(item.get("total_trades", 0)),
                        "winning_trades": int(item.get("winning_trades", 0)),
                        "losing_trades": int(item.get("losing_trades", 0)),
                        "win_rate_pct": float(item.get("win_rate", 0.0)) * 100.0,
                        "total_profit": float(item.get("total_profit", 0.0)),
                        "avg_win_krw": float(item.get("avg_win_krw", 0.0)),
                        "avg_loss_krw": float(item.get("avg_loss_krw", 0.0)),
                        "profit_factor": float(item.get("profit_factor", 0.0)),
                    }
                )

    return {
        "profile_name": profile_name,
        "strategies_csv": strategies_csv,
        "dataset_path": dataset_path,
        "file": dataset_path.name,
        "final_balance": final_balance,
        "total_profit": total_profit,
        "mdd_pct": mdd_pct,
        "total_trades": total_trades,
        "winning_trades": winning_trades,
        "win_rate_pct": win_rate_pct,
        "strategy_rows": strategy_rows,
    }


def safe_sum(values: List[float]) -> float:
    return sum(values) if values else 0.0


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    data_dir = resolve_repo_path(args.data_dir)
    output_csv = resolve_repo_path(args.output_csv)
    output_strategy_csv = resolve_repo_path(args.output_strategy_csv)
    output_profile_csv = resolve_repo_path(args.output_profile_csv)
    output_json = resolve_repo_path(args.output_json)

    if not exe_path.exists():
        raise FileNotFoundError(f"Executable not found: {exe_path}")
    if not data_dir.exists():
        raise FileNotFoundError(f"Backtest data dir not found: {data_dir}")

    datasets = sorted(
        [p for p in data_dir.rglob("*.csv")] if args.recurse else [p for p in data_dir.glob("*.csv")],
        key=lambda p: str(p).lower(),
    )
    if not datasets:
        raise RuntimeError(f"No CSV files found in {data_dir}")

    include_strategy_matrix = True if not args.include_strategy_matrix else True
    profiles = [{"name": "all", "strategies": []}]
    if include_strategy_matrix:
        profiles.extend(
            [
                {"name": "scalping_only", "strategies": ["scalping"]},
                {"name": "momentum_only", "strategies": ["momentum"]},
                {"name": "breakout_only", "strategies": ["breakout"]},
                {"name": "mean_reversion_only", "strategies": ["mean_reversion"]},
                {"name": "grid_only", "strategies": ["grid_trading"]},
                {"name": "scalp_momentum", "strategies": ["scalping", "momentum"]},
                {"name": "scalp_breakout", "strategies": ["scalping", "breakout"]},
                {"name": "scalp_meanrev", "strategies": ["scalping", "mean_reversion"]},
                {"name": "trend_pair", "strategies": ["momentum", "breakout"]},
                {"name": "range_pair", "strategies": ["mean_reversion", "grid_trading"]},
            ]
        )

    rows: List[Dict[str, Any]] = []
    strategy_rows: List[Dict[str, Any]] = []

    tasks = []
    for profile in profiles:
        csv_value = ",".join(profile["strategies"]) if profile["strategies"] else ""
        for ds in datasets:
            tasks.append((profile["name"], csv_value, ds))

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, int(args.max_workers))) as pool:
        future_map = {
            pool.submit(run_profile_dataset, exe_path, ds, profile_name, strategies_csv): (profile_name, ds)
            for profile_name, strategies_csv, ds in tasks
        }
        for future in concurrent.futures.as_completed(future_map):
            row = future.result()
            dataset_path = row.pop("dataset_path")
            relative_path = str(dataset_path.relative_to(data_dir)) if str(dataset_path).startswith(str(data_dir)) else dataset_path.name
            row["relative_path"] = relative_path.replace("\\", "/")
            strategy_rows.extend(row.pop("strategy_rows"))
            rows.append(row)

    rows.sort(key=lambda r: (str(r["profile_name"]), str(r["relative_path"]).lower()))
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with output_csv.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(
            fh,
            fieldnames=[
                "profile_name",
                "strategies_csv",
                "file",
                "relative_path",
                "final_balance",
                "total_profit",
                "mdd_pct",
                "total_trades",
                "winning_trades",
                "win_rate_pct",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    strategy_summary = []
    if strategy_rows:
        grouped: Dict[tuple, List[Dict[str, Any]]] = {}
        for item in strategy_rows:
            key = (str(item["profile_name"]), str(item["strategy_name"]))
            grouped.setdefault(key, []).append(item)
        for (profile_name, strategy_name), group in grouped.items():
            sum_trades = int(sum(int(g["total_trades"]) for g in group))
            sum_wins = int(sum(int(g["winning_trades"]) for g in group))
            sum_losses = int(sum(int(g["losing_trades"]) for g in group))
            sum_profit = float(sum(float(g["total_profit"]) for g in group))
            win_rate_pct = round((sum_wins / float(sum_trades)) * 100.0, 2) if sum_trades > 0 else 0.0
            strategy_summary.append(
                {
                    "profile_name": profile_name,
                    "strategy_name": strategy_name,
                    "total_trades": sum_trades,
                    "winning_trades": sum_wins,
                    "losing_trades": sum_losses,
                    "win_rate_pct": win_rate_pct,
                    "total_profit": sum_profit,
                }
            )
        strategy_summary.sort(key=lambda r: (str(r["profile_name"]), -float(r["total_profit"])))

    output_strategy_csv.parent.mkdir(parents=True, exist_ok=True)
    with output_strategy_csv.open("w", encoding="utf-8", newline="") as fh:
        fieldnames = [
            "profile_name",
            "strategy_name",
            "total_trades",
            "winning_trades",
            "losing_trades",
            "win_rate_pct",
            "total_profit",
        ]
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(strategy_summary)

    profile_summary = []
    rows_by_profile: Dict[str, List[Dict[str, Any]]] = {}
    for row in rows:
        rows_by_profile.setdefault(str(row["profile_name"]), []).append(row)

    for profile_name in sorted(rows_by_profile.keys()):
        group_rows = rows_by_profile[profile_name]
        evaluated = [r for r in group_rows if int(r["total_trades"] or 0) >= int(args.min_trades)]
        profitable = [r for r in evaluated if float(r["total_profit"] or 0.0) > 0.0]
        strict = [
            r
            for r in evaluated
            if float(r["total_profit"] or 0.0) > 0.0
            and float(r["mdd_pct"] or 0.0) <= 10.0
            and float(r["win_rate_pct"] or 0.0) >= 55.0
        ]
        profit_ratio = round(len(profitable) / float(len(evaluated)), 4) if evaluated else 0.0
        profile_summary.append(
            {
                "profile_name": profile_name,
                "dataset_total": len(group_rows),
                "dataset_evaluated": len(evaluated),
                "profitable_datasets": len(profitable),
                "strict_pass_datasets": len(strict),
                "profitable_ratio": profit_ratio,
                "is_ready_for_live_profile": profit_ratio >= 0.60 and len(strict) >= 2,
            }
        )

    output_profile_csv.parent.mkdir(parents=True, exist_ok=True)
    with output_profile_csv.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(
            fh,
            fieldnames=[
                "profile_name",
                "dataset_total",
                "dataset_evaluated",
                "profitable_datasets",
                "strict_pass_datasets",
                "profitable_ratio",
                "is_ready_for_live_profile",
            ],
        )
        writer.writeheader()
        writer.writerows(profile_summary)

    primary_rows = [r for r in rows if str(r["profile_name"]) == "all"]
    evaluated = [r for r in primary_rows if int(r["total_trades"] or 0) >= int(args.min_trades)]
    profitable = [r for r in evaluated if float(r["total_profit"] or 0.0) > 0.0]
    strict = [
        r
        for r in evaluated
        if float(r["total_profit"] or 0.0) > 0.0
        and float(r["mdd_pct"] or 0.0) <= 10.0
        and float(r["win_rate_pct"] or 0.0) >= 55.0
    ]
    profit_ratio = round(len(profitable) / float(len(evaluated)), 4) if evaluated else 0.0
    is_ready = profit_ratio >= 0.60 and len(strict) >= 2

    report = {
        "generated_at_utc": datetime.now(tz=timezone.utc).isoformat(),
        "dataset_total": len(primary_rows),
        "dataset_evaluated": len(evaluated),
        "min_trades_threshold": int(args.min_trades),
        "recursive_scan": bool(args.recurse),
        "profitable_datasets": len(profitable),
        "strict_pass_datasets": len(strict),
        "profitable_ratio": profit_ratio,
        "readiness_gate_profitable": ">= 0.60",
        "readiness_gate_strict_pass": ">= 2",
        "is_ready_for_live_by_backtest": is_ready,
        "strategy_summary": strategy_summary,
        "profile_summary": profile_summary,
        "aggregate_total_profit": safe_sum([float(r["total_profit"] or 0.0) for r in primary_rows]),
    }
    dump_json(output_json, report)

    print("=== Backtest Matrix Summary ===")
    print(f"rows={len(rows)}, profiles={len(profiles)}, datasets={len(datasets)}")
    print("=== Readiness Verdict ===")
    print(f"is_ready_for_live_by_backtest={is_ready}")
    print(f"saved_csv={output_csv}")
    print(f"saved_strategy_csv={output_strategy_csv}")
    print(f"saved_profile_csv={output_profile_csv}")
    print(f"saved_json={output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
