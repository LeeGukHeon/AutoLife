#!/usr/bin/env python3
import argparse
import bisect
import csv
import hashlib
import json
import pathlib
import time
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Tuple

from _script_common import dump_json, resolve_repo_path

TF_ORDER = ["1m", "5m", "15m", "1h", "4h"]
TF_STEP_MS = {
    "1m": 60_000,
    "5m": 300_000,
    "15m": 900_000,
    "1h": 3_600_000,
    "4h": 14_400_000,
}
TF_WINDOW_BARS = {
    "1m": 200,
    "5m": 120,
    "15m": 120,
    "1h": 120,
    "4h": 90,
}


@dataclass
class CandleRow:
    timestamp: int
    open: float
    high: float
    low: float
    close: float
    volume: float


def normalize_timestamp_ms(ts: int) -> int:
    if 0 < ts < 1_000_000_000_000:
        return ts * 1000
    return ts


def parse_int(value: Any, default: int = 0) -> int:
    try:
        return int(float(value))
    except Exception:
        return default


def parse_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return default


def load_candles_csv(path: pathlib.Path) -> List[CandleRow]:
    out: List[CandleRow] = []
    with path.open("r", encoding="utf-8-sig", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            ts = normalize_timestamp_ms(parse_int(row.get("timestamp"), 0))
            if ts <= 0:
                continue
            c = parse_float(row.get("close"), 0.0)
            if c <= 0.0:
                continue
            out.append(
                CandleRow(
                    timestamp=ts,
                    open=parse_float(row.get("open"), c),
                    high=parse_float(row.get("high"), c),
                    low=parse_float(row.get("low"), c),
                    close=c,
                    volume=parse_float(row.get("volume"), 0.0),
                )
            )
    return out


def to_sorted_ascending(candles: List[CandleRow]) -> List[CandleRow]:
    return sorted(candles, key=lambda x: x.timestamp)


def ordering_flags(timestamps: List[int]) -> Dict[str, bool]:
    if len(timestamps) <= 1:
        return {
            "raw_monotonic_non_decreasing": True,
            "raw_monotonic_non_increasing": True,
            "raw_strictly_increasing": True,
            "raw_strictly_decreasing": True,
        }
    non_dec = all(timestamps[i] >= timestamps[i - 1] for i in range(1, len(timestamps)))
    non_inc = all(timestamps[i] <= timestamps[i - 1] for i in range(1, len(timestamps)))
    strict_inc = all(timestamps[i] > timestamps[i - 1] for i in range(1, len(timestamps)))
    strict_dec = all(timestamps[i] < timestamps[i - 1] for i in range(1, len(timestamps)))
    return {
        "raw_monotonic_non_decreasing": non_dec,
        "raw_monotonic_non_increasing": non_inc,
        "raw_strictly_increasing": strict_inc,
        "raw_strictly_decreasing": strict_dec,
    }


def compute_gap_stats(sorted_timestamps: List[int], expected_step_ms: int) -> Dict[str, Any]:
    if len(sorted_timestamps) <= 1:
        return {
            "expected_step_ms": expected_step_ms,
            "delta_count": 0,
            "median_delta_ms": 0,
            "max_delta_ms": 0,
            "gap_count": 0,
            "gap_ratio": 0.0,
        }

    deltas = []
    for i in range(1, len(sorted_timestamps)):
        d = sorted_timestamps[i] - sorted_timestamps[i - 1]
        if d > 0:
            deltas.append(d)
    if not deltas:
        return {
            "expected_step_ms": expected_step_ms,
            "delta_count": 0,
            "median_delta_ms": 0,
            "max_delta_ms": 0,
            "gap_count": 0,
            "gap_ratio": 0.0,
        }

    deltas_sorted = sorted(deltas)
    mid = len(deltas_sorted) // 2
    if len(deltas_sorted) % 2 == 0:
        median_delta = int((deltas_sorted[mid - 1] + deltas_sorted[mid]) / 2)
    else:
        median_delta = int(deltas_sorted[mid])

    gap_threshold = int(expected_step_ms * 1.5)
    gap_count = sum(1 for d in deltas if d > gap_threshold)
    return {
        "expected_step_ms": expected_step_ms,
        "delta_count": len(deltas),
        "median_delta_ms": median_delta,
        "max_delta_ms": max(deltas),
        "gap_count": gap_count,
        "gap_ratio": round(gap_count / float(len(deltas)), 6),
    }


def aggregate_by_step(source: List[CandleRow], step: int, max_bars: int) -> List[CandleRow]:
    out: List[CandleRow] = []
    if step <= 0 or len(source) < step:
        return out
    for i in range(0, len(source) - step + 1, step):
        chunk = source[i : i + step]
        out.append(
            CandleRow(
                timestamp=chunk[0].timestamp,
                open=chunk[0].open,
                high=max(x.high for x in chunk),
                low=min(x.low for x in chunk),
                close=chunk[-1].close,
                volume=sum(x.volume for x in chunk),
            )
        )
    if max_bars > 0 and len(out) > max_bars:
        return out[-max_bars:]
    return out


def extract_market_token(primary_1m_path: pathlib.Path) -> Optional[str]:
    stem = primary_1m_path.stem.lower()
    pivot = "_1m_"
    if not stem.startswith("upbit_") or pivot not in stem:
        return None
    idx = stem.index(pivot)
    if idx <= 6:
        return None
    return stem[6:idx]


def pick_latest_file(paths: Iterable[pathlib.Path]) -> Optional[pathlib.Path]:
    items = [p for p in paths if p.exists() and p.is_file()]
    if not items:
        return None
    return max(items, key=lambda p: (p.stat().st_mtime, p.name))


def find_companion_path(primary_1m_path: pathlib.Path, token: str, tf_key: str) -> Optional[pathlib.Path]:
    tf_aliases = {
        "5m": ["5m"],
        "15m": ["15m"],
        "1h": ["60m"],
        "4h": ["240m"],
    }[tf_key]
    candidates: List[pathlib.Path] = []
    for alias in tf_aliases:
        pattern = f"upbit_{token}_{alias}_*.csv"
        candidates.extend(primary_1m_path.parent.glob(pattern))
    return pick_latest_file(candidates)


def select_checkpoints(ts_1m: List[int], max_points: int) -> List[int]:
    if not ts_1m:
        return []
    if max_points <= 1 or len(ts_1m) <= max_points:
        return list(ts_1m)
    result: List[int] = []
    last_idx = len(ts_1m) - 1
    for i in range(max_points):
        pos = int(round((i * last_idx) / float(max_points - 1)))
        result.append(ts_1m[pos])
    dedup = sorted(set(result))
    return dedup


def summarize_window(candles: List[CandleRow]) -> str:
    if not candles:
        return "0:0:0:0:0"
    close_sum = sum(x.close for x in candles)
    vol_sum = sum(x.volume for x in candles)
    return (
        f"{len(candles)}:{candles[0].timestamp}:{candles[-1].timestamp}:"
        f"{close_sum:.6f}:{vol_sum:.6f}"
    )


def rolling_hash_binary_search(
    candles_by_tf: Dict[str, List[CandleRow]],
    checkpoints: List[int],
) -> str:
    ts_cache = {tf: [x.timestamp for x in candles_by_tf.get(tf, [])] for tf in TF_ORDER}
    sha = hashlib.sha256()
    for t in checkpoints:
        sha.update(f"cp:{t}|".encode("utf-8"))
        for tf in TF_ORDER:
            candles = candles_by_tf.get(tf, [])
            ts = ts_cache.get(tf, [])
            if not candles or not ts:
                sha.update(f"{tf}:0:0:0:0:0|".encode("utf-8"))
                continue
            idx = bisect.bisect_right(ts, t)
            if idx <= 0:
                sha.update(f"{tf}:0:0:0:0:0|".encode("utf-8"))
                continue
            start = max(0, idx - int(TF_WINDOW_BARS[tf]))
            sig = summarize_window(candles[start:idx])
            sha.update(f"{tf}:{sig}|".encode("utf-8"))
    return sha.hexdigest()


def rolling_hash_incremental_cursor(
    candles_by_tf: Dict[str, List[CandleRow]],
    checkpoints: List[int],
) -> str:
    cursors = {tf: 0 for tf in TF_ORDER}
    sha = hashlib.sha256()
    for t in checkpoints:
        sha.update(f"cp:{t}|".encode("utf-8"))
        for tf in TF_ORDER:
            candles = candles_by_tf.get(tf, [])
            if not candles:
                sha.update(f"{tf}:0:0:0:0:0|".encode("utf-8"))
                continue
            cursor = cursors[tf]
            while cursor < len(candles) and candles[cursor].timestamp <= t:
                cursor += 1
            cursors[tf] = cursor
            if cursor <= 0:
                sha.update(f"{tf}:0:0:0:0:0|".encode("utf-8"))
                continue
            start = max(0, cursor - int(TF_WINDOW_BARS[tf]))
            sig = summarize_window(candles[start:cursor])
            sha.update(f"{tf}:{sig}|".encode("utf-8"))
    return sha.hexdigest()


def coverage_ratio(reference: List[CandleRow], target: List[CandleRow]) -> float:
    if not reference:
        return 0.0
    ref_start = reference[0].timestamp
    ref_end = reference[-1].timestamp
    if ref_end <= ref_start:
        return 1.0
    if not target:
        return 0.0
    tgt_start = target[0].timestamp
    tgt_end = target[-1].timestamp
    overlap_start = max(ref_start, tgt_start)
    overlap_end = min(ref_end, tgt_end)
    if overlap_end <= overlap_start:
        return 0.0
    return max(0.0, min(1.0, (overlap_end - overlap_start) / float(ref_end - ref_start)))


def tf_health_block(
    tf_key: str,
    candles_raw: List[CandleRow],
    candles_sorted: List[CandleRow],
    now_ms: int,
    source: str,
) -> Dict[str, Any]:
    raw_ts = [x.timestamp for x in candles_raw]
    sorted_ts = [x.timestamp for x in candles_sorted]
    unique_count = len(set(sorted_ts))
    duplicate_count = max(0, len(sorted_ts) - unique_count)
    duplicate_ratio = (duplicate_count / float(len(sorted_ts))) if sorted_ts else 0.0
    stale_tail_minutes = (
        (now_ms - sorted_ts[-1]) / 60_000.0 if sorted_ts and now_ms >= sorted_ts[-1] else 0.0
    )
    ordering = ordering_flags(raw_ts)
    gaps = compute_gap_stats(sorted_ts, TF_STEP_MS[tf_key])
    return {
        "source": source,
        "rows": len(sorted_ts),
        "first_timestamp_ms": sorted_ts[0] if sorted_ts else 0,
        "last_timestamp_ms": sorted_ts[-1] if sorted_ts else 0,
        "ordering": ordering,
        "normalized_monotonic_non_decreasing": all(
            sorted_ts[i] >= sorted_ts[i - 1] for i in range(1, len(sorted_ts))
        ),
        "duplicate_count": duplicate_count,
        "duplicate_ratio": round(duplicate_ratio, 6),
        "stale_tail_minutes": round(stale_tail_minutes, 4),
        "gaps": gaps,
    }


def analyze_primary_dataset(primary_1m_path: pathlib.Path, rolling_points: int) -> Dict[str, Any]:
    now_ms = int(time.time() * 1000)
    token = extract_market_token(primary_1m_path)

    raw_1m = load_candles_csv(primary_1m_path)
    sorted_1m = to_sorted_ascending(raw_1m)

    candles_by_tf: Dict[str, List[CandleRow]] = {"1m": sorted_1m}
    tf_meta: Dict[str, Dict[str, Any]] = {}
    tf_meta["1m"] = tf_health_block("1m", raw_1m, sorted_1m, now_ms, source="primary")

    companion_paths: Dict[str, Optional[pathlib.Path]] = {k: None for k in ("5m", "15m", "1h", "4h")}
    if token is not None:
        for tf in companion_paths.keys():
            companion_paths[tf] = find_companion_path(primary_1m_path, token, tf)

    for tf in ("5m", "1h", "4h"):
        path = companion_paths.get(tf)
        if path is None:
            candles_by_tf[tf] = []
            tf_meta[tf] = tf_health_block(tf, [], [], now_ms, source="missing")
            continue
        raw = load_candles_csv(path)
        sorted_rows = to_sorted_ascending(raw)
        candles_by_tf[tf] = sorted_rows
        tf_meta[tf] = tf_health_block(tf, raw, sorted_rows, now_ms, source=str(path))

    path_15m = companion_paths.get("15m")
    if path_15m is not None:
        raw_15m = load_candles_csv(path_15m)
        sorted_15m = to_sorted_ascending(raw_15m)
        candles_by_tf["15m"] = sorted_15m
        tf_meta["15m"] = tf_health_block("15m", raw_15m, sorted_15m, now_ms, source=str(path_15m))
    else:
        derived_15m: List[CandleRow] = []
        source = "missing"
        if len(sorted_1m) >= 15:
            derived_15m = aggregate_by_step(sorted_1m, 15, TF_WINDOW_BARS["15m"])
            source = "derived_from_1m"
        elif len(candles_by_tf.get("5m", [])) >= 3:
            derived_15m = aggregate_by_step(candles_by_tf["5m"], 3, TF_WINDOW_BARS["15m"])
            source = "derived_from_5m"
        candles_by_tf["15m"] = derived_15m
        tf_meta["15m"] = tf_health_block("15m", derived_15m, derived_15m, now_ms, source=source)

    cov_ref = candles_by_tf["1m"]
    for tf in TF_ORDER:
        tf_meta[tf]["coverage_ratio_vs_1m"] = round(coverage_ratio(cov_ref, candles_by_tf.get(tf, [])), 6)

    checkpoints = select_checkpoints([x.timestamp for x in cov_ref], rolling_points)
    hash_binary = rolling_hash_binary_search(candles_by_tf, checkpoints)
    hash_cursor = rolling_hash_incremental_cursor(candles_by_tf, checkpoints)
    rolling_hash_equivalent = hash_binary == hash_cursor

    required_tf_available = all(tf_meta[tf]["rows"] > 0 for tf in TF_ORDER)
    ordering_pass = all(bool(tf_meta[tf]["normalized_monotonic_non_decreasing"]) for tf in TF_ORDER)
    duplicate_tolerance_pass = all(float(tf_meta[tf]["duplicate_ratio"]) <= 0.01 for tf in TF_ORDER)
    gap_tolerance_pass = all(float(tf_meta[tf]["gaps"]["gap_ratio"]) <= 0.20 for tf in TF_ORDER)

    checks = {
        "required_tf_available": required_tf_available,
        "ordering_monotonic_after_normalization": ordering_pass,
        "rolling_window_hash_equivalent": rolling_hash_equivalent,
        "duplicate_ratio_tolerance_pass": duplicate_tolerance_pass,
        "gap_ratio_tolerance_pass": gap_tolerance_pass,
    }
    invariant_pass = all(bool(v) for v in checks.values())

    return {
        "dataset": str(primary_1m_path.resolve()),
        "market_token": token or "unknown",
        "checks": checks,
        "invariant_pass": invariant_pass,
        "rolling_window": {
            "checkpoint_count": len(checkpoints),
            "hash_binary_search": hash_binary,
            "hash_incremental_cursor": hash_cursor,
            "hash_equivalent": rolling_hash_equivalent,
        },
        "timeframes": tf_meta,
    }


def list_primary_datasets(data_dir: pathlib.Path) -> List[pathlib.Path]:
    if not data_dir.exists():
        return []
    out = []
    for p in sorted(data_dir.glob("*.csv"), key=lambda x: x.name.lower()):
        name = p.name.lower()
        if name.startswith("upbit_") and "_1m_" in name:
            out.append(p.resolve())
    return out


def resolve_explicit_datasets(dataset_names: List[str], data_dir: pathlib.Path) -> List[pathlib.Path]:
    out: List[pathlib.Path] = []
    roots = [pathlib.Path.cwd().resolve(), data_dir.resolve()]
    for raw in dataset_names:
        token = str(raw).strip()
        if not token:
            continue
        cand = pathlib.Path(token)
        found: Optional[pathlib.Path] = None
        if cand.is_absolute() and cand.exists():
            found = cand.resolve()
        else:
            for root in roots:
                probe = (root / cand).resolve()
                if probe.exists():
                    found = probe
                    break
        if found is None:
            raise FileNotFoundError(f"Dataset not found: {token}")
        out.append(found)
    uniq = sorted(set(out), key=lambda x: str(x).lower())
    return uniq


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", "-DataDir", default=r".\data\backtest_real")
    parser.add_argument("--dataset-names", "-DatasetNames", nargs="*", default=[])
    parser.add_argument("--rolling-checkpoints", "-RollingCheckpoints", type=int, default=64)
    parser.add_argument("--output-json", "-OutputJson", default=r".\build\Release\logs\parity_invariant_report.json")
    parser.add_argument("--fail-on-invariant", "-FailOnInvariant", action="store_true")
    args = parser.parse_args(argv)

    data_dir = resolve_repo_path(args.data_dir)
    output_json = resolve_repo_path(args.output_json)

    if args.dataset_names:
        datasets = resolve_explicit_datasets(list(args.dataset_names), data_dir)
    else:
        datasets = list_primary_datasets(data_dir)
    if not datasets:
        raise RuntimeError("No primary 1m datasets found for parity invariant report.")

    reports = []
    for ds in datasets:
        reports.append(analyze_primary_dataset(ds, max(4, int(args.rolling_checkpoints))))

    pass_count = sum(1 for x in reports if bool(x.get("invariant_pass", False)))
    tf_rows_nonzero = {tf: 0 for tf in TF_ORDER}
    for row in reports:
        tfs = row.get("timeframes", {})
        for tf in TF_ORDER:
            if int((tfs.get(tf) or {}).get("rows", 0)) > 0:
                tf_rows_nonzero[tf] += 1

    summary = {
        "dataset_count": len(reports),
        "invariant_pass_count": pass_count,
        "invariant_fail_count": len(reports) - pass_count,
        "overall_invariant_pass": pass_count == len(reports),
        "tf_availability_rate": {
            tf: round(tf_rows_nonzero[tf] / float(len(reports)), 6) for tf in TF_ORDER
        },
    }

    payload = {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "inputs": {
            "data_dir": str(data_dir),
            "datasets": [str(x) for x in datasets],
            "rolling_checkpoints": int(args.rolling_checkpoints),
        },
        "summary": summary,
        "datasets": reports,
    }
    dump_json(output_json, payload)

    print("[ParityInvariant] Completed")
    print(f"output_json={output_json}")
    print(f"dataset_count={summary['dataset_count']}")
    print(f"overall_invariant_pass={summary['overall_invariant_pass']}")

    if bool(args.fail_on_invariant) and not bool(summary["overall_invariant_pass"]):
        print("[ParityInvariant] FAILED (invariant)")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
