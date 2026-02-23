#!/usr/bin/env python3
import argparse
import csv
import math
import pathlib
import re
from collections import deque
from datetime import datetime, timezone
from typing import Dict, List, Tuple

from _script_common import dump_json, ensure_parent_directory, resolve_repo_path


ANCHOR_TF = 1
CONTEXT_TFS = (5, 15, 60, 240)
MARKET_FILE_RE = re.compile(r"^upbit_([A-Z0-9_]+)_1m_full\.csv$")


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build leakage-safe probabilistic feature dataset from fetched MTF OHLCV CSV files."
    )
    parser.add_argument("--input-dir", default=r".\data\backtest_probabilistic")
    parser.add_argument("--output-dir", default=r".\data\model_input\probabilistic_features_v1")
    parser.add_argument(
        "--summary-json",
        default=r".\build\Release\logs\probabilistic_feature_build_summary.json",
    )
    parser.add_argument(
        "--manifest-json",
        default=r".\data\model_input\probabilistic_features_v1\feature_dataset_manifest.json",
    )
    parser.add_argument("--markets", default="")
    parser.add_argument("--max-rows-per-market", type=int, default=0)
    parser.add_argument("--skip-existing", action="store_true")
    parser.add_argument("--roundtrip-cost-bps", type=float, default=12.0)
    parser.add_argument("--label-h1", type=int, default=1)
    parser.add_argument("--label-h5", type=int, default=5)
    parser.add_argument("--enable-triple-barrier-labels", action="store_true")
    parser.add_argument("--triple-barrier-horizon", type=int, default=30)
    parser.add_argument("--triple-barrier-take-profit-bps", type=float, default=45.0)
    parser.add_argument("--triple-barrier-stop-loss-bps", type=float, default=35.0)
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_markets(raw: str) -> List[str]:
    out: List[str] = []
    seen = set()
    for token in str(raw or "").split(","):
        market = token.strip().upper()
        if not market:
            continue
        if market in seen:
            continue
        seen.add(market)
        out.append(market)
    return out


def market_to_safe(market: str) -> str:
    return market.replace("-", "_")


def safe_to_market(safe: str) -> str:
    return safe.replace("_", "-")


def discover_markets(input_dir: pathlib.Path) -> List[str]:
    markets: List[str] = []
    seen = set()
    for path in sorted(input_dir.glob("upbit_*_1m_full.csv")):
        match = MARKET_FILE_RE.match(path.name)
        if not match:
            continue
        market = safe_to_market(match.group(1))
        if market in seen:
            continue
        seen.add(market)
        markets.append(market)
    return markets


def load_candles(path_value: pathlib.Path) -> Dict[str, List[float]]:
    if not path_value.exists():
        raise FileNotFoundError(f"missing candle file: {path_value}")

    ts: List[int] = []
    opens: List[float] = []
    highs: List[float] = []
    lows: List[float] = []
    closes: List[float] = []
    volumes: List[float] = []
    with path_value.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            try:
                ts_v = int(float(row.get("timestamp", 0)))
                o_v = float(row.get("open", 0.0))
                h_v = float(row.get("high", 0.0))
                l_v = float(row.get("low", 0.0))
                c_v = float(row.get("close", 0.0))
                v_v = float(row.get("volume", 0.0))
            except Exception:
                continue
            ts.append(ts_v)
            opens.append(o_v)
            highs.append(h_v)
            lows.append(l_v)
            closes.append(c_v)
            volumes.append(v_v)

    if not ts:
        raise RuntimeError(f"no rows loaded: {path_value}")

    # Keep monotonic ascending order.
    if any(ts[i] <= ts[i - 1] for i in range(1, len(ts))):
        idx = list(range(len(ts)))
        idx.sort(key=lambda i: ts[i])
        ts = [ts[i] for i in idx]
        opens = [opens[i] for i in idx]
        highs = [highs[i] for i in idx]
        lows = [lows[i] for i in idx]
        closes = [closes[i] for i in idx]
        volumes = [volumes[i] for i in idx]

    return {
        "timestamp": ts,
        "open": opens,
        "high": highs,
        "low": lows,
        "close": closes,
        "volume": volumes,
    }


def compute_ema(values: List[float], period: int) -> List[float]:
    out = [math.nan] * len(values)
    if not values:
        return out
    alpha = 2.0 / (float(period) + 1.0)
    ema = float(values[0])
    out[0] = ema
    for i in range(1, len(values)):
        ema = (alpha * float(values[i])) + ((1.0 - alpha) * ema)
        out[i] = ema
    return out


def compute_rsi(values: List[float], period: int = 14) -> List[float]:
    out = [math.nan] * len(values)
    if len(values) <= period:
        return out

    gains = [0.0] * len(values)
    losses = [0.0] * len(values)
    for i in range(1, len(values)):
        delta = float(values[i]) - float(values[i - 1])
        gains[i] = max(0.0, delta)
        losses[i] = max(0.0, -delta)

    avg_gain = sum(gains[1 : period + 1]) / float(period)
    avg_loss = sum(losses[1 : period + 1]) / float(period)
    if avg_loss == 0.0:
        out[period] = 100.0
    else:
        rs = avg_gain / avg_loss
        out[period] = 100.0 - (100.0 / (1.0 + rs))

    for i in range(period + 1, len(values)):
        avg_gain = ((avg_gain * (period - 1)) + gains[i]) / float(period)
        avg_loss = ((avg_loss * (period - 1)) + losses[i]) / float(period)
        if avg_loss == 0.0:
            out[i] = 100.0
        else:
            rs = avg_gain / avg_loss
            out[i] = 100.0 - (100.0 / (1.0 + rs))
    return out


def compute_atr(highs: List[float], lows: List[float], closes: List[float], period: int = 14) -> List[float]:
    out = [math.nan] * len(closes)
    if len(closes) <= period:
        return out
    tr = [0.0] * len(closes)
    for i in range(1, len(closes)):
        h = float(highs[i])
        l = float(lows[i])
        prev_c = float(closes[i - 1])
        tr[i] = max(h - l, abs(h - prev_c), abs(l - prev_c))

    atr = sum(tr[1 : period + 1]) / float(period)
    out[period] = atr
    for i in range(period + 1, len(closes)):
        atr = ((atr * (period - 1)) + tr[i]) / float(period)
        out[i] = atr
    return out


def safe_ret(values: List[float], idx_now: int, idx_prev: int) -> float:
    if idx_prev < 0 or idx_now < 0 or idx_now >= len(values) or idx_prev >= len(values):
        return math.nan
    base = float(values[idx_prev])
    cur = float(values[idx_now])
    if base <= 0.0:
        return math.nan
    return (cur / base) - 1.0


def compute_triple_barrier_label(
    *,
    entry_price: float,
    highs: List[float],
    lows: List[float],
    closes: List[float],
    start_idx: int,
    horizon_bars: int,
    take_profit_bps: float,
    stop_loss_bps: float,
) -> Tuple[int, int, float, str]:
    if entry_price <= 0.0 or horizon_bars <= 0:
        return 0, 0, 0.0, "disabled"
    if start_idx < 0 or start_idx >= len(closes):
        return 0, 0, 0.0, "invalid_index"

    up_px = entry_price * (1.0 + (float(take_profit_bps) / 10000.0))
    down_px = entry_price * (1.0 - (float(stop_loss_bps) / 10000.0))
    end_idx = min(len(closes) - 1, int(start_idx + horizon_bars))

    for idx in range(start_idx + 1, end_idx + 1):
        high_v = float(highs[idx])
        low_v = float(lows[idx])
        hit_up = high_v >= up_px
        hit_down = low_v <= down_px
        if hit_up and hit_down:
            ret_bps = ((float(closes[idx]) / entry_price) - 1.0) * 10000.0
            return 0, int(idx - start_idx), float(ret_bps), "ambiguous"
        if hit_up:
            return 1, int(idx - start_idx), float(take_profit_bps), "tp_hit"
        if hit_down:
            return -1, int(idx - start_idx), float(-abs(stop_loss_bps)), "sl_hit"

    close_end = float(closes[end_idx])
    ret_bps = ((close_end / entry_price) - 1.0) * 10000.0 if entry_price > 0.0 else 0.0
    direction = 1 if ret_bps > 0.0 else (-1 if ret_bps < 0.0 else 0)
    return int(direction), int(end_idx - start_idx), float(ret_bps), "timeout"


def sign3(value: float) -> int:
    if not math.isfinite(value):
        return 0
    if value > 0.0:
        return 1
    if value < 0.0:
        return -1
    return 0


def to_iso(ts_ms: int) -> str:
    return datetime.fromtimestamp(float(ts_ms) / 1000.0, tz=timezone.utc).isoformat()


def round6(value: float) -> float:
    if not math.isfinite(value):
        return math.nan
    return round(float(value), 6)


def count_csv_data_rows(path_value: pathlib.Path) -> int:
    if not path_value.exists():
        return 0
    count = 0
    with path_value.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
        next(fh, None)  # header
        for _ in fh:
            count += 1
    return count


def build_market_dataset(
    *,
    market: str,
    input_dir: pathlib.Path,
    output_dir: pathlib.Path,
    max_rows_per_market: int,
    label_h1: int,
    label_h5: int,
    enable_triple_barrier_labels: bool,
    triple_barrier_horizon: int,
    triple_barrier_take_profit_bps: float,
    triple_barrier_stop_loss_bps: float,
    roundtrip_cost_bps: float,
    skip_existing: bool,
) -> Dict[str, object]:
    safe_market = market_to_safe(market)
    anchor_path = input_dir / f"upbit_{safe_market}_1m_full.csv"
    context_paths = {
        tf: input_dir / f"upbit_{safe_market}_{tf}m_full.csv"
        for tf in CONTEXT_TFS
    }

    anchor = load_candles(anchor_path)
    contexts = {tf: load_candles(path) for tf, path in context_paths.items()}

    anchor_close = anchor["close"]
    anchor_volume = anchor["volume"]
    anchor_high = anchor["high"]
    anchor_low = anchor["low"]
    anchor_ts = anchor["timestamp"]

    ema12 = compute_ema(anchor_close, 12)
    ema26 = compute_ema(anchor_close, 26)
    rsi14 = compute_rsi(anchor_close, 14)
    atr14 = compute_atr(anchor_high, anchor_low, anchor_close, 14)

    ctx_ind = {}
    for tf in CONTEXT_TFS:
        c = contexts[tf]
        ctx_ind[tf] = {
            "close": c["close"],
            "timestamp": c["timestamp"],
            "ema20": compute_ema(c["close"], 20),
            "rsi14": compute_rsi(c["close"], 14),
            "atr14": compute_atr(c["high"], c["low"], c["close"], 14),
        }

    output_dir.mkdir(parents=True, exist_ok=True)
    out_path = output_dir / f"prob_features_{safe_market}_1m_v1.csv"
    ensure_parent_directory(out_path)
    if bool(skip_existing) and out_path.exists() and out_path.stat().st_size > 0:
        existing_rows = count_csv_data_rows(out_path)
        return {
            "market": market,
            "status": "skipped_existing",
            "anchor_input_path": str(anchor_path),
            "output_path": str(out_path),
            "anchor_rows": len(anchor_ts),
            "feature_rows_written": int(existing_rows),
            "skipped_warmup": 0,
            "skipped_missing_context": 0,
            "from_utc": to_iso(int(anchor_ts[0])),
            "to_utc": to_iso(int(anchor_ts[-1])),
            "output_size_bytes": out_path.stat().st_size,
        }

    fieldnames = [
        "timestamp",
        "timestamp_utc",
        "market",
        "close",
        "ret_1m",
        "ret_5m",
        "ret_20m",
        "ema_gap_12_26",
        "rsi_14",
        "atr_pct_14",
        "bb_width_20",
        "vol_ratio_20",
        "notional_ratio_20",
    ]
    for tf in CONTEXT_TFS:
        fieldnames.extend(
            [
                f"ctx_{tf}m_age_min",
                f"ctx_{tf}m_ret_3",
                f"ctx_{tf}m_ret_12",
                f"ctx_{tf}m_ema_gap_20",
                f"ctx_{tf}m_rsi_14",
                f"ctx_{tf}m_atr_pct_14",
            ]
        )
    fieldnames.extend(
        [
            "regime_trend_60_sign",
            "regime_trend_240_sign",
            "regime_vol_60_atr_pct",
            "label_up_h1",
            "label_up_h5",
            "label_edge_bps_h5",
        ]
    )
    if bool(enable_triple_barrier_labels):
        fieldnames.extend(
            [
                "label_tb_dir",
                "label_tb_hit_bars",
                "label_tb_exit_bps",
                "label_tb_event",
            ]
        )

    max_h = max(
        int(label_h1),
        int(label_h5),
        int(triple_barrier_horizon) if bool(enable_triple_barrier_labels) else 0,
    )
    written = 0
    skipped_warmup = 0
    skipped_missing_ctx = 0
    first_written_ts = 0
    last_written_ts = 0

    tf_ptrs = {tf: -1 for tf in CONTEXT_TFS}
    close_win = deque()
    close_sum = 0.0
    close_sq_sum = 0.0
    vol_win = deque()
    vol_sum = 0.0
    notional_win = deque()
    notional_sum = 0.0

    tmp_out_path = out_path.with_suffix(out_path.suffix + ".tmp")
    if tmp_out_path.exists():
        tmp_out_path.unlink(missing_ok=True)

    with tmp_out_path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()

        for i in range(len(anchor_ts)):
            t = int(anchor_ts[i])
            c = float(anchor_close[i])
            v = float(anchor_volume[i])
            notional = c * v

            close_win.append(c)
            close_sum += c
            close_sq_sum += (c * c)
            if len(close_win) > 20:
                old = close_win.popleft()
                close_sum -= old
                close_sq_sum -= (old * old)

            vol_win.append(v)
            vol_sum += v
            if len(vol_win) > 20:
                vol_sum -= vol_win.popleft()

            notional_win.append(notional)
            notional_sum += notional
            if len(notional_win) > 20:
                notional_sum -= notional_win.popleft()

            if i + max_h >= len(anchor_ts):
                break

            if i < 30 or len(close_win) < 20:
                skipped_warmup += 1
                continue
            if not (math.isfinite(rsi14[i]) and math.isfinite(atr14[i]) and math.isfinite(ema12[i]) and math.isfinite(ema26[i])):
                skipped_warmup += 1
                continue

            ctx_values: Dict[int, Dict[str, float]] = {}
            missing_ctx = False
            for tf in CONTEXT_TFS:
                ts_arr = ctx_ind[tf]["timestamp"]
                ptr = tf_ptrs[tf]
                while (ptr + 1) < len(ts_arr) and int(ts_arr[ptr + 1]) <= t:
                    ptr += 1
                tf_ptrs[tf] = ptr
                if ptr < 0 or ptr < 20:
                    missing_ctx = True
                    break

                ts_tf = int(ts_arr[ptr])
                close_tf = float(ctx_ind[tf]["close"][ptr])
                ema20_tf = float(ctx_ind[tf]["ema20"][ptr])
                rsi_tf = float(ctx_ind[tf]["rsi14"][ptr])
                atr_tf = float(ctx_ind[tf]["atr14"][ptr])
                if not (math.isfinite(close_tf) and close_tf > 0.0 and math.isfinite(ema20_tf) and math.isfinite(rsi_tf) and math.isfinite(atr_tf)):
                    missing_ctx = True
                    break

                ctx_values[tf] = {
                    "age_min": max(0.0, (float(t) - float(ts_tf)) / 60000.0),
                    "ret_3": safe_ret(ctx_ind[tf]["close"], ptr, ptr - 3),
                    "ret_12": safe_ret(ctx_ind[tf]["close"], ptr, ptr - 12),
                    "ema_gap_20": (close_tf - ema20_tf) / close_tf,
                    "rsi_14": rsi_tf,
                    "atr_pct_14": atr_tf / close_tf,
                }

            if missing_ctx:
                skipped_missing_ctx += 1
                continue

            mean20_close = close_sum / 20.0
            var20_close = max(0.0, (close_sq_sum / 20.0) - (mean20_close * mean20_close))
            std20_close = math.sqrt(var20_close)
            bb_width_20 = (4.0 * std20_close / c) if c > 0.0 else math.nan

            mean20_vol = vol_sum / 20.0 if len(vol_win) == 20 else math.nan
            mean20_notional = notional_sum / 20.0 if len(notional_win) == 20 else math.nan
            vol_ratio_20 = (v / mean20_vol) if (math.isfinite(mean20_vol) and mean20_vol > 0.0) else math.nan
            notional_ratio_20 = (notional / mean20_notional) if (math.isfinite(mean20_notional) and mean20_notional > 0.0) else math.nan

            row = {
                "timestamp": t,
                "timestamp_utc": to_iso(t),
                "market": market,
                "close": round6(c),
                "ret_1m": round6(safe_ret(anchor_close, i, i - 1)),
                "ret_5m": round6(safe_ret(anchor_close, i, i - 5)),
                "ret_20m": round6(safe_ret(anchor_close, i, i - 20)),
                "ema_gap_12_26": round6((float(ema12[i]) - float(ema26[i])) / c if c > 0 else math.nan),
                "rsi_14": round6(float(rsi14[i])),
                "atr_pct_14": round6(float(atr14[i]) / c if c > 0 else math.nan),
                "bb_width_20": round6(bb_width_20),
                "vol_ratio_20": round6(vol_ratio_20),
                "notional_ratio_20": round6(notional_ratio_20),
            }

            for tf in CONTEXT_TFS:
                vtf = ctx_values[tf]
                row[f"ctx_{tf}m_age_min"] = round6(vtf["age_min"])
                row[f"ctx_{tf}m_ret_3"] = round6(vtf["ret_3"])
                row[f"ctx_{tf}m_ret_12"] = round6(vtf["ret_12"])
                row[f"ctx_{tf}m_ema_gap_20"] = round6(vtf["ema_gap_20"])
                row[f"ctx_{tf}m_rsi_14"] = round6(vtf["rsi_14"])
                row[f"ctx_{tf}m_atr_pct_14"] = round6(vtf["atr_pct_14"])

            regime_ret_60 = ctx_values[60]["ret_12"]
            regime_ret_240 = ctx_values[240]["ret_12"]
            row["regime_trend_60_sign"] = sign3(regime_ret_60)
            row["regime_trend_240_sign"] = sign3(regime_ret_240)
            row["regime_vol_60_atr_pct"] = round6(ctx_values[60]["atr_pct_14"])

            close_h1 = float(anchor_close[i + int(label_h1)])
            close_h5 = float(anchor_close[i + int(label_h5)])
            row["label_up_h1"] = 1 if close_h1 > c else 0
            row["label_up_h5"] = 1 if close_h5 > c else 0
            gross_bps_h5 = ((close_h5 / c) - 1.0) * 10000.0 if c > 0.0 else math.nan
            row["label_edge_bps_h5"] = round6(gross_bps_h5 - float(roundtrip_cost_bps))
            if bool(enable_triple_barrier_labels):
                tb_dir, tb_hit_bars, tb_exit_bps, tb_event = compute_triple_barrier_label(
                    entry_price=c,
                    highs=anchor_high,
                    lows=anchor_low,
                    closes=anchor_close,
                    start_idx=i,
                    horizon_bars=int(triple_barrier_horizon),
                    take_profit_bps=float(triple_barrier_take_profit_bps),
                    stop_loss_bps=float(triple_barrier_stop_loss_bps),
                )
                row["label_tb_dir"] = int(tb_dir)
                row["label_tb_hit_bars"] = int(tb_hit_bars)
                row["label_tb_exit_bps"] = round6(tb_exit_bps - float(roundtrip_cost_bps))
                row["label_tb_event"] = str(tb_event)

            writer.writerow(row)
            written += 1
            if first_written_ts <= 0:
                first_written_ts = t
            last_written_ts = t
            if int(max_rows_per_market) > 0 and written >= int(max_rows_per_market):
                break

    tmp_out_path.replace(out_path)

    from_ts = int(first_written_ts if first_written_ts > 0 else anchor_ts[0])
    to_ts = int(last_written_ts if last_written_ts > 0 else anchor_ts[-1])
    return {
        "market": market,
        "status": "built",
        "anchor_input_path": str(anchor_path),
        "output_path": str(out_path),
        "anchor_rows": len(anchor_ts),
        "feature_rows_written": int(written),
        "skipped_warmup": int(skipped_warmup),
        "skipped_missing_context": int(skipped_missing_ctx),
        "from_utc": to_iso(from_ts),
        "to_utc": to_iso(to_ts),
        "output_size_bytes": out_path.stat().st_size if out_path.exists() else 0,
    }


def main(argv=None) -> int:
    args = parse_args(argv)
    input_dir = resolve_repo_path(args.input_dir)
    output_dir = resolve_repo_path(args.output_dir)
    summary_json = resolve_repo_path(args.summary_json)
    manifest_json = resolve_repo_path(args.manifest_json)

    if not input_dir.exists():
        raise FileNotFoundError(f"input dir not found: {input_dir}")

    requested_markets = parse_markets(args.markets)
    if requested_markets:
        markets = requested_markets
    else:
        markets = discover_markets(input_dir)
    if not markets:
        raise RuntimeError("no markets found for 1m anchor files")

    started = utc_now_iso()
    jobs: List[Dict[str, object]] = []
    failed: List[Dict[str, object]] = []

    for market in markets:
        try:
            print(f"[BuildProbFeatures] start market={market}", flush=True)
            job = build_market_dataset(
                market=market,
                input_dir=input_dir,
                output_dir=output_dir,
                max_rows_per_market=int(args.max_rows_per_market),
                label_h1=int(args.label_h1),
                label_h5=int(args.label_h5),
                enable_triple_barrier_labels=bool(args.enable_triple_barrier_labels),
                triple_barrier_horizon=int(args.triple_barrier_horizon),
                triple_barrier_take_profit_bps=float(args.triple_barrier_take_profit_bps),
                triple_barrier_stop_loss_bps=float(args.triple_barrier_stop_loss_bps),
                roundtrip_cost_bps=float(args.roundtrip_cost_bps),
                skip_existing=bool(args.skip_existing),
            )
            jobs.append(job)
            status = str(job.get("status", "built"))
            if status == "skipped_existing":
                print(
                    f"[BuildProbFeatures] skipped_existing market={market} "
                    f"rows={job['feature_rows_written']} size={job['output_size_bytes']}B",
                    flush=True,
                )
            else:
                print(
                    f"[BuildProbFeatures] done market={market} "
                    f"rows={job['feature_rows_written']} size={job['output_size_bytes']}B",
                    flush=True,
                )
        except Exception as exc:
            failed.append({"market": market, "error": str(exc)})
            print(f"[BuildProbFeatures] failed market={market} error={exc}", flush=True)

    total_rows = sum(int(x.get("feature_rows_written", 0)) for x in jobs)
    total_bytes = sum(int(x.get("output_size_bytes", 0)) for x in jobs)
    finished = utc_now_iso()
    payload = {
        "version": "prob_features_v1",
        "started_at_utc": started,
        "finished_at_utc": finished,
        "input_dir": str(input_dir),
        "output_dir": str(output_dir),
        "anchor_tf": "1m",
        "context_tfs": [int(x) for x in CONTEXT_TFS],
        "markets": markets,
        "roundtrip_cost_bps": float(args.roundtrip_cost_bps),
        "label_h1": int(args.label_h1),
        "label_h5": int(args.label_h5),
        "enable_triple_barrier_labels": bool(args.enable_triple_barrier_labels),
        "triple_barrier_horizon": int(args.triple_barrier_horizon),
        "triple_barrier_take_profit_bps": float(args.triple_barrier_take_profit_bps),
        "triple_barrier_stop_loss_bps": float(args.triple_barrier_stop_loss_bps),
        "max_rows_per_market": int(args.max_rows_per_market),
        "job_count": len(markets),
        "success_count": len(jobs),
        "failed_count": len(failed),
        "total_feature_rows": int(total_rows),
        "total_output_bytes": int(total_bytes),
        "jobs": jobs,
        "failed": failed,
    }
    dump_json(summary_json, payload)
    dump_json(manifest_json, payload)

    print("[BuildProbFeatures] Completed", flush=True)
    print(f"summary={summary_json}", flush=True)
    print(f"manifest={manifest_json}", flush=True)
    print(f"success_count={len(jobs)}", flush=True)
    print(f"failed_count={len(failed)}", flush=True)
    print(f"total_feature_rows={total_rows}", flush=True)
    print(f"total_output_bytes={total_bytes}", flush=True)
    return 0 if not failed else 2


if __name__ == "__main__":
    raise SystemExit(main())
