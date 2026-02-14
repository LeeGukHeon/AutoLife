#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import itertools
import json
import os
from datetime import datetime, timezone
from typing import Any, Dict, List

from _script_common import parse_last_json_line, resolve_repo_path, run_command


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="./build/Release/AutoLifeTrading.exe")
    parser.add_argument("--config-path", "-ConfigPath", default="./build/Release/config/config.json")
    parser.add_argument("--source-config-path", "-SourceConfigPath", default="./config/config.json")
    parser.add_argument("--dataset-a", "-DatasetA", default="./data/backtest/simulation_2000.csv")
    parser.add_argument("--dataset-b", "-DatasetB", default="./data/backtest/simulation_large.csv")
    parser.add_argument("--output-csv", "-OutputCsv", default="./build/Release/logs/strategy_set_grid.csv")
    parser.add_argument("--output-json", "-OutputJson", default="./build/Release/logs/strategy_set_best.json")
    parser.add_argument("--max-workers", "-MaxWorkers", type=int, default=0)
    parser.add_argument("--apply-best", "-ApplyBest", action="store_true")
    return parser.parse_args(argv)


def invoke_backtest_json(exe_path, csv_path, strategy_csv):
    cmd = [str(exe_path), "--backtest", str(csv_path), "--json"]
    if strategy_csv:
        cmd.extend(["--strategies", strategy_csv])
    result = run_command(cmd)
    return parse_last_json_line(result.stdout + "\n" + result.stderr)


def compute_score(a: Dict[str, Any] | None, b: Dict[str, Any] | None) -> float:
    if a is None or b is None:
        return -1e15
    p_a = float(a.get("total_profit", 0.0))
    p_b = float(b.get("total_profit", 0.0))
    pf_a = float(a.get("profit_factor", 0.0))
    pf_b = float(b.get("profit_factor", 0.0))
    exp_a = float(a.get("expectancy_krw", 0.0))
    exp_b = float(b.get("expectancy_krw", 0.0))
    mdd_a = float(a.get("max_drawdown", 0.0)) * 100.0
    mdd_b = float(b.get("max_drawdown", 0.0)) * 100.0
    t_a = int(a.get("total_trades", 0))
    t_b = int(b.get("total_trades", 0))

    score = 0.0
    score += (p_a + p_b)
    score += 250.0 * (exp_a + exp_b)
    score += 900.0 * ((pf_a - 1.0) + (pf_b - 1.0))
    score -= 220.0 * (max(0.0, mdd_a - 12.0) + max(0.0, mdd_b - 12.0))
    if p_a <= 0.0 or p_b <= 0.0:
        score -= 400.0
    if t_a < 30 or t_b < 30:
        score -= 100000.0
    return round(score, 4)


def evaluate_strategy_set(exe_path, dataset_a, dataset_b, strategy_set: List[str]) -> Dict[str, Any]:
    set_text = ",".join(strategy_set)
    a = invoke_backtest_json(exe_path, dataset_a, set_text)
    b = invoke_backtest_json(exe_path, dataset_b, set_text)
    score = compute_score(a, b)
    return {
        "enabled_strategies": set_text,
        "strategy_count": len(strategy_set),
        "score": score,
        "profit_a": float(a.get("total_profit", 0.0)) if a else None,
        "pf_a": float(a.get("profit_factor", 0.0)) if a else None,
        "exp_a": float(a.get("expectancy_krw", 0.0)) if a else None,
        "mdd_a_pct": (float(a.get("max_drawdown", 0.0)) * 100.0) if a else None,
        "trades_a": int(a.get("total_trades", 0)) if a else None,
        "profit_b": float(b.get("total_profit", 0.0)) if b else None,
        "pf_b": float(b.get("profit_factor", 0.0)) if b else None,
        "exp_b": float(b.get("expectancy_krw", 0.0)) if b else None,
        "mdd_b_pct": (float(b.get("max_drawdown", 0.0)) * 100.0) if b else None,
        "trades_b": int(b.get("total_trades", 0)) if b else None,
    }


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    config_path = resolve_repo_path(args.config_path)
    source_config_path = resolve_repo_path(args.source_config_path)
    dataset_a = resolve_repo_path(args.dataset_a)
    dataset_b = resolve_repo_path(args.dataset_b)
    output_csv = resolve_repo_path(args.output_csv)
    output_json = resolve_repo_path(args.output_json)
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    output_json.parent.mkdir(parents=True, exist_ok=True)

    for p, label in [
        (exe_path, "Executable"),
        (config_path, "Config"),
        (dataset_a, "DatasetA"),
        (dataset_b, "DatasetB"),
    ]:
        if not p.exists():
            raise FileNotFoundError(f"{label} not found: {p}")

    all_strategies = ["scalping", "momentum", "breakout", "mean_reversion", "grid"]
    sets = []
    for r in range(1, len(all_strategies) + 1):
        for combo in itertools.combinations(all_strategies, r):
            sets.append(list(combo))

    rows: List[Dict[str, Any]] = []
    default_workers = max(1, min(len(sets), os.cpu_count() or 4))
    max_workers = int(args.max_workers) if int(args.max_workers) > 0 else default_workers
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {pool.submit(evaluate_strategy_set, exe_path, dataset_a, dataset_b, s): s for s in sets}
        completed = 0
        for fut in concurrent.futures.as_completed(futures):
            strategy_set = futures[fut]
            completed += 1
            set_text = ",".join(strategy_set)
            print(f"[{completed}/{len(sets)}] enabled={set_text}")
            rows.append(fut.result())

    if not rows:
        raise RuntimeError("No strategy set rows generated.")

    sorted_rows = sorted(rows, key=lambda x: float(x["score"]), reverse=True)
    with output_csv.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(sorted_rows[0].keys()))
        writer.writeheader()
        writer.writerows(sorted_rows)

    best = sorted_rows[0]
    report = {
        "generated_at_utc": datetime.now(tz=timezone.utc).isoformat(),
        "best": best,
        "top10": sorted_rows[:10],
    }
    output_json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("=== Strategy Set Top 10 ===")
    for item in sorted_rows[:10]:
        print(f"{item['enabled_strategies']} score={item['score']}")
    print(f"saved_csv={output_csv}")
    print(f"saved_json={output_json}")

    if args.apply_best:
        cfg = json.loads(config_path.read_text(encoding="utf-8-sig"))
        trading = cfg.setdefault("trading", {})
        best_set = [x for x in str(best["enabled_strategies"]).split(",") if x]
        trading["enabled_strategies"] = best_set
        config_path.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"Applied best strategy set to {config_path}: {','.join(best_set)}")
        if source_config_path.exists():
            source_cfg = json.loads(source_config_path.read_text(encoding="utf-8-sig"))
            source_cfg.setdefault("trading", {})["enabled_strategies"] = best_set
            source_config_path.write_text(json.dumps(source_cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            print(f"Synced strategy set to {source_config_path}: {','.join(best_set)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
