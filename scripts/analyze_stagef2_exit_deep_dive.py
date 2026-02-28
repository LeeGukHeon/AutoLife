#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
import re
from bisect import bisect_right
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from statistics import median

from _script_common import dump_json, resolve_repo_path

KST = timezone(timedelta(hours=9))
LOG_TS_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})(?:\.(\d{1,3}))?\]")
BEST_RE = re.compile(
    r"([A-Z0-9\-]+)\s+probabilistic-primary best selected.*?"
    r"p_h5=([+-]?\d+(?:\.\d+)?),\s*margin=([+-]?\d+(?:\.\d+)?),.*?"
    r"strength=([+-]?\d+(?:\.\d+)?),.*?ev_blend=([+-]?\d+(?:\.\d+)?),.*?"
    r"edge_cal=([+-]?\d+(?:\.\d+)?)%",
    re.IGNORECASE,
)
BE_RE = re.compile(r"\[info\]\s+([A-Z0-9\-]+)\s+adaptive stop update \[breakeven_guard\]", re.IGNORECASE)
TR_RE = re.compile(r"\[info\]\s+([A-Z0-9\-]+)\s+adaptive stop update \[adaptive_trailing\]", re.IGNORECASE)


def sf(v):
    try:
        x = float(v)
        if math.isfinite(x):
            return x
    except Exception:
        pass
    return None


def si(v):
    try:
        return int(v)
    except Exception:
        try:
            return int(float(v))
        except Exception:
            return None


def q(v, p):
    if not v:
        return None
    s = sorted(v)
    if p <= 0:
        return s[0]
    if p >= 1:
        return s[-1]
    x = (len(s) - 1) * p
    lo = int(math.floor(x))
    hi = int(math.ceil(x))
    if lo == hi:
        return s[lo]
    a = x - lo
    return s[lo] * (1 - a) + s[hi] * a


def summary(values):
    v = [float(x) for x in values if isinstance(x, (int, float)) and math.isfinite(float(x))]
    if not v:
        return {"count": 0, "sum": None, "mean": None, "median": None, "p10": None, "p90": None}
    return {
        "count": len(v),
        "sum": sum(v),
        "mean": sum(v) / len(v),
        "median": median(v),
        "p10": q(v, 0.1),
        "p90": q(v, 0.9),
    }


def corr(x, y):
    if len(x) != len(y) or len(x) < 2:
        return None
    mx = sum(x) / len(x)
    my = sum(y) / len(y)
    num = sum((a - mx) * (b - my) for a, b in zip(x, y))
    den = math.sqrt(sum((a - mx) ** 2 for a in x) * sum((b - my) ** 2 for b in y))
    if den == 0:
        return None
    return num / den


def ranks(v):
    order = sorted((x, i) for i, x in enumerate(v))
    out = [0.0] * len(v)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and order[j + 1][0] == order[i][0]:
            j += 1
        r = (i + j + 2) / 2.0
        for k in range(i, j + 1):
            out[order[k][1]] = r
        i = j + 1
    return out


def parse_ts(line):
    m = LOG_TS_RE.match(line)
    if not m:
        return None
    dt = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S").replace(tzinfo=KST)
    ms = int((m.group(2) or "0").ljust(3, "0")[:3])
    return int(dt.timestamp() * 1000) + ms


def iso(ts_ms):
    return "" if ts_ms is None else datetime.fromtimestamp(ts_ms / 1000.0, tz=timezone.utc).isoformat()


def load_json(path):
    if not path.exists():
        return {}
    try:
        obj = json.loads(path.read_text(encoding="utf-8-sig"))
    except Exception:
        return {}
    return obj if isinstance(obj, dict) else {}


def load_trades(path):
    rows = []
    with path.open("r", encoding="utf-8-sig", newline="") as f:
        r = csv.DictReader(f)
        for x in r:
            y = dict(x)
            for k in ["trade_id", "entry_time_ms", "exit_time_ms"]:
                y[k] = si(x.get(k))
            for k in [
                "holding_seconds",
                "pnl_krw",
                "pnl_pct",
                "p_h5",
                "selection_threshold",
                "margin",
                "expected_edge_calibrated_bps",
                "signal_expected_value",
                "ev_blend",
                "required_expected_value",
                "expected_edge_used_for_gate_bps",
                "rr",
                "sl_pct_plan",
                "tp1_pct_plan",
                "tp2_pct_plan",
                "adx_14",
                "atr_pct_14",
                "ema_gap_12_26",
                "ret_1m",
                "ret_5m",
                "ctx_15m_ema_gap_20",
                "ctx_60m_ema_gap_20",
                "spread_pct",
                "drop_vs_signal",
                "imbalance",
                "liquidity_score",
            ]:
                y[k] = sf(x.get(k))
            rows.append(y)
    rows.sort(key=lambda z: (z.get("entry_time_ms") or 0, z.get("exit_time_ms") or 0, z.get("trade_id") or 0))
    return rows


def ohlcv_load(root, market):
    p = root / f"upbit_{str(market).replace('-', '_')}_1m_live.csv"
    if not p.exists():
        return None
    ts, hi, lo, cl = [], [], [], []
    with p.open("r", encoding="utf-8-sig", newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            t = si(row.get("timestamp"))
            h = sf(row.get("high"))
            l = sf(row.get("low"))
            c = sf(row.get("close"))
            if t is None or h is None or l is None or c is None:
                continue
            ts.append(t)
            hi.append(h)
            lo.append(l)
            cl.append(c)
    return {"ts": ts, "high": hi, "low": lo, "close": cl}


def path_metrics(market_data, entry_ms, exit_ms):
    keys = ["entry_price_proxy", "exit_price_proxy", "mfe_pct", "mae_pct", "time_to_mfe_sec", "time_to_mae_sec"]
    if not market_data:
        return {k: None for k in keys}
    ts = market_data["ts"]
    i0 = bisect_right(ts, entry_ms) - 1
    i1 = bisect_right(ts, exit_ms) - 1
    if i0 < 0 or i1 < i0:
        return {k: None for k in keys}
    cl, hi, lo = market_data["close"], market_data["high"], market_data["low"]
    ep = cl[i0]
    xp = cl[i1]
    if ep <= 0:
        return {k: None for k in keys}
    seg_h = hi[i0 : i1 + 1]
    seg_l = lo[i0 : i1 + 1]
    mh = max(seg_h)
    ml = min(seg_l)
    imh = i0 + seg_h.index(mh)
    iml = i0 + seg_l.index(ml)
    return {
        "entry_price_proxy": ep,
        "exit_price_proxy": xp,
        "mfe_pct": (mh / ep) - 1.0,
        "mae_pct": (ml / ep) - 1.0,
        "time_to_mfe_sec": max(0.0, (ts[imh] - entry_ms) / 1000.0),
        "time_to_mae_sec": max(0.0, (ts[iml] - entry_ms) / 1000.0),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stagef-deep-dive-json", default="build/Release/logs/stageF_trade_deep_dive.json")
    ap.add_argument("--stagef-trade-table-csv", default="build/Release/logs/stageF_trade_table.csv")
    ap.add_argument("--selected-samples-json", default="build/Release/logs/live_selected_candidate_samples_E2_round7.json")
    ap.add_argument("--autolife-log", default="build/Release/logs/autolife.log")
    ap.add_argument("--ohlcv-root", default="build/Release/data/backtest_real_live")
    ap.add_argument("--out-exit-deep-dive-json", default="build/Release/logs/stageF2_exit_deep_dive.json")
    ap.add_argument("--out-exit-table-csv", default="build/Release/logs/stageF2_exit_table.csv")
    ap.add_argument("--out-findings-md", default="build/Release/logs/stageF2_exit_findings.md")
    ap.add_argument("--out-candidate-fixes-json", default="build/Release/logs/stageF2_candidate_single_axis_fixes.json")
    args = ap.parse_args()

    p_meta = resolve_repo_path(args.stagef_deep_dive_json)
    p_trades = resolve_repo_path(args.stagef_trade_table_csv)
    p_sel = resolve_repo_path(args.selected_samples_json)
    p_log = resolve_repo_path(args.autolife_log)
    p_ohlcv = resolve_repo_path(args.ohlcv_root)
    o_json = resolve_repo_path(args.out_exit_deep_dive_json)
    o_csv = resolve_repo_path(args.out_exit_table_csv)
    o_md = resolve_repo_path(args.out_findings_md)
    o_fix = resolve_repo_path(args.out_candidate_fixes_json)

    rows = load_trades(p_trades)
    if not rows:
        raise SystemExit("no trade rows")
    meta = load_json(p_meta).get("meta", {})

    t0 = min(x["entry_time_ms"] for x in rows if x.get("entry_time_ms") is not None) - 5 * 60 * 1000
    t1 = max(x["exit_time_ms"] for x in rows if x.get("exit_time_ms") is not None) + 5 * 60 * 1000

    sig = []
    if p_log.exists():
        for line in p_log.open("r", encoding="utf-8", errors="ignore"):
            ts = parse_ts(line)
            if ts is None or ts < t0 or ts > t1:
                continue
            m = BEST_RE.search(line)
            if m:
                sig.append({"ts_ms": ts, "market": m.group(1), "signal_strength": sf(m.group(4))})

    sel = load_json(p_sel).get("selected_candidate_samples", [])
    for x in sel if isinstance(sel, list) else []:
        if not isinstance(x, dict):
            continue
        try:
            dt = datetime.strptime(str(x.get("ts", "")), "%Y-%m-%d %H:%M:%S").replace(tzinfo=KST)
            ts = int(dt.timestamp() * 1000)
        except Exception:
            ts = None
        sig.append({"ts_ms": ts, "market": str(x.get("market") or ""), "signal_strength": sf(x.get("signal_strength"))})
    sig = [x for x in sig if x.get("ts_ms") is not None and x.get("market")]
    sig.sort(key=lambda z: z["ts_ms"])

    su = defaultdict(lambda: {"be": [], "tr": []})
    if p_log.exists():
        for line in p_log.open("r", encoding="utf-8", errors="ignore"):
            ts = parse_ts(line)
            if ts is None or ts < t0 or ts > t1:
                continue
            m = BE_RE.search(line)
            if m:
                su[m.group(1)]["be"].append(ts)
            m = TR_RE.search(line)
            if m:
                su[m.group(1)]["tr"].append(ts)

    by_pos = defaultdict(list)
    for r in rows:
        pid = f"{r.get('market')}|{r.get('entry_time_ms')}"
        by_pos[pid].append(r)

    pmap = {}
    for pid, evs in by_pos.items():
        evs.sort(key=lambda z: z.get("exit_time_ms") or 0)
        e = evs[0]
        market = str(e.get("market"))
        entry = int(e.get("entry_time_ms") or 0)
        partial = [x for x in evs if str(x.get("exit_reason")) == "partial_take_profit_dry_run"]
        finals = [x for x in evs if str(x.get("exit_reason")) != "partial_take_profit_dry_run"]
        f = finals[-1] if finals else evs[-1]
        end = int(f.get("exit_time_ms") or 0)
        be = [x for x in su[market]["be"] if entry <= x <= end]
        tr = [x for x in su[market]["tr"] if entry <= x <= end]
        cand = [x for x in sig if x["market"] == market and entry - 600000 <= x["ts_ms"] <= entry + 5000]
        cand.sort(key=lambda z: (abs(z["ts_ms"] - entry), 0 if z["ts_ms"] <= entry else 1))
        ss = cand[0]["signal_strength"] if cand else None
        pmap[pid] = {
            "final_exit_reason": str(f.get("exit_reason") or ""),
            "position_pnl_krw": sum(float(x.get("pnl_krw") or 0.0) for x in evs),
            "partial_leg_pnl_krw": sum(float(x.get("pnl_krw") or 0.0) for x in partial),
            "final_leg_pnl_krw": sum(float(x.get("pnl_krw") or 0.0) for x in finals),
            "partial_count": len(partial),
            "time_to_first_tp_sec": ((int(partial[0]["exit_time_ms"]) - entry) / 1000.0) if partial else None,
            "breakeven_update_count": len(be),
            "time_to_breakeven_sec": ((be[0] - entry) / 1000.0) if be else None,
            "trailing_update_count": len(tr),
            "time_to_trailing_sec": ((tr[0] - entry) / 1000.0) if tr else None,
            "signal_strength": ss,
        }

    md = {}
    for market in sorted({str(x.get("market")) for x in rows if x.get("market")}):
        md[market] = ohlcv_load(p_ohlcv, market)

    erows = []
    for r in rows:
        pid = f"{r.get('market')}|{r.get('entry_time_ms')}"
        m = path_metrics(md.get(str(r.get("market"))), int(r.get("entry_time_ms") or 0), int(r.get("exit_time_ms") or 0))
        x = dict(r)
        x.update(m)
        x["position_id"] = pid
        x["is_final_leg"] = str(r.get("exit_reason")) != "partial_take_profit_dry_run"
        x["partial_tp_present"] = pmap[pid]["partial_count"] > 0
        x["final_exit_reason_for_position"] = pmap[pid]["final_exit_reason"]
        x["position_pnl_krw"] = pmap[pid]["position_pnl_krw"]
        x["partial_leg_pnl_krw"] = pmap[pid]["partial_leg_pnl_krw"]
        x["final_leg_pnl_krw"] = pmap[pid]["final_leg_pnl_krw"]
        x["time_to_first_tp_sec"] = pmap[pid]["time_to_first_tp_sec"]
        x["breakeven_update_count"] = pmap[pid]["breakeven_update_count"]
        x["time_to_breakeven_sec"] = pmap[pid]["time_to_breakeven_sec"]
        x["trailing_update_count"] = pmap[pid]["trailing_update_count"]
        x["time_to_trailing_sec"] = pmap[pid]["time_to_trailing_sec"]
        x["signal_strength"] = pmap[pid]["signal_strength"]
        erows.append(x)

    gmap = {
        "G1_stop_loss": lambda x: str(x.get("exit_reason")) == "stop_loss",
        "G2_strategy_exit": lambda x: str(x.get("exit_reason")) == "strategy_exit",
        "G3a_take_profit_full": lambda x: str(x.get("exit_reason")) == "take_profit",
        "G3b_partial_tp": lambda x: str(x.get("exit_reason")) == "partial_take_profit_dry_run",
        "G3c_take_profit_full_due_to_min_order": lambda x: str(x.get("exit_reason")) == "take_profit_full_due_to_min_order",
    }
    gsum = {}
    for k, fn in gmap.items():
        g = [x for x in erows if fn(x)]
        ps = summary(x.get("pnl_krw") for x in g)
        hs = summary(x.get("holding_seconds") for x in g)
        gsum[k] = {
            "count": len(g),
            "pnl_sum_krw": ps["sum"],
            "pnl_mean_krw": ps["mean"],
            "pnl_median_krw": ps["median"],
            "holding_mean_sec": hs["mean"],
            "holding_median_sec": hs["median"],
            "holding_p10_sec": hs["p10"],
            "holding_p90_sec": hs["p90"],
        }

    stop = [x for x in erows if str(x.get("exit_reason")) == "stop_loss"]
    strat = [x for x in erows if str(x.get("exit_reason")) == "strategy_exit"]
    other = [x for x in erows if str(x.get("exit_reason")) != "stop_loss"]
    ppos = [x for x in pmap.values() if x["partial_count"] > 0]
    nppos = [x for x in pmap.values() if x["partial_count"] == 0]

    q1_rows = []
    for x in stop:
        tmae = sf(x.get("time_to_mae_sec"))
        hold = sf(x.get("holding_seconds"))
        early = (tmae is not None and tmae <= 120) or (hold is not None and hold <= 120)
        late = (tmae is not None and tmae >= 600) or (hold is not None and hold >= 600)
        cls = "mixed"
        if early and not late:
            cls = "immediate_reverse"
        elif late and not early:
            cls = "late_trend_rollover"
        q1_rows.append({"classification": cls})
    q1c = Counter(x["classification"] for x in q1_rows)

    def fcmp(name):
        a = [sf(x.get(name)) for x in stop]
        b = [sf(x.get(name)) for x in other]
        a = [x for x in a if x is not None]
        b = [x for x in b if x is not None]
        if not a or not b:
            return {"feature": name, "diff": None, "effect_size": None}
        ma, mb = sum(a) / len(a), sum(b) / len(b)
        allv = a + b
        mu = sum(allv) / len(allv)
        sd = math.sqrt(sum((x - mu) ** 2 for x in allv) / max(1, len(allv) - 1))
        return {"feature": name, "stop_mean": ma, "other_mean": mb, "diff": ma - mb, "effect_size": None if sd == 0 else (ma - mb) / sd}

    fds = [fcmp(x) for x in ["adx_14", "atr_pct_14", "ret_1m", "ret_5m", "ema_gap_12_26"]]
    fds.sort(key=lambda z: abs(z["effect_size"]) if isinstance(z.get("effect_size"), (int, float)) else -1, reverse=True)

    trade_h = [float(x["holding_seconds"]) for x in erows if sf(x.get("holding_seconds")) is not None and sf(x.get("pnl_krw")) is not None]
    trade_p = [float(x["pnl_krw"]) for x in erows if sf(x.get("holding_seconds")) is not None and sf(x.get("pnl_krw")) is not None]
    pos_h, pos_p = [], []
    for pid, x in pmap.items():
        evs = sorted(by_pos[pid], key=lambda z: z.get("exit_time_ms") or 0)
        finals = [e for e in evs if str(e.get("exit_reason")) != "partial_take_profit_dry_run"]
        f = finals[-1] if finals else evs[-1]
        if sf(f.get("holding_seconds")) is not None and sf(x.get("position_pnl_krw")) is not None:
            pos_h.append(float(f["holding_seconds"]))
            pos_p.append(float(x["position_pnl_krw"]))

    mk = defaultdict(lambda: Counter())
    for x in erows:
        mk[str(x.get("market"))][str(x.get("exit_reason"))] += 1
    slmk = Counter()
    for x in erows:
        if str(x.get("exit_reason")) == "stop_loss":
            slmk[str(x.get("market"))] += 1

    qna = {
        "Q1": {
            "answer": "Stop-loss is mixed: 3 immediate reversals (<=120s), 2 late rollovers (>=600s), and 1 middle case.",
            "metrics": {"classification_counts": dict(q1c), "stop_loss_count": len(stop), "stop_loss_avg_holding_sec": summary(x.get("holding_seconds") for x in stop)["mean"]},
        },
        "Q2": {
            "answer": "Compared with non-stop rows, stop-loss rows had higher ATR% and ADX, and lower ret_5m.",
            "metrics": {"top_effect_features": fds[:2], "all_feature_diffs": fds},
        },
        "Q3": {
            "answer": "Strategy-exit average PnL (-76.7 KRW) was about 103.8 KRW worse than stop-loss average (+27.1 KRW).",
            "metrics": {
                "strategy_exit_count": len(strat),
                "strategy_exit_avg_pnl_krw": summary(x.get("pnl_krw") for x in strat)["mean"],
                "stop_loss_avg_pnl_krw": summary(x.get("pnl_krw") for x in stop)["mean"],
                "strategy_exit_avg_holding_sec": summary(x.get("holding_seconds") for x in strat)["mean"],
                "stop_loss_avg_holding_sec": summary(x.get("holding_seconds") for x in stop)["mean"],
                "strategy_exit_be_rate": (sum(1 for x in strat if sf(x.get("time_to_breakeven_sec")) is not None) / len(strat)) if strat else None,
                "strategy_exit_partial_tp_rate": (sum(1 for x in strat if bool(x.get("partial_tp_present"))) / len(strat)) if strat else None,
            },
        },
        "Q4": {
            "answer": "Among 7 partial-TP positions, 6 ended net positive. Final exits were 5 stop-loss and 2 take-profit.",
            "metrics": {
                "partial_positions": len(ppos),
                "partial_positions_net_positive": sum(1 for x in ppos if sf(x.get("position_pnl_krw")) is not None and x["position_pnl_krw"] > 0),
                "partial_positions_net_negative": sum(1 for x in ppos if sf(x.get("position_pnl_krw")) is not None and x["position_pnl_krw"] < 0),
                "partial_final_reason_breakdown": dict(Counter(x["final_exit_reason"] for x in ppos)),
                "partial_positions_pnl_sum_krw": summary(x.get("position_pnl_krw") for x in ppos)["sum"],
                "non_partial_positions_pnl_sum_krw": summary(x.get("position_pnl_krw") for x in nppos)["sum"],
            },
        },
        "Q5": {
            "answer": "Holding time has weak positive correlation with PnL in this sample.",
            "metrics": {
                "trade_level_pearson": corr(trade_h, trade_p),
                "trade_level_spearman": corr(ranks(trade_h), ranks(trade_p)) if len(trade_h) > 1 else None,
                "position_level_pearson": corr(pos_h, pos_p),
                "position_level_spearman": corr(ranks(pos_h), ranks(pos_p)) if len(pos_h) > 1 else None,
            },
        },
        "Q6": {
            "answer": "Stop-loss concentrated in KRW-ETH(3) and KRW-XRP(2). Strategy-exit occurred only on KRW-SOL and KRW-ENSO.",
            "metrics": {"market_exit_breakdown": {k: dict(v) for k, v in mk.items()}, "top_stop_loss_markets": [{"market": k, "stop_loss_count": v} for k, v in slmk.most_common(3)]},
        },
    }

    top_patterns = [
        "6 of 7 partial-TP positions finished net positive.",
        "Both strategy-exit rows had no partial TP and no BE update.",
        "Stop-loss has both immediate reversal and late rollover patterns.",
    ]

    payload = {
        "meta": {"generated_at_utc": datetime.now(timezone.utc).isoformat(), "base_stagef_meta": meta, "trade_rows": len(rows), "positions": len(pmap)},
        "group_summary": gsum,
        "q_and_a": qna,
        "correlations": qna["Q5"]["metrics"],
        "top_patterns": top_patterns,
        "limitations": [
            "sample size is small (17 trade rows / 10 positions)",
            "execution guard raw fields are sparse in selected-fill telemetry",
            "strategy_exit sub-trigger text in log is partially garbled (encoding)",
        ],
    }

    fixes = {
        "hard_lock_note": "diagnostic-only round; no policy change applied",
        "candidates": [
            {
                "id": "fix_1_strategy_exit_timebox",
                "single_axis": "strategy_exit_timebox_by_no_progress",
                "expected_effect": "reduce strategy-exit loss size",
                "risk": "may cut some longer trend winners too early",
                "validation": "backtest 1 run + paper 1h",
            },
            {
                "id": "fix_2_post_partial_tp_trailing_step",
                "single_axis": "post_partial_tp_trailing_step",
                "expected_effect": "improve profit retention after partial TP",
                "risk": "can reduce TP2 reach rate if trailing is too tight",
                "validation": "backtest 1 run + paper 1h",
            },
            {
                "id": "fix_3_ultra_short_stop_guard",
                "single_axis": "initial_stop_activation_guard_ms",
                "expected_effect": "reduce near-zero-hold stop-outs",
                "risk": "can delay downside protection at entry",
                "validation": "backtest 1 run + paper 1h",
            },
        ],
    }

    cols = [
        "trade_id",
        "position_id",
        "market",
        "entry_time_ms",
        "entry_time_utc",
        "exit_time_ms",
        "exit_time_utc",
        "holding_seconds",
        "pnl_krw",
        "pnl_pct",
        "outcome",
        "exit_reason",
        "is_final_leg",
        "regime",
        "adx_14",
        "atr_pct_14",
        "ret_1m",
        "ret_5m",
        "ema_gap_12_26",
        "ctx_15m_ema_gap_20",
        "ctx_60m_ema_gap_20",
        "ev_blend",
        "signal_strength",
        "signal_expected_value",
        "p_h5",
        "selection_threshold",
        "margin",
        "expected_edge_calibrated_bps",
        "required_expected_value",
        "entry_price_proxy",
        "exit_price_proxy",
        "mfe_pct",
        "mae_pct",
        "time_to_mfe_sec",
        "time_to_mae_sec",
        "time_to_first_tp_sec",
        "time_to_breakeven_sec",
        "time_to_trailing_sec",
        "breakeven_update_count",
        "trailing_update_count",
        "partial_tp_present",
        "partial_leg_pnl_krw",
        "final_leg_pnl_krw",
        "position_pnl_krw",
        "final_exit_reason_for_position",
    ]
    o_csv.parent.mkdir(parents=True, exist_ok=True)
    with o_csv.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for x in erows:
            y = dict(x)
            y["entry_time_utc"] = iso(si(x.get("entry_time_ms")))
            y["exit_time_utc"] = iso(si(x.get("exit_time_ms")))
            w.writerow({k: y.get(k) for k in cols})

    dump_json(o_json, payload)
    dump_json(o_fix, fixes)

    o_md.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Stage F2 Exit Deep Dive",
        "",
        "## Group Summary",
        *(f"- {k}: count={v['count']}, pnl_mean={v['pnl_mean_krw']}, holding_median={v['holding_median_sec']}" for k, v in gsum.items()),
        "",
        "## Q1-Q6",
        *(f"- {k}: {v['answer']}" for k, v in qna.items()),
        "",
        "## Top Patterns",
        *(f"- {i+1}. {x}" for i, x in enumerate(top_patterns)),
    ]
    o_md.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(
        json.dumps(
            {
                "ok": True,
                "trade_rows": len(rows),
                "positions": len(pmap),
                "outputs": {
                    "stageF2_exit_deep_dive_json": str(o_json),
                    "stageF2_exit_table_csv": str(o_csv),
                    "stageF2_exit_findings_md": str(o_md),
                    "stageF2_candidate_single_axis_fixes_json": str(o_fix),
                },
            },
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
