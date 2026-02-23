#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import sys
from datetime import datetime, timezone

from _script_common import resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run probabilistic bundle fetch with real-time console + log tee."
    )
    parser.add_argument("--timeframes", default="240,60")
    parser.add_argument("--markets-major", default="KRW-BTC,KRW-ETH,KRW-XRP,KRW-SOL,KRW-DOGE")
    parser.add_argument("--markets-alt", default="KRW-ETC,KRW-BCH,KRW-EOS,KRW-XLM,KRW-QTUM")
    parser.add_argument("--sleep-ms-between-jobs", type=int, default=350)
    parser.add_argument("--sleep-ms-per-request", type=int, default=120)
    parser.add_argument("--max-jobs", type=int, default=0)
    parser.add_argument("--skip-existing", dest="skip_existing", action="store_true", default=True)
    parser.add_argument("--no-skip-existing", dest="skip_existing", action="store_false")
    parser.add_argument("--log-path", default=r".\build\Release\logs\fetch_probabilistic_live.log")
    parser.add_argument("--fresh-log", dest="fresh_log", action="store_true", default=True)
    parser.add_argument("--reuse-log", dest="fresh_log", action="store_false")
    parser.add_argument("--python-exe", default=sys.executable)
    parser.add_argument("--fetch-script", default=r".\scripts\fetch_probabilistic_training_bundle.py")
    parser.add_argument("--universe-file", default="")
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def choose_log_path(base_log_path: pathlib.Path, fresh_log: bool) -> pathlib.Path:
    if bool(fresh_log) and base_log_path.exists():
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        return base_log_path.with_name(f"{base_log_path.stem}_{stamp}{base_log_path.suffix}")
    try:
        with base_log_path.open("a", encoding="utf-8", newline=""):
            pass
        return base_log_path
    except PermissionError:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        return base_log_path.with_name(f"{base_log_path.stem}_{stamp}{base_log_path.suffix}")


def main(argv=None) -> int:
    args = parse_args(argv)
    fetch_script = resolve_repo_path(args.fetch_script)
    if not fetch_script.exists():
        raise FileNotFoundError(f"fetch script not found: {fetch_script}")

    log_path = resolve_repo_path(args.log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(args.python_exe),
        "-u",
        str(fetch_script),
        "--timeframes",
        str(args.timeframes),
        "--markets-major",
        str(args.markets_major),
        "--markets-alt",
        str(args.markets_alt),
        "--sleep-ms-between-jobs",
        str(int(args.sleep_ms_between_jobs)),
        "--sleep-ms-per-request",
        str(int(args.sleep_ms_per_request)),
    ]
    if bool(args.skip_existing):
        cmd.append("--skip-existing")
    if int(args.max_jobs) > 0:
        cmd.extend(["--max-jobs", str(int(args.max_jobs))])
    if str(args.universe_file).strip():
        cmd.extend(["--universe-file", str(resolve_repo_path(args.universe_file))])

    log_path = choose_log_path(log_path, bool(args.fresh_log))
    print(f"[RunProbabilisticFetch] started_at={utc_now_iso()}", flush=True)
    print(f"[RunProbabilisticFetch] cmd={' '.join(cmd)}", flush=True)
    print(f"[RunProbabilisticFetch] log={log_path}", flush=True)

    with log_path.open("a", encoding="utf-8", newline="") as lf:
        lf.write(f"[RunProbabilisticFetch] started_at={utc_now_iso()}\n")
        lf.write(f"[RunProbabilisticFetch] cmd={' '.join(cmd)}\n")
        lf.write(f"[RunProbabilisticFetch] log={log_path}\n")
        lf.flush()

        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="ignore",
            bufsize=1,
        )
        try:
            assert proc.stdout is not None
            for line in proc.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                lf.write(line)
                lf.flush()
        finally:
            exit_code = int(proc.wait())
            lf.write(f"[RunProbabilisticFetch] finished_at={utc_now_iso()} exit_code={exit_code}\n")
            lf.flush()

    print(f"[RunProbabilisticFetch] exit_code={exit_code}", flush=True)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
