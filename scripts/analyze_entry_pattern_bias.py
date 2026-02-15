#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import json
import os
from datetime import datetime, timezone
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
    parser.add_argument("--profile-id", "-ProfileId", default="core_full")
    parser.add_argument("--max-datasets", "-MaxDatasets", type=int, default=12)
    parser.add_argument("--min-pattern-trades", "-MinPatternTrades", type=int, default=6)
    parser.add_argument("--include-only-loss-datasets", "-IncludeOnlyLossDatasets", action="store_true")
    parser.add_argument("--output-winning-csv", "-OutputWinningCsv", default="./build/Release/logs/entry_patterns_winning.csv")
    parser.add_argument("--output-losing-csv", "-OutputLosingCsv", default="./build/Release/logs/entry_patterns_losing.csv")
    parser.add_argument(
        "--output-recommendation-json",
        "-OutputRecommendationJson",
        default="./build/Release/logs/entry_pattern_recommendations.json",
    )
    parser.add_argument("--max-workers", "-MaxWorkers", type=int, default=4)
    return parser.parse_args(argv)


def invoke_backtest_json(exe_file, dataset_path):
    env = os.environ.copy()
    env["AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO"] = "1"
    result = run_command([str(exe_file), "--backtest", str(dataset_path), "--json"], env=env)
    return parse_last_json_line(result.stdout + "\n" + result.stderr)


def sum_property(rows: List[Dict[str, Any]], prop_name: str) -> float:
    total = 0.0
    for row in rows:
        value = row.get(prop_name)
        if value is None:
            continue
        total += float(value)
    return total


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    config_path = resolve_repo_path(args.config_path)
    gate_report_path = resolve_repo_path(args.gate_report_json)
    output_winning_csv = resolve_repo_path(args.output_winning_csv)
    output_losing_csv = resolve_repo_path(args.output_losing_csv)
    output_reco_json = resolve_repo_path(args.output_recommendation_json)
    output_winning_csv.parent.mkdir(parents=True, exist_ok=True)
    output_losing_csv.parent.mkdir(parents=True, exist_ok=True)
    output_reco_json.parent.mkdir(parents=True, exist_ok=True)

    for p, label in [(exe_path, "Executable"), (config_path, "Runtime config"), (gate_report_path, "Gate report")]:
        if not p.exists():
            raise FileNotFoundError(f"{label} not found: {p}")

    dataset_lookup: Dict[str, str] = {}
    for data_dir in args.data_dirs:
        d = resolve_repo_path(data_dir)
        if not d.exists():
            continue
        for file in d.glob("*.csv"):
            dataset_lookup.setdefault(file.name.lower(), str(file))
    if not dataset_lookup:
        raise RuntimeError("No dataset files found in DataDirs.")

    report = json.loads(gate_report_path.read_text(encoding="utf-8-sig"))
    matrix_rows = [
        r
        for r in report.get("matrix_rows", [])
        if str(r.get("profile_id", "")) == args.profile_id and float(r.get("total_trades", 0.0)) > 0.0
    ]
    if not matrix_rows:
        raise RuntimeError(f"No matrix rows found for profile_id={args.profile_id}")

    if args.include_only_loss_datasets:
        selected_rows = sorted(
            [r for r in matrix_rows if float(r.get("total_profit_krw", 0.0)) < 0.0],
            key=lambda x: float(x.get("total_profit_krw", 0.0)),
        )[: args.max_datasets]
    else:
        loss_count = max(1, args.max_datasets // 2)
        gain_count = max(1, args.max_datasets - loss_count)
        loss_rows = sorted(
            [r for r in matrix_rows if float(r.get("total_profit_krw", 0.0)) < 0.0],
            key=lambda x: float(x.get("total_profit_krw", 0.0)),
        )[:loss_count]
        gain_rows = sorted(
            [r for r in matrix_rows if float(r.get("total_profit_krw", 0.0)) > 0.0],
            key=lambda x: float(x.get("total_profit_krw", 0.0)),
            reverse=True,
        )[:gain_count]
        selected_rows = loss_rows + gain_rows

    if len(selected_rows) < args.max_datasets:
        used = {str(r.get("dataset", "")).lower() for r in selected_rows}
        remain = args.max_datasets - len(selected_rows)
        fill_rows = [
            r
            for r in sorted(
                [r for r in matrix_rows if str(r.get("dataset", "")).lower() not in used],
                key=lambda x: abs(float(x.get("total_profit_krw", 0.0))),
                reverse=True,
            )
        ][:remain]
        selected_rows += fill_rows

    uniq = {}
    for row in selected_rows:
        uniq[str(row.get("dataset", "")).lower()] = row
    selected_rows = sorted(list(uniq.values()), key=lambda x: str(x.get("dataset", "")).lower())[: args.max_datasets]
    if not selected_rows:
        raise RuntimeError("No datasets selected.")

    pattern_agg: Dict[str, Dict[str, Any]] = {}
    dataset_run_summaries = []
    missing_datasets = []
    failed_datasets = []

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
        for row in selected_rows:
            dataset_file = str(row.get("dataset", ""))
            dataset_path = dataset_lookup.get(dataset_file.lower())
            if not dataset_path:
                missing_datasets.append(dataset_file)
                continue
            pending_runs.append((dataset_file, dataset_path))

        if pending_runs:
            cpu_bound = os.cpu_count() or 4
            configured = args.max_workers if args.max_workers > 0 else cpu_bound
            max_workers = max(1, min(len(pending_runs), cpu_bound, configured))
            with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as pool:
                futures = {
                    pool.submit(invoke_backtest_json, exe_path, dataset_path): dataset_file
                    for dataset_file, dataset_path in pending_runs
                }
                for fut in concurrent.futures.as_completed(futures):
                    dataset_file = futures[fut]
                    try:
                        bt = fut.result()
                    except Exception:
                        failed_datasets.append(dataset_file)
                        continue
                    if bt is None:
                        failed_datasets.append(dataset_file)
                        continue
                    print(f"[EntryPattern] Backtest done: {dataset_file}")
                    dataset_run_summaries.append(
                        {
                            "dataset": dataset_file,
                            "total_trades": int(bt.get("total_trades", 0)),
                            "total_profit": float(bt.get("total_profit", 0.0)),
                            "expectancy_krw": float(bt.get("expectancy_krw", 0.0)),
                        }
                    )
                    summaries = bt.get("pattern_summaries")
                    if not isinstance(summaries, list):
                        continue
                    for p in summaries:
                        if not isinstance(p, dict):
                            continue
                        key = "|".join(
                            [
                                str(p.get("strategy_name", "")),
                                str(p.get("entry_archetype", "UNSPECIFIED")),
                                str(p.get("regime", "")),
                                str(p.get("strength_bucket", "")),
                                str(p.get("expected_value_bucket", "")),
                                str(p.get("reward_risk_bucket", "")),
                            ]
                        )
                        agg = pattern_agg.setdefault(
                            key,
                            {
                                "strategy_name": str(p.get("strategy_name", "")),
                                "entry_archetype": str(p.get("entry_archetype", "UNSPECIFIED")),
                                "regime": str(p.get("regime", "")),
                                "strength_bucket": str(p.get("strength_bucket", "")),
                                "expected_value_bucket": str(p.get("expected_value_bucket", "")),
                                "reward_risk_bucket": str(p.get("reward_risk_bucket", "")),
                                "total_trades": 0,
                                "winning_trades": 0,
                                "losing_trades": 0,
                                "total_profit": 0.0,
                                "gross_profit": 0.0,
                                "gross_loss_abs": 0.0,
                            },
                        )
                        total_profit = float(p.get("total_profit", 0.0))
                        agg["total_trades"] += int(p.get("total_trades", 0))
                        agg["winning_trades"] += int(p.get("winning_trades", 0))
                        agg["losing_trades"] += int(p.get("losing_trades", 0))
                        agg["total_profit"] += total_profit
                        if total_profit > 0:
                            agg["gross_profit"] += total_profit
                        elif total_profit < 0:
                            agg["gross_loss_abs"] += abs(total_profit)
    finally:
        config_path.write_text(original_cfg_raw, encoding="utf-8")

    all_pattern_rows = []
    for item in pattern_agg.values():
        trades = int(item["total_trades"])
        if trades <= 0:
            continue
        win_rate = float(item["winning_trades"]) / float(trades)
        avg_profit = float(item["total_profit"]) / float(trades)
        pf = (float(item["gross_profit"]) / float(item["gross_loss_abs"])) if float(item["gross_loss_abs"]) > 1e-9 else 0.0
        all_pattern_rows.append(
            {
                "strategy_name": item["strategy_name"],
                "entry_archetype": item["entry_archetype"],
                "regime": item["regime"],
                "strength_bucket": item["strength_bucket"],
                "expected_value_bucket": item["expected_value_bucket"],
                "reward_risk_bucket": item["reward_risk_bucket"],
                "total_trades": trades,
                "winning_trades": int(item["winning_trades"]),
                "losing_trades": int(item["losing_trades"]),
                "win_rate": round(win_rate, 4),
                "total_profit": round(float(item["total_profit"]), 4),
                "avg_profit_krw": round(avg_profit, 4),
                "profit_factor": round(pf, 4),
            }
        )

    winning_rows = sorted(
        [
            r
            for r in all_pattern_rows
            if int(r["total_trades"]) >= int(args.min_pattern_trades)
            and float(r["avg_profit_krw"]) > 0.0
            and float(r["win_rate"]) >= 0.55
            and float(r["profit_factor"]) >= 1.05
        ],
        key=lambda x: (int(x["total_trades"]), float(x["avg_profit_krw"])),
        reverse=True,
    )
    losing_rows = sorted(
        [
            r
            for r in all_pattern_rows
            if int(r["total_trades"]) >= int(args.min_pattern_trades)
            and float(r["avg_profit_krw"]) < 0.0
            and (float(r["win_rate"]) <= 0.45 or float(r["profit_factor"]) < 0.95)
        ],
        key=lambda x: (int(x["total_trades"]), -float(x["avg_profit_krw"])),
        reverse=True,
    )

    with output_winning_csv.open("w", encoding="utf-8", newline="") as fh:
        if winning_rows:
            writer = csv.DictWriter(fh, fieldnames=list(winning_rows[0].keys()))
            writer.writeheader()
            writer.writerows(winning_rows)
        else:
            fh.write("")
    with output_losing_csv.open("w", encoding="utf-8", newline="") as fh:
        if losing_rows:
            writer = csv.DictWriter(fh, fieldnames=list(losing_rows[0].keys()))
            writer.writeheader()
            writer.writerows(losing_rows)
        else:
            fh.write("")

    recommendations = []
    grouped: Dict[tuple, List[Dict[str, Any]]] = {}
    for row in all_pattern_rows:
        key = (str(row["strategy_name"]), str(row["entry_archetype"]), str(row["regime"]))
        grouped.setdefault(key, []).append(row)

    for (strategy_name, entry_archetype, regime), rows_in_group in grouped.items():
        group_trades = int(sum_property(rows_in_group, "total_trades"))
        if group_trades <= 0:
            continue
        sum_profit = sum_property(rows_in_group, "total_profit")
        group_avg_profit = sum_profit / float(group_trades)

        low_rows = [r for r in rows_in_group if r["strength_bucket"] in ("strength_low", "strength_mid")]
        high_rows = [r for r in rows_in_group if r["strength_bucket"] == "strength_high"]
        ev_neg_rows = [r for r in rows_in_group if r["expected_value_bucket"] == "ev_negative"]
        rr_low_rows = [r for r in rows_in_group if r["reward_risk_bucket"] == "rr_low"]

        low_trades = int(sum_property(low_rows, "total_trades"))
        high_trades = int(sum_property(high_rows, "total_trades"))
        low_avg = (sum_property(low_rows, "total_profit") / float(low_trades)) if low_trades > 0 else 0.0
        high_avg = (sum_property(high_rows, "total_profit") / float(high_trades)) if high_trades > 0 else 0.0
        ev_neg_share = sum_property(ev_neg_rows, "total_trades") / float(group_trades)
        rr_low_share = sum_property(rr_low_rows, "total_trades") / float(group_trades)

        min_strength = 0.50
        if low_trades >= max(3, args.min_pattern_trades // 2) and low_avg < 0.0 and high_trades >= 3 and high_avg > 0.0:
            min_strength = 0.70
        elif low_avg < 0.0:
            min_strength = 0.62
        elif ev_neg_share >= 0.35:
            min_strength = 0.60

        rr_add = 0.0
        if rr_low_share >= 0.50:
            rr_add += 0.20
        elif rr_low_share >= 0.30:
            rr_add += 0.10
        if ev_neg_share >= 0.45:
            rr_add += 0.08
        elif ev_neg_share >= 0.30:
            rr_add += 0.04

        edge_add = 0.0
        if ev_neg_share >= 0.45:
            edge_add += 0.00040
        elif ev_neg_share >= 0.30:
            edge_add += 0.00020
        if rr_low_share >= 0.50:
            edge_add += 0.00020

        recommend_block = group_trades >= 12 and group_avg_profit < -15.0
        if group_trades >= 40:
            confidence = "high"
        elif group_trades >= 18:
            confidence = "medium"
        else:
            confidence = "low"

        recommendations.append(
            {
                "strategy_name": strategy_name,
                "entry_archetype": entry_archetype,
                "regime": regime,
                "trades": group_trades,
                "avg_profit_krw": round(group_avg_profit, 4),
                "low_strength_avg_profit_krw": round(low_avg, 4),
                "high_strength_avg_profit_krw": round(high_avg, 4),
                "ev_negative_share": round(ev_neg_share, 4),
                "rr_low_share": round(rr_low_share, 4),
                "recommended_min_strength": round(min_strength, 2),
                "recommended_rr_add": round(rr_add, 2),
                "recommended_edge_add": round(edge_add, 6),
                "recommend_block": bool(recommend_block),
                "confidence": confidence,
            }
        )

    recommendations = sorted(recommendations, key=lambda r: (int(r["trades"]), float(r["avg_profit_krw"])), reverse=True)
    payload = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "profile_id": args.profile_id,
        "max_datasets": int(args.max_datasets),
        "min_pattern_trades": int(args.min_pattern_trades),
        "datasets_analyzed": [str(r.get("dataset", "")) for r in selected_rows],
        "missing_datasets": missing_datasets,
        "failed_datasets": failed_datasets,
        "dataset_run_summaries": dataset_run_summaries,
        "winning_pattern_count": len(winning_rows),
        "losing_pattern_count": len(losing_rows),
        "recommendations": recommendations,
    }
    output_reco_json.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("[EntryPattern] Completed")
    print(f"winning_csv={output_winning_csv}")
    print(f"losing_csv={output_losing_csv}")
    print(f"recommendation_json={output_reco_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
