#!/usr/bin/env python3
import argparse
import json
import pathlib
import time
from datetime import datetime

from _script_common import resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Watch probabilistic fetch progress from manifest + data directory size."
    )
    parser.add_argument(
        "--manifest-path",
        default=r".\data\backtest_probabilistic\probabilistic_bundle_manifest.json",
    )
    parser.add_argument("--data-dir", default=r".\data\backtest_probabilistic")
    parser.add_argument("--interval-sec", type=float, default=3.0)
    parser.add_argument("--max-iterations", type=int, default=0)
    return parser.parse_args(argv)


def dir_size_mb(path_value: pathlib.Path) -> float:
    if not path_value.exists():
        return 0.0
    total = 0
    for item in path_value.glob("*"):
        if item.is_file():
            try:
                total += int(item.stat().st_size)
            except Exception:
                pass
    return round(float(total) / (1024.0 * 1024.0), 2)


def read_json(path_value: pathlib.Path):
    try:
        return json.loads(path_value.read_text(encoding="utf-8-sig"))
    except Exception:
        return None


def now_str() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def main(argv=None) -> int:
    args = parse_args(argv)
    manifest_path = resolve_repo_path(args.manifest_path)
    data_dir = resolve_repo_path(args.data_dir)
    interval = max(0.2, float(args.interval_sec))
    max_iterations = max(0, int(args.max_iterations))

    print("[WatchProbabilistic] Start monitoring. Ctrl+C to stop.")
    iteration = 0
    while True:
        size_mb = dir_size_mb(data_dir)
        stamp = now_str()
        if not manifest_path.exists():
            print(f"[{stamp}] manifest not found | dir_size={size_mb}MB", flush=True)
        else:
            payload = read_json(manifest_path)
            if not isinstance(payload, dict):
                print(f"[{stamp}] manifest parse error | dir_size={size_mb}MB", flush=True)
            else:
                run_state = str(payload.get("run_state", ""))
                progress = float(payload.get("progress_pct", 0.0))
                planned = int(payload.get("planned_job_count", 0))
                done = int(payload.get("completed_job_count", 0))
                success = int(payload.get("success_count", 0))
                failed = int(payload.get("failed_count", 0))
                skipped = int(payload.get("skipped_existing_count", 0))
                actual_csv = str(payload.get("actual_totals", {}).get("csv_human", ""))
                current_job = payload.get("current_job", {}) if isinstance(payload.get("current_job", {}), dict) else {}
                current_job_text = ""
                if current_job:
                    cur_idx = int(current_job.get("index", 0))
                    cur_total = int(current_job.get("total", 0))
                    cur_market = str(current_job.get("market", ""))
                    cur_unit = int(current_job.get("unit_min", 0))
                    cur_phase = str(current_job.get("phase", ""))
                    current_job_text = (
                        f" current_job={cur_idx}/{cur_total}:{cur_market}:{cur_unit}m:{cur_phase}"
                    )
                print(
                    f"[{stamp}] state={run_state} progress={progress}% jobs={done}/{planned} "
                    f"success={success} failed={failed} skipped={skipped} "
                    f"actual_csv={actual_csv} dir_size={size_mb}MB{current_job_text}",
                    flush=True,
                )

        iteration += 1
        if max_iterations > 0 and iteration >= max_iterations:
            break
        time.sleep(interval)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
