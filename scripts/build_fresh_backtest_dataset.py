#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
import math
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from _script_common import dump_json, resolve_repo_path


@dataclass
class OhlcvRow:
    ts_ms: int
    open: float
    high: float
    low: float
    close: float
    volume: float


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=(
            "Build fresh backtest dataset with companion TF files (1m/5m/15m/60m/240m) "
            "for probabilistic runtime feature compatibility."
        )
    )
    p.add_argument("--dataset-root", default=r".\data\backtest_fresh_14d")
    p.add_argument("--window-days", type=int, default=14)
    p.add_argument("--collect-prewarm-hours", type=int, default=168)
    p.add_argument("--max-tf-margin-hours", type=int, default=120)
    p.add_argument(
        "--markets",
        nargs="*",
        default=["KRW-BTC", "KRW-ETH", "KRW-XRP", "KRW-SOL", "KRW-DOGE", "KRW-ADA"],
    )
    p.add_argument(
        "--end-kst",
        default="",
        help=(
            "Anchor end datetime in KST ISO8601. Empty means today's KST 00:00:00. "
            "Example: 2026-03-01T00:00:00+09:00"
        ),
    )
    p.add_argument("--fetch-script", default=r".\scripts\fetch_upbit_historical_candles.py")
    p.add_argument("--fetch-sleep-ms", type=int, default=120)
    p.add_argument("--fetch-base-url", default="https://api.upbit.com")
    p.add_argument("--temp-raw-dir", default=r".\build\Release\logs\fresh_dataset_build")
    p.add_argument(
        "--dataset-tag",
        default="fresh14",
        help="Alias suffix used for root-level upbit_* files (default: fresh14).",
    )
    p.add_argument(
        "--report-json",
        default=r".\build\Release\logs\fresh14_dataset_build_report.json",
    )
    return p.parse_args(argv)


def format_number(value: float, max_decimals: int) -> str:
    token = f"{value:.{max_decimals}f}"
    token = token.rstrip("0").rstrip(".")
    return token if token else "0"


def write_rows_csv(path_value: Path, rows: List[OhlcvRow]) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)
    with path_value.open("w", encoding="utf-8", newline="\n") as fp:
        writer = csv.DictWriter(fp, fieldnames=["ts_ms", "open", "high", "low", "close", "volume"])
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    "ts_ms": int(row.ts_ms),
                    "open": format_number(row.open, 10),
                    "high": format_number(row.high, 10),
                    "low": format_number(row.low, 10),
                    "close": format_number(row.close, 10),
                    "volume": format_number(row.volume, 12),
                }
            )


def read_fetch_csv(path_value: Path) -> List[Dict[str, str]]:
    with path_value.open("r", encoding="utf-8", newline="") as fp:
        reader = csv.DictReader(fp)
        return list(reader)


def parse_float(cell: str, default: float = 0.0) -> float:
    token = str(cell or "").strip()
    if not token:
        return default
    try:
        value = float(token)
    except Exception:
        return default
    if not math.isfinite(value):
        return default
    return float(value)


def parse_int(cell: str, default: int = -1) -> int:
    token = str(cell or "").strip()
    if not token:
        return default
    try:
        value = int(float(token))
    except Exception:
        return default
    return int(value)


def normalize_1m_rows(
    fetched_rows: List[Dict[str, str]],
    collect_start_ts_ms: int,
    collect_end_ts_ms: int,
) -> List[OhlcvRow]:
    # Deduplicate by canonical 1-minute bucket and keep the latest row per bucket.
    by_bucket: Dict[int, Tuple[int, OhlcvRow]] = {}
    for row in fetched_rows:
        ts_raw = parse_int(row.get("timestamp", ""))
        if ts_raw <= 0:
            continue
        bucket_ts = (int(ts_raw) // 60000) * 60000
        if bucket_ts < collect_start_ts_ms or bucket_ts >= collect_end_ts_ms:
            continue
        item = OhlcvRow(
            ts_ms=int(bucket_ts),
            open=parse_float(row.get("open", "")),
            high=parse_float(row.get("high", "")),
            low=parse_float(row.get("low", "")),
            close=parse_float(row.get("close", "")),
            volume=max(0.0, parse_float(row.get("volume", ""))),
        )
        prev = by_bucket.get(bucket_ts)
        if prev is None or ts_raw >= prev[0]:
            by_bucket[bucket_ts] = (ts_raw, item)

    out = [x[1] for _, x in sorted(by_bucket.items(), key=lambda kv: kv[0])]
    return out


def resample_rows(rows_1m: List[OhlcvRow], tf_minutes: int) -> List[OhlcvRow]:
    if tf_minutes == 1:
        return list(rows_1m)
    tf_ms = int(tf_minutes) * 60 * 1000
    out: List[OhlcvRow] = []
    cur_bucket = None
    cur_row: Optional[OhlcvRow] = None
    for row in rows_1m:
        bucket = (int(row.ts_ms) // tf_ms) * tf_ms
        if cur_bucket is None or bucket != cur_bucket:
            if cur_row is not None:
                out.append(cur_row)
            cur_bucket = bucket
            cur_row = OhlcvRow(
                ts_ms=int(bucket),
                open=float(row.open),
                high=float(row.high),
                low=float(row.low),
                close=float(row.close),
                volume=float(row.volume),
            )
            continue
        if cur_row is None:
            continue
        cur_row.high = max(float(cur_row.high), float(row.high))
        cur_row.low = min(float(cur_row.low), float(row.low))
        cur_row.close = float(row.close)
        cur_row.volume = float(cur_row.volume) + float(row.volume)
    if cur_row is not None:
        out.append(cur_row)
    return out


def sha256_file(path_value: Path) -> str:
    h = hashlib.sha256()
    with path_value.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def iso_z(dt_utc: datetime) -> str:
    return dt_utc.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_end_kst(end_kst_token: str) -> datetime:
    kst = timezone(timedelta(hours=9))
    token = str(end_kst_token or "").strip()
    if token:
        parsed = datetime.fromisoformat(token.replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            parsed = parsed.replace(tzinfo=kst)
        return parsed.astimezone(kst)
    now_kst = datetime.now(tz=kst)
    return now_kst.replace(hour=0, minute=0, second=0, microsecond=0)


def run_fetch(
    python_exe: str,
    fetch_script: Path,
    market: str,
    start_utc: datetime,
    end_utc: datetime,
    output_path: Path,
    sleep_ms: int,
    base_url: str,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        python_exe,
        str(fetch_script),
        "--market",
        str(market),
        "--unit",
        "1",
        "--candles",
        "0",
        "--start-utc",
        iso_z(start_utc),
        "--end-utc",
        iso_z(end_utc),
        "--output-path",
        str(output_path),
        "--sleep-ms",
        str(max(0, int(sleep_ms))),
        "--base-url",
        str(base_url),
    ]
    subprocess.run(cmd, check=True)


def build_dataset(args: argparse.Namespace) -> Dict[str, object]:
    dataset_root = resolve_repo_path(args.dataset_root)
    fetch_script = resolve_repo_path(args.fetch_script)
    report_path = resolve_repo_path(args.report_json)
    temp_raw_dir = resolve_repo_path(args.temp_raw_dir)
    temp_raw_dir.mkdir(parents=True, exist_ok=True)
    dataset_root.mkdir(parents=True, exist_ok=True)

    if not fetch_script.exists():
        raise FileNotFoundError(f"fetch script not found: {fetch_script}")

    window_days = max(1, int(args.window_days))
    collect_prewarm_hours = max(0, int(args.collect_prewarm_hours))
    max_tf_margin_hours = max(0, int(args.max_tf_margin_hours))
    markets = [str(x).strip().upper() for x in args.markets if str(x).strip()]
    if not markets:
        raise RuntimeError("No markets configured.")

    kst = timezone(timedelta(hours=9))
    end_kst = parse_end_kst(args.end_kst)
    eval_end_kst = end_kst
    eval_start_kst = eval_end_kst - timedelta(days=window_days)
    collect_start_kst = eval_start_kst - timedelta(hours=collect_prewarm_hours + max_tf_margin_hours)
    collect_end_kst = eval_end_kst

    eval_start_ms = int(eval_start_kst.timestamp() * 1000)
    eval_end_ms = int(eval_end_kst.timestamp() * 1000)
    collect_start_ms = int(collect_start_kst.timestamp() * 1000)
    collect_end_ms = int(collect_end_kst.timestamp() * 1000)

    collect_start_utc = collect_start_kst.astimezone(timezone.utc)
    collect_end_utc = collect_end_kst.astimezone(timezone.utc)

    dataset_name = dataset_root.name
    tf_list = [1, 5, 15, 60, 240]
    report_markets: Dict[str, object] = {}
    meta_hashes: Dict[str, Dict[str, Dict[str, object]]] = {}
    python_exe = sys.executable

    for market in markets:
        market_token = market.replace("-", "_")
        raw_csv = temp_raw_dir / f"{market_token}_collect_raw_1m.csv"
        run_fetch(
            python_exe=python_exe,
            fetch_script=fetch_script,
            market=market,
            start_utc=collect_start_utc,
            end_utc=collect_end_utc,
            output_path=raw_csv,
            sleep_ms=int(args.fetch_sleep_ms),
            base_url=str(args.fetch_base_url),
        )
        fetched_rows = read_fetch_csv(raw_csv)
        rows_1m = normalize_1m_rows(
            fetched_rows=fetched_rows,
            collect_start_ts_ms=collect_start_ms,
            collect_end_ts_ms=collect_end_ms,
        )
        per_tf_rows: Dict[int, List[OhlcvRow]] = {}
        per_tf_rows[1] = rows_1m
        for tf in tf_list:
            if tf == 1:
                continue
            per_tf_rows[tf] = resample_rows(rows_1m=rows_1m, tf_minutes=tf)

        market_dir = dataset_root / market
        market_dir.mkdir(parents=True, exist_ok=True)
        market_report: Dict[str, object] = {
            "rows_raw_fetch": int(len(fetched_rows)),
            "collect_range": [int(collect_start_ms), int(collect_end_ms)],
            "eval_range": [int(eval_start_ms), int(eval_end_ms)],
            "timeframes": {},
        }
        meta_hashes[market] = {}

        for tf in tf_list:
            tf_rows = per_tf_rows.get(tf, [])
            market_tf_path = market_dir / f"ohlcv_{tf}m.csv"
            write_rows_csv(market_tf_path, tf_rows)

            # Runtime-compatible root alias (same folder prefix discovery path).
            alias_tf_path = dataset_root / f"upbit_{market_token}_{tf}m_{args.dataset_tag}.csv"
            write_rows_csv(alias_tf_path, tf_rows)

            rows_before_eval = 0
            if tf_rows:
                rows_before_eval = sum(1 for x in tf_rows if int(x.ts_ms) < eval_start_ms)
            meets_context_25 = rows_before_eval >= 25

            entry = {
                "rows": int(len(tf_rows)),
                "rows_before_eval_start": int(rows_before_eval),
                "meets_min_25_before_eval_start": bool(meets_context_25),
                "market_file": str(market_tf_path),
                "alias_file": str(alias_tf_path),
                "sha256_market_file": sha256_file(market_tf_path),
                "sha256_alias_file": sha256_file(alias_tf_path),
            }
            market_report["timeframes"][f"{tf}m"] = entry
            meta_hashes[market][f"{tf}m"] = {
                "rows": int(len(tf_rows)),
                "sha256": str(entry["sha256_market_file"]),
                "rows_before_eval_start": int(rows_before_eval),
            }

        report_markets[market] = market_report

    meta = {
        "dataset_name": dataset_name,
        "created_at_kst": datetime.now(tz=kst).isoformat(),
        "window_days": int(window_days),
        "eval_start_ts_ms": int(eval_start_ms),
        "eval_end_ts_ms": int(eval_end_ms),
        "collect_start_ts_ms": int(collect_start_ms),
        "collect_end_ts_ms": int(collect_end_ms),
        "collect_prewarm_hours": int(collect_prewarm_hours),
        "max_tf_margin_hours": int(max_tf_margin_hours),
        "markets": markets,
        "granularity": "1m+companions",
        "schema": ["ts_ms", "open", "high", "low", "close", "volume"],
        "source": "upbit_ohlcv",
        "hashes": meta_hashes,
        "runtime_alias": {
            "enabled": True,
            "pattern": f"upbit_<MARKET_TOKEN>_<TF>m_{args.dataset_tag}.csv",
            "tfs": ["1m", "5m", "15m", "60m", "240m"],
        },
    }
    meta_path = dataset_root / "meta.json"
    dump_json(meta_path, meta)

    report = {
        "generated_at_kst": datetime.now(tz=kst).isoformat(),
        "dataset_root": str(dataset_root),
        "dataset_name": dataset_name,
        "hard_lock_data_only": {
            "eval_window_days": int(window_days),
            "eval_start_ts_ms": int(eval_start_ms),
            "eval_end_ts_ms": int(eval_end_ms),
            "collect_start_ts_ms": int(collect_start_ms),
            "collect_end_ts_ms": int(collect_end_ms),
            "collect_prewarm_hours": int(collect_prewarm_hours),
            "max_tf_margin_hours": int(max_tf_margin_hours),
            "runtime_alias_dataset_tag": str(args.dataset_tag),
        },
        "markets": report_markets,
    }
    dump_json(report_path, report)
    return {
        "dataset_root": str(dataset_root),
        "meta_json": str(meta_path),
        "report_json": str(report_path),
        "markets": report_markets,
    }


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    summary = build_dataset(args)
    print("[BuildFreshDataset] Completed")
    print(f"dataset_root={summary['dataset_root']}")
    print(f"meta_json={summary['meta_json']}")
    print(f"report_json={summary['report_json']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
