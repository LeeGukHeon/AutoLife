#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
import math
import pathlib
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from typing import Dict, List, Tuple, Optional

from _script_common import dump_json, ensure_parent_directory, load_json_or_none, resolve_repo_path


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
    parser.add_argument(
        "--incremental-update",
        action="store_true",
        help=(
            "Incremental mode: append new candles to existing CSV using last timestamp "
            "(with small overlap)."
        ),
    )
    parser.add_argument(
        "--incremental-overlap-bars",
        type=int,
        default=3,
        help="Overlap bars per timeframe when incremental-update is enabled.",
    )
    parser.add_argument("--max-jobs", type=int, default=0)
    parser.add_argument("--sleep-ms-between-jobs", type=int, default=350)
    parser.add_argument("--chunk-size", type=int, default=200)
    parser.add_argument("--sleep-ms-per-request", type=int, default=120)
    parser.add_argument(
        "--disk-budget-policy",
        choices=("halt", "skip"),
        default="halt",
        help="When estimated next job exceeds storage guard, halt run or skip only that job.",
    )
    parser.add_argument(
        "--max-output-gb",
        type=float,
        default=0.0,
        help="Optional hard cap for output-dir total size (0 disables cap).",
    )
    parser.add_argument(
        "--min-free-gb",
        type=float,
        default=20.0,
        help="Keep at least this much free disk space while fetching.",
    )
    parser.add_argument("--indicator-columns", type=int, default=30)
    parser.add_argument("--est-bytes-per-row-ohlcv", type=int, default=110)
    parser.add_argument("--est-bytes-per-indicator-col", type=int, default=15)
    parser.add_argument("--python-exe", default=sys.executable)
    parser.add_argument(
        "--fetch-script",
        default=r".\scripts\fetch_upbit_historical_candles.py",
    )
    parser.add_argument(
        "--universe-file",
        default="",
        help="Optional dynamic universe JSON; when set, 1m fetch scope follows final_1m_markets.",
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


def dedup_upper(values: List[str]) -> List[str]:
    out: List[str] = []
    seen = set()
    for raw in values:
        token = str(raw or "").strip().upper()
        if not token or token in seen:
            continue
        seen.add(token)
        out.append(token)
    return out


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


def load_universe_final_1m_markets(universe_path: pathlib.Path) -> List[str]:
    payload = load_json_or_none(universe_path)
    if not isinstance(payload, dict):
        raise RuntimeError(f"invalid universe file json: {universe_path}")
    markets_raw = payload.get("final_1m_markets", [])
    if not isinstance(markets_raw, list):
        raise RuntimeError(f"invalid final_1m_markets in universe file: {universe_path}")
    markets = dedup_upper([str(x) for x in markets_raw])
    if not markets:
        raise RuntimeError(f"empty final_1m_markets in universe file: {universe_path}")
    return markets


def sha256_file(path_value: pathlib.Path) -> str:
    hasher = hashlib.sha256()
    with path_value.open("rb") as fh:
        while True:
            chunk = fh.read(1024 * 1024)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()


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
    universe_1m_markets: Optional[List[str]] = None,
) -> List[Dict[str, object]]:
    jobs: List[Dict[str, object]] = []
    seen_keys = set()
    scoped_1m_markets = dedup_upper(universe_1m_markets or [])
    if not scoped_1m_markets:
        # Baseline ordering (market-major) remains unchanged when universe scope is OFF.
        for market in markets:
            safe_market = market.replace("-", "_")
            for unit in timeframe_units:
                key = (str(market), int(unit))
                if key in seen_keys:
                    continue
                seen_keys.add(key)
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

    for unit in timeframe_units:
        target_markets = scoped_1m_markets if int(unit) == 1 else markets
        for market in target_markets:
            key = (str(market), int(unit))
            if key in seen_keys:
                continue
            seen_keys.add(key)
            safe_market = market.replace("-", "_")
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


def bytes_from_gb(gb_value: float) -> int:
    if not math.isfinite(float(gb_value)) or float(gb_value) <= 0.0:
        return 0
    return int(float(gb_value) * (1024.0 ** 3))


def dir_size_bytes(path_value: pathlib.Path) -> int:
    if not path_value.exists():
        return 0
    total = 0
    for item in path_value.rglob("*"):
        if not item.is_file():
            continue
        try:
            total += int(item.stat().st_size)
        except Exception:
            continue
    return int(total)


def build_storage_guard_state(
    output_dir: pathlib.Path,
    current_output_bytes: int,
    max_output_bytes: int,
    min_free_bytes: int,
) -> Dict[str, object]:
    usage = shutil.disk_usage(output_dir)
    free_bytes = int(max(0, usage.free))
    required_headroom = int(max(0, min_free_bytes))
    free_after_headroom = int(max(0, free_bytes - required_headroom))

    output_cap_remaining = -1
    if int(max_output_bytes) > 0:
        output_cap_remaining = int(max(0, max_output_bytes - current_output_bytes))

    if output_cap_remaining >= 0:
        allowed_new_bytes = int(max(0, min(free_after_headroom, output_cap_remaining)))
    else:
        allowed_new_bytes = int(free_after_headroom)

    return {
        "output_dir": str(output_dir),
        "current_output_bytes": int(current_output_bytes),
        "current_output_human": human_size(int(current_output_bytes)),
        "max_output_bytes": int(max_output_bytes),
        "max_output_human": human_size(int(max_output_bytes)) if int(max_output_bytes) > 0 else "disabled",
        "free_disk_bytes": int(free_bytes),
        "free_disk_human": human_size(int(free_bytes)),
        "min_free_bytes": int(required_headroom),
        "min_free_human": human_size(int(required_headroom)),
        "free_after_headroom_bytes": int(free_after_headroom),
        "free_after_headroom_human": human_size(int(free_after_headroom)),
        "output_cap_remaining_bytes": int(output_cap_remaining),
        "output_cap_remaining_human": human_size(int(output_cap_remaining)) if output_cap_remaining >= 0 else "unbounded",
        "allowed_new_bytes": int(allowed_new_bytes),
        "allowed_new_human": human_size(int(allowed_new_bytes)),
    }


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
    incremental_update: bool,
    incremental_overlap_bars: int,
    markets_scope: str,
    universe_file_path: str,
    universe_file_hash: str,
    universe_final_1m_markets: List[str],
    planned_job_count: int,
    total_estimated_rows: int,
    total_estimated_raw_bytes: int,
    total_estimated_enriched_bytes: int,
    indicator_columns: int,
    storage_guard: Dict[str, object],
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
        "markets_scope": str(markets_scope),
        "universe_file_path": str(universe_file_path),
        "universe_file_hash": str(universe_file_hash),
        "universe_final_1m_markets": universe_final_1m_markets,
        "timeframes_min": timeframe_units,
        "estimate_only": bool(estimate_only),
        "skip_existing": bool(skip_existing),
        "incremental_update": bool(incremental_update),
        "incremental_overlap_bars": int(incremental_overlap_bars),
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
        "storage_guard": storage_guard,
        "current_job": current_job if isinstance(current_job, dict) else {},
        "jobs": manifest_jobs,
    }


def build_summary_payload(manifest_json: pathlib.Path, manifest_payload: Dict[str, object]) -> Dict[str, object]:
    storage_guard = manifest_payload.get("storage_guard", {})
    if not isinstance(storage_guard, dict):
        storage_guard = {}
    return {
        "generated_at_utc": manifest_payload["generated_at_utc"],
        "run_state": manifest_payload.get("run_state", ""),
        "manifest_json": str(manifest_json),
        "markets_scope": str(manifest_payload.get("markets_scope", "all")),
        "universe_file_path": str(manifest_payload.get("universe_file_path", "")),
        "universe_file_hash": str(manifest_payload.get("universe_file_hash", "")),
        "universe_final_1m_count": len(manifest_payload.get("universe_final_1m_markets", []) or []),
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
        "storage_guard_allowed_new_human": storage_guard.get("allowed_new_human", ""),
        "storage_guard_current_output_human": storage_guard.get("current_output_human", ""),
        "storage_guard_free_disk_human": storage_guard.get("free_disk_human", ""),
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
    start_utc: Optional[datetime] = None,
    append_existing: bool = False,
) -> subprocess.CompletedProcess:
    cmd = [
        str(python_exe),
        str(fetch_script),
        "--market",
        str(market),
        "--unit",
        str(unit_min),
        "--output-path",
        str(output_path),
        "--end-utc",
        end_utc.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "--chunk-size",
        str(int(chunk_size)),
        "--sleep-ms",
        str(int(sleep_ms_per_request)),
    ]
    if start_utc is not None:
        cmd.extend(["--start-utc", start_utc.strftime("%Y-%m-%dT%H:%M:%SZ"), "--candles", "0"])
    else:
        cmd.extend(["--candles", str(int(candles))])
    if bool(append_existing):
        cmd.append("--append-existing")
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )


def main(argv=None) -> int:
    args = parse_args(argv)
    incremental_mode = bool(args.incremental_update)
    incremental_overlap_bars = max(0, int(args.incremental_overlap_bars))
    if incremental_mode and bool(args.skip_existing):
        print(
            "[FetchProbabilisticBundle] incremental-update enabled: ignoring --skip-existing behavior",
            flush=True,
        )

    start_utc = parse_utc(args.history_start_utc, default_to_now=False)
    end_utc = parse_utc(args.history_end_utc, default_to_now=True)
    if end_utc <= start_utc:
        raise RuntimeError("history_end_utc must be later than history_start_utc.")

    major_markets, alt_markets, all_markets = normalize_markets(args.markets_major, args.markets_alt)
    timeframe_units = normalize_timeframes(args.timeframes)

    markets_scope = "all"
    universe_file_path = ""
    universe_file_hash = ""
    universe_final_1m_markets: List[str] = []
    if str(args.universe_file).strip():
        universe_path = resolve_repo_path(str(args.universe_file).strip())
        if not universe_path.exists():
            raise FileNotFoundError(f"universe file not found: {universe_path}")
        universe_final_1m_markets = load_universe_final_1m_markets(universe_path)
        markets_scope = "universe"
        universe_file_path = str(universe_path)
        universe_file_hash = sha256_file(universe_path)
        print(
            f"[FetchProbabilisticBundle] universe_scope enabled "
            f"final_1m_count={len(universe_final_1m_markets)} file={universe_path}",
            flush=True,
        )

    output_dir = resolve_repo_path(args.output_dir)
    manifest_json = resolve_repo_path(args.manifest_json)
    summary_json = resolve_repo_path(args.summary_json)
    fetch_script = resolve_repo_path(args.fetch_script)
    if not fetch_script.exists():
        raise FileNotFoundError(f"fetch script not found: {fetch_script}")

    output_dir.mkdir(parents=True, exist_ok=True)
    ensure_parent_directory(manifest_json)
    ensure_parent_directory(summary_json)

    initial_output_bytes = dir_size_bytes(output_dir)
    current_output_bytes = int(initial_output_bytes)
    max_output_bytes = bytes_from_gb(float(args.max_output_gb))
    min_free_bytes = bytes_from_gb(float(args.min_free_gb))
    current_storage_guard = build_storage_guard_state(
        output_dir=output_dir,
        current_output_bytes=current_output_bytes,
        max_output_bytes=max_output_bytes,
        min_free_bytes=min_free_bytes,
    )

    jobs = build_job_list(
        markets=all_markets,
        timeframe_units=timeframe_units,
        start_utc=start_utc,
        end_utc=end_utc,
        output_dir=output_dir,
        universe_1m_markets=universe_final_1m_markets,
    )
    if int(args.max_jobs) > 0:
        jobs = jobs[: int(args.max_jobs)]
    planned_job_count = len(jobs)

    manifest_jobs: List[Dict[str, object]] = []
    total_estimated_rows = 0
    total_estimated_raw_bytes = 0
    total_estimated_enriched_bytes = 0
    current_job: Dict[str, object] | None = None
    stopped_due_to_budget = False

    def flush_progress(run_state: str) -> Dict[str, object]:
        nonlocal current_storage_guard
        current_storage_guard = build_storage_guard_state(
            output_dir=output_dir,
            current_output_bytes=current_output_bytes,
            max_output_bytes=max_output_bytes,
            min_free_bytes=min_free_bytes,
        )
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
            incremental_update=bool(incremental_mode),
            incremental_overlap_bars=int(incremental_overlap_bars),
            markets_scope=markets_scope,
            universe_file_path=universe_file_path,
            universe_file_hash=universe_file_hash,
            universe_final_1m_markets=universe_final_1m_markets,
            planned_job_count=planned_job_count,
            total_estimated_rows=total_estimated_rows,
            total_estimated_raw_bytes=total_estimated_raw_bytes,
            total_estimated_enriched_bytes=total_estimated_enriched_bytes,
            indicator_columns=int(args.indicator_columns),
            storage_guard=current_storage_guard,
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
            "incremental_mode": bool(incremental_mode),
            "status": "planned",
            "rows": 0,
            "file_size_bytes": 0,
            "from_utc": "",
            "to_utc": "",
            "stderr_tail": "",
        }

        output_path = pathlib.Path(str(job["output_path"]))
        if (not incremental_mode) and bool(args.skip_existing) and output_path.exists():
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

        fetch_start_utc: Optional[datetime] = None
        if incremental_mode:
            rows_existing, first_ts_existing, last_ts_existing = read_csv_window(output_path)
            job_record["existing_rows_before"] = int(rows_existing)
            job_record["existing_from_utc"] = utc_iso_from_ms(first_ts_existing)
            job_record["existing_to_utc"] = utc_iso_from_ms(last_ts_existing)

            if rows_existing > 0 and last_ts_existing > 0:
                unit_ms = int(max(1, int(job["unit_min"])) * 60 * 1000)
                overlap_ms = int(incremental_overlap_bars * unit_ms)
                start_ms = int(max(int(start_utc.timestamp() * 1000), int(last_ts_existing - overlap_ms)))
                fetch_start_utc = datetime.fromtimestamp(float(start_ms) / 1000.0, tz=timezone.utc)
            else:
                fetch_start_utc = start_utc
            job_record["incremental_start_utc"] = fetch_start_utc.isoformat() if fetch_start_utc else ""

        if incremental_mode and fetch_start_utc is not None:
            est_rows_delta = estimate_candle_count(
                fetch_start_utc,
                end_utc,
                int(job["unit_min"]),
            )
            est_size_delta = estimate_size_bytes(
                rows=est_rows_delta,
                indicator_columns=int(args.indicator_columns),
                bytes_per_row_ohlcv=int(args.est_bytes_per_row_ohlcv),
                bytes_per_indicator_col=int(args.est_bytes_per_indicator_col),
            )
            job_record["estimated_incremental_candles"] = int(est_rows_delta)
            job_record["estimated_incremental_sizes"] = est_size_delta
            estimated_next_bytes = int(est_size_delta["raw_ohlcv_csv_bytes"])
        else:
            estimated_next_bytes = int(est_size["raw_ohlcv_csv_bytes"])

        current_storage_guard = build_storage_guard_state(
            output_dir=output_dir,
            current_output_bytes=current_output_bytes,
            max_output_bytes=max_output_bytes,
            min_free_bytes=min_free_bytes,
        )
        allowed_new_bytes = int(current_storage_guard.get("allowed_new_bytes", 0))
        if estimated_next_bytes > allowed_new_bytes:
            job_record["status"] = "blocked_disk_budget"
            job_record["stderr_tail"] = (
                f"storage_guard_block estimated_next={human_size(estimated_next_bytes)} "
                f"allowed_new={current_storage_guard.get('allowed_new_human', '')}"
            )
            manifest_jobs.append(job_record)
            current_job = None
            if str(args.disk_budget_policy) == "halt":
                stopped_due_to_budget = True
                flush_progress("stopped_disk_budget")
                print(
                    f"[FetchProbabilisticBundle] progress={idx}/{planned_job_count} "
                    f"status=blocked_disk_budget policy=halt market={job_record['market']} unit={job_record['unit_min']}m "
                    f"need={human_size(estimated_next_bytes)} allowed={current_storage_guard.get('allowed_new_human', '')}"
                )
                break

            flush_progress("in_progress")
            print(
                f"[FetchProbabilisticBundle] progress={idx}/{planned_job_count} "
                f"status=blocked_disk_budget policy=skip market={job_record['market']} unit={job_record['unit_min']}m "
                f"need={human_size(estimated_next_bytes)} allowed={current_storage_guard.get('allowed_new_human', '')}"
            )
            continue

        if not bool(args.estimate_only):
            started = time.time()
            before_bytes = int(output_path.stat().st_size) if output_path.exists() else 0
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
                start_utc=fetch_start_utc if incremental_mode else None,
                append_existing=bool(incremental_mode),
            )
            elapsed = round(time.time() - started, 2)
            job_record["elapsed_sec"] = float(elapsed)
            if int(proc.returncode) != 0:
                after_bytes = int(output_path.stat().st_size) if output_path.exists() else 0
                current_output_bytes = max(0, int(current_output_bytes + (after_bytes - before_bytes)))
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
            current_output_bytes = max(0, int(current_output_bytes + (int(file_size) - before_bytes)))
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
                f"rows={job_record['rows']} size={human_size(int(job_record['file_size_bytes']))} "
                f"guard_remaining={current_storage_guard.get('allowed_new_human', '')}"
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
    if stopped_due_to_budget:
        manifest_payload = flush_progress("completed_with_budget_stop")
    success_jobs, failed_jobs, skipped_jobs = classify_jobs(manifest_jobs)

    print("[FetchProbabilisticBundle] Completed")
    print(f"manifest={manifest_json}")
    print(f"summary={summary_json}")
    print(f"job_count={len(manifest_jobs)}")
    print(f"success_count={len(success_jobs)}")
    print(f"failed_count={len(failed_jobs)}")
    print(f"skipped_existing_count={len(skipped_jobs)}")
    print(f"markets_scope={markets_scope}")
    if markets_scope == "universe":
        print(f"universe_file={universe_file_path}")
        print(f"universe_file_hash={universe_file_hash}")
        print(f"universe_final_1m_count={len(universe_final_1m_markets)}")
    print(f"estimated_raw_csv={manifest_payload['estimated_totals']['raw_ohlcv_csv_human']}")
    print(f"estimated_enriched_csv={manifest_payload['estimated_totals']['enriched_csv_human']}")
    print(f"actual_csv={manifest_payload['actual_totals']['csv_human']}")
    print(f"storage_guard_allowed_new={manifest_payload['storage_guard']['allowed_new_human']}")
    print(f"storage_guard_free_disk={manifest_payload['storage_guard']['free_disk_human']}")
    print(f"storage_guard_current_output={manifest_payload['storage_guard']['current_output_human']}")

    if stopped_due_to_budget and str(args.disk_budget_policy) == "halt":
        return 3
    return 0 if len(failed_jobs) == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
