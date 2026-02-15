#!/usr/bin/env python3
import argparse
import pathlib
import subprocess

from _script_common import read_nonempty_lines, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="build/Release/AutoLifeLiveExecutionProbe.exe")
    parser.add_argument("--market", "-Market", default="KRW-BTC")
    parser.add_argument("--notional-krw", "-NotionalKrw", type=float, default=5100.0)
    parser.add_argument("--discount-pct", "-DiscountPct", type=float, default=2.0)
    parser.add_argument("--cancel-delay-ms", "-CancelDelayMs", type=int, default=1500)
    parser.add_argument(
        "--live-execution-updates-path",
        "-LiveExecutionUpdatesPath",
        default="build/Release/logs/execution_updates_live.jsonl",
    )
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    live_path = resolve_repo_path(args.live_execution_updates_path)

    if not exe_path.exists():
        raise FileNotFoundError(f"Probe executable not found: {exe_path}")

    before_count = len(read_nonempty_lines(live_path))
    cmd = [
        str(exe_path),
        "--market",
        args.market,
        "--notional-krw",
        str(args.notional_krw),
        "--discount-pct",
        str(args.discount_pct),
        "--cancel-delay-ms",
        str(args.cancel_delay_ms),
    ]
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"Live execution probe failed with exit code {proc.returncode}")

    if not pathlib.Path(live_path).exists():
        raise FileNotFoundError(f"Live execution artifact not found: {live_path}")
    after_count = len(read_nonempty_lines(live_path))
    if after_count <= before_count:
        raise RuntimeError(f"Live execution artifact was not appended: {live_path}")

    print(f"[LiveExecutionProbe] PASSED - lines(before={before_count}, after={after_count}), path={live_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
