#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import json
import os
from datetime import datetime, timezone
from typing import Any, Dict, List

from _script_common import dump_json, parse_last_json_line, resolve_repo_path, run_command


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="./build/Release/AutoLifeTrading.exe")
    parser.add_argument("--data-dir", "-DataDir", default="./data/backtest_curated")
    parser.add_argument("--output-csv", "-OutputCsv", default="./build/Release/logs/small_seed_matrix.csv")
    parser.add_argument("--output-json", "-OutputJson", default="./build/Release/logs/small_seed_summary.json")
    parser.add_argument("--seeds", "-Seeds", nargs="*", type=float, default=[50000.0, 100000.0])
    parser.add_argument("--min-trades", "-MinTrades", type=int, default=5)
    parser.add_argument("--max-workers", "-MaxWorkers", type=int, default=max(1, min(8, os.cpu_count() or 4)))
    return parser.parse_args()


def run_single(exe_path, dataset_path, seed, min_trades):
    cmd = [str(exe_path), "--backtest", str(dataset_path), "--json", "--initial-capital", str(seed)]
    result = run_command(cmd)
    merged = result.stdout + "\n" + result.stderr
    parsed = parse_last_json_line(merged)
    if parsed is None:
        raise RuntimeError(f"No JSON result found for dataset={dataset_path.name}, seed={seed}")

    total_profit = float(parsed.get("total_profit", 0.0))
    total_trades = int(parsed.get("total_trades", 0))
    return {
        "seed_krw": float(seed),
        "dataset": dataset_path.name,
        "final_balance": float(parsed.get("final_balance", 0.0)),
        "total_profit": total_profit,
        "total_trades": total_trades,
        "win_rate": float(parsed.get("win_rate", 0.0)),
        "profit_factor": float(parsed.get("profit_factor", 0.0)),
        "max_drawdown": float(parsed.get("max_drawdown", 0.0)),
        "is_profitable": total_profit > 0.0,
        "is_tradable": total_trades >= int(min_trades),
    }


def safe_avg(values: List[float]) -> float:
    return sum(values) / float(len(values)) if values else 0.0


def main() -> int:
    args = parse_args()
    exe_path = resolve_repo_path(args.exe_path)
    data_dir = resolve_repo_path(args.data_dir)
    output_csv = resolve_repo_path(args.output_csv)
    output_json = resolve_repo_path(args.output_json)

    if not exe_path.exists():
        raise FileNotFoundError(f"exe not found: {exe_path}")
    if not data_dir.exists():
        raise FileNotFoundError(f"data dir not found: {data_dir}")

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    output_json.parent.mkdir(parents=True, exist_ok=True)

    datasets = sorted([p for p in data_dir.glob("*.csv") if p.is_file()], key=lambda p: p.name.lower())
    if not datasets:
        raise RuntimeError(f"No CSV datasets found in {data_dir}")

    rows: List[Dict[str, Any]] = []
    tasks = [(seed, ds) for seed in args.seeds for ds in datasets]
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, int(args.max_workers))) as pool:
        future_map = {
            pool.submit(run_single, exe_path, ds, seed, args.min_trades): (seed, ds)
            for seed, ds in tasks
        }
        for future in concurrent.futures.as_completed(future_map):
            rows.append(future.result())

    rows.sort(key=lambda r: (float(r["seed_krw"]), str(r["dataset"]).lower()))
    with output_csv.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    summary_by_seed = []
    for seed in args.seeds:
        group = [r for r in rows if float(r["seed_krw"]) == float(seed)]
        if not group:
            continue
        profitable = [r for r in group if bool(r["is_profitable"])]
        tradable = [r for r in group if bool(r["is_tradable"])]
        summary_by_seed.append(
            {
                "seed_krw": float(seed),
                "datasets": len(group),
                "profitable_datasets": len(profitable),
                "tradable_datasets": len(tradable),
                "profitable_ratio": (len(profitable) / float(len(group))) if group else 0.0,
                "tradable_ratio": (len(tradable) / float(len(group))) if group else 0.0,
                "avg_profit": safe_avg([float(r["total_profit"]) for r in group]),
                "avg_drawdown": safe_avg([float(r["max_drawdown"]) for r in group]),
            }
        )

    summary = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "exe_path": str(exe_path),
        "data_dir": str(data_dir),
        "min_trades": int(args.min_trades),
        "seeds": [float(x) for x in args.seeds],
        "results": summary_by_seed,
    }
    dump_json(output_json, summary)

    print("Small-seed validation complete.")
    print(f"CSV: {output_csv}")
    print(f"JSON: {output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
