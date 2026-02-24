#!/usr/bin/env python3
import argparse
import subprocess

import compare_execution_event_distribution
import generate_live_execution_probe
import prepare_operational_readiness_fixture
import validate_operational_readiness
import validate_readiness
from _script_common import read_nonempty_lines, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="build/Release/AutoLifeTrading.exe")
    parser.add_argument("--include-backtest", "-IncludeBacktest", action="store_true")
    parser.add_argument("--run-live-probe", "-RunLiveProbe", action="store_true")
    parser.add_argument("--allow-live-probe-order", "-AllowLiveProbeOrder", action="store_true")
    parser.add_argument("--strict-execution-parity", "-StrictExecutionParity", action="store_true")
    parser.add_argument("--backtest-prime-csv", "-BacktestPrimeCsv", default="data/backtest/simulation_large.csv")
    parser.add_argument(
        "--backtest-prime-fallback-csv",
        "-BacktestPrimeFallbackCsv",
        default="data/backtest/auto_sim_500.csv",
    )
    parser.add_argument("--fixture-log-dir", "-FixtureLogDir", default="build/Release/logs/ci_fixture")
    parser.add_argument(
        "--execution-prime-datasets",
        "-ExecutionPrimeDatasets",
        default=(
            "data/backtest_real/upbit_KRW_DOGE_1m_12000.csv,"
            "data/backtest_real/upbit_KRW_XRP_1m_12000.csv,"
            "data/backtest_real/upbit_KRW_BTC_1m_12000.csv"
        ),
        help=(
            "Comma-separated extra datasets to prime execution_updates_backtest.jsonl "
            "when default backtest prime dataset produces no fills."
        ),
    )
    parser.add_argument("--probe-market", "-ProbeMarket", default="KRW-BTC")
    parser.add_argument("--probe-notional-krw", "-ProbeNotionalKrw", type=float, default=5100.0)
    parser.add_argument("--probe-discount-pct", "-ProbeDiscountPct", type=float, default=2.0)
    parser.add_argument("--probe-cancel-delay-ms", "-ProbeCancelDelayMs", type=int, default=1500)
    parser.add_argument(
        "--skip-execution-distribution-compare",
        "-SkipExecutionDistributionCompare",
        action="store_true",
        help="Skip execution event distribution comparison artifact generation.",
    )
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    backtest_prime_csv = resolve_repo_path(args.backtest_prime_csv)
    backtest_prime_fallback_csv = resolve_repo_path(args.backtest_prime_fallback_csv)
    fixture_log_dir = resolve_repo_path(args.fixture_log_dir)
    fixture_log_path = fixture_log_dir / "autolife_ci_fixture.log"
    extra_execution_prime_raw = str(args.execution_prime_datasets).strip()

    if not exe_path.exists():
        raise FileNotFoundError(f"Executable not found: {exe_path}")
    prime_available = backtest_prime_csv.exists()
    fallback_available = backtest_prime_fallback_csv.exists()
    if not prime_available and not fallback_available:
        raise FileNotFoundError(
            "Backtest prime dataset not found and fallback missing: "
            f"prime={backtest_prime_csv}, fallback={backtest_prime_fallback_csv}"
        )
    if not prime_available and fallback_available:
        print(
            "[CIGate] Backtest prime dataset missing, using fallback first: "
            f"{backtest_prime_fallback_csv}"
        )
    if not fallback_available:
        print(f"[CIGate] Backtest fallback dataset missing, continuing without it: {backtest_prime_fallback_csv}")

    fixture_exit = prepare_operational_readiness_fixture.main(
        ["-LogPath", str(fixture_log_path)]
    )
    if fixture_exit != 0:
        raise RuntimeError(f"Fixture preparation failed with exit code {fixture_exit}")

    if args.include_backtest:
        prewarm_exit = validate_readiness.main(
            ["-ExePath", str(exe_path), "-OutputJson", "build/Release/logs/readiness_report_ci_prewarm.json"]
        )
        if prewarm_exit != 0:
            raise RuntimeError(f"Backtest prewarm failed with exit code {prewarm_exit}")

    backtest_artifact_path = resolve_repo_path("build/Release/logs/execution_updates_backtest.jsonl")
    prime_candidates = []
    if prime_available:
        prime_candidates.append(backtest_prime_csv)
    if fallback_available and backtest_prime_fallback_csv not in prime_candidates:
        prime_candidates.append(backtest_prime_fallback_csv)
    if extra_execution_prime_raw:
        for token in extra_execution_prime_raw.split(","):
            candidate_raw = token.strip()
            if not candidate_raw:
                continue
            candidate_path = resolve_repo_path(candidate_raw)
            if not candidate_path.exists():
                continue
            if candidate_path not in prime_candidates:
                prime_candidates.append(candidate_path)
    artifact_populated = False
    for prime_csv in prime_candidates:
        prime_proc = subprocess.run(
            [str(exe_path), "--backtest", str(prime_csv), "--json"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if prime_proc.returncode != 0:
            raise RuntimeError(f"Backtest execution artifact prime run failed with exit code {prime_proc.returncode}")
        if backtest_artifact_path.exists() and len(read_nonempty_lines(backtest_artifact_path)) > 0:
            artifact_populated = True
            break
    if not artifact_populated:
        if not backtest_artifact_path.exists():
            print(f"[CIGate] Backtest execution artifact missing after prime runs: {backtest_artifact_path}")
        else:
            print(f"[CIGate] Backtest execution artifact empty after prime runs: {backtest_artifact_path}")

    if not args.skip_execution_distribution_compare:
        compare_exit = compare_execution_event_distribution.main(
            [
                "--live-execution-updates-path",
                "build/Release/logs/execution_updates_live.jsonl",
                "--backtest-execution-updates-path",
                "build/Release/logs/execution_updates_backtest.jsonl",
                "--output-json",
                "build/Release/logs/execution_event_distribution_comparison_ci.json",
            ]
        )
        if compare_exit != 0:
            raise RuntimeError(
                "Execution event distribution comparison failed "
                f"with exit code {compare_exit}"
            )

    if args.run_live_probe:
        if not args.allow_live_probe_order:
            print("[CIGate] BLOCKED - add --allow-live-probe-order with --run-live-probe")
            return 1
        probe_exit = generate_live_execution_probe.main(
            [
                "-Market",
                args.probe_market,
                "-NotionalKrw",
                str(args.probe_notional_krw),
                "-DiscountPct",
                str(args.probe_discount_pct),
                "-CancelDelayMs",
                str(args.probe_cancel_delay_ms),
                "-AllowLiveOrder",
            ]
        )
        if probe_exit != 0:
            raise RuntimeError(f"Live probe failed with exit code {probe_exit}")

    operational_args = [
        "-StrictLogCheck",
        "-LogDir",
        str(fixture_log_dir),
    ]
    if args.include_backtest:
        operational_args.append("-IncludeBacktest")
    if args.strict_execution_parity:
        operational_args.append("-StrictExecutionParity")

    operational_exit = validate_operational_readiness.main(operational_args)
    if operational_exit != 0:
        raise RuntimeError(f"Operational readiness gate failed with exit code {operational_exit}")

    print("[CIGate] PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
