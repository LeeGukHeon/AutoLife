#!/usr/bin/env python3
import argparse
import csv
import math
import pathlib
import re
from collections import deque
from datetime import datetime, timezone
from typing import Any, Dict, List, Tuple

from _script_common import dump_json, ensure_parent_directory, load_json_or_none, resolve_repo_path
from probabilistic_cost_model import DEFAULT_COST_MODEL_CONFIG, resolve_label_cost_bps


ANCHOR_TF = 1
CONTEXT_TFS = (5, 15, 60, 240)
MARKET_FILE_RE = re.compile(r"^upbit_([A-Z0-9_]+)_1m_full\.csv$")
DEFAULT_LABEL_TP1_POLICY = {
    "risk_pct_policy": {
        "RANGING": {"mult": 1.15, "floor_pct": 0.35, "cap_pct": 1.40},
        "TRENDING_UP": {"mult": 1.15, "floor_pct": 0.35, "cap_pct": 1.40},
        "TRENDING_DOWN": {"mult": 1.15, "floor_pct": 0.35, "cap_pct": 1.40},
        "DEFAULT": {"mult": 1.15, "floor_pct": 0.35, "cap_pct": 1.40},
    },
    "tp1_rr": 0.50,
    "horizon_bars": 20,
    "label_cost_bps": 12.0,
}


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build leakage-safe probabilistic feature dataset from fetched MTF OHLCV CSV files."
    )
    parser.add_argument("--input-dir", default=r".\data\backtest_probabilistic")
    parser.add_argument("--output-dir", default=r".\data\model_input\probabilistic_features_v2_draft_latest")
    parser.add_argument(
        "--summary-json",
        default=r".\build\Release\logs\probabilistic_feature_build_summary.json",
    )
    parser.add_argument(
        "--manifest-json",
        default=r".\data\model_input\probabilistic_features_v2_draft_latest\feature_dataset_manifest.json",
    )
    parser.add_argument("--markets", default="")
    parser.add_argument("--max-rows-per-market", type=int, default=0)
    parser.add_argument("--skip-existing", action="store_true")
    parser.add_argument("--roundtrip-cost-bps", type=float, default=12.0)
    parser.add_argument("--enable-conditional-cost-model", action="store_true")
    parser.add_argument("--cost-fee-floor-bps", type=float, default=float(DEFAULT_COST_MODEL_CONFIG["fee_floor_bps"]))
    parser.add_argument(
        "--cost-volatility-weight",
        type=float,
        default=float(DEFAULT_COST_MODEL_CONFIG["volatility_weight"]),
    )
    parser.add_argument(
        "--cost-range-weight",
        type=float,
        default=float(DEFAULT_COST_MODEL_CONFIG["range_weight"]),
    )
    parser.add_argument(
        "--cost-liquidity-weight",
        type=float,
        default=float(DEFAULT_COST_MODEL_CONFIG["liquidity_weight"]),
    )
    parser.add_argument(
        "--cost-volatility-norm-bps",
        type=float,
        default=float(DEFAULT_COST_MODEL_CONFIG["volatility_norm_bps"]),
    )
    parser.add_argument(
        "--cost-range-norm-bps",
        type=float,
        default=float(DEFAULT_COST_MODEL_CONFIG["range_norm_bps"]),
    )
    parser.add_argument(
        "--cost-liquidity-ref-ratio",
        type=float,
        default=float(DEFAULT_COST_MODEL_CONFIG["liquidity_ref_ratio"]),
    )
    parser.add_argument(
        "--cost-liquidity-penalty-cap",
        type=float,
        default=float(DEFAULT_COST_MODEL_CONFIG["liquidity_penalty_cap"]),
    )
    parser.add_argument(
        "--cost-cap-bps",
        type=float,
        default=float(DEFAULT_COST_MODEL_CONFIG["cost_cap_bps"]),
    )
    parser.add_argument("--label-h1", type=int, default=1)
    parser.add_argument("--label-h5", type=int, default=5)
    parser.add_argument("--enable-triple-barrier-labels", action="store_true")
    parser.add_argument("--triple-barrier-horizon", type=int, default=30)
    parser.add_argument("--triple-barrier-take-profit-bps", type=float, default=45.0)
    parser.add_argument("--triple-barrier-stop-loss-bps", type=float, default=35.0)
    parser.add_argument(
        "--sample-mode",
        "--sample_mode",
        choices=("time", "dollar", "volatility"),
        default="time",
        help="EXT-52 optional: training row sampling mode (default time = baseline).",
    )
    parser.add_argument(
        "--sample-threshold",
        "--sample_threshold",
        type=float,
        default=0.0,
        help="EXT-52 optional: threshold used by dollar/volatility sampling modes.",
    )
    parser.add_argument(
        "--sample-lookback-minutes",
        "--sample_lookback_minutes",
        type=int,
        default=60,
        help="EXT-52 optional: rolling lookback window for sampling metrics.",
    )
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("v2",),
        default="v2",
        help="MODE switch. v2 only.",
    )
    parser.add_argument(
        "--universe-file",
        default="",
        help="Optional runtime universe JSON. Applies strict/skip rules for missing 1m anchors.",
    )
    parser.add_argument(
        "--label-policy-json",
        default=r".\scripts\label_policies\label_tp1_policy.json",
        help="Stage G2 label policy json (TP1 hit label semantics).",
    )
    parser.add_argument(
        "--stageg2-label-report-json",
        default=r".\build\Release\logs\stageG2_label_report.json",
        help="Optional Stage G2 label diagnostics output path.",
    )
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


def merge_markets(primary: List[str], secondary: List[str]) -> List[str]:
    out: List[str] = []
    seen = set()
    for token in primary + secondary:
        market = str(token or "").strip().upper()
        if not market or market in seen:
            continue
        seen.add(market)
        out.append(market)
    return out


def load_universe_final_1m_markets(universe_path: pathlib.Path) -> List[str]:
    payload = load_json_or_none(universe_path)
    if not isinstance(payload, dict):
        raise RuntimeError(f"invalid universe file json: {universe_path}")
    raw_markets = payload.get("final_1m_markets", [])
    if not isinstance(raw_markets, list):
        raise RuntimeError(f"invalid final_1m_markets in universe file: {universe_path}")
    markets = merge_markets([], [str(x) for x in raw_markets])
    if not markets:
        raise RuntimeError(f"empty final_1m_markets in universe file: {universe_path}")
    return markets


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


def normalize_regime(sign60: int, sign240: int) -> str:
    if int(sign60) > 0 and int(sign240) > 0:
        return "TRENDING_UP"
    if int(sign60) < 0 and int(sign240) < 0:
        return "TRENDING_DOWN"
    return "RANGING"


def _safe_policy_entry(raw: object, default: Dict[str, float]) -> Dict[str, float]:
    entry = raw if isinstance(raw, dict) else {}
    return {
        "mult": float(entry.get("mult", default["mult"])),
        "floor_pct": float(entry.get("floor_pct", default["floor_pct"])),
        "cap_pct": float(entry.get("cap_pct", default["cap_pct"])),
    }


def load_label_tp1_policy(policy_path: pathlib.Path) -> Dict[str, object]:
    payload = load_json_or_none(policy_path)
    if not isinstance(payload, dict):
        payload = {}
    default = DEFAULT_LABEL_TP1_POLICY
    default_risk = dict(default["risk_pct_policy"]["DEFAULT"])
    payload_risk = payload.get("risk_pct_policy", {})
    if not isinstance(payload_risk, dict):
        payload_risk = {}
    policy = {
        "risk_pct_policy": {
            "RANGING": _safe_policy_entry(payload_risk.get("RANGING"), default_risk),
            "TRENDING_UP": _safe_policy_entry(payload_risk.get("TRENDING_UP"), default_risk),
            "TRENDING_DOWN": _safe_policy_entry(payload_risk.get("TRENDING_DOWN"), default_risk),
            "DEFAULT": _safe_policy_entry(payload_risk.get("DEFAULT"), default_risk),
        },
        "tp1_rr": float(payload.get("tp1_rr", default["tp1_rr"])),
        "horizon_bars": max(1, int(payload.get("horizon_bars", default["horizon_bars"]))),
        "label_cost_bps": float(payload.get("label_cost_bps", default["label_cost_bps"])),
        "policy_source_json": str(policy_path),
        "policy_source_exists": bool(policy_path.exists()),
    }
    return policy


def compute_tp1_risk_and_target(
    *,
    atr_pct_fraction: float,
    entry_price: float,
    regime_name: str,
    label_tp1_policy: Dict[str, object],
) -> Tuple[float, float, float]:
    risk_table = label_tp1_policy.get("risk_pct_policy", {})
    if not isinstance(risk_table, dict):
        risk_table = {}
    default_cfg = risk_table.get("DEFAULT", DEFAULT_LABEL_TP1_POLICY["risk_pct_policy"]["DEFAULT"])
    if not isinstance(default_cfg, dict):
        default_cfg = DEFAULT_LABEL_TP1_POLICY["risk_pct_policy"]["DEFAULT"]
    cfg = risk_table.get(str(regime_name), default_cfg)
    if not isinstance(cfg, dict):
        cfg = default_cfg

    atr_pct_percent = float(atr_pct_fraction) * 100.0
    mult = float(cfg.get("mult", default_cfg.get("mult", 1.15)))
    floor_pct = float(cfg.get("floor_pct", default_cfg.get("floor_pct", 0.35)))
    cap_pct = float(cfg.get("cap_pct", default_cfg.get("cap_pct", 1.40)))
    risk_pct_raw = atr_pct_percent * mult
    risk_pct = min(cap_pct, max(floor_pct, risk_pct_raw))
    tp1_rr = float(label_tp1_policy.get("tp1_rr", DEFAULT_LABEL_TP1_POLICY["tp1_rr"]))
    tp1_pct = (risk_pct / 100.0) * tp1_rr
    target_price = float(entry_price) * (1.0 + tp1_pct)
    return float(risk_pct), float(tp1_pct), float(target_price)


def build_stageg2_label_report(
    *,
    manifest_payload: Dict[str, object],
    report_json_path: pathlib.Path,
) -> None:
    jobs = manifest_payload.get("jobs", [])
    if not isinstance(jobs, list):
        jobs = []
    dropped_due_to_future_window = 0
    csv_paths: List[pathlib.Path] = []
    for job in jobs:
        if not isinstance(job, dict):
            continue
        out_path = str(job.get("output_path", "")).strip()
        if out_path:
            csv_paths.append(pathlib.Path(out_path))
        dropped_due_to_future_window += int(job.get("dropped_due_to_future_window", 0) or 0)

    total_rows = 0
    usable_rows = 0
    positive_rows = 0
    risk_values: List[float] = []
    tp1_values: List[float] = []
    by_market: Dict[str, Dict[str, int]] = {}
    by_regime: Dict[str, Dict[str, int]] = {}
    for csv_path in csv_paths:
        if not csv_path.exists():
            continue
        with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                total_rows += 1
                market = str(row.get("market", "")).strip() or "UNKNOWN"
                label_raw = str(row.get("label_tp1_hit_h20m", "")).strip()
                if not label_raw:
                    # Backward-compat for pre-rename datasets.
                    label_raw = str(row.get("label_tp1_hit_h5m", "")).strip()
                risk_raw = str(row.get("label_tp1_risk_pct", "")).strip()
                tp1_raw = str(row.get("label_tp1_pct", "")).strip()
                try:
                    sign60_raw = float(row.get("regime_trend_60_sign", 0) or 0)
                except Exception:
                    sign60_raw = 0.0
                try:
                    sign240_raw = float(row.get("regime_trend_240_sign", 0) or 0)
                except Exception:
                    sign240_raw = 0.0
                sign60 = sign3(sign60_raw)
                sign240 = sign3(sign240_raw)
                regime = normalize_regime(sign60, sign240)
                try:
                    label_value = int(float(label_raw))
                except Exception:
                    continue
                if label_value not in (0, 1):
                    continue
                usable_rows += 1
                if label_value == 1:
                    positive_rows += 1
                market_stat = by_market.setdefault(market, {"total": 0, "positive": 0})
                market_stat["total"] += 1
                market_stat["positive"] += int(label_value == 1)
                regime_stat = by_regime.setdefault(regime, {"total": 0, "positive": 0})
                regime_stat["total"] += 1
                regime_stat["positive"] += int(label_value == 1)
                try:
                    rv = float(risk_raw)
                    if math.isfinite(rv):
                        risk_values.append(rv)
                except Exception:
                    pass
                try:
                    tv = float(tp1_raw)
                    if math.isfinite(tv):
                        tp1_values.append(tv)
                except Exception:
                    pass

    def _quantiles(values: List[float]) -> Dict[str, float]:
        if not values:
            return {"p10": math.nan, "p50": math.nan, "p90": math.nan, "mean": math.nan}
        arr = sorted(float(v) for v in values)
        n = len(arr)
        def _at(q: float) -> float:
            idx = int(round((n - 1) * q))
            idx = max(0, min(n - 1, idx))
            return float(arr[idx])
        return {
            "p10": _at(0.10),
            "p50": _at(0.50),
            "p90": _at(0.90),
            "mean": float(sum(arr) / float(n)),
        }

    by_market_rows = []
    for market, stat in sorted(by_market.items()):
        total = int(stat["total"])
        positive = int(stat["positive"])
        by_market_rows.append(
            {
                "market": market,
                "sample_count": total,
                "positive_count": positive,
                "positive_ratio": (float(positive) / float(total)) if total > 0 else math.nan,
            }
        )
    by_regime_rows = []
    for regime, stat in sorted(by_regime.items()):
        total = int(stat["total"])
        positive = int(stat["positive"])
        by_regime_rows.append(
            {
                "regime": regime,
                "sample_count": total,
                "positive_count": positive,
                "positive_ratio": (float(positive) / float(total)) if total > 0 else math.nan,
            }
        )

    payload = {
        "generated_at_utc": utc_now_iso(),
        "status": "pass",
        "target_column": "label_tp1_hit_h20m",
        "dataset_rows": int(total_rows),
        "usable_label_rows": int(usable_rows),
        "dropped_due_to_future_window": int(dropped_due_to_future_window),
        "positive_ratio": (float(positive_rows) / float(usable_rows)) if usable_rows > 0 else math.nan,
        "by_market_positive_ratio": by_market_rows,
        "by_regime_positive_ratio": by_regime_rows,
        "risk_pct_distribution": _quantiles(risk_values),
        "tp1_pct_distribution": _quantiles(tp1_values),
    }
    dump_json(report_json_path, payload)


def to_iso(ts_ms: int) -> str:
    return datetime.fromtimestamp(float(ts_ms) / 1000.0, tz=timezone.utc).isoformat()


def round6(value: float) -> float:
    if not math.isfinite(value):
        return math.nan
    return round(float(value), 6)


def normalize_sample_mode(sample_mode: str) -> str:
    mode = str(sample_mode or "time").strip().lower()
    return mode if mode in ("time", "dollar", "volatility") else "time"


def update_sampling_state(
    *,
    mode: str,
    threshold: float,
    lookback_minutes: int,
    notional_value: float,
    ret_1m_value: float,
    state: Dict[str, object],
) -> Tuple[bool, float]:
    norm_mode = normalize_sample_mode(mode)
    if norm_mode == "time":
        return True, 0.0

    window_values: deque = state.setdefault("window_values", deque())  # type: ignore[assignment]
    rolling_sum = float(state.get("rolling_sum", 0.0) or 0.0)
    rolling_sq_sum = float(state.get("rolling_sq_sum", 0.0) or 0.0)
    window_limit = max(2, int(lookback_minutes))
    threshold_value = max(0.0, float(threshold))

    if norm_mode == "dollar":
        value = float(notional_value) if math.isfinite(notional_value) else 0.0
    else:
        value = float(ret_1m_value) if math.isfinite(ret_1m_value) else 0.0
    window_values.append(value)
    rolling_sum += value
    rolling_sq_sum += (value * value)
    if len(window_values) > window_limit:
        old = float(window_values.popleft())
        rolling_sum -= old
        rolling_sq_sum -= (old * old)
    state["rolling_sum"] = rolling_sum
    state["rolling_sq_sum"] = rolling_sq_sum

    n = len(window_values)
    if n < window_limit:
        metric = rolling_sum if norm_mode == "dollar" else 0.0
        return False, metric

    if norm_mode == "dollar":
        metric = rolling_sum
    else:
        mean = rolling_sum / float(n)
        var = max(0.0, (rolling_sq_sum / float(n)) - (mean * mean))
        metric = math.sqrt(var)
    return metric >= threshold_value, metric


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
    sample_mode: str,
    sample_threshold: float,
    sample_lookback_minutes: int,
    roundtrip_cost_bps: float,
    cost_model_config: Dict[str, object],
    label_tp1_policy: Dict[str, object],
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
    out_path = output_dir / f"prob_features_{safe_market}_1m_v2_draft.csv"
    ensure_parent_directory(out_path)
    normalized_sample_mode = normalize_sample_mode(sample_mode)
    sample_threshold_value = float(sample_threshold)
    sample_lookback_value = max(2, int(sample_lookback_minutes))
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
            "sample_mode": normalized_sample_mode,
            "sample_threshold": sample_threshold_value,
            "sample_lookback_minutes": sample_lookback_value,
            "sampling_rows_considered": int(existing_rows),
            "sampling_rows_selected": int(existing_rows),
            "sampling_rows_dropped": 0,
            "sampling_selected_ratio": 1.0,
            "sampling_metric_min": math.nan,
            "sampling_metric_max": math.nan,
            "sampling_metric_mean": math.nan,
            "from_utc": to_iso(int(anchor_ts[0])),
            "to_utc": to_iso(int(anchor_ts[-1])),
            "output_size_bytes": out_path.stat().st_size,
            "dropped_due_to_future_window": 0,
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
            "label_tp1_hit_h20m",
            "label_tp1_hit_h5m",
            "label_tp1_risk_pct",
            "label_tp1_pct",
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

    tp1_horizon_bars = max(1, int(label_tp1_policy.get("horizon_bars", DEFAULT_LABEL_TP1_POLICY["horizon_bars"])))
    max_h = max(
        int(label_h1),
        int(label_h5),
        int(tp1_horizon_bars),
        int(triple_barrier_horizon) if bool(enable_triple_barrier_labels) else 0,
    )
    written = 0
    skipped_warmup = 0
    skipped_missing_ctx = 0
    first_written_ts = 0
    last_written_ts = 0
    sampling_rows_considered = 0
    sampling_rows_selected = 0
    sampling_rows_dropped = 0
    sampling_metric_sum = 0.0
    sampling_metric_min = math.inf
    sampling_metric_max = -math.inf
    sampling_state: Dict[str, object] = {}
    dropped_due_to_future_window = 0

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
                dropped_due_to_future_window += 1
                continue

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
            regime_sign_60 = sign3(regime_ret_60)
            regime_sign_240 = sign3(regime_ret_240)
            row["regime_trend_60_sign"] = regime_sign_60
            row["regime_trend_240_sign"] = regime_sign_240
            row["regime_vol_60_atr_pct"] = round6(ctx_values[60]["atr_pct_14"])
            regime_name = normalize_regime(regime_sign_60, regime_sign_240)

            close_h1 = float(anchor_close[i + int(label_h1)])
            close_h5 = float(anchor_close[i + int(label_h5)])
            row["label_up_h1"] = 1 if close_h1 > c else 0
            row["label_up_h5"] = 1 if close_h5 > c else 0
            risk_pct, tp1_pct, tp1_target_price = compute_tp1_risk_and_target(
                atr_pct_fraction=float(atr14[i]) / c if c > 0.0 else math.nan,
                entry_price=c,
                regime_name=regime_name,
                label_tp1_policy=label_tp1_policy,
            )
            future_high_window = anchor_high[i + 1 : i + 1 + int(tp1_horizon_bars)]
            max_future_high = max(float(x) for x in future_high_window) if future_high_window else math.nan
            label_tp1_hit = 1 if (math.isfinite(max_future_high) and max_future_high >= tp1_target_price) else 0
            row["label_tp1_hit_h20m"] = int(label_tp1_hit)
            # Deprecated alias kept for backward compatibility; values are identical.
            row["label_tp1_hit_h5m"] = int(label_tp1_hit)
            row["label_tp1_risk_pct"] = round6(risk_pct)
            row["label_tp1_pct"] = round6(tp1_pct)
            gross_bps_h5 = ((close_h5 / c) - 1.0) * 10000.0 if c > 0.0 else math.nan
            row_cost_bps = resolve_label_cost_bps(
                roundtrip_cost_bps=float(roundtrip_cost_bps),
                cost_model_config=cost_model_config,
                atr_pct_14=float(atr14[i]) / c if c > 0.0 else math.nan,
                bb_width_20=bb_width_20,
                vol_ratio_20=vol_ratio_20,
                notional_ratio_20=notional_ratio_20,
            )
            row["label_edge_bps_h5"] = round6(gross_bps_h5 - float(row_cost_bps))
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
                row["label_tb_exit_bps"] = round6(tb_exit_bps - float(row_cost_bps))
                row["label_tb_event"] = str(tb_event)

            sampling_rows_considered += 1
            keep_row, sampling_metric = update_sampling_state(
                mode=normalized_sample_mode,
                threshold=sample_threshold_value,
                lookback_minutes=sample_lookback_value,
                notional_value=notional,
                ret_1m_value=float(row["ret_1m"]) if isinstance(row.get("ret_1m"), (float, int)) else 0.0,
                state=sampling_state,
            )
            if not keep_row:
                sampling_rows_dropped += 1
                continue

            sampling_rows_selected += 1
            if math.isfinite(sampling_metric):
                sampling_metric_sum += float(sampling_metric)
                sampling_metric_min = min(float(sampling_metric_min), float(sampling_metric))
                sampling_metric_max = max(float(sampling_metric_max), float(sampling_metric))
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
    sampling_ratio = (
        float(sampling_rows_selected) / float(sampling_rows_considered)
        if sampling_rows_considered > 0
        else 0.0
    )
    sampling_metric_mean = (
        float(sampling_metric_sum) / float(sampling_rows_selected)
        if sampling_rows_selected > 0
        else math.nan
    )
    return {
        "market": market,
        "status": "built",
        "anchor_input_path": str(anchor_path),
        "output_path": str(out_path),
        "anchor_rows": len(anchor_ts),
        "feature_rows_written": int(written),
        "skipped_warmup": int(skipped_warmup),
        "skipped_missing_context": int(skipped_missing_ctx),
        "sample_mode": normalized_sample_mode,
        "sample_threshold": sample_threshold_value,
        "sample_lookback_minutes": sample_lookback_value,
        "sampling_rows_considered": int(sampling_rows_considered),
        "sampling_rows_selected": int(sampling_rows_selected),
        "sampling_rows_dropped": int(sampling_rows_dropped),
        "sampling_selected_ratio": round6(sampling_ratio),
        "sampling_metric_min": round6(sampling_metric_min) if math.isfinite(sampling_metric_min) else math.nan,
        "sampling_metric_max": round6(sampling_metric_max) if math.isfinite(sampling_metric_max) else math.nan,
        "sampling_metric_mean": round6(sampling_metric_mean),
        "dropped_due_to_future_window": int(dropped_due_to_future_window),
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
    pipeline_version = str(args.pipeline_version).strip().lower()

    if not input_dir.exists():
        raise FileNotFoundError(f"input dir not found: {input_dir}")

    universe_file_path = ""
    universe_final_1m_markets: List[str] = []
    if str(args.universe_file).strip():
        universe_path = resolve_repo_path(str(args.universe_file).strip())
        if not universe_path.exists():
            raise FileNotFoundError(f"universe file not found: {universe_path}")
        universe_file_path = str(universe_path)
        universe_final_1m_markets = load_universe_final_1m_markets(universe_path)
        print(
            f"[BuildProbFeatures] universe_scope enabled "
            f"final_1m_count={len(universe_final_1m_markets)} file={universe_path}",
            flush=True,
        )
    universe_final_1m_set = set(universe_final_1m_markets)
    sample_mode = normalize_sample_mode(args.sample_mode)
    sample_threshold = float(args.sample_threshold)
    sample_lookback_minutes = max(2, int(args.sample_lookback_minutes))
    if sample_mode in ("dollar", "volatility") and sample_threshold <= 0.0:
        raise ValueError("--sample-threshold must be > 0 when --sample-mode is dollar or volatility")
    print(
        f"[BuildProbFeatures] sample_mode={sample_mode} "
        f"threshold={sample_threshold} lookback_minutes={sample_lookback_minutes}",
        flush=True,
    )
    print(f"[BuildProbFeatures] pipeline_version={pipeline_version}", flush=True)
    label_policy_path = resolve_repo_path(str(args.label_policy_json).strip())
    label_tp1_policy = load_label_tp1_policy(label_policy_path)
    print(
        "[BuildProbFeatures] label_tp1_policy "
        f"horizon_bars={label_tp1_policy.get('horizon_bars')} "
        f"tp1_rr={label_tp1_policy.get('tp1_rr')} "
        "target_column=label_tp1_hit_h20m "
        "deprecated_alias=label_tp1_hit_h5m "
        f"policy_path={label_policy_path}",
        flush=True,
    )

    cost_model_config = {
        "enabled": bool(args.enable_conditional_cost_model),
        "fee_floor_bps": float(args.cost_fee_floor_bps),
        "volatility_weight": float(args.cost_volatility_weight),
        "range_weight": float(args.cost_range_weight),
        "liquidity_weight": float(args.cost_liquidity_weight),
        "volatility_norm_bps": float(args.cost_volatility_norm_bps),
        "range_norm_bps": float(args.cost_range_norm_bps),
        "liquidity_ref_ratio": float(args.cost_liquidity_ref_ratio),
        "liquidity_penalty_cap": float(args.cost_liquidity_penalty_cap),
        "cost_cap_bps": float(args.cost_cap_bps),
    }

    requested_markets = parse_markets(args.markets)
    if requested_markets:
        markets = requested_markets
    else:
        markets = discover_markets(input_dir)
        if universe_final_1m_markets:
            # Include required universe markets so missing anchors fail closed in this run.
            markets = merge_markets(markets, universe_final_1m_markets)
    if not markets:
        raise RuntimeError("no markets found for 1m anchor files")

    started = utc_now_iso()
    jobs: List[Dict[str, object]] = []
    failed: List[Dict[str, object]] = []
    warnings: List[Dict[str, object]] = []

    missing_required_universe_anchors: List[str] = []
    if universe_final_1m_markets:
        for market in universe_final_1m_markets:
            anchor_path = input_dir / f"upbit_{market_to_safe(market)}_1m_full.csv"
            if anchor_path.exists():
                continue
            missing_required_universe_anchors.append(market)
            failed.append(
                {
                    "market": market,
                    "error": f"missing required 1m anchor for universe market: {anchor_path}",
                    "kind": "missing_required_universe_anchor",
                }
            )
            print(
                f"[BuildProbFeatures] failed market={market} "
                f"error=missing required 1m anchor for universe market",
                flush=True,
            )
    missing_required_set = set(missing_required_universe_anchors)

    for market in markets:
        anchor_path = input_dir / f"upbit_{market_to_safe(market)}_1m_full.csv"
        if not anchor_path.exists():
            if market in universe_final_1m_set:
                if market not in missing_required_set:
                    failed.append(
                        {
                            "market": market,
                            "error": f"missing required 1m anchor for universe market: {anchor_path}",
                            "kind": "missing_required_universe_anchor",
                        }
                    )
                    missing_required_universe_anchors.append(market)
                    missing_required_set.add(market)
                    print(
                        f"[BuildProbFeatures] failed market={market} "
                        f"error=missing required 1m anchor for universe market",
                        flush=True,
                    )
            else:
                warning_message = "missing 1m anchor outside final_1m_markets; skipped with warning"
                warnings.append({"market": market, "warning": warning_message, "anchor_input_path": str(anchor_path)})
                jobs.append(
                    {
                        "market": market,
                        "status": "skipped_missing_anchor_non_universe",
                        "anchor_input_path": str(anchor_path),
                        "output_path": str(output_dir / f"prob_features_{market_to_safe(market)}_1m_v2_draft.csv"),
                        "anchor_rows": 0,
                        "feature_rows_written": 0,
                        "skipped_warmup": 0,
                        "skipped_missing_context": 0,
                        "sample_mode": sample_mode,
                        "sample_threshold": sample_threshold,
                        "sample_lookback_minutes": sample_lookback_minutes,
                        "sampling_rows_considered": 0,
                        "sampling_rows_selected": 0,
                        "sampling_rows_dropped": 0,
                        "sampling_selected_ratio": 0.0,
                        "sampling_metric_min": math.nan,
                        "sampling_metric_max": math.nan,
                        "sampling_metric_mean": math.nan,
                        "dropped_due_to_future_window": 0,
                        "from_utc": "",
                        "to_utc": "",
                        "output_size_bytes": 0,
                        "warning": warning_message,
                    }
                )
                print(f"[BuildProbFeatures] warn market={market} {warning_message}", flush=True)
            continue
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
                sample_mode=sample_mode,
                sample_threshold=sample_threshold,
                sample_lookback_minutes=sample_lookback_minutes,
                roundtrip_cost_bps=float(args.roundtrip_cost_bps),
                cost_model_config=cost_model_config,
                label_tp1_policy=label_tp1_policy,
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
    sampling_rows_considered_total = sum(int(x.get("sampling_rows_considered", 0) or 0) for x in jobs)
    sampling_rows_selected_total = sum(int(x.get("sampling_rows_selected", 0) or 0) for x in jobs)
    sampling_rows_dropped_total = sum(int(x.get("sampling_rows_dropped", 0) or 0) for x in jobs)
    sampling_selected_ratio_total = (
        float(sampling_rows_selected_total) / float(sampling_rows_considered_total)
        if sampling_rows_considered_total > 0
        else 0.0
    )
    success_count = sum(
        1
        for x in jobs
        if str(x.get("status", "")).strip().lower() in ("built", "skipped_existing")
    )
    skipped_missing_anchor_non_universe_count = sum(
        1 for x in jobs if str(x.get("status", "")).strip().lower() == "skipped_missing_anchor_non_universe"
    )
    finished = utc_now_iso()
    dataset_version = "prob_features_v2_draft"
    payload = {
        "version": dataset_version,
        "started_at_utc": started,
        "finished_at_utc": finished,
        "input_dir": str(input_dir),
        "output_dir": str(output_dir),
        "anchor_tf": "1m",
        "context_tfs": [int(x) for x in CONTEXT_TFS],
        "markets": markets,
        "universe_file_path": universe_file_path,
        "universe_final_1m_markets": universe_final_1m_markets,
        "universe_final_1m_markets_count": len(universe_final_1m_markets),
        "roundtrip_cost_bps": float(args.roundtrip_cost_bps),
        "cost_model": cost_model_config,
        "label_h1": int(args.label_h1),
        "label_h5": int(args.label_h5),
        "label_tp1_target_column": "label_tp1_hit_h20m",
        "label_tp1_deprecated_alias_column": "label_tp1_hit_h5m",
        "label_tp1_policy": label_tp1_policy,
        "label_tp1_policy_json": str(label_policy_path),
        "enable_triple_barrier_labels": bool(args.enable_triple_barrier_labels),
        "triple_barrier_horizon": int(args.triple_barrier_horizon),
        "triple_barrier_take_profit_bps": float(args.triple_barrier_take_profit_bps),
        "triple_barrier_stop_loss_bps": float(args.triple_barrier_stop_loss_bps),
        "sample_mode": sample_mode,
        "sample_threshold": sample_threshold,
        "sample_lookback_minutes": sample_lookback_minutes,
        "max_rows_per_market": int(args.max_rows_per_market),
        "job_count": len(markets),
        "success_count": int(success_count),
        "skipped_missing_anchor_non_universe_count": int(skipped_missing_anchor_non_universe_count),
        "failed_count": len(failed),
        "warning_count": len(warnings),
        "missing_required_universe_anchors": missing_required_universe_anchors,
        "total_feature_rows": int(total_rows),
        "total_output_bytes": int(total_bytes),
        "sampling_rows_considered_total": int(sampling_rows_considered_total),
        "sampling_rows_selected_total": int(sampling_rows_selected_total),
        "sampling_rows_dropped_total": int(sampling_rows_dropped_total),
        "sampling_selected_ratio_total": round6(sampling_selected_ratio_total),
        "jobs": jobs,
        "failed": failed,
        "warnings": warnings,
    }
    payload["pipeline_version"] = str(pipeline_version)
    payload["feature_contract_version"] = "v2_draft"
    dump_json(summary_json, payload)
    dump_json(manifest_json, payload)
    stageg2_label_report_json = resolve_repo_path(str(args.stageg2_label_report_json).strip())
    if str(args.stageg2_label_report_json).strip():
        build_stageg2_label_report(
            manifest_payload=payload,
            report_json_path=stageg2_label_report_json,
        )

    print("[BuildProbFeatures] Completed", flush=True)
    print(f"summary={summary_json}", flush=True)
    print(f"manifest={manifest_json}", flush=True)
    print(f"success_count={int(success_count)}", flush=True)
    print(f"skipped_missing_anchor_non_universe_count={int(skipped_missing_anchor_non_universe_count)}", flush=True)
    print(f"failed_count={len(failed)}", flush=True)
    print(f"warning_count={len(warnings)}", flush=True)
    print(f"total_feature_rows={total_rows}", flush=True)
    print(f"total_output_bytes={total_bytes}", flush=True)
    if str(args.stageg2_label_report_json).strip():
        print(f"stageg2_label_report={stageg2_label_report_json}", flush=True)
    return 0 if not failed else 2


if __name__ == "__main__":
    raise SystemExit(main())
