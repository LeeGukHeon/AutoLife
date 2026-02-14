#!/usr/bin/env python3
import argparse
from datetime import datetime

import validate_readiness
import validate_small_seed
import walk_forward_validate
from _script_common import resolve_repo_path


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="./build/Release/AutoLifeTrading.exe")
    parser.add_argument("--curated-data-dir", "-CuratedDataDir", default="./data/backtest_curated")
    parser.add_argument("--walk-forward-input", "-WalkForwardInput", default="./data/backtest/simulation_2000.csv")
    parser.add_argument("--log-dir", "-LogDir", default="./build/Release/logs")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    log_dir = resolve_repo_path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)

    date_tag = datetime.now().strftime("%Y%m%d_%H%M%S")

    small_seed_exit = validate_small_seed.main(
        [
            "-ExePath",
            args.exe_path,
            "-DataDir",
            args.curated_data_dir,
            "-OutputCsv",
            str(log_dir / f"small_seed_matrix_baseline_{date_tag}.csv"),
            "-OutputJson",
            str(log_dir / f"small_seed_summary_baseline_{date_tag}.json"),
        ]
    )
    if small_seed_exit != 0:
        return small_seed_exit

    readiness_exit = validate_readiness.main(
        [
            "-ExePath",
            args.exe_path,
            "-DataDir",
            args.curated_data_dir,
            "-OutputCsv",
            str(log_dir / f"backtest_matrix_summary_baseline_{date_tag}.csv"),
            "-OutputStrategyCsv",
            str(log_dir / f"backtest_strategy_summary_baseline_{date_tag}.csv"),
            "-OutputProfileCsv",
            str(log_dir / f"backtest_profile_summary_baseline_{date_tag}.csv"),
            "-OutputJson",
            str(log_dir / f"readiness_report_baseline_{date_tag}.json"),
        ]
    )
    if readiness_exit != 0:
        return readiness_exit

    wf_exit = walk_forward_validate.main(
        [
            "-ExePath",
            args.exe_path,
            "-InputCsv",
            args.walk_forward_input,
            "-OutputCsv",
            str(log_dir / f"walk_forward_windows_baseline_{date_tag}.csv"),
            "-OutputJson",
            str(log_dir / f"walk_forward_report_baseline_{date_tag}.json"),
        ]
    )
    if wf_exit != 0:
        return wf_exit

    print(f"Baseline capture completed. tag={date_tag}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
