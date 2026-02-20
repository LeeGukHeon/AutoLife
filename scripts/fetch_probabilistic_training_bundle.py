#!/usr/bin/env python3
import argparse
import csv
import json
import math
import pathlib
import subprocess
import sys
import time
from datetime import datetime, timezone
from typing import Dict, List, Tuple

from _script_common import dump_json, ensure_parent_directory, resolve_repo_path


UPBIT_EPOCH_UTC = "2017-10-24T00:00:00Z"
DEFAULT_MAJOR_MARKETS = "KRW-BTC,KRW-ETH,KRW-XRP,KRW-SOL,KRW-DOGE"
DEFAULT_ALT_MARKETS = "KRW-ETC,KRW-BCH,KRW-EOS,KRW-XLM,KRW-QTUM"
DEFAULT_TIMEFRAMES = "1,5,15,60,240"


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Sequential Upbit long-horizon dataset bundle fetcher for "
            "probabilistic pattern-model training."
        )
    )
    parser.add_argument("--markets-major", default=DEFAULT_MAJOR_MARKETS)
    parser.add_argument("--markets-alt", default=DEFAULT_ALT_MARKETS)
    parser.add_argument("--timeframes", default=DEFAULT_TIMEFRAMES)
    parser.add_argument("--history-start-utc", default=UPBIT_EPOCH_UTC)
    parser.add_argument("--history-end-utc", default="")
    parser.add_argument("--output-dir", default=r".\data\backtest_probabilistic")
    parser.add_argument(
        "--manifest-json",
        default=r".\data\backtest_probabilistic\probabilistic_bundle_manifest.json",
    )
    parser.add_argument(
        "--summary-json",
        default=r".\build\Release\logs\probabilistic_bundle_summary.json",
    )
    parser.add_argument("--estimate-only", action="store_true")
    parser.add_argument("--skip-existing", action="store_true")
    parser.add_argument("--max-jobs", type=int, default=0)
    parser.add_argument("--sleep-ms-between-jobs", type=int, default=350)
    parser.add_argument("--chunk-size", type=int, default=200)
    parser.add_argument("--sleep-ms-per-request", type=int, default=120)
    parser.add_argument("--indicator-columns", type=int, default=30)
    parser.add_argument("--est-bytes-per-row-ohlcv", type=int, default=110)
    parser.add_argument("--est-bytes-per-indicator-col", type=int, default=15)
    parser.add_argument("--python-exe", default=sys.executable)
    parser.add_argument(
        "--fetch-script",
        default=r".\scripts\fetch_upbit_historical_candles.py",
    )
    return parser.parse_args(argv)


def parse_utc(value: str, default_to_now: bool) -> datetime:
    token = str(value or "").strip()
    if not token:
        if default_to_now:
            return datetime.now(tz=timezone.utc)
        raise RuntimeError("UTC value is required.")
    return datetime.fromisoformat(token.replace("Z", "+00:00")).astimezone(timezone.utc)


def split_tokens(raw: str) -> List[str]:
    return [x.strip().upper() for x in str(raw or "").split(",") if x.strip()]


def normalize_markets(major_raw: str, alt_raw: str) -> Tuple[List[str], List[str], List[str]]:
    major = split_tokens(major_raw)
    alt = split_tokens(alt_raw)
    dedup_ordered: List[str] = []
    seen = set()
    for token in major + alt:
        if token in seen:
            continue
        seen.add(token)
        dedup_ordered.append(token)
    return major, alt, dedup_ordered


def normalize_timeframes(raw: str) -> List[int]:
    valid = {1, 3, 5, 10, 15, 30, 60, 240}
    out: List[int] = []
    seen = set()
    for token in split_tokens(raw):
        try:
            unit = int(token)
        except Exception:
            continue
        if unit not in valid or unit in seen:
            continue
        seen.add(unit)
        out.append(unit)
    if not out:
        raise RuntimeError("No valid timeframes configured.")
    return sorted(out)


def estimate_candle_count(start_utc: datetime, end_utc: datetime, unit_min: int) -> int:
    if end_utc <= start_utc:
        return 0
    span_min = (end_utc - start_utc).total_seconds() / 60.0
    return max(1, int(math.ceil(span_min / float(max(1, unit_min)))))


def build_job_list(
    markets: List[str],
    timeframe_units: List[int],
    start_utc: datetime,
    end_utc: datetime,
    output_dir: pathlib.Path,
) -> List[Dict[str, object]]:
    jobs: List[Dict[str, object]] = []
    for market in markets:
        safe_market = market.replace("-", "_")
        for unit in timeframe_units:
            candles = estimate_candle_count(start_utc, end_utc, unit)
            file_name = f"upbit_{safe_market}_{unit}m_full.csv"
            output_path = output_dir / file_name
            jobs.append(
                {
                    "market": market,
                    "unit_min": int(unit),
                    "estimated_candles": int(candles),
                    "output_path": str(output_path),
                }
            )
    return jobs


def estimate_size_bytes(
    rows: int,
    indicator_columns: int,
    bytes_per_row_ohlcv: int,
    bytes_per_indicator_col: int,
) -> Dict[str, int]:
    base = max(0, int(rows)) * max(1, int(bytes_per_row_ohlcv))
    enriched = max(0, int(rows)) * (
        max(1, int(bytes_per_row_ohlcv)) + (max(0, int(indicator_columns)) * max(1, int(bytes_per_indicator_col)))
    )
    return {"raw_ohlcv_csv_bytes": int(base), "enriched_csv_bytes": int(enriched)}


def human_size(byte_count: int) -> str:
    value = float(max(0, int(byte_count)))
    units = ["B", "KB", "MB", "GB", "TB"]
    idx = 0
    while value >= 1024.0 and idx < len(units) - 1:
        value /= 1024.0
        idx += 1
    return f"{value:.2f} {units[idx]}"


def classify_jobs(manifest_jobs: List[Dict[str, object]]) -> Tuple[List[Dict[str, object]], List[Dict[str, object]], List[Dict[str, object]]]:
    success_jobs = [x for x in manifest_jobs if str(x.get("status", "")).startswith("fetched")]
    failed_jobs = [x for x in manifest_jobs if str(x.get("status", "")) == "failed"]
    skipped_jobs = [x for x in manifest_jobs if str(x.get("status", "")) == "skipped_existing"]
    return success_jobs, failed_jobs, skipped_jobs


def build_manifest_payload(
    *,
    generated_at_utc: str,
    run_state: str,
    start_utc: datetime,
    end_utc: datetime,
    major_markets: List[str],
    alt_markets: List[str],
    all_markets: List[str],
    timeframe_units: List[int],
    estimate_only: bool,
    skip_existing: bool,
    planned_job_count: int,
    total_estimated_rows: int,
    total_estimated_raw_bytes: int,
    total_estimated_enriched_bytes: int,
    indicator_columns: int,
    manifest_jobs: List[Dict[str, object]],
    current_job: Dict[str, object] | None,
) -> Dict[str, object]:
    success_jobs, failed_jobs, skipped_jobs = classify_jobs(manifest_jobs)
    actual_rows = sum(int(x.get("rows", 0)) for x in success_jobs + skipped_jobs)
    actual_bytes = sum(int(x.get("file_size_bytes", 0)) for x in success_jobs + skipped_jobs)
    completed_job_count = len(manifest_jobs)
    progress_pct = (
        round((float(completed_job_count) / float(planned_job_count)) * 100.0, 2)
        if int(planned_job_count) > 0
        else 0.0
    )

    return {
        "generated_at_utc": generated_at_utc,
        "run_state": str(run_state),
        "history_window": {
            "start_utc": start_utc.isoformat(),
            "end_utc": end_utc.isoformat(),
        },
        "market_groups": {
            "major": major_markets,
            "alt": alt_markets,
            "all_unique": all_markets,
        },
        "timeframes_min": timeframe_units,
        "estimate_only": bool(estimate_only),
        "skip_existing": bool(skip_existing),
        "planned_job_count": int(planned_job_count),
        "completed_job_count": int(completed_job_count),
        "progress_pct": float(progress_pct),
        "job_count": completed_job_count,
        "success_count": len(success_jobs),
        "failed_count": len(failed_jobs),
        "skipped_existing_count": len(skipped_jobs),
        "estimated_totals": {
            "rows": int(total_estimated_rows),
            "raw_ohlcv_csv_bytes": int(total_estimated_raw_bytes),
            "raw_ohlcv_csv_human": human_size(total_estimated_raw_bytes),
            "enriched_csv_bytes": int(total_estimated_enriched_bytes),
            "enriched_csv_human": human_size(total_estimated_enriched_bytes),
            "indicator_columns_assumed": int(indicator_columns),
        },
        "actual_totals": {
            "rows": int(actual_rows),
            "csv_bytes": int(actual_bytes),
            "csv_human": human_size(actual_bytes),
        },
        "current_job": current_job if isinstance(current_job, dict) else {},
        "jobs": manifest_jobs,
    }


def build_summary_payload(manifest_json: pathlib.Path, manifest_payload: Dict[str, object]) -> Dict[str, object]:
    return {
        "generated_at_utc": manifest_payload["generated_at_utc"],
        "run_state": manifest_payload.get("run_state", ""),
        "manifest_json": str(manifest_json),
        "estimate_only": bool(manifest_payload.get("estimate_only", False)),
        "planned_job_count": int(manifest_payload.get("planned_job_count", 0)),
        "job_count": int(manifest_payload.get("job_count", 0)),
        "success_count": int(manifest_payload.get("success_count", 0)),
        "failed_count": int(manifest_payload.get("failed_count", 0)),
        "skipped_existing_count": int(manifest_payload.get("skipped_existing_count", 0)),
        "progress_pct": float(manifest_payload.get("progress_pct", 0.0)),
        "estimated_raw_csv_human": manifest_payload["estimated_totals"]["raw_ohlcv_csv_human"],
        "estimated_enriched_csv_human": manifest_payload["estimated_totals"]["enriched_csv_human"],
        "actual_csv_human": manifest_payload["actual_totals"]["csv_human"],
    }


def read_csv_window(path_value: pathlib.Path) -> Tuple[int, int, int]:
    if not path_value.exists():
        return 0, 0, 0
    first_ts = 0
    last_ts = 0
    rows = 0
    with path_value.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            try:
                ts = int(float(row.get("timestamp", 0)))
            except Exception:
                ts = 0
            if rows == 0:
                first_ts = ts
            last_ts = ts
            rows += 1
    return rows, first_ts, last_ts


def utc_iso_from_ms(ts_ms: int) -> str:
    if int(ts_ms) <= 0:
        return ""
    return datetime.fromtimestamp(float(ts_ms) / 1000.0, tz=timezone.utc).isoformat()


def run_fetch_job(
    python_exe: str,
    fetch_script: pathlib.Path,
    market: str,
    unit_min: int,
    candles: int,
    output_path: pathlib.Path,
    end_utc: datetime,
    chunk_size: int,
    sleep_ms_per_request: int,
) -> subprocess.CompletedProcess:
    cmd = [
        str(python_exe),
        str(fetch_script),
        "--market",
        str(market),
        "--unit",
        str(unit_min),
        "--candles",
        str(int(candles)),
        "--output-path",
        str(output_path),
        "--end-utc",
        end_utc.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "--chunk-size",
        str(int(chunk_size)),
        "--sleep-ms",
        str(int(sleep_ms_per_request)),
    ]
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )


def main(argv=None) -> int:
    args = parse_args(argv)

    start_utc = parse_utc(args.history_start_utc, default_to_now=False)
    end_utc = parse_utc(args.history_end_utc, default_to_now=True)
    if end_utc <= start_utc:
        raise RuntimeError("history_end_utc must be later than history_start_utc.")

    major_markets, alt_markets, all_markets = normalize_markets(args.markets_major, args.markets_alt)
    timeframe_units = normalize_timeframes(args.timeframes)

    output_dir = resolve_repo_path(args.output_dir)
    manifest_json = resolve_repo_path(args.manifest_json)
    summary_json = resolve_repo_path(args.summary_json)
    fetch_script = resolve_repo_path(args.fetch_script)
    if not fetch_script.exists():
        raise FileNotFoundError(f"fetch script not found: {fetch_script}")

    output_dir.mkdir(parents=True, exist_ok=True)
    ensure_parent_directory(manifest_json)
    ensure_parent_directory(summary_json)

    jobs = build_job_list(
        markets=all_markets,
        timeframe_units=timeframe_units,
        start_utc=start_utc,
        end_utc=end_utc,
        output_dir=output_dir,
    )
    if int(args.max_jobs) > 0:
        jobs = jobs[: int(args.max_jobs)]
    planned_job_count = len(jobs)

    manifest_jobs: List[Dict[str, object]] = []
    total_estimated_rows = 0
    total_estimated_raw_bytes = 0
    total_estimated_enriched_bytes = 0
    current_job: Dict[str, object] | None = None

    def flush_progress(run_state: str) -> Dict[str, object]:
        payload = build_manifest_payload(
            generated_at_utc=datetime.now(tz=timezone.utc).isoformat(),
            run_state=run_state,
            start_utc=start_utc,
            end_utc=end_utc,
            major_markets=major_markets,
            alt_markets=alt_markets,
            all_markets=all_markets,
            timeframe_units=timeframe_units,
            estimate_only=bool(args.estimate_only),
            skip_existing=bool(args.skip_existing),
            planned_job_count=planned_job_count,
            total_estimated_rows=total_estimated_rows,
            total_estimated_raw_bytes=total_estimated_raw_bytes,
            total_estimated_enriched_bytes=total_estimated_enriched_bytes,
            indicator_columns=int(args.indicator_columns),
            manifest_jobs=manifest_jobs,
            current_job=current_job,
        )
        dump_json(manifest_json, payload)
        dump_json(summary_json, build_summary_payload(manifest_json, payload))
        return payload

    flush_progress("in_progress")

    for idx, job in enumerate(jobs, start=1):
        current_job = {
            "index": int(idx),
            "total": int(planned_job_count),
            "market": str(job["market"]),
            "unit_min": int(job["unit_min"]),
            "phase": "preparing",
            "started_at_utc": datetime.now(tz=timezone.utc).isoformat(),
        }
        flush_progress("in_progress")
        print(
            f"[FetchProbabilisticBundle] start={idx}/{planned_job_count} "
            f"market={job['market']} unit={job['unit_min']}m"
        )

        est_rows = int(job["estimated_candles"])
        est_size = estimate_size_bytes(
            rows=est_rows,
            indicator_columns=int(args.indicator_columns),
            bytes_per_row_ohlcv=int(args.est_bytes_per_row_ohlcv),
            bytes_per_indicator_col=int(args.est_bytes_per_indicator_col),
        )
        total_estimated_rows += est_rows
        total_estimated_raw_bytes += int(est_size["raw_ohlcv_csv_bytes"])
        total_estimated_enriched_bytes += int(est_size["enriched_csv_bytes"])

        job_record: Dict[str, object] = {
            "market": str(job["market"]),
            "unit_min": int(job["unit_min"]),
            "output_path": str(job["output_path"]),
            "estimated_candles": est_rows,
            "estimated_sizes": est_size,
            "status": "planned",
            "rows": 0,
            "file_size_bytes": 0,
            "from_utc": "",
            "to_utc": "",
            "stderr_tail": "",
        }

        output_path = pathlib.Path(str(job["output_path"]))
        if bool(args.skip_existing) and output_path.exists():
            rows, first_ts, last_ts = read_csv_window(output_path)
            file_size = output_path.stat().st_size if output_path.exists() else 0
            job_record["status"] = "skipped_existing"
            job_record["rows"] = int(rows)
            job_record["file_size_bytes"] = int(file_size)
            job_record["from_utc"] = utc_iso_from_ms(first_ts)
            job_record["to_utc"] = utc_iso_from_ms(last_ts)
            manifest_jobs.append(job_record)
            current_job = None
            flush_progress("in_progress")
            print(
                f"[FetchProbabilisticBundle] progress={idx}/{planned_job_count} "
                f"status=skipped_existing market={job_record['market']} unit={job_record['unit_min']}m "
                f"rows={job_record['rows']} size={human_size(int(job_record['file_size_bytes']))}"
            )
            continue

        if not bool(args.estimate_only):
            started = time.time()
            current_job = {
                "index": int(idx),
                "total": int(planned_job_count),
                "market": str(job["market"]),
                "unit_min": int(job["unit_min"]),
                "phase": "fetching",
                "started_at_utc": datetime.now(tz=timezone.utc).isoformat(),
                "estimated_rows": int(est_rows),
            }
            flush_progress("in_progress")
            proc = run_fetch_job(
                python_exe=str(args.python_exe),
                fetch_script=fetch_script,
                market=str(job["market"]),
                unit_min=int(job["unit_min"]),
                candles=est_rows,
                output_path=output_path,
                end_utc=end_utc,
                chunk_size=int(args.chunk_size),
                sleep_ms_per_request=int(args.sleep_ms_per_request),
            )
            elapsed = round(time.time() - started, 2)
            job_record["elapsed_sec"] = float(elapsed)
            if int(proc.returncode) != 0:
                job_record["status"] = "failed"
                stderr_lines = [x.strip() for x in (proc.stderr or "").splitlines() if x.strip()]
                stdout_lines = [x.strip() for x in (proc.stdout or "").splitlines() if x.strip()]
                merged_tail = stderr_lines[-2:] + stdout_lines[-2:]
                job_record["stderr_tail"] = " || ".join(merged_tail)[-600:]
                manifest_jobs.append(job_record)
                current_job = None
                flush_progress("in_progress")
                print(
                    f"[FetchProbabilisticBundle] progress={idx}/{planned_job_count} "
                    f"status=failed market={job_record['market']} unit={job_record['unit_min']}m"
                )
                continue

            rows, first_ts, last_ts = read_csv_window(output_path)
            file_size = output_path.stat().st_size if output_path.exists() else 0
            job_record["status"] = "fetched"
            job_record["rows"] = int(rows)
            job_record["file_size_bytes"] = int(file_size)
            job_record["from_utc"] = utc_iso_from_ms(first_ts)
            job_record["to_utc"] = utc_iso_from_ms(last_ts)
            manifest_jobs.append(job_record)
            current_job = None
            flush_progress("in_progress")
            print(
                f"[FetchProbabilisticBundle] progress={idx}/{planned_job_count} "
                f"status=fetched market={job_record['market']} unit={job_record['unit_min']}m "
                f"rows={job_record['rows']} size={human_size(int(job_record['file_size_bytes']))}"
            )

            if int(args.sleep_ms_between_jobs) > 0:
                time.sleep(float(args.sleep_ms_between_jobs) / 1000.0)
        else:
            job_record["status"] = "estimated"
            manifest_jobs.append(job_record)
            current_job = None
            flush_progress("in_progress")
            print(
                f"[FetchProbabilisticBundle] progress={idx}/{planned_job_count} "
                f"status=estimated market={job_record['market']} unit={job_record['unit_min']}m "
                f"est_rows={job_record['estimated_candles']}"
            )
    manifest_payload = flush_progress("completed")
    success_jobs, failed_jobs, skipped_jobs = classify_jobs(manifest_jobs)

    print("[FetchProbabilisticBundle] Completed")
    print(f"manifest={manifest_json}")
    print(f"summary={summary_json}")
    print(f"job_count={len(manifest_jobs)}")
    print(f"success_count={len(success_jobs)}")
    print(f"failed_count={len(failed_jobs)}")
    print(f"skipped_existing_count={len(skipped_jobs)}")
    print(f"estimated_raw_csv={manifest_payload['estimated_totals']['raw_ohlcv_csv_human']}")
    print(f"estimated_enriched_csv={manifest_payload['estimated_totals']['enriched_csv_human']}")
    print(f"actual_csv={manifest_payload['actual_totals']['csv_human']}")

    return 0 if len(failed_jobs) == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
