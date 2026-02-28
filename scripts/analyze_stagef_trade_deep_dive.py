#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
import re
from bisect import bisect_right
from datetime import datetime, timedelta, timezone
from itertools import combinations
from pathlib import Path
from statistics import median
from typing import Any, Dict, Iterable, List, Optional, Tuple

from _script_common import dump_json, resolve_repo_path


KST = timezone(timedelta(hours=9))
REGIME_BY_CODE = {0: "UNKNOWN", 1: "TRENDING_UP", 2: "TRENDING_DOWN", 3: "RANGING", 4: "HIGH_VOLATILITY"}

LOG_TS_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})(?:\.(\d{1,3}))?\]")
BEST_SELECTED_RE = re.compile(
    r"([A-Z0-9\-]+)\s+probabilistic-primary best selected:\s*"
    r"p_h5=([+-]?\d+(?:\.\d+)?),\s*margin=([+-]?\d+(?:\.\d+)?),.*?"
    r"ev_blend=([+-]?\d+(?:\.\d+)?),.*?"
    r"edge_cal=([+-]?\d+(?:\.\d+)?)%",
    re.IGNORECASE,
)
POSITION_ENTERED_RE = re.compile(r"Position entered:\s*([A-Z0-9\-]+)\s*\|", re.IGNORECASE)
RISK_PLAN_RE = re.compile(
    r"risk plan:\s*SL=[^ ]+\s+\(([+-]?\d+(?:\.\d+)?)%\),\s*"
    r"TP1=[^ ]+\s+\(([+-]?\d+(?:\.\d+)?)%\),\s*"
    r"TP2=[^ ]+\s+\(([+-]?\d+(?:\.\d+)?)%\)",
    re.IGNORECASE,
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Stage F1 deep-dive analyzer for E2 Round7 17 trades.")
    p.add_argument("--paper-summary-json", default="build/Release/logs/paper_run_summary_E2_4h_round7.json")
    p.add_argument("--selected-samples-json", default="build/Release/logs/live_selected_candidate_samples_E2_round7.json")
    p.add_argument("--timeseries-jsonl", default="build/Release/logs/paper_time_series_metrics_E2_4h_round7.jsonl")
    p.add_argument("--autolife-log", default="build/Release/logs/autolife.log")
    p.add_argument("--state-json", default="build/Release/state/snapshot_state.json")
    p.add_argument("--phase4-candidates-jsonl", default="build/Release/logs/phase4_portfolio_candidates_live.jsonl")
    p.add_argument("--policy-decisions-jsonl", default="build/Release/logs/policy_decisions.jsonl")
    p.add_argument("--ohlcv-root", default="build/Release/data/backtest_real_live")
    p.add_argument("--out-deep-dive-json", default="build/Release/logs/stageF_trade_deep_dive.json")
    p.add_argument("--out-trade-table-csv", default="build/Release/logs/stageF_trade_table.csv")
    p.add_argument("--out-feature-stats-json", default="build/Release/logs/stageF_feature_stats.json")
    return p.parse_args()


def read_json(path_value: Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {}
    try:
        raw = json.loads(path_value.read_text(encoding="utf-8-sig"))
        return raw if isinstance(raw, dict) else {}
    except Exception:
        return {}


def read_jsonl(path_value: Path) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    if not path_value.exists():
        return out
    with path_value.open("r", encoding="utf-8", errors="ignore") as fh:
        for line in fh:
            token = line.strip()
            if not token:
                continue
            try:
                raw = json.loads(token)
            except Exception:
                continue
            if isinstance(raw, dict):
                out.append(raw)
    return out


def safe_float(value: Any) -> Optional[float]:
    try:
        f = float(value)
        if math.isfinite(f):
            return f
    except Exception:
        return None
    return None


def safe_int(value: Any) -> Optional[int]:
    try:
        return int(value)
    except Exception:
        try:
            return int(float(value))
        except Exception:
            return None


def parse_utc_to_ms(iso_text: str) -> int:
    return int(datetime.fromisoformat(str(iso_text).replace("Z", "+00:00")).timestamp() * 1000.0)


def parse_log_ts_ms(line: str) -> Optional[int]:
    m = LOG_TS_RE.match(line)
    if not m:
        return None
    try:
        dt = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S").replace(tzinfo=KST)
        ms = int((m.group(2) or "0").ljust(3, "0")[:3])
        return int(dt.timestamp() * 1000.0) + ms
    except Exception:
        return None


def iso_utc(ts_ms: Optional[int]) -> str:
    if ts_ms is None:
        return ""
    return datetime.fromtimestamp(ts_ms / 1000.0, tz=timezone.utc).isoformat()


def to_market_safe(market: str) -> str:
    return str(market).replace("-", "_")


def quantile(values: List[float], q: float) -> Optional[float]:
    if not values:
        return None
    if q <= 0.0:
        return min(values)
    if q >= 1.0:
        return max(values)
    data = sorted(values)
    pos = (len(data) - 1) * q
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return data[lo]
    frac = pos - lo
    return data[lo] * (1.0 - frac) + data[hi] * frac


def summarize(values: Iterable[Optional[float]]) -> Dict[str, Any]:
    v = [float(x) for x in values if isinstance(x, (int, float)) and math.isfinite(float(x))]
    if not v:
        return {"count": 0, "mean": None, "std": None, "min": None, "p25": None, "median": None, "p75": None, "max": None}
    mu = sum(v) / len(v)
    sd = 0.0 if len(v) == 1 else math.sqrt(max(sum((x - mu) ** 2 for x in v) / (len(v) - 1), 0.0))
    return {"count": len(v), "mean": mu, "std": sd, "min": min(v), "p25": quantile(v, 0.25), "median": median(v), "p75": quantile(v, 0.75), "max": max(v)}


def safe_ret(values: List[float], idx_now: int, idx_prev: int) -> Optional[float]:
    if idx_now < 0 or idx_prev < 0 or idx_now >= len(values) or idx_prev >= len(values):
        return None
    base = values[idx_prev]
    cur = values[idx_now]
    if base <= 0.0:
        return None
    return (cur / base) - 1.0


def compute_ema(values: List[float], period: int) -> List[Optional[float]]:
    out: List[Optional[float]] = [None] * len(values)
    if not values:
        return out
    alpha = 2.0 / (float(period) + 1.0)
    ema = float(values[0])
    out[0] = ema
    for i in range(1, len(values)):
        ema = alpha * float(values[i]) + (1.0 - alpha) * ema
        out[i] = ema
    return out


def compute_atr(highs: List[float], lows: List[float], closes: List[float], period: int = 14) -> List[Optional[float]]:
    out: List[Optional[float]] = [None] * len(closes)
    if len(closes) <= period:
        return out
    tr = [0.0] * len(closes)
    for i in range(1, len(closes)):
        tr1 = highs[i] - lows[i]
        tr2 = abs(highs[i] - closes[i - 1])
        tr3 = abs(lows[i] - closes[i - 1])
        tr[i] = max(tr1, tr2, tr3)
    atr = sum(tr[1 : period + 1]) / float(period)
    out[period] = atr
    for i in range(period + 1, len(closes)):
        atr = ((atr * (period - 1)) + tr[i]) / float(period)
        out[i] = atr
    return out


def calculate_sma_tail(values: List[float], period: int) -> float:
    if len(values) < period or period <= 0:
        return 0.0
    return sum(values[-period:]) / float(period)


def compute_adx_latest(highs: List[float], lows: List[float], closes: List[float], idx: int, period: int = 14) -> Optional[float]:
    end = idx + 1
    if end <= period * 2:
        return None
    h = highs[:end]
    l = lows[:end]
    c = closes[:end]
    tr_vec: List[float] = []
    dm_plus_vec: List[float] = []
    dm_minus_vec: List[float] = []
    for i in range(1, len(c)):
        ch, cl, ph, pl, pc = h[i], l[i], h[i - 1], l[i - 1], c[i - 1]
        tr_vec.append(max(ch - cl, abs(ch - pc), abs(cl - pc)))
        up_move = ch - ph
        down_move = pl - cl
        dm_plus_vec.append(up_move if (up_move > down_move and up_move > 0.0) else 0.0)
        dm_minus_vec.append(down_move if (down_move > up_move and down_move > 0.0) else 0.0)

    def smooth(vec: List[float], p: int) -> List[float]:
        if len(vec) < p:
            return []
        seed = sum(vec[:p])
        out = [seed]
        prev = seed
        for j in range(p, len(vec)):
            current = prev - (prev / p) + vec[j]
            out.append(current)
            prev = current
        return out

    tr_s = smooth(tr_vec, period)
    dmp_s = smooth(dm_plus_vec, period)
    dmm_s = smooth(dm_minus_vec, period)
    n = min(len(tr_s), len(dmp_s), len(dmm_s))
    if n <= 0:
        return None
    dx_vec: List[float] = []
    for i in range(n):
        trv = tr_s[i]
        if trv == 0.0:
            dx_vec.append(0.0)
            continue
        di_plus = (dmp_s[i] / trv) * 100.0
        di_minus = (dmm_s[i] / trv) * 100.0
        denom = di_plus + di_minus
        dx_vec.append(0.0 if denom == 0.0 else (abs(di_plus - di_minus) / denom) * 100.0)
    if len(dx_vec) < period:
        return None
    return calculate_sma_tail(dx_vec, period)


class OHLCVCache:
    def __init__(self, root: Path):
        self.root = root
        self.cache: Dict[Tuple[str, int], Dict[str, Any]] = {}

    def _load_tf(self, market: str, tf_min: int) -> Dict[str, Any]:
        key = (market, tf_min)
        if key in self.cache:
            return self.cache[key]
        safe = to_market_safe(market)
        path = self.root / f"upbit_{safe}_{tf_min}m_live.csv"
        out: Dict[str, Any] = {"path": str(path), "timestamp": [], "open": [], "high": [], "low": [], "close": [], "volume": []}
        if not path.exists():
            self.cache[key] = out
            return out
        with path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                ts = safe_int(row.get("timestamp"))
                o = safe_float(row.get("open"))
                h = safe_float(row.get("high"))
                l = safe_float(row.get("low"))
                c = safe_float(row.get("close"))
                v = safe_float(row.get("volume"))
                if None in (ts, o, h, l, c, v):
                    continue
                out["timestamp"].append(int(ts))
                out["open"].append(float(o))
                out["high"].append(float(h))
                out["low"].append(float(l))
                out["close"].append(float(c))
                out["volume"].append(float(v))
        ts_arr: List[int] = out["timestamp"]
        if any(ts_arr[i] <= ts_arr[i - 1] for i in range(1, len(ts_arr))):
            idx = sorted(range(len(ts_arr)), key=lambda i: ts_arr[i])
            for col in ("timestamp", "open", "high", "low", "close", "volume"):
                out[col] = [out[col][i] for i in idx]
        out["ema12"] = compute_ema(out["close"], 12) if tf_min == 1 else []
        out["ema26"] = compute_ema(out["close"], 26) if tf_min == 1 else []
        out["ema20"] = compute_ema(out["close"], 20) if tf_min in (15, 60) else []
        out["atr14"] = compute_atr(out["high"], out["low"], out["close"], 14) if tf_min == 1 else []
        self.cache[key] = out
        return out

    def feature_at(self, market: str, entry_ms: int) -> Dict[str, Any]:
        one = self._load_tf(market, 1)
        t1: List[int] = one["timestamp"]
        i = bisect_right(t1, entry_ms) - 1
        out: Dict[str, Any] = {
            "adx_14": None,
            "atr_pct_14": None,
            "ema_gap_12_26": None,
            "ret_1m": None,
            "ret_5m": None,
            "ctx_15m_ema_gap_20": None,
            "ctx_60m_ema_gap_20": None,
        }
        if i < 0:
            return out
        close = one["close"][i]
        atr14 = one["atr14"][i] if i < len(one["atr14"]) else None
        ema12 = one["ema12"][i] if i < len(one["ema12"]) else None
        ema26 = one["ema26"][i] if i < len(one["ema26"]) else None
        if isinstance(atr14, (int, float)) and close > 0.0:
            out["atr_pct_14"] = float(atr14) / close
        if isinstance(ema12, (int, float)) and isinstance(ema26, (int, float)) and close > 0.0:
            out["ema_gap_12_26"] = (float(ema12) - float(ema26)) / close
        out["ret_1m"] = safe_ret(one["close"], i, i - 1)
        out["ret_5m"] = safe_ret(one["close"], i, i - 5)
        out["adx_14"] = compute_adx_latest(one["high"], one["low"], one["close"], i, 14)
        for tf in (15, 60):
            ctx = self._load_tf(market, tf)
            ts_ctx: List[int] = ctx["timestamp"]
            j = bisect_right(ts_ctx, entry_ms) - 1
            if j < 0:
                continue
            ctf = ctx["close"][j]
            ema20 = ctx["ema20"][j] if j < len(ctx["ema20"]) else None
            if isinstance(ema20, (int, float)) and ctf > 0.0:
                out["ctx_15m_ema_gap_20" if tf == 15 else "ctx_60m_ema_gap_20"] = (ctf - float(ema20)) / ctf
        return out


def group_by_market(rows: List[Dict[str, Any]], ts_key: str) -> Dict[str, List[Dict[str, Any]]]:
    grouped: Dict[str, List[Dict[str, Any]]] = {}
    for row in rows:
        market = str(row.get("market", "")).strip().upper()
        if not market:
            continue
        grouped.setdefault(market, []).append(row)
    for market in grouped:
        grouped[market].sort(key=lambda x: int(x.get(ts_key, 0)))
    return grouped


def pick_nearest_prior(
    grouped: Dict[str, List[Dict[str, Any]]], market: str, target_ts: int, ts_key: str, max_lag_ms: int
) -> Optional[Dict[str, Any]]:
    rows = grouped.get(market, [])
    if not rows:
        return None
    best = None
    best_lag: Optional[int] = None
    for row in rows:
        ts = safe_int(row.get(ts_key))
        if ts is None or ts > target_ts:
            continue
        lag = target_ts - ts
        if lag < 0 or lag > max_lag_ms:
            continue
        if best is None or lag < (best_lag or 10**18):
            best = row
            best_lag = lag
    return best


def parse_autolife_context(log_path: Path, start_ms: int, end_ms: int) -> Dict[str, Any]:
    best_rows: List[Dict[str, Any]] = []
    entered_rows: List[Dict[str, Any]] = []
    risk_rows: List[Dict[str, Any]] = []
    last_entered: Optional[Dict[str, Any]] = None
    if not log_path.exists():
        return {"best_rows": [], "risk_rows": [], "entered_rows": []}
    with log_path.open("r", encoding="utf-8", errors="ignore") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            ts = parse_log_ts_ms(line)
            if ts is None:
                continue
            if ts < start_ms - 15 * 60 * 1000 or ts > end_ms + 15 * 60 * 1000:
                continue
            m_best = BEST_SELECTED_RE.search(line)
            if m_best:
                edge_pct = safe_float(m_best.group(5))
                best_rows.append(
                    {
                        "market": m_best.group(1).strip().upper(),
                        "ts": ts,
                        "p_h5": safe_float(m_best.group(2)),
                        "margin": safe_float(m_best.group(3)),
                        "ev_blend": safe_float(m_best.group(4)),
                        "expected_edge_calibrated_bps_log": edge_pct * 100.0 if edge_pct is not None else None,
                    }
                )
                continue
            m_enter = POSITION_ENTERED_RE.search(line)
            if m_enter:
                last_entered = {"market": m_enter.group(1).strip().upper(), "entry_ts": ts}
                entered_rows.append(dict(last_entered))
                continue
            m_risk = RISK_PLAN_RE.search(line)
            if m_risk and last_entered is not None:
                risk_rows.append(
                    {
                        "market": str(last_entered.get("market", "")).upper(),
                        "entry_ts": int(last_entered.get("entry_ts", 0)),
                        "ts": ts,
                        "sl_pct": safe_float(m_risk.group(1)),
                        "tp1_pct": safe_float(m_risk.group(2)),
                        "tp2_pct": safe_float(m_risk.group(3)),
                    }
                )
    return {"best_rows": best_rows, "risk_rows": risk_rows, "entered_rows": entered_rows}


def parse_selected_phase4(path_value: Path, start_ms: int, end_ms: int) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for item in read_jsonl(path_value):
        ts = safe_int(item.get("ts"))
        if ts is None or ts < start_ms - 15 * 60 * 1000 or ts > end_ms + 15 * 60 * 1000:
            continue
        if str(item.get("source", "")).lower() != "live":
            continue
        candidates = item.get("candidates", [])
        if not isinstance(candidates, list):
            continue
        for cand in candidates:
            if not isinstance(cand, dict) or not bool(cand.get("selected", False)):
                continue
            market = str(cand.get("market_id", item.get("market", ""))).strip().upper()
            if not market:
                continue
            proxy = cand.get("execution_proxy", {}) if isinstance(cand.get("execution_proxy"), dict) else {}
            rows.append(
                {
                    "market": market,
                    "decision_time": safe_int(cand.get("decision_time")) or ts,
                    "ts": ts,
                    "p_h5": safe_float(cand.get("prob_confidence")),
                    "margin": safe_float(cand.get("margin")),
                    "expected_edge_calibrated_bps": safe_float(cand.get("expected_edge_calibrated_bps")),
                    "expected_edge_used_for_gate_bps": safe_float(cand.get("expected_edge_used_for_gate_bps")),
                    "regime": str(cand.get("regime", "")).strip().upper() or None,
                    "volatility_bucket": cand.get("volatility_bucket"),
                    "liquidity_score": safe_float(proxy.get("liquidity_score")),
                    "position_size": safe_float(proxy.get("position_size")),
                }
            )
    return rows


def parse_selected_policy(path_value: Path, start_ms: int, end_ms: int) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for item in read_jsonl(path_value):
        ts = safe_int(item.get("ts"))
        if ts is None or ts < start_ms - 15 * 60 * 1000 or ts > end_ms + 15 * 60 * 1000:
            continue
        decisions = item.get("decisions", [])
        if not isinstance(decisions, list):
            continue
        for dec in decisions:
            if not isinstance(dec, dict):
                continue
            if not bool(dec.get("selected", False)) or str(dec.get("reason", "")).strip().lower() != "selected":
                continue
            market = str(dec.get("market", "")).strip().upper()
            if not market:
                continue
            rows.append(
                {
                    "market": market,
                    "ts": ts,
                    "expected_value": safe_float(dec.get("expected_value")),
                    "required_expected_value": safe_float(dec.get("required_expected_value")),
                }
            )
    return rows


def exact_permutation_pvalue(win_vals: List[float], loss_vals: List[float]) -> Optional[float]:
    if len(win_vals) < 2 or len(loss_vals) < 2:
        return None
    vals = list(win_vals) + list(loss_vals)
    n = len(vals)
    k = len(win_vals)
    total_sum = sum(vals)
    obs = (sum(win_vals) / len(win_vals)) - (sum(loss_vals) / len(loss_vals))
    total = 0
    extreme = 0
    for comb in combinations(range(n), k):
        total += 1
        s = sum(vals[i] for i in comb)
        diff = (s / k) - ((total_sum - s) / (n - k))
        if abs(diff) >= abs(obs) - 1e-15:
            extreme += 1
    return None if total == 0 else (extreme / float(total))


def main() -> int:
    args = parse_args()
    paper_summary_path = resolve_repo_path(args.paper_summary_json)
    selected_samples_path = resolve_repo_path(args.selected_samples_json)
    timeseries_path = resolve_repo_path(args.timeseries_jsonl)
    autolife_path = resolve_repo_path(args.autolife_log)
    state_path = resolve_repo_path(args.state_json)
    phase4_path = resolve_repo_path(args.phase4_candidates_jsonl)
    policy_path = resolve_repo_path(args.policy_decisions_jsonl)
    ohlcv_root = resolve_repo_path(args.ohlcv_root)
    out_deep_dive_path = resolve_repo_path(args.out_deep_dive_json)
    out_trade_table_path = resolve_repo_path(args.out_trade_table_csv)
    out_feature_stats_path = resolve_repo_path(args.out_feature_stats_json)

    paper_summary = read_json(paper_summary_path)
    selected_samples = read_json(selected_samples_path)
    timeseries_rows = read_jsonl(timeseries_path)
    state = read_json(state_path)
    if not paper_summary:
        raise SystemExit(f"missing/invalid paper summary: {paper_summary_path}")

    window = paper_summary.get("window", {}) if isinstance(paper_summary.get("window", {}), dict) else {}
    started_at_utc = str(window.get("started_at_utc", ""))
    ended_at_utc = str(window.get("ended_at_utc", ""))
    if not started_at_utc or not ended_at_utc:
        raise SystemExit("paper summary missing window.started_at_utc/ended_at_utc")
    start_ms = parse_utc_to_ms(started_at_utc)
    end_ms = parse_utc_to_ms(ended_at_utc)

    raw_trade_history = state.get("trade_history", []) if isinstance(state.get("trade_history", []), list) else []
    trades = []
    for item in raw_trade_history:
        if not isinstance(item, dict):
            continue
        exit_ms = safe_int(item.get("exit_time"))
        if exit_ms is None:
            continue
        if start_ms <= exit_ms <= end_ms + 120000:
            trades.append(item)
    trades.sort(key=lambda x: int(x.get("exit_time", 0)))

    phase4_selected = parse_selected_phase4(phase4_path, start_ms, end_ms)
    policy_selected = parse_selected_policy(policy_path, start_ms, end_ms)
    log_ctx = parse_autolife_context(autolife_path, start_ms, end_ms)
    phase4_by_market = group_by_market(phase4_selected, "decision_time")
    policy_by_market = group_by_market(policy_selected, "ts")
    best_by_market = group_by_market(log_ctx.get("best_rows", []), "ts")
    risk_by_market = group_by_market(log_ctx.get("risk_rows", []), "entry_ts")
    ohlcv = OHLCVCache(ohlcv_root)

    rows: List[Dict[str, Any]] = []
    for i, trade in enumerate(trades, start=1):
        market = str(trade.get("market", "")).strip().upper()
        entry_ms = safe_int(trade.get("entry_time")) or 0
        exit_ms = safe_int(trade.get("exit_time")) or 0
        pnl = safe_float(trade.get("profit_loss")) or 0.0
        outcome = "win" if pnl > 0 else ("loss" if pnl < 0 else "draw")
        cand = pick_nearest_prior(phase4_by_market, market, entry_ms, "decision_time", 10 * 60 * 1000)
        pol = pick_nearest_prior(policy_by_market, market, entry_ms, "ts", 10 * 60 * 1000)
        best = pick_nearest_prior(best_by_market, market, entry_ms, "ts", 2 * 60 * 1000)
        risk = pick_nearest_prior(risk_by_market, market, entry_ms, "entry_ts", 10 * 1000)
        feat = ohlcv.feature_at(market, entry_ms)

        p_h5 = safe_float(trade.get("probabilistic_h5_calibrated"))
        if p_h5 is None and isinstance(cand, dict):
            p_h5 = safe_float(cand.get("p_h5"))
        margin = safe_float(trade.get("probabilistic_h5_margin"))
        if margin is None and isinstance(cand, dict):
            margin = safe_float(cand.get("margin"))
        signal_ev = safe_float(trade.get("expected_value"))
        if signal_ev is None and isinstance(pol, dict):
            signal_ev = safe_float(pol.get("expected_value"))
        req_ev = safe_float(trade.get("required_expected_value"))
        if req_ev is None and isinstance(pol, dict):
            req_ev = safe_float(pol.get("required_expected_value"))
        edge_cal_bps = safe_float(cand.get("expected_edge_calibrated_bps")) if isinstance(cand, dict) else None
        if edge_cal_bps is None and isinstance(best, dict):
            edge_cal_bps = safe_float(best.get("expected_edge_calibrated_bps_log"))

        rows.append(
            {
                "trade_id": i,
                "market": market,
                "entry_time_ms": entry_ms,
                "exit_time_ms": exit_ms,
                "entry_time_utc": iso_utc(entry_ms),
                "exit_time_utc": iso_utc(exit_ms),
                "holding_seconds": (exit_ms - entry_ms) / 1000.0 if exit_ms >= entry_ms > 0 else None,
                "pnl_krw": pnl,
                "pnl_pct": safe_float(trade.get("profit_loss_pct")),
                "outcome": outcome,
                "exit_reason": str(trade.get("exit_reason", "")).strip().lower(),
                "regime": REGIME_BY_CODE.get(safe_int(trade.get("market_regime")) or 0, "UNKNOWN"),
                "p_h5": p_h5,
                "selection_threshold": safe_float(trade.get("probabilistic_h5_threshold")),
                "margin": margin,
                "expected_edge_calibrated_bps": edge_cal_bps,
                "signal_expected_value": signal_ev,
                "ev_blend": safe_float(best.get("ev_blend")) if isinstance(best, dict) else None,
                "required_expected_value": req_ev,
                "expected_edge_used_for_gate_bps": safe_float(cand.get("expected_edge_used_for_gate_bps")) if isinstance(cand, dict) else None,
                "rr": safe_float(trade.get("reward_risk_ratio")),
                "sl_pct_plan": safe_float(risk.get("sl_pct")) if isinstance(risk, dict) else None,
                "tp1_pct_plan": safe_float(risk.get("tp1_pct")) if isinstance(risk, dict) else None,
                "tp2_pct_plan": safe_float(risk.get("tp2_pct")) if isinstance(risk, dict) else None,
                "adx_14": safe_float(feat.get("adx_14")),
                "atr_pct_14": safe_float(feat.get("atr_pct_14")),
                "ema_gap_12_26": safe_float(feat.get("ema_gap_12_26")),
                "ret_1m": safe_float(feat.get("ret_1m")),
                "ret_5m": safe_float(feat.get("ret_5m")),
                "ctx_15m_ema_gap_20": safe_float(feat.get("ctx_15m_ema_gap_20")),
                "ctx_60m_ema_gap_20": safe_float(feat.get("ctx_60m_ema_gap_20")),
                "spread_pct": None,
                "drop_vs_signal": None,
                "imbalance": None,
                "liquidity_score": safe_float(cand.get("liquidity_score")) if isinstance(cand, dict) else None,
            }
        )

    csv_fields = list(rows[0].keys()) if rows else []
    out_trade_table_path.parent.mkdir(parents=True, exist_ok=True)
    with out_trade_table_path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=csv_fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    wins = [r for r in rows if r["outcome"] == "win"]
    losses = [r for r in rows if r["outcome"] == "loss"]
    draws = [r for r in rows if r["outcome"] == "draw"]
    exit_counts: Dict[str, int] = {}
    for r in rows:
        k = str(r.get("exit_reason", "other") or "other")
        exit_counts[k] = exit_counts.get(k, 0) + 1

    compare_metrics = ["p_h5", "margin", "signal_expected_value", "adx_14"]
    compare: Dict[str, Any] = {}
    significant = []
    for metric in compare_metrics:
        w = [float(r[metric]) for r in wins if isinstance(r.get(metric), (int, float))]
        l = [float(r[metric]) for r in losses if isinstance(r.get(metric), (int, float))]
        pval = exact_permutation_pvalue(w, l)
        wm = (sum(w) / len(w)) if w else None
        lm = (sum(l) / len(l)) if l else None
        diff = (wm - lm) if (wm is not None and lm is not None) else None
        is_sig = bool(isinstance(pval, float) and pval < 0.05)
        if is_sig:
            significant.append(metric)
        compare[metric] = {
            "win_count": len(w),
            "loss_count": len(l),
            "win_mean": wm,
            "loss_mean": lm,
            "mean_diff_win_minus_loss": diff,
            "exact_permutation_pvalue_two_sided": pval,
            "is_significant_p_lt_0_05": is_sig,
        }

    stop_loss_rows = [r for r in rows if str(r.get("exit_reason", "")).startswith("stop_loss")]
    strategy_exit_rows = [r for r in rows if str(r.get("exit_reason", "")).startswith("strategy_exit")]
    tp_like_rows = [r for r in rows if str(r.get("exit_reason", "")).startswith("take_profit") or str(r.get("exit_reason", "")).startswith("partial_take_profit")]
    feature_stats = {
        "counts": {"total_trades": len(rows), "win_count": len(wins), "loss_count": len(losses), "draw_count": len(draws)},
        "entry_quality": {
            "p_h5": summarize(r.get("p_h5") for r in rows),
            "selection_threshold": summarize(r.get("selection_threshold") for r in rows),
            "margin": summarize(r.get("margin") for r in rows),
            "expected_edge_calibrated_bps": summarize(r.get("expected_edge_calibrated_bps") for r in rows),
            "signal_expected_value": summarize(r.get("signal_expected_value") for r in rows),
            "ev_blend": summarize(r.get("ev_blend") for r in rows),
            "required_expected_value": summarize(r.get("required_expected_value") for r in rows),
        },
        "regime_structure": {
            "adx_14": summarize(r.get("adx_14") for r in rows),
            "atr_pct_14": summarize(r.get("atr_pct_14") for r in rows),
            "ema_gap_12_26": summarize(r.get("ema_gap_12_26") for r in rows),
            "ret_1m": summarize(r.get("ret_1m") for r in rows),
            "ret_5m": summarize(r.get("ret_5m") for r in rows),
            "ctx_15m_ema_gap_20": summarize(r.get("ctx_15m_ema_gap_20") for r in rows),
            "ctx_60m_ema_gap_20": summarize(r.get("ctx_60m_ema_gap_20") for r in rows),
        },
        "execution_quality": {
            "holding_seconds": summarize(r.get("holding_seconds") for r in rows),
            "exit_reason_breakdown": exit_counts,
            "spread_pct": summarize(r.get("spread_pct") for r in rows),
            "drop_vs_signal": summarize(r.get("drop_vs_signal") for r in rows),
            "imbalance": summarize(r.get("imbalance") for r in rows),
        },
        "pnl_structure": {
            "rr": summarize(r.get("rr") for r in rows),
            "sl_pct_plan_abs": summarize(abs(float(r["sl_pct_plan"])) for r in rows if isinstance(r.get("sl_pct_plan"), (int, float))),
            "tp_reach_ratio": (len(tp_like_rows) / len(rows)) if rows else None,
            "stop_loss_vs_strategy_exit": {
                "stop_loss_count": len(stop_loss_rows),
                "stop_loss_avg_pnl_krw": (sum(float(r["pnl_krw"]) for r in stop_loss_rows) / len(stop_loss_rows)) if stop_loss_rows else None,
                "strategy_exit_count": len(strategy_exit_rows),
                "strategy_exit_avg_pnl_krw": (sum(float(r["pnl_krw"]) for r in strategy_exit_rows) / len(strategy_exit_rows)) if strategy_exit_rows else None,
            },
        },
        "winner_vs_loser": compare,
    }
    deep_dive = {
        "meta": {
            "run_label": paper_summary.get("run_label"),
            "window_started_at_utc": started_at_utc,
            "window_ended_at_utc": ended_at_utc,
            "inputs": {
                "paper_run_summary": str(paper_summary_path),
                "selected_samples": str(selected_samples_path),
                "timeseries_jsonl": str(timeseries_path),
                "autolife_log": str(autolife_path),
            },
            "counts": {
                "trade_rows_from_state_window": len(rows),
                "summary_trade_total": paper_summary.get("kpi", {}).get("trades_total", None) if isinstance(paper_summary.get("kpi"), dict) else None,
                "selected_samples_count": int(selected_samples.get("sample_count", 0)) if isinstance(selected_samples, dict) else 0,
                "timeseries_rows": len(timeseries_rows),
            },
        },
        "entry_quality_section": feature_stats["entry_quality"],
        "regime_structure_section": feature_stats["regime_structure"],
        "execution_quality_section": feature_stats["execution_quality"],
        "pnl_structure_section": feature_stats["pnl_structure"],
        "winner_vs_loser_section": feature_stats["winner_vs_loser"],
        "key_question": {
            "question": "Are p_h5, margin, ADX, or EV significantly different between winners and losers?",
            "tested_metrics": compare_metrics,
            "significant_metrics_p_lt_0_05": significant,
            "conclusion": "Significant difference detected" if significant else "No significant difference at this 17-trade sample size",
            "note": "two-sided exact permutation test on mean difference (wins vs losses)",
        },
        "limitations": [
            "spread_pct / drop_vs_signal / imbalance were not present in selected-fill logs, so kept as null",
            "required_expected_value was not available in this run's selected decision payload",
            "ADX/ATR/EMA/context metrics were post-computed from live 1m/15m/60m OHLCV captures",
        ],
    }
    dump_json(out_feature_stats_path, feature_stats)
    dump_json(out_deep_dive_path, deep_dive)
    print(json.dumps({"ok": True, "trade_rows": len(rows), "outputs": {"stageF_trade_deep_dive_json": str(out_deep_dive_path), "stageF_trade_table_csv": str(out_trade_table_path), "stageF_feature_stats_json": str(out_feature_stats_path)}, "key_question_significant_metrics": significant}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
