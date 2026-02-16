#!/usr/bin/env python3
import argparse
import csv
import json
import time
import urllib.parse
import urllib.error
import urllib.request
from datetime import datetime, timezone

from _script_common import dump_json, ensure_parent_directory, resolve_repo_path


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
    parser.add_argument(
        "--compliance-telemetry-jsonl",
        "-ComplianceTelemetryJsonl",
        default="",
    )
    parser.add_argument(
        "--compliance-summary-json",
        "-ComplianceSummaryJson",
        default="",
    )
    parser.add_argument("--max-retries-429", "-MaxRetries429", type=int, default=5)
    parser.add_argument("--max-retries-418", "-MaxRetries418", type=int, default=2)
    parser.add_argument("--retry-base-ms", "-RetryBaseMs", type=int, default=600)
    return parser.parse_args(argv)


def to_unix_ms(item):
    if "timestamp" in item:
        return int(item["timestamp"])
    if "candle_date_time_utc" in item:
        dt = datetime.strptime(item["candle_date_time_utc"], "%Y-%m-%dT%H:%M:%S")
        dt = dt.replace(tzinfo=timezone.utc)
        return int(dt.timestamp() * 1000)
    raise RuntimeError("Cannot derive timestamp from candle payload.")


def parse_remaining_req(header_value: str) -> dict:
    out = {}
    for token in str(header_value or "").split(";"):
        token = token.strip()
        if "=" not in token:
            continue
        k, v = token.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def append_jsonl(path_value, payload):
    ensure_parent_directory(path_value)
    with path_value.open("a", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(payload, ensure_ascii=False) + "\n")


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.candles <= 0:
        raise RuntimeError("Candles must be > 0.")
    if args.chunk_size <= 0 or args.chunk_size > 200:
        raise RuntimeError("ChunkSize must be between 1 and 200.")
    if args.sleep_ms < 0:
        raise RuntimeError("SleepMs must be >= 0.")
    if args.retry_base_ms < 0:
        raise RuntimeError("RetryBaseMs must be >= 0.")

    output_path_value = args.output_path.strip()
    if not output_path_value:
        safe_market = args.market.replace("-", "_")
        output_path_value = f"./data/backtest_real/upbit_{safe_market}_{args.unit}m_{args.candles}.csv"
    output_path = resolve_repo_path(output_path_value)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    compliance_telemetry_path_value = args.compliance_telemetry_jsonl.strip()
    if not compliance_telemetry_path_value:
        compliance_telemetry_path_value = r".\build\Release\logs\upbit_compliance_telemetry.jsonl"
    compliance_summary_path_value = args.compliance_summary_json.strip()
    if not compliance_summary_path_value:
        safe_market = args.market.replace("-", "_")
        compliance_summary_path_value = (
            rf".\build\Release\logs\upbit_compliance_summary_{safe_market}_{args.unit}m.json"
        )

    compliance_telemetry_jsonl = resolve_repo_path(compliance_telemetry_path_value)
    compliance_summary_json = resolve_repo_path(compliance_summary_path_value)
    ensure_parent_directory(compliance_telemetry_jsonl)
    ensure_parent_directory(compliance_summary_json)

    cursor_utc = None
    if args.end_utc.strip():
        cursor_utc = datetime.fromisoformat(args.end_utc.replace("Z", "+00:00")).astimezone(timezone.utc)

    endpoint = f"/v1/candles/minutes/{args.unit}"
    rows = []
    req_count = 0
    ok_count = 0
    err_429_count = 0
    err_418_count = 0
    retry_count = 0
    backoff_sleep_ms_total = 0
    throttle_events = []
    recover_events = []

    while len(rows) < args.candles:
        remaining = args.candles - len(rows)
        count = min(args.chunk_size, remaining)
        query = {"market": args.market, "count": str(count)}
        if cursor_utc is not None:
            query["to"] = cursor_utc.strftime("%Y-%m-%dT%H:%M:%SZ")

        query_string = urllib.parse.urlencode(query)
        url = f"{args.base_url.rstrip('/')}{endpoint}?{query_string}"
        request = urllib.request.Request(url=url, method="GET")
        req_count += 1
        batch = None
        recovered_this_request = False
        for attempt in range(0, max(1, int(args.max_retries_429)) + max(1, int(args.max_retries_418)) + 1):
            try:
                with urllib.request.urlopen(request, timeout=30) as response:
                    payload = response.read().decode("utf-8")
                    remaining_req = parse_remaining_req(response.headers.get("Remaining-Req", ""))
                batch = json.loads(payload)
                ok_count += 1
                append_jsonl(
                    compliance_telemetry_jsonl,
                    {
                        "ts_utc": utc_now_iso(),
                        "event": "http_success",
                        "market": args.market,
                        "unit": args.unit,
                        "status_code": 200,
                        "attempt": attempt,
                        "remaining_req": remaining_req,
                    },
                )
                if recovered_this_request:
                    recover_events.append(
                        {
                            "ts_utc": utc_now_iso(),
                            "market": args.market,
                            "unit": args.unit,
                            "event": "recover_after_rate_limit",
                        }
                    )
                break
            except urllib.error.HTTPError as e:
                status = int(getattr(e, "code", 0) or 0)
                if status not in (429, 418):
                    raise

                recovered_this_request = True
                if status == 429:
                    err_429_count += 1
                    retry_cap = max(1, int(args.max_retries_429))
                    backoff_ms = int(max(0, int(args.retry_base_ms)) * (2 ** min(attempt, 5)))
                else:
                    err_418_count += 1
                    retry_cap = max(1, int(args.max_retries_418))
                    backoff_ms = int(max(1500, int(args.retry_base_ms) * 6) * (attempt + 1))

                append_jsonl(
                    compliance_telemetry_jsonl,
                    {
                        "ts_utc": utc_now_iso(),
                        "event": "rate_limit_error",
                        "market": args.market,
                        "unit": args.unit,
                        "status_code": status,
                        "attempt": attempt,
                        "backoff_ms": backoff_ms,
                    },
                )
                throttle_events.append(
                    {
                        "ts_utc": utc_now_iso(),
                        "market": args.market,
                        "unit": args.unit,
                        "status_code": status,
                        "attempt": attempt,
                        "backoff_ms": backoff_ms,
                    }
                )

                if attempt >= retry_cap:
                    raise RuntimeError(
                        f"Rate-limit retry exhausted: status={status}, attempt={attempt}, url={url}"
                    )

                retry_count += 1
                backoff_sleep_ms_total += backoff_ms
                if backoff_ms > 0:
                    time.sleep(backoff_ms / 1000.0)

        if batch is None:
            raise RuntimeError(f"Fetch failed: market={args.market}, unit={args.unit}, url={url}")
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
    compliance_summary = {
        "generated_at": utc_now_iso(),
        "market": args.market,
        "unit": f"{args.unit}m",
        "request_count": req_count,
        "http_success_count": ok_count,
        "rate_limit_429_count": err_429_count,
        "rate_limit_418_count": err_418_count,
        "retry_count": retry_count,
        "backoff_sleep_ms_total": backoff_sleep_ms_total,
        "throttle_event_count": len(throttle_events),
        "recover_event_count": len(recover_events),
        "telemetry_jsonl": str(compliance_telemetry_jsonl),
        "rows": len(sorted_rows),
        "from_utc": first_utc,
        "to_utc": last_utc,
    }
    dump_json(compliance_summary_json, compliance_summary)
    print(f"compliance_telemetry={compliance_telemetry_jsonl}")
    print(f"compliance_summary={compliance_summary_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
