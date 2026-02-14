#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import json
import os
from typing import Any, Dict, List

from _script_common import parse_last_json_line, resolve_repo_path, run_command


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="./build/Release/AutoLifeTrading.exe")
    parser.add_argument("--config-path", "-ConfigPath", default="./build/Release/config/config.json")
    parser.add_argument(
        "--gate-report-json",
        "-GateReportJson",
        default="./build/Release/logs/profitability_gate_report_realdata.json",
    )
    parser.add_argument(
        "--data-dirs",
        "-DataDirs",
        nargs="*",
        default=["./data/backtest", "./data/backtest_curated", "./data/backtest_real"],
    )
    parser.add_argument("--output-market-csv", "-OutputMarketCsv", default="./build/Release/logs/loss_contributor_by_market.csv")
    parser.add_argument(
        "--output-strategy-csv",
        "-OutputStrategyCsv",
        default="./build/Release/logs/loss_contributor_by_strategy.csv",
    )
    return parser.parse_args(argv)


def market_label_from_dataset(dataset_file_name: str) -> str:
    name = dataset_file_name.rsplit(".", 1)[0]
    parts = name.split("_")
    if len(parts) >= 3 and parts[0].lower() == "upbit":
        return f"{parts[1].upper()}-{parts[2].upper()}"
    return f"synthetic:{dataset_file_name}"


def invoke_backtest_json(exe_file, dataset_path):
    result = run_command([str(exe_file), "--backtest", str(dataset_path), "--json"])
    return parse_last_json_line(result.stdout + "\n" + result.stderr)


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    config_path = resolve_repo_path(args.config_path)
    gate_report_path = resolve_repo_path(args.gate_report_json)
    output_market_csv = resolve_repo_path(args.output_market_csv)
    output_strategy_csv = resolve_repo_path(args.output_strategy_csv)
    output_market_csv.parent.mkdir(parents=True, exist_ok=True)
    output_strategy_csv.parent.mkdir(parents=True, exist_ok=True)

    for p, label in [(exe_path, "Executable"), (config_path, "Runtime config"), (gate_report_path, "Gate report")]:
        if not p.exists():
            raise FileNotFoundError(f"{label} not found: {p}")

    dataset_lookup: Dict[str, str] = {}
    for data_dir in args.data_dirs:
        d = resolve_repo_path(data_dir)
        if not d.exists():
            continue
        for file in d.glob("*.csv"):
            key = file.name.lower()
            if key not in dataset_lookup:
                dataset_lookup[key] = str(file)
    if not dataset_lookup:
        raise RuntimeError("No data directories were resolved.")

    report = json.loads(gate_report_path.read_text(encoding="utf-8-sig"))
    matrix_rows = report.get("matrix_rows")
    if not isinstance(matrix_rows, list):
        raise RuntimeError(f"Gate report does not contain matrix_rows: {gate_report_path}")

    core_loss_rows = [
        r
        for r in matrix_rows
        if str(r.get("profile_id", "")) == "core_full"
        and float(r.get("total_trades", 0.0)) > 0.0
        and float(r.get("total_profit_krw", 0.0)) < 0.0
    ]
    if not core_loss_rows:
        with output_market_csv.open("w", encoding="utf-8", newline="") as fh:
            fh.write("")
        with output_strategy_csv.open("w", encoding="utf-8", newline="") as fh:
            fh.write("")
        print("[LossContrib] No negative core_full rows found. Empty outputs were written.")
        print(f"market_csv={output_market_csv}")
        print(f"strategy_csv={output_strategy_csv}")
        return 0

    market_agg: Dict[str, Dict[str, Any]] = {}
    for row in core_loss_rows:
        dataset_name = str(row.get("dataset", ""))
        market = market_label_from_dataset(dataset_name)
        loss_value = abs(float(row.get("total_profit_krw", 0.0)))
        trades = int(row.get("total_trades", 0))
        agg = market_agg.setdefault(
            market,
            {"market": market, "loss_krw": 0.0, "total_trades": 0, "losing_runs": 0},
        )
        agg["loss_krw"] += loss_value
        agg["total_trades"] += trades
        agg["losing_runs"] += 1

    total_market_loss = sum(float(x["loss_krw"]) for x in market_agg.values())
    market_rows = []
    for item in market_agg.values():
        loss = float(item["loss_krw"])
        share = (loss / total_market_loss) * 100.0 if total_market_loss > 0 else 0.0
        market_rows.append(
            {
                "market": item["market"],
                "loss_krw": round(loss, 4),
                "loss_share_pct": round(share, 4),
                "total_trades": int(item["total_trades"]),
                "losing_runs": int(item["losing_runs"]),
            }
        )
    market_rows.sort(key=lambda x: float(x["loss_krw"]), reverse=True)
    with output_market_csv.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(market_rows[0].keys()))
        writer.writeheader()
        writer.writerows(market_rows)

    strategy_agg: Dict[str, Dict[str, Any]] = {}
    missing_datasets: List[str] = []
    backtest_failures: List[str] = []

    original_cfg_raw = config_path.read_text(encoding="utf-8-sig")
    try:
        cfg = json.loads(original_cfg_raw)
        trading = cfg.setdefault("trading", {})
        trading["enable_core_plane_bridge"] = True
        trading["enable_core_policy_plane"] = True
        trading["enable_core_risk_plane"] = True
        trading["enable_core_execution_plane"] = True
        config_path.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

        pending_runs: List[tuple[str, str]] = []
        for row in sorted(core_loss_rows, key=lambda r: float(r.get("total_profit_krw", 0.0))):
            dataset_name = str(row.get("dataset", ""))
            dataset_path = dataset_lookup.get(dataset_name.lower())
            if not dataset_path:
                missing_datasets.append(dataset_name)
                continue
            pending_runs.append((dataset_name, dataset_path))

        if pending_runs:
            max_workers = max(1, min(len(pending_runs), os.cpu_count() or 4))
            with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as pool:
                futures = {
                    pool.submit(invoke_backtest_json, exe_path, dataset_path): dataset_name
                    for dataset_name, dataset_path in pending_runs
                }
                for fut in concurrent.futures.as_completed(futures):
                    dataset_name = futures[fut]
                    try:
                        result = fut.result()
                    except Exception:
                        backtest_failures.append(dataset_name)
                        continue
                    if result is None:
                        backtest_failures.append(dataset_name)
                        continue
                    summaries = result.get("strategy_summaries")
                    if not isinstance(summaries, list):
                        continue
                    for summary in summaries:
                        if not isinstance(summary, dict):
                            continue
                        strategy_name = str(summary.get("strategy_name", "")).strip()
                        if not strategy_name:
                            continue
                        strategy_profit = float(summary.get("total_profit", 0.0))
                        if strategy_profit >= 0.0:
                            continue
                        agg = strategy_agg.setdefault(
                            strategy_name,
                            {
                                "strategy_name": strategy_name,
                                "loss_krw": 0.0,
                                "total_trades": 0,
                                "losing_trades": 0,
                                "losing_datasets": 0,
                            },
                        )
                        agg["loss_krw"] += abs(strategy_profit)
                        agg["total_trades"] += int(summary.get("total_trades", 0))
                        agg["losing_trades"] += int(summary.get("losing_trades", 0))
                        agg["losing_datasets"] += 1
    finally:
        config_path.write_text(original_cfg_raw, encoding="utf-8")

    strategy_total_loss = sum(float(x["loss_krw"]) for x in strategy_agg.values())
    strategy_rows = []
    for item in strategy_agg.values():
        loss = float(item["loss_krw"])
        share = (loss / strategy_total_loss) * 100.0 if strategy_total_loss > 0 else 0.0
        strategy_rows.append(
            {
                "strategy_name": item["strategy_name"],
                "loss_krw": round(loss, 4),
                "loss_share_pct": round(share, 4),
                "total_trades": int(item["total_trades"]),
                "losing_trades": int(item["losing_trades"]),
                "losing_datasets": int(item["losing_datasets"]),
            }
        )
    strategy_rows.sort(key=lambda x: float(x["loss_krw"]), reverse=True)
    with output_strategy_csv.open("w", encoding="utf-8", newline="") as fh:
        if strategy_rows:
            writer = csv.DictWriter(fh, fieldnames=list(strategy_rows[0].keys()))
            writer.writeheader()
            writer.writerows(strategy_rows)
        else:
            fh.write("")

    print("[LossContrib] Completed")
    print(f"market_csv={output_market_csv}")
    print(f"strategy_csv={output_strategy_csv}")
    for m in market_rows[:5]:
        print(f"[LossContrib][Market] {m['market']} | loss={m['loss_krw']} | share={m['loss_share_pct']}%")
    for s in strategy_rows[:2]:
        print(f"[LossContrib][Strategy] {s['strategy_name']} | loss={s['loss_krw']} | share={s['loss_share_pct']}%")
    if missing_datasets:
        print(f"[LossContrib] missing_datasets={','.join(sorted(set(missing_datasets)))}")
    if backtest_failures:
        print(f"[LossContrib] backtest_failures={','.join(sorted(set(backtest_failures)))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
