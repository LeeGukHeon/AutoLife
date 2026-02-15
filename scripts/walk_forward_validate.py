#!/usr/bin/env python3
import argparse
import csv
from datetime import datetime, timezone
from typing import Any, Dict, List

from _script_common import dump_json, parse_last_json_line, resolve_repo_path, run_command


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="./build/Release/AutoLifeTrading.exe")
    parser.add_argument("--input-csv", "-InputCsv", default="./data/backtest/simulation_2000.csv")
    parser.add_argument("--train-size", "-TrainSize", type=int, default=600)
    parser.add_argument("--test-size", "-TestSize", type=int, default=200)
    parser.add_argument("--step-size", "-StepSize", type=int, default=200)
    parser.add_argument("--min-train-trades", "-MinTrainTrades", type=int, default=30)
    parser.add_argument("--output-csv", "-OutputCsv", default="./build/Release/logs/walk_forward_windows.csv")
    parser.add_argument("--run-all-tests", "-RunAllTests", type=lambda s: str(s).lower() == "true", default=True)
    parser.add_argument("--output-json", "-OutputJson", default="./build/Release/logs/walk_forward_report.json")
    return parser.parse_args(argv)


def invoke_backtest_json(exe_path, csv_path):
    result = run_command([str(exe_path), "--backtest", str(csv_path), "--json"])
    parsed = parse_last_json_line(result.stdout + "\n" + result.stderr)
    return parsed


def write_slice_csv(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as fh:
        fh.write(header + "\n")
        for row in rows:
            fh.write(row + "\n")


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    input_csv = resolve_repo_path(args.input_csv)
    output_csv = resolve_repo_path(args.output_csv)
    output_json = resolve_repo_path(args.output_json)

    if not exe_path.exists():
        raise FileNotFoundError(f"Executable not found: {exe_path}")
    if not input_csv.exists():
        raise FileNotFoundError(f"Input CSV not found: {input_csv}")

    lines = input_csv.read_text(encoding="utf-8", errors="ignore").splitlines()
    if len(lines) < 2:
        raise RuntimeError(f"CSV has no data rows: {input_csv}")
    header = lines[0]
    rows = lines[1:]
    n = len(rows)

    tmp_dir = resolve_repo_path("./build/Release/logs/walk_forward_tmp")
    tmp_dir.mkdir(parents=True, exist_ok=True)

    windows: List[Dict[str, Any]] = []
    window_id = 0
    start = 0
    while start + args.train_size + args.test_size - 1 <= n:
        window_id += 1
        train_start = start
        train_end = start + args.train_size - 1
        test_start = train_end + 1
        test_end = test_start + args.test_size - 1

        train_rows = rows[train_start : train_end + 1]
        test_rows = rows[test_start : test_end + 1]

        train_csv = tmp_dir / f"wf_train_{window_id}.csv"
        test_csv = tmp_dir / f"wf_test_{window_id}.csv"
        write_slice_csv(train_csv, header, train_rows)
        write_slice_csv(test_csv, header, test_rows)

        train = invoke_backtest_json(exe_path, train_csv)
        if train is None:
            start += args.step_size
            continue

        train_trades = int(train.get("total_trades", 0))
        train_wins = int(train.get("winning_trades", 0))
        train_win_rate = round((train_wins / float(train_trades)) * 100.0, 2) if train_trades > 0 else 0.0
        train_mdd_pct = float(train.get("max_drawdown", 0.0)) * 100.0
        train_profit = float(train.get("total_profit", 0.0))

        train_pass = (
            train_trades >= int(args.min_train_trades)
            and train_profit > 0.0
            and train_mdd_pct <= 10.0
            and train_win_rate >= 55.0
        )

        test_profit = 0.0
        test_mdd_pct = 0.0
        test_trades = 0
        test_wins = 0
        test_win_rate = 0.0
        test_ran = False
        if train_pass or bool(args.run_all_tests):
            test = invoke_backtest_json(exe_path, test_csv)
            if test is not None:
                test_ran = True
                test_profit = float(test.get("total_profit", 0.0))
                test_mdd_pct = float(test.get("max_drawdown", 0.0)) * 100.0
                test_trades = int(test.get("total_trades", 0))
                test_wins = int(test.get("winning_trades", 0))
                test_win_rate = round((test_wins / float(test_trades)) * 100.0, 2) if test_trades > 0 else 0.0

        windows.append(
            {
                "window_id": window_id,
                "train_start": train_start,
                "train_end": train_end,
                "test_start": test_start,
                "test_end": test_end,
                "train_trades": train_trades,
                "train_win_rate": train_win_rate,
                "train_profit": round(train_profit, 4),
                "train_mdd_pct": round(train_mdd_pct, 4),
                "train_pass": train_pass,
                "test_ran": test_ran,
                "test_trades": test_trades,
                "test_win_rate": test_win_rate,
                "test_profit": round(test_profit, 4),
                "test_mdd_pct": round(test_mdd_pct, 4),
                "test_profitable": test_profit > 0.0,
            }
        )
        start += args.step_size

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with output_csv.open("w", encoding="utf-8", newline="") as fh:
        if windows:
            writer = csv.DictWriter(fh, fieldnames=list(windows[0].keys()))
            writer.writeheader()
            writer.writerows(windows)
        else:
            fh.write(
                "window_id,train_start,train_end,test_start,test_end,train_trades,train_win_rate,train_profit,train_mdd_pct,train_pass,test_ran,test_trades,test_win_rate,test_profit,test_mdd_pct,test_profitable\n"
            )

    ran = [w for w in windows if bool(w["test_ran"])]
    ran_count = len(ran)
    profitable_count = len([w for w in ran if bool(w["test_profitable"])])
    profit_ratio = round(profitable_count / float(ran_count), 4) if ran_count > 0 else 0.0
    oos_profit_sum = round(sum(float(w["test_profit"]) for w in ran), 4) if ran_count > 0 else 0.0
    oos_max_mdd = round(max(float(w["test_mdd_pct"]) for w in ran), 4) if ran_count > 0 else 0.0

    is_ready = ran_count >= 3 and profit_ratio >= 0.55 and oos_profit_sum > 0.0 and oos_max_mdd <= 12.0
    report = {
        "generated_at_utc": datetime.now(tz=timezone.utc).isoformat(),
        "input_csv": str(input_csv),
        "windows_total": len(windows),
        "windows_oos_ran": ran_count,
        "oos_profitable_windows": profitable_count,
        "oos_profitable_ratio": profit_ratio,
        "oos_total_profit": oos_profit_sum,
        "oos_max_mdd_pct": oos_max_mdd,
        "gate_oos_windows_min": 3,
        "gate_oos_profitable_ratio_min": 0.55,
        "gate_oos_profit_sum_positive": True,
        "gate_oos_max_mdd_pct_max": 12.0,
        "is_ready_for_live_walkforward": is_ready,
    }
    dump_json(output_json, report)

    print("=== Walk-Forward Windows ===")
    print(f"windows_total={len(windows)}, windows_oos_ran={ran_count}")
    print("=== Walk-Forward Verdict ===")
    print(f"is_ready_for_live_walkforward={is_ready}")
    print(f"saved_csv={output_csv}")
    print(f"saved_json={output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
