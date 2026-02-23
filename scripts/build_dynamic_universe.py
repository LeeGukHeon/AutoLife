#!/usr/bin/env python3
import argparse
import hashlib
import json
import random
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from typing import Dict, List, Tuple

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build dynamic runtime universe (active + maintenance + final 1m scope)."
    )
    parser.add_argument("--base-url", default="https://api.upbit.com")
    parser.add_argument("--market-prefix", default="KRW-")
    parser.add_argument("--active-size", type=int, default=25)
    parser.add_argument("--buffer-size", type=int, default=50)
    parser.add_argument("--membership-ttl-days", type=int, default=7)
    parser.add_argument(
        "--output-json",
        default=r".\config\universe\runtime_universe.json",
    )
    parser.add_argument(
        "--history-json",
        default=r".\config\universe\runtime_universe_history.json",
    )
    parser.add_argument("--max-retries-429", type=int, default=5)
    parser.add_argument("--retry-base-ms", type=int, default=500)
    parser.add_argument("--request-timeout-sec", type=int, default=30)
    return parser.parse_args(argv)


def utc_now() -> datetime:
    return datetime.now(tz=timezone.utc)


def utc_now_iso() -> str:
    return utc_now().isoformat()


def parse_remaining_req(header_value: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for token in str(header_value or "").split(";"):
        token = token.strip()
        if "=" not in token:
            continue
        k, v = token.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def parse_int_safe(text: str, default: int = -1) -> int:
    try:
        return int(str(text).strip())
    except Exception:
        return int(default)


def sleep_until_next_second_with_jitter_ms(jitter_max_ms: int = 50) -> int:
    now = time.time()
    frac = now - int(now)
    until_next_sec = max(0.0, 1.0 - frac)
    jitter = random.uniform(0.0, max(0.0, float(jitter_max_ms)) / 1000.0)
    sleep_sec = until_next_sec + jitter
    if sleep_sec > 0.0:
        time.sleep(sleep_sec)
    return int(round(sleep_sec * 1000.0))


def request_json(
    *,
    base_url: str,
    endpoint: str,
    params: Dict[str, str],
    timeout_sec: int,
    max_retries_429: int,
    retry_base_ms: int,
) -> Tuple[object, Dict[str, str]]:
    query = urllib.parse.urlencode(params)
    url = f"{base_url.rstrip('/')}{endpoint}"
    if query:
        url = f"{url}?{query}"

    request = urllib.request.Request(url=url, method="GET")
    for attempt in range(max(1, int(max_retries_429)) + 1):
        try:
            with urllib.request.urlopen(request, timeout=max(1, int(timeout_sec))) as response:
                body = response.read().decode("utf-8")
                remaining_req = parse_remaining_req(response.headers.get("Remaining-Req", ""))
            payload = json.loads(body)
            sec_left = parse_int_safe(remaining_req.get("sec", ""), default=-1)
            if sec_left == 0:
                slept_ms = sleep_until_next_second_with_jitter_ms(50)
                print(
                    f"[BuildDynamicUniverse] throttle remaining_sec=0 "
                    f"endpoint={endpoint} slept_ms={slept_ms}",
                    flush=True,
                )
            return payload, remaining_req
        except urllib.error.HTTPError as exc:
            status = int(getattr(exc, "code", 0) or 0)
            if status != 429:
                raise
            if attempt >= int(max_retries_429):
                raise RuntimeError(f"429 retries exhausted endpoint={endpoint} params={params}") from exc
            backoff_ms = int(max(50, int(retry_base_ms)) * (2 ** min(attempt, 5)))
            backoff_ms = min(backoff_ms, 8000)
            print(
                f"[BuildDynamicUniverse] 429 backoff endpoint={endpoint} "
                f"attempt={attempt} backoff_ms={backoff_ms}",
                flush=True,
            )
            time.sleep(backoff_ms / 1000.0)
    raise RuntimeError(f"request failed endpoint={endpoint} params={params}")


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


def fetch_market_list(
    *,
    base_url: str,
    market_prefix: str,
    timeout_sec: int,
    max_retries_429: int,
    retry_base_ms: int,
) -> Tuple[List[str], Dict[str, str]]:
    payload, rem = request_json(
        base_url=base_url,
        endpoint="/v1/market/all",
        params={"isDetails": "false"},
        timeout_sec=timeout_sec,
        max_retries_429=max_retries_429,
        retry_base_ms=retry_base_ms,
    )
    if not isinstance(payload, list):
        raise RuntimeError("invalid /v1/market/all payload")
    prefix = str(market_prefix or "").strip().upper()
    markets: List[str] = []
    for item in payload:
        if not isinstance(item, dict):
            continue
        market = str(item.get("market", "")).strip().upper()
        if not market:
            continue
        if prefix and not market.startswith(prefix):
            continue
        markets.append(market)
    return dedup_upper(markets), rem


def chunk_list(values: List[str], chunk_size: int) -> List[List[str]]:
    size = max(1, int(chunk_size))
    return [values[i : i + size] for i in range(0, len(values), size)]


def fetch_ticker_rows(
    *,
    base_url: str,
    markets: List[str],
    timeout_sec: int,
    max_retries_429: int,
    retry_base_ms: int,
) -> Tuple[List[Dict[str, object]], List[Dict[str, object]]]:
    out_rows: List[Dict[str, object]] = []
    remaining_samples: List[Dict[str, object]] = []
    for idx, chunk in enumerate(chunk_list(markets, 80), start=1):
        payload, rem = request_json(
            base_url=base_url,
            endpoint="/v1/ticker",
            params={"markets": ",".join(chunk)},
            timeout_sec=timeout_sec,
            max_retries_429=max_retries_429,
            retry_base_ms=retry_base_ms,
        )
        if not isinstance(payload, list):
            raise RuntimeError(f"invalid /v1/ticker payload for chunk={idx}")
        for item in payload:
            if not isinstance(item, dict):
                continue
            market = str(item.get("market", "")).strip().upper()
            if not market:
                continue
            try:
                volume_24h = float(item.get("acc_trade_price_24h", item.get("acc_trade_price", 0.0)))
            except Exception:
                volume_24h = 0.0
            try:
                ts_ms = int(float(item.get("timestamp", 0)))
            except Exception:
                ts_ms = 0
            out_rows.append(
                {
                    "market": market,
                    "acc_trade_price_24h": float(volume_24h),
                    "timestamp": int(ts_ms),
                }
            )
        remaining_samples.append(
            {
                "chunk_index": int(idx),
                "chunk_size": len(chunk),
                "remaining_req": rem,
            }
        )
    return out_rows, remaining_samples


def rank_markets_by_volume(ticker_rows: List[Dict[str, object]]) -> List[str]:
    scored: List[Tuple[float, str]] = []
    for row in ticker_rows:
        market = str(row.get("market", "")).strip().upper()
        if not market:
            continue
        try:
            volume = float(row.get("acc_trade_price_24h", 0.0))
        except Exception:
            volume = 0.0
        if volume < 0.0:
            volume = 0.0
        scored.append((float(volume), market))
    scored.sort(key=lambda x: (-x[0], x[1]))
    ordered: List[str] = []
    seen = set()
    for _, market in scored:
        if market in seen:
            continue
        seen.add(market)
        ordered.append(market)
    return ordered


def load_history_records(history_path) -> List[Dict[str, object]]:
    raw = load_json_or_none(history_path)
    if not isinstance(raw, dict):
        return []
    records = raw.get("records", [])
    if not isinstance(records, list):
        return []
    out: List[Dict[str, object]] = []
    for item in records:
        if not isinstance(item, dict):
            continue
        date_utc = str(item.get("date_utc", "")).strip()
        if not date_utc:
            continue
        active_markets = dedup_upper([str(x) for x in (item.get("active_markets", []) or [])])
        out.append({"date_utc": date_utc, "active_markets": active_markets})
    return out


def update_history_records(
    *,
    previous_records: List[Dict[str, object]],
    today_utc: str,
    active_markets: List[str],
    membership_ttl_days: int,
) -> List[Dict[str, object]]:
    by_date: Dict[str, List[str]] = {}
    for item in previous_records:
        date_utc = str(item.get("date_utc", "")).strip()
        if not date_utc:
            continue
        by_date[date_utc] = dedup_upper([str(x) for x in (item.get("active_markets", []) or [])])

    by_date[today_utc] = dedup_upper(active_markets)

    today_dt = datetime.fromisoformat(f"{today_utc}T00:00:00+00:00")
    keep_from_dt = today_dt - timedelta(days=max(0, int(membership_ttl_days) - 1))
    keep_from = keep_from_dt.strftime("%Y-%m-%d")

    kept_dates = sorted([d for d in by_date.keys() if d >= keep_from and d <= today_utc])
    out: List[Dict[str, object]] = []
    for date_utc in kept_dates:
        out.append({"date_utc": date_utc, "active_markets": dedup_upper(by_date[date_utc])})
    return out


def build_snapshot_sha256(markets: List[str], ticker_rows: List[Dict[str, object]]) -> str:
    normalized_rows = []
    for row in ticker_rows:
        normalized_rows.append(
            {
                "market": str(row.get("market", "")).strip().upper(),
                "acc_trade_price_24h": float(row.get("acc_trade_price_24h", 0.0)),
                "timestamp": int(row.get("timestamp", 0) or 0),
            }
        )
    normalized_rows.sort(key=lambda x: x["market"])
    payload = {
        "markets": sorted(dedup_upper(markets)),
        "ticker_rows": normalized_rows,
    }
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
    return hashlib.sha256(body.encode("utf-8")).hexdigest()


def ordered_union(primary: List[str], secondary: List[str]) -> List[str]:
    out: List[str] = []
    seen = set()
    for token in primary + secondary:
        m = str(token or "").strip().upper()
        if not m or m in seen:
            continue
        seen.add(m)
        out.append(m)
    return out


def main(argv=None) -> int:
    args = parse_args(argv)
    active_size = max(1, int(args.active_size))
    buffer_size = max(0, int(args.buffer_size))
    membership_ttl_days = max(1, int(args.membership_ttl_days))

    output_json = resolve_repo_path(args.output_json)
    history_json = resolve_repo_path(args.history_json)

    markets, _ = fetch_market_list(
        base_url=str(args.base_url),
        market_prefix=str(args.market_prefix),
        timeout_sec=int(args.request_timeout_sec),
        max_retries_429=int(args.max_retries_429),
        retry_base_ms=int(args.retry_base_ms),
    )
    if not markets:
        raise RuntimeError("no markets resolved from /v1/market/all")

    ticker_rows, remaining_samples = fetch_ticker_rows(
        base_url=str(args.base_url),
        markets=markets,
        timeout_sec=int(args.request_timeout_sec),
        max_retries_429=int(args.max_retries_429),
        retry_base_ms=int(args.retry_base_ms),
    )
    if not ticker_rows:
        raise RuntimeError("no ticker rows fetched")

    ranked_markets = rank_markets_by_volume(ticker_rows)
    active_markets = ranked_markets[:active_size]
    buffer_top = ranked_markets[:buffer_size]

    today_utc = utc_now().strftime("%Y-%m-%d")
    previous_records = load_history_records(history_json)
    history_records = update_history_records(
        previous_records=previous_records,
        today_utc=today_utc,
        active_markets=active_markets,
        membership_ttl_days=membership_ttl_days,
    )

    union_recent_active: List[str] = []
    for item in history_records:
        union_recent_active = ordered_union(
            union_recent_active,
            [str(x) for x in (item.get("active_markets", []) or [])],
        )

    rank_index = {market: idx for idx, market in enumerate(ranked_markets)}
    recent_ordered = sorted(
        dedup_upper(union_recent_active),
        key=lambda m: (rank_index.get(m, 10**9), m),
    )

    maintenance_markets = ordered_union(recent_ordered, buffer_top)
    final_1m_markets = ordered_union(active_markets, maintenance_markets)

    ticker_ts_ms = max(int(row.get("timestamp", 0) or 0) for row in ticker_rows)
    ticker_ts = (
        datetime.fromtimestamp(float(ticker_ts_ms) / 1000.0, tz=timezone.utc).isoformat()
        if ticker_ts_ms > 0
        else ""
    )
    snapshot_sha256 = build_snapshot_sha256(markets, ticker_rows)

    universe_payload = {
        "generated_at": utc_now_iso(),
        "market_prefix": str(args.market_prefix).upper(),
        "active_markets": active_markets,
        "maintenance_markets": maintenance_markets,
        "final_1m_markets": final_1m_markets,
        "params": {
            "active_size": int(active_size),
            "buffer_size": int(buffer_size),
            "membership_ttl_days": int(membership_ttl_days),
        },
        "history_window": {
            "records_kept": int(len(history_records)),
            "from_date_utc": history_records[0]["date_utc"] if history_records else "",
            "to_date_utc": history_records[-1]["date_utc"] if history_records else "",
        },
        "source_snapshot": {
            "ticker_ts": ticker_ts,
            "sha256": snapshot_sha256,
            "markets_count": int(len(markets)),
            "ticker_rows_count": int(len(ticker_rows)),
            "remaining_req_samples": remaining_samples,
        },
    }
    dump_json(output_json, universe_payload)
    dump_json(
        history_json,
        {
            "generated_at": utc_now_iso(),
            "records": history_records,
        },
    )

    print("[BuildDynamicUniverse] Completed", flush=True)
    print(f"output={output_json}", flush=True)
    print(f"history={history_json}", flush=True)
    print(f"active_count={len(active_markets)}", flush=True)
    print(f"maintenance_count={len(maintenance_markets)}", flush=True)
    print(f"final_1m_count={len(final_1m_markets)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
