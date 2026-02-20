#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
from typing import List, Tuple


_UPBIT_DATASET_RE = re.compile(
    r"^upbit_([A-Za-z0-9]+)_([A-Za-z0-9]+)_([0-9]+)m_([0-9]+)$",
    re.IGNORECASE,
)
_DEFAULT_DATASET_TOKENS = ("simulation_2000.csv", "simulation_large.csv")


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


def _split_dataset_tokens(raw: str) -> List[str]:
    return [x.strip() for x in str(raw or "").split(",") if x.strip()]


def _find_duplicate_strings(values: List[str]) -> List[str]:
    counts = {}
    for item in values:
        key = str(item).strip().lower()
        if not key:
            continue
        counts[key] = counts.get(key, 0) + 1
    return sorted([k for k, c in counts.items() if int(c) > 1])


def _has_realdata_higher_tf_companions(data_dir: pathlib.Path, primary_1m_file: pathlib.Path) -> bool:
    stem = primary_1m_file.stem
    if "_1m_" not in stem:
        return False
    prefix = stem.split("_1m_", 1)[0]
    for tf in ("5m", "15m", "60m", "240m"):
        if not any(data_dir.glob(f"{prefix}_{tf}_*.csv")):
            return False
    return True


def discover_default_realdata_datasets(data_dir: pathlib.Path) -> Tuple[List[str], List[str]]:
    datasets: List[str] = []
    missing_companion: List[str] = []
    for candidate in sorted(data_dir.glob("upbit_*_1m_*.csv"), key=lambda p: p.name.lower()):
        if _has_realdata_higher_tf_companions(data_dir, candidate):
            datasets.append(candidate.name)
        else:
            missing_companion.append(candidate.name)
    return datasets, missing_companion


def _validate_realdata_only_dataset_selection(data_dir: pathlib.Path, dataset_paths: List[pathlib.Path]) -> None:
    data_dir_resolved = data_dir.resolve()
    for dataset_path in dataset_paths:
        if dataset_path.resolve().parent != data_dir_resolved:
            raise RuntimeError(
                "Verification CLI is fixed to data/backtest_real only: "
                f"{dataset_path}"
            )
        name = dataset_path.name
        if not (name.lower().startswith("upbit_") and "_1m_" in name.lower()):
            raise RuntimeError(
                "Verification CLI accepts only primary 1m realdata datasets: "
                f"{name}"
            )
        if not _has_realdata_higher_tf_companions(data_dir, dataset_path):
            raise RuntimeError(
                "Missing companion timeframe csv (5m/15m/60m/240m) for: "
                f"{name}"
            )


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

def detect_cmake_path(explicit_path: str) -> str:
    token = str(explicit_path or "").strip()
    if token:
        candidate = pathlib.Path(token)
        if candidate.exists():
            return str(candidate)
        raise FileNotFoundError(f"cmake executable not found: {candidate}")

    found = shutil.which("cmake")
    if found:
        return found

    fallback = pathlib.Path(
        r"D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"
    )
    if fallback.exists():
        return str(fallback)

    raise FileNotFoundError(
        "cmake executable not found. Use --cmake-path or add cmake to PATH."
    )


def run_build_with_retry(
    cmake_path: str,
    repo_root: pathlib.Path,
    build_dir: str,
    build_config: str,
    build_target: str,
    build_jobs: int,
    build_retries: int,
    build_retry_wait_sec: float,
) -> None:
    cmd = [
        cmake_path,
        "--build",
        str((repo_root / build_dir).resolve()),
        "--config",
        str(build_config),
        "--target",
        str(build_target),
        "-j",
        str(max(1, int(build_jobs))),
    ]
    tries = max(1, int(build_retries))
    wait_sec = max(0.2, float(build_retry_wait_sec))
    for attempt in range(1, tries + 1):
        print(f"[verify_baseline] build attempt {attempt}/{tries}", flush=True)
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="ignore")
        if int(proc.returncode) == 0:
            if attempt > 1:
                print(f"[verify_baseline] build recovered on retry {attempt}/{tries}", flush=True)
            else:
                print("[verify_baseline] build completed", flush=True)
            return

        merged = (proc.stdout or "") + "\n" + (proc.stderr or "")
        lock_like_error = (
            "LNK1104" in merged and "AutoLifeTrading.exe" in merged
        ) or ("cannot open" in merged.lower() and "autolifetrading.exe" in merged.lower())
        if lock_like_error and attempt < tries:
            print(
                f"[verify_baseline] build lock detected (attempt {attempt}/{tries}), waiting {wait_sec:.1f}s...",
                flush=True
            )
            time.sleep(wait_sec)
            continue

        if proc.stdout:
            print(proc.stdout, end="")
        if proc.stderr:
            print(proc.stderr, end="", file=sys.stderr)
        raise RuntimeError(f"Build failed (exit={proc.returncode})")

    raise RuntimeError("Build failed after retries.")


def load_json_report(path_value: pathlib.Path):
    if not path_value.exists():
        raise FileNotFoundError(f"Verification report not found: {path_value}")
    try:
        payload = json.loads(path_value.read_text(encoding="utf-8"))
    except Exception as exc:
        raise RuntimeError(f"Failed to parse verification report JSON: {path_value}") from exc
    if not isinstance(payload, dict):
        raise RuntimeError(f"Verification report JSON is not an object: {path_value}")
    return payload


def enforce_baseline_contract(report_path: pathlib.Path) -> None:
    report = load_json_report(report_path)
    comparison = report.get("baseline_comparison", {})
    if not isinstance(comparison, dict):
        raise RuntimeError("baseline_comparison block missing in verification report")
    if not bool(comparison.get("available", False)):
        reason = str(comparison.get("reason", "")).strip() or "baseline_report_missing_or_invalid"
        raise RuntimeError(f"baseline comparison unavailable: {reason}")

    contract = comparison.get("non_degradation_contract", {})
    if not isinstance(contract, dict):
        raise RuntimeError("non_degradation_contract block missing in baseline comparison")
    if not bool(contract.get("applied", False)):
        reason = str(contract.get("reason", "")).strip() or "dataset_set_mismatch"
        raise RuntimeError(f"baseline contract not applied: {reason}")
    if not bool(contract.get("all_pass", False)):
        failed = contract.get("failed_checks", [])
        failed_text = ",".join(str(x) for x in failed) if isinstance(failed, list) else str(failed)
        raise RuntimeError(f"baseline contract failed: {failed_text}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Realdata-only verification entrypoint (adaptive validation baseline)."
    )
    parser.add_argument(
        "--datasets",
        default="",
        help=(
            "Comma-separated dataset names (upbit_*_1m_*.csv only). "
            "If empty, auto-discover from data/backtest_real with required 5m/15m/60m/240m companions."
        ),
    )
    parser.add_argument(
        "--data-dir",
        default=r".\data\backtest_real",
        help="(Fixed) dataset directory for realdata verification.",
    )
    parser.add_argument(
        "--realdata-only",
        action="store_true",
        help="Deprecated: verification is now always realdata-only.",
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
    parser.add_argument(
        "--build-first",
        action="store_true",
        help="Build target before verification (sequential-safe mode).",
    )
    parser.add_argument(
        "--cmake-path",
        default="",
        help="Optional explicit cmake executable path.",
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="CMake build directory.",
    )
    parser.add_argument(
        "--build-config",
        default="Release",
        help="Build configuration (e.g., Release/Debug).",
    )
    parser.add_argument(
        "--build-target",
        default="AutoLifeTrading",
        help="Build target name.",
    )
    parser.add_argument(
        "--build-jobs",
        type=int,
        default=1,
        help="Parallel jobs for build step.",
    )
    parser.add_argument(
        "--build-retries",
        type=int,
        default=4,
        help="Retries when build fails due to executable lock.",
    )
    parser.add_argument(
        "--build-retry-wait-sec",
        type=float,
        default=2.0,
        help="Wait seconds between build retries.",
    )
    parser.add_argument(
        "--baseline-report-path",
        default=r".\build\Release\logs\verification_report_baseline_current.json",
        help="Baseline report path used by run_verification baseline comparison.",
    )
    parser.add_argument(
        "--require-baseline-contract-pass",
        action="store_true",
        help=(
            "Fail command when baseline comparison is unavailable/mismatched or "
            "non_degradation_contract does not pass."
        ),
    )
    parser.add_argument(
        "--enable-experiment-a-signal-supply",
        action="store_true",
        help="Enable Foundation strategy candidate-supply experiment A path.",
    )
    parser.add_argument(
        "--enable-experiment-b-manager-soft-queue",
        action="store_true",
        help="Enable manager prefilter soft-queue experiment B path.",
    )
    args = parser.parse_args(argv)

    repo_root = pathlib.Path(__file__).resolve().parents[1]
    verification_script = repo_root / "scripts" / "run_verification.py"
    fetch_script = repo_root / "scripts" / "fetch_upbit_historical_candles.py"
    fixed_data_dir = (repo_root / "data" / "backtest_real").resolve()
    requested_data_dir = pathlib.Path(args.data_dir)
    requested_data_dir_resolved = (
        requested_data_dir if requested_data_dir.is_absolute() else (repo_root / requested_data_dir)
    ).resolve()
    if requested_data_dir_resolved != fixed_data_dir:
        raise RuntimeError(
            "Verification CLI is realdata-only and fixed to data/backtest_real. "
            f"requested={requested_data_dir_resolved}"
        )
    data_dir = fixed_data_dir

    datasets = _split_dataset_tokens(str(args.datasets))
    using_legacy_default_tokens = tuple(datasets) == _DEFAULT_DATASET_TOKENS
    if using_legacy_default_tokens:
        raise RuntimeError(
            "Legacy synthetic default dataset tokens are no longer accepted. "
            "Use --datasets with upbit_*_1m_*.csv or omit --datasets for realdata auto-discovery."
        )
    if not datasets:
        discovered, missing_companion = discover_default_realdata_datasets(data_dir)
        if not discovered:
            raise RuntimeError(
                "No realdata 1m datasets with required companions found under "
                f"{data_dir.resolve()} (need 5m/15m/60m/240m)."
            )
        datasets = discovered
        print(
            f"[verify_baseline] realdata-only dataset auto-discovery: count={len(datasets)}",
            flush=True,
        )
        if missing_companion:
            print(
                "[verify_baseline] realdata-only skipped (missing higher TF companions): "
                + ",".join(missing_companion),
                flush=True,
            )

    if not datasets:
        raise RuntimeError("No datasets provided.")
    duplicated_tokens = _find_duplicate_strings(datasets)
    if duplicated_tokens:
        raise RuntimeError(
            "Duplicate dataset tokens are not allowed: "
            + ",".join(duplicated_tokens)
        )
    dataset_paths = [resolve_dataset_path(data_dir, token) for token in datasets]
    duplicated_paths = _find_duplicate_strings([str(x.resolve()) for x in dataset_paths])
    if duplicated_paths:
        raise RuntimeError(
            "Duplicate dataset paths are not allowed: "
            + ",".join(pathlib.Path(x).name for x in duplicated_paths)
        )
    _validate_realdata_only_dataset_selection(data_dir, dataset_paths)

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
    baseline_report_path = pathlib.Path(str(args.baseline_report_path))
    if not baseline_report_path.is_absolute():
        baseline_report_path = (repo_root / baseline_report_path).resolve()
    resolved_build_dir = (repo_root / str(args.build_dir)).resolve()
    resolved_exe = resolved_build_dir / str(args.build_config) / "AutoLifeTrading.exe"
    resolved_cfg = resolved_build_dir / str(args.build_config) / "config" / "config.json"

    if args.build_first:
        cmake_path = detect_cmake_path(str(args.cmake_path))
        print(
            f"[verify_baseline] build-first mode: target={args.build_target} "
            f"config={args.build_config} dir={resolved_build_dir}",
            flush=True
        )
        run_build_with_retry(
            cmake_path=cmake_path,
            repo_root=repo_root,
            build_dir=str(args.build_dir),
            build_config=str(args.build_config),
            build_target=str(args.build_target),
            build_jobs=int(args.build_jobs),
            build_retries=int(args.build_retries),
            build_retry_wait_sec=float(args.build_retry_wait_sec),
        )

    cmd = [
        sys.executable,
        str(verification_script),
        "--exe-path",
        str(resolved_exe),
        "--config-path",
        str(resolved_cfg),
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
        "--baseline-report-path",
        str(baseline_report_path),
    ]
    cmd.append("--require-higher-tf-companions")
    if bool(args.enable_experiment_a_signal_supply):
        cmd.append("--enable-experiment-a-signal-supply")
    if bool(args.enable_experiment_b_manager_soft_queue):
        cmd.append("--enable-experiment-b-manager-soft-queue")

    proc = subprocess.run(cmd)
    if int(proc.returncode) != 0:
        return int(proc.returncode)
    if bool(args.require_baseline_contract_pass):
        try:
            enforce_baseline_contract(output_json.resolve())
        except Exception as exc:
            print(f"[verify_baseline] baseline contract enforcement failed: {exc}", file=sys.stderr)
            return 2
        print("[verify_baseline] baseline contract enforcement: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
