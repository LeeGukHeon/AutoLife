#!/usr/bin/env python3
import argparse
import csv
import json
import math
import random
import time
import urllib.parse
import urllib.error
import urllib.request
from datetime import datetime, timezone
from typing import Dict, Any, Optional

from _script_common import dump_json, ensure_parent_directory, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--market", "-Market", default="KRW-BTC")
    parser.add_argument("--unit", "-Unit", choices=["1", "3", "5", "10", "15", "30", "60", "240"], default="1")
    parser.add_argument("--candles", "-Candles", type=int, default=12000)
    parser.add_argument("--output-path", "-OutputPath", default="")
    parser.add_argument("--start-utc", "-StartUtc", default="")
    parser.add_argument("--end-utc", "-EndUtc", default="")
    parser.add_argument("--append-existing", "-AppendExisting", action="store_true")
    parser.add_argument("--auto-start-from-output", "-AutoStartFromOutput", action="store_true")
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
    parser.add_argument(
        "--retry-max-backoff-ms",
        "-RetryMaxBackoffMs",
        type=int,
        default=10000,
    )
    parser.add_argument(
        "--sec-zero-jitter-max-ms",
        "-SecZeroJitterMaxMs",
        type=int,
        default=50,
    )
    return parser.parse_args(argv)


def to_unix_ms(item):
    if "timestamp" in item and item.get("timestamp") is not None:
        try:
            return int(float(item["timestamp"]))
        except Exception:
            pass
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


def parse_remaining_req_sec(header_value: str) -> Optional[int]:
    tokens = parse_remaining_req(header_value)
    raw = tokens.get("sec")
    if raw is None:
        return None
    try:
        sec = int(str(raw).strip())
    except Exception:
        return None
    return sec if sec >= 0 else None


def parse_retry_after_ms(header_value: str) -> int:
    raw = str(header_value or "").strip()
    if not raw:
        return 0
    try:
        sec = float(raw)
    except Exception:
        return 0
    if not math.isfinite(sec) or sec <= 0.0:
        return 0
    return int(sec * 1000.0)


def bounded_exponential_backoff_ms(base_ms: int, attempt: int, max_backoff_ms: int) -> int:
    base = max(0, int(base_ms))
    exponent = min(max(0, int(attempt)), 10)
    backoff = base * (2 ** exponent)
    cap = max(0, int(max_backoff_ms))
    if cap > 0:
        backoff = min(backoff, cap)
    return int(backoff)


def compute_next_second_boundary_sleep_ms(now_epoch_sec: float, jitter_ms: int) -> int:
    now_value = float(now_epoch_sec)
    if not math.isfinite(now_value):
        return max(0, int(jitter_ms))
    next_boundary_sec = math.floor(now_value) + 1.0
    base_ms = int(max(0.0, math.ceil((next_boundary_sec - now_value) * 1000.0)))
    return int(base_ms + max(0, int(jitter_ms)))


def compute_sec_zero_throttle_sleep_ms(
    remaining_req_header: str,
    jitter_max_ms: int,
    now_epoch_sec: Optional[float] = None,
    jitter_ms: Optional[int] = None,
) -> int:
    sec = parse_remaining_req_sec(remaining_req_header)
    if sec is None or sec > 0:
        return 0
    if now_epoch_sec is None:
        now_epoch_sec = time.time()
    if jitter_ms is None:
        jitter_ms = random.randint(0, max(0, int(jitter_max_ms)))
    return compute_next_second_boundary_sleep_ms(now_epoch_sec, int(jitter_ms))


def strip_origin_header(request: urllib.request.Request) -> bool:
    removed = False
    for container_name in ("headers", "unredirected_hdrs"):
        container = getattr(request, container_name, None)
        if not isinstance(container, dict):
            continue
        for key in list(container.keys()):
            if str(key).strip().lower() == "origin":
                container.pop(key, None)
                removed = True
    return removed


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def append_jsonl(path_value, payload):
    ensure_parent_directory(path_value)
    with path_value.open("a", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(payload, ensure_ascii=False) + "\n")


def load_existing_rows(path_value) -> Dict[int, Dict[str, float]]:
    rows: Dict[int, Dict[str, float]] = {}
    if not path_value.exists():
        return rows
    with path_value.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            try:
                ts = int(float(row.get("timestamp", 0)))
                if ts <= 0:
                    continue
                rows[int(ts)] = {
                    "timestamp": int(ts),
                    "open": float(row.get("open", 0.0)),
                    "high": float(row.get("high", 0.0)),
                    "low": float(row.get("low", 0.0)),
                    "close": float(row.get("close", 0.0)),
                    "volume": float(row.get("volume", 0.0)),
                }
            except Exception:
                continue
    return rows


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.candles < 0:
        raise RuntimeError("Candles must be >= 0.")
    if args.chunk_size <= 0 or args.chunk_size > 200:
        raise RuntimeError("ChunkSize must be between 1 and 200.")
    if args.sleep_ms < 0:
        raise RuntimeError("SleepMs must be >= 0.")
    if args.retry_base_ms < 0:
        raise RuntimeError("RetryBaseMs must be >= 0.")
    if args.retry_max_backoff_ms < 0:
        raise RuntimeError("RetryMaxBackoffMs must be >= 0.")
    if args.sec_zero_jitter_max_ms < 0 or args.sec_zero_jitter_max_ms > 50:
        raise RuntimeError("SecZeroJitterMaxMs must be between 0 and 50.")

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
    if cursor_utc is None:
        cursor_utc = datetime.now(tz=timezone.utc)

    start_ts_ms = None
    if args.start_utc.strip():
        start_dt = datetime.fromisoformat(args.start_utc.replace("Z", "+00:00")).astimezone(timezone.utc)
        start_ts_ms = int(start_dt.timestamp() * 1000)

    endpoint = f"/v1/candles/minutes/{args.unit}"
    rows_by_ts: Dict[int, Dict[str, float]] = {}
    existing_rows_loaded = 0
    existing_last_ts = 0
    if bool(args.append_existing):
        rows_by_ts = load_existing_rows(output_path)
        existing_rows_loaded = int(len(rows_by_ts))
        if existing_rows_loaded > 0:
            existing_last_ts = int(max(rows_by_ts.keys()))
    if bool(args.auto_start_from_output) and existing_last_ts > 0:
        auto_start_ts = int(existing_last_ts + 1)
        if start_ts_ms is None:
            start_ts_ms = auto_start_ts
        else:
            start_ts_ms = max(int(start_ts_ms), auto_start_ts)

    if args.candles == 0 and start_ts_ms is None:
        raise RuntimeError("Candles=0 requires --start-utc or --auto-start-from-output.")

    initial_rows = int(len(rows_by_ts))
    prev_oldest_ts = None
    stagnant_cursor_count = 0
    stagnant_unique_count = 0
    broke_on_stagnant_cursor = False
    broke_on_stagnant_unique = False
    req_count = 0
    ok_count = 0
    err_429_count = 0
    err_418_count = 0
    retry_count = 0
    backoff_sleep_ms_total = 0
    remaining_req_missing_count = 0
    remaining_req_sec0_throttle_count = 0
    remaining_req_sec0_sleep_ms_total = 0
    origin_header_stripped_count = 0
    throttle_events = []
    recover_events = []

    reached_start_boundary = False
    while True:
        if args.candles > 0 and len(rows_by_ts) >= args.candles:
            break
        if reached_start_boundary:
            break
        remaining = max(1, args.candles - len(rows_by_ts)) if args.candles > 0 else args.chunk_size
        count = min(args.chunk_size, remaining)
        query = {"market": args.market, "count": str(count)}
        if cursor_utc is not None:
            query["to"] = cursor_utc.strftime("%Y-%m-%dT%H:%M:%SZ")

        query_string = urllib.parse.urlencode(query)
        url = f"{args.base_url.rstrip('/')}{endpoint}?{query_string}"
        request = urllib.request.Request(url=url, method="GET")
        request.add_header("Accept", "application/json")
        if strip_origin_header(request):
            origin_header_stripped_count += 1

        request_meta = {
            "endpoint": endpoint,
            "params": {
                "market": str(query.get("market", "")),
                "count": str(query.get("count", "")),
                "to": str(query.get("to", "")),
            },
        }
        batch = None
        recovered_this_request = False
        retry_429_for_request = 0
        retry_418_for_request = 0
        while True:
            req_count += 1
            started_perf = time.perf_counter()
            try:
                with urllib.request.urlopen(request, timeout=30) as response:
                    payload = response.read().decode("utf-8")
                    remaining_req_raw = response.headers.get("Remaining-Req", "")
                    remaining_req = parse_remaining_req(remaining_req_raw)
                    remaining_req_sec = parse_remaining_req_sec(remaining_req_raw)
                    if remaining_req_sec is None:
                        remaining_req_missing_count += 1
                latency_ms = int(round((time.perf_counter() - started_perf) * 1000.0))
                batch = json.loads(payload)
                ok_count += 1
                append_jsonl(
                    compliance_telemetry_jsonl,
                    {
                        "ts_utc": utc_now_iso(),
                        "event": "http_success",
                        "market": args.market,
                        "unit": args.unit,
                        "request": request_meta,
                        "status_code": 200,
                        "latency_ms": latency_ms,
                        "attempt": int(retry_429_for_request + retry_418_for_request),
                        "remaining_req_raw": str(remaining_req_raw or ""),
                        "remaining_req": remaining_req,
                        "remaining_req_sec": remaining_req_sec,
                    },
                )
                throttle_sleep_ms = compute_sec_zero_throttle_sleep_ms(
                    str(remaining_req_raw or ""),
                    int(args.sec_zero_jitter_max_ms),
                )
                if throttle_sleep_ms > 0:
                    remaining_req_sec0_throttle_count += 1
                    remaining_req_sec0_sleep_ms_total += int(throttle_sleep_ms)
                    append_jsonl(
                        compliance_telemetry_jsonl,
                        {
                            "ts_utc": utc_now_iso(),
                            "event": "remaining_req_sec0_throttle",
                            "market": args.market,
                            "unit": args.unit,
                            "request": request_meta,
                            "remaining_req_raw": str(remaining_req_raw or ""),
                            "remaining_req": remaining_req,
                            "sleep_ms": int(throttle_sleep_ms),
                        },
                    )
                    throttle_events.append(
                        {
                            "ts_utc": utc_now_iso(),
                            "market": args.market,
                            "unit": args.unit,
                            "event": "remaining_req_sec0_throttle",
                            "sleep_ms": int(throttle_sleep_ms),
                        }
                    )
                    time.sleep(float(throttle_sleep_ms) / 1000.0)
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
                latency_ms = int(round((time.perf_counter() - started_perf) * 1000.0))
                status = int(getattr(e, "code", 0) or 0)
                if status not in (429, 418):
                    raise

                recovered_this_request = True
                headers = getattr(e, "headers", None)
                remaining_req_raw = ""
                retry_after_raw = ""
                if headers is not None:
                    remaining_req_raw = str(headers.get("Remaining-Req", "") or "")
                    retry_after_raw = str(headers.get("Retry-After", "") or "")
                remaining_req = parse_remaining_req(remaining_req_raw)
                remaining_req_sec = parse_remaining_req_sec(remaining_req_raw)
                if remaining_req_sec is None:
                    remaining_req_missing_count += 1
                retry_after_ms = parse_retry_after_ms(retry_after_raw)
                if status == 429:
                    err_429_count += 1
                    retry_cap = max(0, int(args.max_retries_429))
                    if retry_429_for_request >= retry_cap:
                        raise RuntimeError(
                            f"Rate-limit retry exhausted: status={status}, retries={retry_429_for_request}, url={url}"
                        )
                    exp_backoff_ms = bounded_exponential_backoff_ms(
                        base_ms=int(args.retry_base_ms),
                        attempt=int(retry_429_for_request),
                        max_backoff_ms=int(args.retry_max_backoff_ms),
                    )
                    backoff_ms = max(int(exp_backoff_ms), int(retry_after_ms))
                    retry_429_for_request += 1
                else:
                    err_418_count += 1
                    retry_cap = max(0, int(args.max_retries_418))
                    if retry_418_for_request >= retry_cap:
                        raise RuntimeError(
                            f"Rate-limit retry exhausted: status={status}, retries={retry_418_for_request}, url={url}"
                        )
                    step_base = max(1500, int(args.retry_base_ms) * 6)
                    linear_backoff_ms = int(step_base * (retry_418_for_request + 1))
                    cap_ms = max(0, int(args.retry_max_backoff_ms))
                    if cap_ms > 0:
                        linear_backoff_ms = min(linear_backoff_ms, cap_ms)
                    backoff_ms = max(int(linear_backoff_ms), int(retry_after_ms))
                    retry_418_for_request += 1

                append_jsonl(
                    compliance_telemetry_jsonl,
                    {
                        "ts_utc": utc_now_iso(),
                        "event": "rate_limit_error",
                        "market": args.market,
                        "unit": args.unit,
                        "request": request_meta,
                        "status_code": status,
                        "latency_ms": latency_ms,
                        "attempt": int(retry_429_for_request + retry_418_for_request),
                        "remaining_req_raw": str(remaining_req_raw),
                        "remaining_req": remaining_req,
                        "remaining_req_sec": remaining_req_sec,
                        "retry_after_ms": int(retry_after_ms),
                        "backoff_ms": int(backoff_ms),
                    },
                )
                throttle_events.append(
                    {
                        "ts_utc": utc_now_iso(),
                        "market": args.market,
                        "unit": args.unit,
                        "status_code": status,
                        "attempt": int(retry_429_for_request + retry_418_for_request),
                        "backoff_ms": int(backoff_ms),
                    }
                )

                retry_count += 1
                backoff_sleep_ms_total += int(backoff_ms)
                if backoff_ms > 0:
                    time.sleep(float(backoff_ms) / 1000.0)

        if batch is None:
            raise RuntimeError(f"Fetch failed: market={args.market}, unit={args.unit}, url={url}")
        if not isinstance(batch, list) or len(batch) == 0:
            break

        batch_new_unique = 0
        for item in batch:
            if not isinstance(item, dict):
                continue
            ts = to_unix_ms(item)
            if start_ts_ms is not None and int(ts) < int(start_ts_ms):
                continue
            if int(ts) not in rows_by_ts:
                batch_new_unique += 1
            rows_by_ts[int(ts)] = {
                "timestamp": int(ts),
                "open": float(item.get("opening_price", 0.0)),
                "high": float(item.get("high_price", 0.0)),
                "low": float(item.get("low_price", 0.0)),
                "close": float(item.get("trade_price", 0.0)),
                "volume": float(item.get("candle_acc_trade_volume", 0.0)),
            }

        oldest = min(to_unix_ms(item) for item in batch if isinstance(item, dict))
        if start_ts_ms is not None and int(oldest) <= int(start_ts_ms):
            reached_start_boundary = True
        if prev_oldest_ts is not None and int(oldest) >= int(prev_oldest_ts):
            stagnant_cursor_count += 1
        else:
            stagnant_cursor_count = 0
        prev_oldest_ts = int(oldest)

        if int(batch_new_unique) <= 0:
            stagnant_unique_count += 1
        else:
            stagnant_unique_count = 0

        if stagnant_cursor_count >= 3:
            broke_on_stagnant_cursor = True
            append_jsonl(
                compliance_telemetry_jsonl,
                {
                    "ts_utc": utc_now_iso(),
                    "event": "stagnant_cursor_break",
                    "market": args.market,
                    "unit": args.unit,
                    "stagnant_cursor_count": stagnant_cursor_count,
                    "oldest_ts_ms": int(oldest),
                },
            )
            break

        if stagnant_unique_count >= 3:
            broke_on_stagnant_unique = True
            append_jsonl(
                compliance_telemetry_jsonl,
                {
                    "ts_utc": utc_now_iso(),
                    "event": "stagnant_unique_break",
                    "market": args.market,
                    "unit": args.unit,
                    "stagnant_unique_count": stagnant_unique_count,
                    "oldest_ts_ms": int(oldest),
                },
            )
            break

        cursor_utc = datetime.fromtimestamp((oldest - 1) / 1000.0, tz=timezone.utc)
        if args.sleep_ms > 0:
            time.sleep(args.sleep_ms / 1000.0)

    sorted_rows = [rows_by_ts[k] for k in sorted(rows_by_ts.keys())]
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
    print(f"new_rows={max(0, len(sorted_rows) - initial_rows)}")
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
        "remaining_req_missing_count": int(remaining_req_missing_count),
        "remaining_req_sec0_throttle_count": int(remaining_req_sec0_throttle_count),
        "remaining_req_sec0_sleep_ms_total": int(remaining_req_sec0_sleep_ms_total),
        "origin_header_stripped_count": int(origin_header_stripped_count),
        "backoff_sleep_ms_total": backoff_sleep_ms_total,
        "throttle_event_count": len(throttle_events),
        "recover_event_count": len(recover_events),
        "telemetry_jsonl": str(compliance_telemetry_jsonl),
        "rows": len(sorted_rows),
        "new_rows": int(max(0, len(sorted_rows) - initial_rows)),
        "append_existing": bool(args.append_existing),
        "auto_start_from_output": bool(args.auto_start_from_output),
        "existing_rows_loaded": int(existing_rows_loaded),
        "existing_last_utc": (
            datetime.fromtimestamp(existing_last_ts / 1000.0, tz=timezone.utc).isoformat()
            if existing_last_ts > 0 else ""
        ),
        "start_utc": (
            datetime.fromtimestamp(int(start_ts_ms) / 1000.0, tz=timezone.utc).isoformat()
            if start_ts_ms is not None else ""
        ),
        "from_utc": first_utc,
        "to_utc": last_utc,
        "stagnant_cursor_break": bool(broke_on_stagnant_cursor),
        "stagnant_unique_break": bool(broke_on_stagnant_unique),
        "stagnant_cursor_count": int(stagnant_cursor_count),
        "stagnant_unique_count": int(stagnant_unique_count),
    }
    dump_json(compliance_summary_json, compliance_summary)
    print(f"compliance_telemetry={compliance_telemetry_jsonl}")
    print(f"compliance_summary={compliance_summary_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
