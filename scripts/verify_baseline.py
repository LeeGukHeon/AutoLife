#!/usr/bin/env python3
import argparse
import pathlib
import re
import subprocess
import sys


_UPBIT_DATASET_RE = re.compile(
    r"^upbit_([A-Za-z0-9]+)_([A-Za-z0-9]+)_([0-9]+)m_([0-9]+)$",
    re.IGNORECASE,
)


def parse_upbit_refresh_spec(dataset_path: pathlib.Path):
    match = _UPBIT_DATASET_RE.match(dataset_path.stem)
    if not match:
        return None
    quote, base, unit, candles = match.groups()
    return {
        "market": f"{quote.upper()}-{base.upper()}",
        "unit": str(int(unit)),
        "candles": str(int(candles)),
    }


def resolve_dataset_path(data_dir: pathlib.Path, token: str) -> pathlib.Path:
    candidate = pathlib.Path(token)
    if candidate.is_absolute():
        return candidate
    return (data_dir / candidate).resolve()


def run_refresh_if_needed(
    python_exe: str,
    fetch_script: pathlib.Path,
    dataset_paths,
    data_mode: str,
    refresh_chunk_size: int,
    refresh_sleep_ms: int,
    refresh_end_utc: str,
    allow_unsupported_refresh_datasets: bool,
):
    if data_mode == "fixed":
        return

    refreshed = []
    for dataset_path in dataset_paths:
        need_refresh = (
            data_mode == "refresh_force" or
            (data_mode == "refresh_if_missing" and not dataset_path.exists())
        )
        if not need_refresh:
            continue

        spec = parse_upbit_refresh_spec(dataset_path)
        if spec is None:
            message = (
                f"Dataset does not support refresh-by-name pattern: {dataset_path.name} "
                f"(expected: upbit_<QUOTE>_<BASE>_<UNIT>m_<CANDLES>.csv)"
            )
            if allow_unsupported_refresh_datasets:
                print(f"[verify_baseline] WARN: {message} -> keeping existing file path")
                continue
            raise RuntimeError(message)

        cmd = [
            python_exe,
            str(fetch_script),
            "--market",
            spec["market"],
            "--unit",
            spec["unit"],
            "--candles",
            spec["candles"],
            "--output-path",
            str(dataset_path),
            "--chunk-size",
            str(int(refresh_chunk_size)),
            "--sleep-ms",
            str(int(refresh_sleep_ms)),
        ]
        if str(refresh_end_utc).strip():
            cmd.extend(["--end-utc", str(refresh_end_utc).strip()])

        print(
            "[verify_baseline] refresh "
            f"dataset={dataset_path.name} market={spec['market']} unit={spec['unit']}m candles={spec['candles']}"
        )
        proc = subprocess.run(cmd)
        if int(proc.returncode) != 0:
            raise RuntimeError(
                f"Refresh failed for dataset: {dataset_path} (exit={proc.returncode})"
            )
        refreshed.append(dataset_path.name)

    if refreshed:
        print(f"[verify_baseline] refreshed_count={len(refreshed)}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Simple verification entrypoint (adaptive validation baseline)."
    )
    parser.add_argument(
        "--datasets",
        default="simulation_2000.csv,simulation_large.csv",
        help="Comma-separated dataset names.",
    )
    parser.add_argument(
        "--data-dir",
        default=r".\data\backtest",
        help="Dataset directory.",
    )
    parser.add_argument(
        "--realdata-only",
        action="store_true",
        help="Use real-data baseline defaults (data/backtest_real + higher TF companions).",
    )
    parser.add_argument(
        "--data-mode",
        choices=["fixed", "refresh_if_missing", "refresh_force"],
        default="fixed",
        help="Verification dataset mode. Gate default is fixed.",
    )
    parser.add_argument(
        "--refresh-chunk-size",
        type=int,
        default=200,
        help="Chunk size for refresh fetch requests.",
    )
    parser.add_argument(
        "--refresh-sleep-ms",
        type=int,
        default=120,
        help="Sleep milliseconds between refresh fetch requests.",
    )
    parser.add_argument(
        "--refresh-end-utc",
        default="",
        help="Optional UTC end cursor for refresh (ISO8601).",
    )
    parser.add_argument(
        "--allow-unsupported-refresh-datasets",
        action="store_true",
        help="Allow refresh mode even when dataset naming cannot map to Upbit fetch spec.",
    )
    parser.add_argument(
        "--output-tag",
        default="",
        help="Optional suffix tag for output artifact names.",
    )
    parser.add_argument(
        "--validation-profile",
        choices=["adaptive", "legacy_gate"],
        default="adaptive",
        help="adaptive (recommended) or legacy_gate compatibility mode.",
    )
    parser.add_argument(
        "--max-downtrend-loss-per-trade-krw",
        type=float,
        default=12.0,
        help="Adaptive validation: max acceptable downtrend loss per trade.",
    )
    parser.add_argument(
        "--max-downtrend-trade-share",
        type=float,
        default=0.55,
        help="Adaptive validation: max trade share allowed in downtrend regime.",
    )
    parser.add_argument(
        "--min-uptrend-expectancy-krw",
        type=float,
        default=0.0,
        help="Adaptive validation: min expectancy required in uptrend regime.",
    )
    parser.add_argument(
        "--min-risk-adjusted-score",
        type=float,
        default=-0.10,
        help="Adaptive validation: min risk-adjusted score.",
    )
    args = parser.parse_args(argv)

    repo_root = pathlib.Path(__file__).resolve().parents[1]
    verification_script = repo_root / "scripts" / "run_verification.py"
    fetch_script = repo_root / "scripts" / "fetch_upbit_historical_candles.py"
    data_dir = pathlib.Path(args.data_dir)
    if args.realdata_only:
        data_dir = pathlib.Path(r".\data\backtest_real")

    datasets = [x.strip() for x in str(args.datasets).split(",") if x.strip()]
    if not datasets:
        raise RuntimeError("No datasets provided.")
    dataset_paths = [resolve_dataset_path(data_dir, token) for token in datasets]

    run_refresh_if_needed(
        python_exe=sys.executable,
        fetch_script=fetch_script,
        dataset_paths=dataset_paths,
        data_mode=str(args.data_mode),
        refresh_chunk_size=int(args.refresh_chunk_size),
        refresh_sleep_ms=int(args.refresh_sleep_ms),
        refresh_end_utc=str(args.refresh_end_utc),
        allow_unsupported_refresh_datasets=bool(args.allow_unsupported_refresh_datasets),
    )

    output_tag = str(args.output_tag).strip()
    if output_tag:
        output_tag = f"_{output_tag}"
    output_json = pathlib.Path(rf".\build\Release\logs\verification_report{output_tag}.json")
    output_csv = pathlib.Path(rf".\build\Release\logs\verification_matrix{output_tag}.csv")

    cmd = [
        sys.executable,
        str(verification_script),
        "--data-dir",
        str(data_dir),
        "--dataset-names",
        *datasets,
        "--data-mode",
        str(args.data_mode),
        "--validation-profile",
        str(args.validation_profile),
        "--max-downtrend-loss-per-trade-krw",
        str(args.max_downtrend_loss_per_trade_krw),
        "--max-downtrend-trade-share",
        str(args.max_downtrend_trade_share),
        "--min-uptrend-expectancy-krw",
        str(args.min_uptrend_expectancy_krw),
        "--min-risk-adjusted-score",
        str(args.min_risk_adjusted_score),
        "--output-json",
        str(output_json),
        "--output-csv",
        str(output_csv),
    ]
    if args.realdata_only:
        cmd.append("--require-higher-tf-companions")

    proc = subprocess.run(cmd)
    return int(proc.returncode)


if __name__ == "__main__":
    raise SystemExit(main())
