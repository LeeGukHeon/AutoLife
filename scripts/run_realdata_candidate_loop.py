#!/usr/bin/env python3
import argparse
import json
import pathlib
import subprocess
import sys
from typing import List


def ensure_directory(path_value: pathlib.Path) -> None:
    path_value.mkdir(parents=True, exist_ok=True)


def build_realdata_file_path(output_dir: pathlib.Path, market: str, tf_unit: str, row_count: int) -> pathlib.Path:
    safe_market = market.replace("-", "_")
    return output_dir / f"upbit_{safe_market}_{tf_unit}m_{row_count}.csv"


def has_higher_tf_companions(primary_path: pathlib.Path) -> bool:
    if not primary_path.exists():
        return False
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


def get_dataset_files(dirs: List[pathlib.Path], only_real_data: bool, require_higher_tf: bool) -> List[pathlib.Path]:
    items: List[pathlib.Path] = []
    for d in dirs:
        if not d.exists():
            continue
        is_real = "backtest_real" in str(d).lower()
        if only_real_data and not is_real:
            continue
        for f in sorted(d.glob("*.csv"), key=lambda x: x.name.lower()):
            if is_real:
                if "_1m_" not in f.name.lower():
                    continue
                if require_higher_tf and not has_higher_tf_companions(f):
                    continue
            elif only_real_data:
                continue
            items.append(f.resolve())
    uniq = sorted(set(items), key=lambda x: str(x).lower())
    return uniq


def run_fetch_script(
    fetch_script: pathlib.Path,
    market: str,
    unit: str,
    candles: int,
    chunk_size: int,
    sleep_ms: int,
    output_path: pathlib.Path,
) -> None:
    cmd = [
        sys.executable,
        str(fetch_script),
        "-Market",
        market,
        "-Unit",
        unit,
        "-Candles",
        str(candles),
        "-ChunkSize",
        str(chunk_size),
        "-SleepMs",
        str(sleep_ms),
        "-OutputPath",
        str(output_path),
    ]
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"fetch script failed: script={fetch_script}, market={market}, unit={unit}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fetch-script", "-FetchScript", default=r".\scripts\fetch_upbit_historical_candles.py")
    parser.add_argument("--matrix-script", "-MatrixScript", default=r".\scripts\run_profitability_matrix.py")
    parser.add_argument("--tune-script", "-TuneScript", default=r".\scripts\tune_candidate_gate_trade_density.py")
    parser.add_argument("--real-data-dir", "-RealDataDir", default=r".\data\backtest_real")
    parser.add_argument("--backtest-data-dir", "-BacktestDataDir", default=r".\data\backtest")
    parser.add_argument("--curated-data-dir", "-CuratedDataDir", default=r".\data\backtest_curated")
    parser.add_argument(
        "--markets",
        "-Markets",
        nargs="*",
        default=[
            "KRW-BTC", "KRW-ETH", "KRW-XRP", "KRW-SOL", "KRW-DOGE", "KRW-ADA",
            "KRW-AVAX", "KRW-LINK", "KRW-DOT", "KRW-TRX", "KRW-SUI", "KRW-BCH",
        ],
    )
    parser.add_argument("--unit", "-Unit", default="1")
    parser.add_argument("--candles", "-Candles", type=int, default=12000)
    parser.add_argument("--candles5m", "-Candles5m", type=int, default=4000)
    parser.add_argument("--candles1h", "-Candles1h", type=int, default=1200)
    parser.add_argument("--candles4h", "-Candles4h", type=int, default=600)
    parser.add_argument("--chunk-size", "-ChunkSize", type=int, default=200)
    parser.add_argument("--sleep-ms", "-SleepMs", type=int, default=120)
    parser.add_argument("--output-matrix-csv", "-OutputMatrixCsv", default=r".\build\Release\logs\profitability_matrix_realdata.csv")
    parser.add_argument("--output-profile-csv", "-OutputProfileCsv", default=r".\build\Release\logs\profitability_profile_summary_realdata.csv")
    parser.add_argument("--output-report-json", "-OutputReportJson", default=r".\build\Release\logs\profitability_gate_report_realdata.json")
    parser.add_argument("--skip-fetch", "-SkipFetch", action="store_true")
    parser.add_argument("--skip-higher-tf-fetch", "-SkipHigherTfFetch", action="store_true")
    parser.add_argument("--skip-tune", "-SkipTune", action="store_true")
    parser.add_argument("--real-data-only", "-RealDataOnly", action="store_true")
    parser.add_argument("--require-higher-tf-companions", "-RequireHigherTfCompanions", action="store_true")
    args = parser.parse_args(argv)

    fetch_script = pathlib.Path(args.fetch_script).resolve()
    matrix_script = pathlib.Path(args.matrix_script).resolve()
    tune_script = pathlib.Path(args.tune_script).resolve()
    real_data_dir = pathlib.Path(args.real_data_dir).resolve()
    backtest_data_dir = pathlib.Path(args.backtest_data_dir).resolve()
    curated_data_dir = pathlib.Path(args.curated_data_dir).resolve()
    output_report_json = pathlib.Path(args.output_report_json).resolve()

    ensure_directory(real_data_dir)

    if not args.skip_fetch:
        for market in args.markets:
            if not market.strip():
                continue
            target_1m = build_realdata_file_path(real_data_dir, market, args.unit, args.candles)
            print(f"[RealDataLoop] Fetching market={market}, unit={args.unit}m, candles={args.candles}")
            run_fetch_script(fetch_script, market, args.unit, args.candles, args.chunk_size, args.sleep_ms, target_1m)

            if not args.skip_higher_tf_fetch:
                target_5m = build_realdata_file_path(real_data_dir, market, "5", args.candles5m)
                print(f"[RealDataLoop] Fetching market={market}, unit=5m, candles={args.candles5m}")
                run_fetch_script(fetch_script, market, "5", args.candles5m, args.chunk_size, args.sleep_ms, target_5m)

                target_1h = build_realdata_file_path(real_data_dir, market, "60", args.candles1h)
                print(f"[RealDataLoop] Fetching market={market}, unit=60m, candles={args.candles1h}")
                run_fetch_script(fetch_script, market, "60", args.candles1h, args.chunk_size, args.sleep_ms, target_1h)

                target_4h = build_realdata_file_path(real_data_dir, market, "240", args.candles4h)
                print(f"[RealDataLoop] Fetching market={market}, unit=240m, candles={args.candles4h}")
                run_fetch_script(fetch_script, market, "240", args.candles4h, args.chunk_size, args.sleep_ms, target_4h)

    dataset_dirs = [real_data_dir] if args.real_data_only else [backtest_data_dir, curated_data_dir, real_data_dir]
    datasets = get_dataset_files(dataset_dirs, args.real_data_only, args.require_higher_tf_companions)
    if not datasets:
        raise RuntimeError("No datasets found after fetch step.")

    print(
        f"[RealDataLoop] dataset_mode={'realdata_only' if args.real_data_only else 'mixed'}, "
        f"require_higher_tf={bool(args.require_higher_tf_companions)}, dataset_count={len(datasets)}"
    )

    print(f"[RealDataLoop] Running profitability matrix with datasets={len(datasets)}")
    matrix_cmd = [
        sys.executable,
        str(matrix_script),
        "--dataset-names",
        *[str(x) for x in datasets],
        "--exclude-low-trade-runs-for-gate",
        "--min-trades-per-run-for-gate",
        "1",
        "--output-csv",
        args.output_matrix_csv,
        "--output-profile-csv",
        args.output_profile_csv,
        "--output-json",
        args.output_report_json,
    ]
    proc = subprocess.run(matrix_cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"run_profitability_matrix.py failed (exit={proc.returncode})")

    report = json.loads(output_report_json.read_text(encoding="utf-8-sig"))
    core_full = next((x for x in report.get("profile_summaries", []) if x.get("profile_id") == "core_full"), None)
    if core_full is not None:
        print(f"[RealDataLoop] core_full.avg_profit_factor={core_full.get('avg_profit_factor')}")
        print(f"[RealDataLoop] core_full.avg_total_trades={core_full.get('avg_total_trades')}")
        print(f"[RealDataLoop] core_full.avg_expectancy_krw={core_full.get('avg_expectancy_krw')}")
    print(f"[RealDataLoop] overall_gate_pass={report.get('overall_gate_pass')}")

    if not args.skip_tune:
        print("[RealDataLoop] Running candidate trade-density tuning with real datasets included")
        tune_cmd = [
            sys.executable,
            str(tune_script),
            "--data-dir",
            str(backtest_data_dir),
            "--curated-data-dir",
            str(curated_data_dir),
            "--extra-data-dirs",
            str(real_data_dir),
        ]
        if args.real_data_only:
            tune_cmd.append("--real-data-only")
        if args.require_higher_tf_companions:
            tune_cmd.append("--require-higher-tf-companions")
        tune_proc = subprocess.run(tune_cmd)
        if tune_proc.returncode != 0:
            raise RuntimeError(f"tune_candidate_gate_trade_density.py failed (exit={tune_proc.returncode})")

    print("[RealDataLoop] Completed")
    print(f"gate_report={output_report_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
