#!/usr/bin/env python3
import argparse
import csv
import json
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from _script_common import resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--market", "-Market", default="KRW-BTC")
    parser.add_argument("--unit", "-Unit", choices=["1", "3", "5", "10", "15", "30", "60", "240"], default="1")
    parser.add_argument("--candles", "-Candles", type=int, default=12000)
    parser.add_argument("--output-path", "-OutputPath", default="")
    parser.add_argument("--end-utc", "-EndUtc", default="")
    parser.add_argument("--chunk-size", "-ChunkSize", type=int, default=200)
    parser.add_argument("--sleep-ms", "-SleepMs", type=int, default=120)
    parser.add_argument("--base-url", "-BaseUrl", default="https://api.upbit.com")
    return parser.parse_args(argv)


def to_unix_ms(item):
    if "timestamp" in item:
        return int(item["timestamp"])
    if "candle_date_time_utc" in item:
        dt = datetime.strptime(item["candle_date_time_utc"], "%Y-%m-%dT%H:%M:%S")
        dt = dt.replace(tzinfo=timezone.utc)
        return int(dt.timestamp() * 1000)
    raise RuntimeError("Cannot derive timestamp from candle payload.")


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.candles <= 0:
        raise RuntimeError("Candles must be > 0.")
    if args.chunk_size <= 0 or args.chunk_size > 200:
        raise RuntimeError("ChunkSize must be between 1 and 200.")
    if args.sleep_ms < 0:
        raise RuntimeError("SleepMs must be >= 0.")

    output_path_value = args.output_path.strip()
    if not output_path_value:
        safe_market = args.market.replace("-", "_")
        output_path_value = f"./data/backtest_real/upbit_{safe_market}_{args.unit}m_{args.candles}.csv"
    output_path = resolve_repo_path(output_path_value)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    cursor_utc = None
    if args.end_utc.strip():
        cursor_utc = datetime.fromisoformat(args.end_utc.replace("Z", "+00:00")).astimezone(timezone.utc)

    endpoint = f"/v1/candles/minutes/{args.unit}"
    rows = []

    while len(rows) < args.candles:
        remaining = args.candles - len(rows)
        count = min(args.chunk_size, remaining)
        query = {"market": args.market, "count": str(count)}
        if cursor_utc is not None:
            query["to"] = cursor_utc.strftime("%Y-%m-%dT%H:%M:%SZ")

        query_string = urllib.parse.urlencode(query)
        url = f"{args.base_url.rstrip('/')}{endpoint}?{query_string}"
        request = urllib.request.Request(url=url, method="GET")
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = response.read().decode("utf-8")
        batch = json.loads(payload)
        if not isinstance(batch, list) or len(batch) == 0:
            break

        for item in batch:
            if not isinstance(item, dict):
                continue
            ts = to_unix_ms(item)
            rows.append(
                {
                    "timestamp": int(ts),
                    "open": float(item.get("opening_price", 0.0)),
                    "high": float(item.get("high_price", 0.0)),
                    "low": float(item.get("low_price", 0.0)),
                    "close": float(item.get("trade_price", 0.0)),
                    "volume": float(item.get("candle_acc_trade_volume", 0.0)),
                }
            )

        oldest = min(to_unix_ms(item) for item in batch if isinstance(item, dict))
        cursor_utc = datetime.fromtimestamp((oldest - 1) / 1000.0, tz=timezone.utc)
        if args.sleep_ms > 0:
            time.sleep(args.sleep_ms / 1000.0)

    dedup = {}
    for row in rows:
        dedup[int(row["timestamp"])] = row
    sorted_rows = [dedup[k] for k in sorted(dedup.keys())]
    if len(sorted_rows) > args.candles:
        sorted_rows = sorted_rows[-args.candles :]
    if not sorted_rows:
        raise RuntimeError("No candles fetched. Check market/unit/time range.")

    with output_path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=["timestamp", "open", "high", "low", "close", "volume"])
        writer.writeheader()
        writer.writerows(sorted_rows)

    first_ts = int(sorted_rows[0]["timestamp"])
    last_ts = int(sorted_rows[-1]["timestamp"])
    first_utc = datetime.fromtimestamp(first_ts / 1000.0, tz=timezone.utc).isoformat()
    last_utc = datetime.fromtimestamp(last_ts / 1000.0, tz=timezone.utc).isoformat()

    print("[FetchUpbitCandles] Completed")
    print(f"market={args.market}")
    print(f"unit={args.unit}m")
    print(f"rows={len(sorted_rows)}")
    print(f"from_utc={first_utc}")
    print(f"to_utc={last_utc}")
    print(f"output={output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
