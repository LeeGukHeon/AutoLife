#!/usr/bin/env python3
import argparse
import subprocess

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
    parser.add_argument("--strict-execution-parity", "-StrictExecutionParity", action="store_true")
    parser.add_argument("--backtest-prime-csv", "-BacktestPrimeCsv", default="data/backtest/simulation_large.csv")
    parser.add_argument(
        "--backtest-prime-fallback-csv",
        "-BacktestPrimeFallbackCsv",
        default="data/backtest/auto_sim_500.csv",
    )
    parser.add_argument("--fixture-log-dir", "-FixtureLogDir", default="build/Release/logs/ci_fixture")
    parser.add_argument("--probe-market", "-ProbeMarket", default="KRW-BTC")
    parser.add_argument("--probe-notional-krw", "-ProbeNotionalKrw", type=float, default=5100.0)
    parser.add_argument("--probe-discount-pct", "-ProbeDiscountPct", type=float, default=2.0)
    parser.add_argument("--probe-cancel-delay-ms", "-ProbeCancelDelayMs", type=int, default=1500)
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    backtest_prime_csv = resolve_repo_path(args.backtest_prime_csv)
    backtest_prime_fallback_csv = resolve_repo_path(args.backtest_prime_fallback_csv)
    fixture_log_dir = resolve_repo_path(args.fixture_log_dir)
    fixture_log_path = fixture_log_dir / "autolife_ci_fixture.log"

    if not exe_path.exists():
        raise FileNotFoundError(f"Executable not found: {exe_path}")
    if not backtest_prime_csv.exists():
        raise FileNotFoundError(f"Backtest prime dataset not found: {backtest_prime_csv}")
    if not backtest_prime_fallback_csv.exists():
        raise FileNotFoundError(f"Backtest prime fallback dataset not found: {backtest_prime_fallback_csv}")

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
    prime_candidates = [backtest_prime_csv]
    if backtest_prime_fallback_csv != backtest_prime_csv:
        prime_candidates.append(backtest_prime_fallback_csv)
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
            raise FileNotFoundError(f"Backtest execution artifact not found: {backtest_artifact_path}")
        raise RuntimeError(f"Backtest execution artifact is empty after prime runs: {backtest_artifact_path}")

    if args.run_live_probe:
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
