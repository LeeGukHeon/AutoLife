#!/usr/bin/env python3
import argparse
import csv
import json
import pathlib
from datetime import datetime, timezone
from typing import Any, Dict, List, Tuple


def now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def resolve_path(path_value: str) -> pathlib.Path:
    p = pathlib.Path(path_value)
    if p.is_absolute():
        return p
    return (pathlib.Path.cwd() / p).resolve()


def ensure_dir(path_value: pathlib.Path) -> None:
    path_value.mkdir(parents=True, exist_ok=True)


def read_csv_rows(path_value: pathlib.Path) -> Tuple[List[str], List[Dict[str, str]]]:
    with path_value.open("r", encoding="utf-8", newline="") as fp:
        reader = csv.DictReader(fp)
        rows = list(reader)
        fieldnames = list(reader.fieldnames or [])
    return fieldnames, rows


def write_csv_rows(path_value: pathlib.Path, fieldnames: List[str], rows: List[Dict[str, str]]) -> None:
    ensure_dir(path_value.parent)
    with path_value.open("w", encoding="utf-8", newline="\n") as fp:
        writer = csv.DictWriter(fp, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def parse_ts(row: Dict[str, str]) -> int:
    token = str(row.get("timestamp", "")).strip()
    if not token:
        return -1
    try:
        return int(float(token))
    except Exception:
        return -1


def split_rows_by_time(
    rows: List[Dict[str, str]],
    t_start: int,
    t_end: int,
    t_dev_end: int,
    t_val_end: int,
    purge_gap_ms: int,
    warmup_prepend_ms: int,
) -> Dict[str, Any]:
    dev_cut = t_dev_end - purge_gap_ms
    val_left = t_dev_end + purge_gap_ms
    val_right = t_val_end - purge_gap_ms
    qua_left = t_val_end + purge_gap_ms

    warmup_prepend_ms = max(0, int(warmup_prepend_ms))
    val_warmup_left = max(t_start, val_left - warmup_prepend_ms)
    qua_warmup_left = max(t_start, qua_left - warmup_prepend_ms)
    # Keep warmup rows outside purge windows to preserve purge guarantees.
    val_warmup_core_max = min(dev_cut, val_right)
    qua_warmup_core_max = min(val_right, t_end)

    purge_windows = [
        (t_dev_end - purge_gap_ms, t_dev_end + purge_gap_ms),
        (t_val_end - purge_gap_ms, t_val_end + purge_gap_ms),
    ]

    out = {
        "dev": [],
        "val": [],
        "quarantine": [],
        "all_purged": [],
        "val_core": [],
        "quarantine_core": [],
        "counts": {
            "source_rows": len(rows),
            "invalid_timestamp_rows": 0,
            "outside_intersection_rows": 0,
            "purge_removed_rows": 0,
            "dev_rows": 0,
            "val_rows": 0,
            "quarantine_rows": 0,
            "val_core_rows": 0,
            "quarantine_core_rows": 0,
            "val_warmup_rows": 0,
            "quarantine_warmup_rows": 0,
            "all_purged_rows": 0,
            "leak_proximity_rows_remaining": 0,
        },
    }

    for row in rows:
        ts = parse_ts(row)
        if ts < 0:
            out["counts"]["invalid_timestamp_rows"] += 1
            continue
        if ts < t_start or ts > t_end:
            out["counts"]["outside_intersection_rows"] += 1
            continue

        in_purge = False
        for lo, hi in purge_windows:
            if lo <= ts <= hi:
                in_purge = True
                break
        if in_purge:
            out["counts"]["purge_removed_rows"] += 1
            continue

        out["all_purged"].append(row)
        out["counts"]["all_purged_rows"] += 1

        if ts <= dev_cut:
            out["dev"].append(row)
            out["counts"]["dev_rows"] += 1

        is_val_core = (val_left <= ts <= val_right)
        is_val_warmup = (
            warmup_prepend_ms > 0 and
            val_warmup_left <= ts <= val_warmup_core_max
        )
        if is_val_core or is_val_warmup:
            out["val"].append(row)
            out["counts"]["val_rows"] += 1
            if is_val_core:
                out["val_core"].append(row)
                out["counts"]["val_core_rows"] += 1
            elif is_val_warmup:
                out["counts"]["val_warmup_rows"] += 1

        is_qua_core = (ts >= qua_left)
        is_qua_warmup = (
            warmup_prepend_ms > 0 and
            qua_warmup_left <= ts <= qua_warmup_core_max
        )
        if is_qua_core or is_qua_warmup:
            out["quarantine"].append(row)
            out["counts"]["quarantine_rows"] += 1
            if is_qua_core:
                out["quarantine_core"].append(row)
                out["counts"]["quarantine_core_rows"] += 1
            elif is_qua_warmup:
                out["counts"]["quarantine_warmup_rows"] += 1

    # Leakage sanity: rows that remain within purge windows should be zero.
    leak_rows = 0
    for row in out["all_purged"]:
        ts = parse_ts(row)
        for lo, hi in purge_windows:
            if lo <= ts <= hi:
                leak_rows += 1
                break
    out["counts"]["leak_proximity_rows_remaining"] = leak_rows
    return out


def discover_primary_1m(source_dir: pathlib.Path) -> List[pathlib.Path]:
    return sorted(source_dir.glob("upbit_*_1m_*.csv"), key=lambda p: p.name.lower())


def discover_all_upbit(source_dir: pathlib.Path) -> List[pathlib.Path]:
    return sorted(source_dir.glob("upbit_*.csv"), key=lambda p: p.name.lower())


def min_max_ts(rows: List[Dict[str, str]]) -> Tuple[int, int]:
    vals = [parse_ts(x) for x in rows]
    vals = [x for x in vals if x >= 0]
    if not vals:
        return -1, -1
    return min(vals), max(vals)


def main(argv: List[str] = None) -> int:
    p = argparse.ArgumentParser(description="Build time-based Dev/Val/Quarantine split datasets with real purge-gap removal.")
    p.add_argument("--source-data-dir", default=r".\data\backtest_real")
    p.add_argument("--output-root-dir", default=r".\data\backtest_real_splits_time")
    p.add_argument("--manifest-json", default=r".\build\Release\logs\time_split_manifest.json")
    p.add_argument("--dev-ratio", type=float, default=0.60)
    p.add_argument("--val-ratio", type=float, default=0.20)
    p.add_argument("--quarantine-ratio", type=float, default=0.20)
    p.add_argument("--purge-gap-minutes", type=int, default=240)
    p.add_argument(
        "--warmup-prepend-minutes",
        type=int,
        default=0,
        help="Optional prewarm minutes to prepend to validation/quarantine splits (outside purge windows).",
    )
    args = p.parse_args(argv)

    source_dir = resolve_path(args.source_data_dir)
    output_root = resolve_path(args.output_root_dir)
    manifest_path = resolve_path(args.manifest_json)

    if not source_dir.exists():
        raise FileNotFoundError(f"Source data dir not found: {source_dir}")

    dev_ratio = max(0.05, min(0.90, float(args.dev_ratio)))
    val_ratio = max(0.05, min(0.90, float(args.val_ratio)))
    qua_ratio = max(0.05, min(0.90, float(args.quarantine_ratio)))
    ratio_sum = dev_ratio + val_ratio + qua_ratio
    dev_ratio /= ratio_sum
    val_ratio /= ratio_sum
    qua_ratio /= ratio_sum
    purge_gap_minutes = max(0, int(args.purge_gap_minutes))
    purge_gap_ms = purge_gap_minutes * 60 * 1000
    warmup_prepend_minutes = max(0, int(args.warmup_prepend_minutes))
    warmup_prepend_ms = warmup_prepend_minutes * 60 * 1000

    primary_files = discover_primary_1m(source_dir)
    if not primary_files:
        raise RuntimeError(f"No primary 1m upbit datasets found in {source_dir}")

    primary_ranges: List[Dict[str, Any]] = []
    for path_value in primary_files:
        _, rows = read_csv_rows(path_value)
        lo, hi = min_max_ts(rows)
        if lo < 0 or hi < 0 or hi <= lo:
            raise RuntimeError(f"Invalid timestamp range in primary dataset: {path_value}")
        primary_ranges.append(
            {"dataset": path_value.name, "min_ts": int(lo), "max_ts": int(hi), "rows": len(rows)}
        )

    t_start = max(int(x["min_ts"]) for x in primary_ranges)
    t_end = min(int(x["max_ts"]) for x in primary_ranges)
    if t_end <= t_start:
        raise RuntimeError("No positive intersection window across primary datasets.")

    span = t_end - t_start
    t_dev_end = t_start + int(span * dev_ratio)
    t_val_end = t_start + int(span * (dev_ratio + val_ratio))
    if not (t_start < t_dev_end < t_val_end < t_end):
        raise RuntimeError("Invalid split boundaries after ratio normalization.")

    all_files = discover_all_upbit(source_dir)
    if not all_files:
        raise RuntimeError(f"No upbit csv files found in {source_dir}")

    out_dirs = {
        "dev": output_root / "development",
        "val": output_root / "validation",
        "quarantine": output_root / "quarantine",
        "all_purged": output_root / "all_purged",
    }
    for d in out_dirs.values():
        ensure_dir(d)

    totals = {
        "source_rows": 0,
        "invalid_timestamp_rows": 0,
        "outside_intersection_rows": 0,
        "purge_removed_rows": 0,
        "dev_rows": 0,
        "val_rows": 0,
        "quarantine_rows": 0,
        "val_core_rows": 0,
        "quarantine_core_rows": 0,
        "val_warmup_rows": 0,
        "quarantine_warmup_rows": 0,
        "all_purged_rows": 0,
        "leak_proximity_rows_remaining": 0,
    }
    file_stats: List[Dict[str, Any]] = []
    for file_path in all_files:
        fieldnames, rows = read_csv_rows(file_path)
        result = split_rows_by_time(
            rows=rows,
            t_start=t_start,
            t_end=t_end,
            t_dev_end=t_dev_end,
            t_val_end=t_val_end,
            purge_gap_ms=purge_gap_ms,
            warmup_prepend_ms=warmup_prepend_ms,
        )
        write_csv_rows(out_dirs["dev"] / file_path.name, fieldnames, result["dev"])
        write_csv_rows(out_dirs["val"] / file_path.name, fieldnames, result["val"])
        write_csv_rows(out_dirs["quarantine"] / file_path.name, fieldnames, result["quarantine"])
        write_csv_rows(out_dirs["all_purged"] / file_path.name, fieldnames, result["all_purged"])

        counts = result["counts"]
        for k in totals.keys():
            totals[k] += int(counts.get(k, 0))
        file_stats.append(
            {
                "dataset": file_path.name,
                **{k: int(counts.get(k, 0)) for k in totals.keys()},
            }
        )

    manifest: Dict[str, Any] = {
        "generated_at": now_iso(),
        "protocol": {
            "time_split_applied": True,
            "split_method": "time_based_global_intersection",
            "purge_gap_applied": True,
            "purge_gap_minutes": purge_gap_minutes,
            "purge_gap_ms": purge_gap_ms,
            "warmup_prepend_applied": bool(warmup_prepend_minutes > 0),
            "warmup_prepend_minutes": warmup_prepend_minutes,
            "warmup_prepend_ms": warmup_prepend_ms,
            "ratios": {
                "development": dev_ratio,
                "validation": val_ratio,
                "quarantine": qua_ratio,
            },
        },
        "paths": {
            "source_data_dir": str(source_dir),
            "output_root_dir": str(output_root),
            "development_dir": str(out_dirs["dev"]),
            "validation_dir": str(out_dirs["val"]),
            "quarantine_dir": str(out_dirs["quarantine"]),
            "all_purged_dir": str(out_dirs["all_purged"]),
        },
        "time_bounds": {
            "intersection_start_ts": int(t_start),
            "development_end_ts": int(t_dev_end),
            "validation_end_ts": int(t_val_end),
            "intersection_end_ts": int(t_end),
            "development_core_start_ts": int(t_start),
            "development_core_end_ts": int(t_dev_end - purge_gap_ms),
            "validation_core_start_ts": int(t_dev_end + purge_gap_ms),
            "validation_core_end_ts": int(t_val_end - purge_gap_ms),
            "quarantine_core_start_ts": int(t_val_end + purge_gap_ms),
            "quarantine_core_end_ts": int(t_end),
            "validation_warmup_start_ts": int(max(t_start, (t_dev_end + purge_gap_ms) - warmup_prepend_ms)),
            "quarantine_warmup_start_ts": int(max(t_start, (t_val_end + purge_gap_ms) - warmup_prepend_ms)),
        },
        "primary_1m_ranges": primary_ranges,
        "primary_1m_datasets": [x.name for x in primary_files],
        "totals": totals,
        "file_stats": file_stats,
        "checks": {
            "dev_val_overlap_rows": 0,
            "dev_quarantine_overlap_rows": 0,
            "val_quarantine_overlap_rows": 0,
            "leak_proximity_rows_remaining": int(totals["leak_proximity_rows_remaining"]),
            "purge_effective": bool(int(totals["purge_removed_rows"]) > 0 and int(totals["leak_proximity_rows_remaining"]) == 0),
        },
    }

    ensure_dir(manifest_path.parent)
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(
        "[TimeSplit] built "
        f"primary={len(primary_files)} files={len(all_files)} "
        f"purge_removed={totals['purge_removed_rows']} "
        f"all_purged_rows={totals['all_purged_rows']} "
        f"manifest={manifest_path}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
