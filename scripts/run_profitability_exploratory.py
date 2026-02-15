#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import sys
from typing import List


def get_dataset_list(dir_path: pathlib.Path) -> List[pathlib.Path]:
    if not dir_path.exists():
        return []
    return sorted([p for p in dir_path.glob("*.csv") if p.is_file()], key=lambda p: p.name.lower())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", default=r".\data\backtest")
    parser.add_argument("--curated-data-dir", default=r".\data\backtest_curated")
    parser.add_argument("--include-walk-forward", action="store_true")
    args = parser.parse_args()

    data_dir = pathlib.Path(args.data_dir).resolve()
    curated_data_dir = pathlib.Path(args.curated_data_dir).resolve()

    datasets = get_dataset_list(data_dir) + get_dataset_list(curated_data_dir)
    if not datasets:
        raise RuntimeError("No datasets found for exploratory profitability run.")

    matrix_script = pathlib.Path("./scripts/run_profitability_matrix.py").resolve()
    cmd = [
        sys.executable,
        str(matrix_script),
        "--dataset-names",
        *[str(p) for p in datasets],
        "--exclude-low-trade-runs-for-gate",
        "--min-trades-per-run-for-gate",
        "1",
        "--min-profit-factor",
        "0.25",
        "--min-avg-trades",
        "1",
        "--min-profitable-ratio",
        "0.70",
        "--output-csv",
        r".\build\Release\logs\profitability_matrix_exploratory.csv",
        "--output-profile-csv",
        r".\build\Release\logs\profitability_profile_summary_exploratory.csv",
        "--output-json",
        r".\build\Release\logs\profitability_gate_report_exploratory.json",
    ]
    if args.include_walk_forward:
        cmd.append("--include-walk-forward")

    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"run_profitability_matrix.py failed with exit code {proc.returncode}")

    print("[ProfitabilityExploratory] Completed")
    print(f"dataset_count={len(datasets)}")
    print("report=build/Release/logs/profitability_gate_report_exploratory.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())

