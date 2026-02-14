#!/usr/bin/env python3
import argparse
import csv
import json
from datetime import datetime, timezone

from _script_common import resolve_repo_path


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", "-DataDir", default="./data/backtest")
    parser.add_argument("--output-csv", "-OutputCsv", default="./build/Release/logs/backtest_data_audit.csv")
    parser.add_argument("--output-json", "-OutputJson", default="./build/Release/logs/backtest_data_audit.json")
    return parser.parse_args(argv)


def safe_avg(values):
    return sum(values) / float(len(values)) if values else 0.0


def safe_max(values):
    return max(values) if values else 0.0


def main(argv=None) -> int:
    args = parse_args(argv)
    data_dir = resolve_repo_path(args.data_dir)
    output_csv = resolve_repo_path(args.output_csv)
    output_json = resolve_repo_path(args.output_json)
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    output_json.parent.mkdir(parents=True, exist_ok=True)

    if not data_dir.exists():
        raise RuntimeError(f"Backtest data dir not found: {data_dir}")

    files = sorted([p for p in data_dir.glob("*.csv") if p.is_file()], key=lambda p: str(p).lower())
    if not files:
        raise RuntimeError(f"No CSV files found in {data_dir}")

    rows = []
    for file_path in files:
        with file_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
            reader = csv.DictReader(fh)
            candles = list(reader)
        if len(candles) < 2:
            continue

        ts = [float(r.get("timestamp", 0.0)) for r in candles]
        open_arr = [float(r.get("open", 0.0)) for r in candles]
        high_arr = [float(r.get("high", 0.0)) for r in candles]
        low_arr = [float(r.get("low", 0.0)) for r in candles]
        close_arr = [float(r.get("close", 0.0)) for r in candles]
        vol_arr = [float(r.get("volume", 0.0)) for r in candles]

        dts = [ts[i] - ts[i - 1] for i in range(1, len(ts))]
        dt_count = {}
        for dt in dts:
            k = str(dt)
            dt_count[k] = dt_count.get(k, 0) + 1
        mode_dt = float(max(dt_count.items(), key=lambda kv: kv[1])[0]) if dt_count else 0.0

        returns = []
        for i in range(1, len(close_arr)):
            prev = close_arr[i - 1]
            if prev != 0.0:
                returns.append((close_arr[i] - prev) / prev)

        avg_abs_ret = safe_avg([abs(x) for x in returns])
        max_abs_ret = safe_max([abs(x) for x in returns])

        bad_ohlc = 0
        for i in range(len(candles)):
            if (
                high_arr[i] < open_arr[i]
                or high_arr[i] < close_arr[i]
                or low_arr[i] > open_arr[i]
                or low_arr[i] > close_arr[i]
                or high_arr[i] < low_arr[i]
            ):
                bad_ohlc += 1

        ts_unit = "ms" if ts[0] > 1e12 else "sec"
        too_short = len(candles) < 1000
        low_regime_coverage = len(candles) < 300
        time_jitter = len(set(dts)) > 1
        very_low_shock = (max_abs_ret * 100.0) < 1.0

        rows.append(
            {
                "file": file_path.name,
                "rows": len(candles),
                "ts_unit": ts_unit,
                "mode_dt": mode_dt,
                "dt_unique": len(set(dts)),
                "bad_ohlc_rows": bad_ohlc,
                "avg_abs_ret_pct": round(avg_abs_ret * 100.0, 4),
                "max_abs_ret_pct": round(max_abs_ret * 100.0, 4),
                "avg_volume": round(safe_avg(vol_arr), 4),
                "too_short_for_tune": too_short,
                "low_regime_coverage": low_regime_coverage,
                "timestamp_mixed_risk": False,
                "time_jitter_risk": time_jitter,
                "low_shock_risk": very_low_shock,
            }
        )

    has_sec = any(r["ts_unit"] == "sec" for r in rows)
    has_ms = any(r["ts_unit"] == "ms" for r in rows)
    if has_sec and has_ms:
        for row in rows:
            row["timestamp_mixed_risk"] = True

    with output_csv.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()) if rows else [])
        if rows:
            writer.writeheader()
            writer.writerows(rows)

    summary = {
        "generated_at_utc": datetime.now(tz=timezone.utc).isoformat(),
        "dataset_count": len(rows),
        "has_mixed_timestamp_units": bool(has_sec and has_ms),
        "short_datasets": len([r for r in rows if bool(r["too_short_for_tune"])]),
        "low_regime_coverage_datasets": len([r for r in rows if bool(r["low_regime_coverage"])]),
        "low_shock_datasets": len([r for r in rows if bool(r["low_shock_risk"])]),
        "bad_ohlc_datasets": len([r for r in rows if int(r["bad_ohlc_rows"]) > 0]),
    }
    report = {"summary": summary, "files": rows}
    output_json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("=== Backtest Data Audit ===")
    print(f"datasets={len(rows)}")
    print("=== Audit Summary ===")
    print(f"mixed_timestamp_units={summary['has_mixed_timestamp_units']}")
    print(f"saved_csv={output_csv}")
    print(f"saved_json={output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
