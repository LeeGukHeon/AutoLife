#!/usr/bin/env python3
from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import pathlib
import re
from dataclasses import dataclass
from datetime import datetime
from typing import Any, Dict, Iterable, List, Optional, Tuple

from _script_common import dump_json, resolve_repo_path


def now_iso() -> str:
    return datetime.now().astimezone().isoformat()


def to_int(v: Any, d: int = 0) -> int:
    try:
        return int(v)
    except Exception:
        try:
            return int(float(v))
        except Exception:
            return int(d)


def to_float(v: Any, d: float = 0.0) -> float:
    try:
        out = float(v)
        return out if math.isfinite(out) else float(d)
    except Exception:
        return float(d)


def qtile(values: List[float], q: float) -> float:
    if not values:
        return 0.0
    xs = sorted(float(x) for x in values)
    if len(xs) == 1:
        return float(xs[0])
    qq = min(1.0, max(0.0, float(q)))
    pos = qq * (len(xs) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(xs[lo])
    frac = pos - lo
    return float(xs[lo] * (1.0 - frac) + xs[hi] * frac)


def mean(values: List[float]) -> float:
    return float(sum(values) / len(values)) if values else 0.0


def norm_market(v: Any) -> str:
    s = str(v or "").strip().upper()
    if not s:
        return ""
    if "-" in s:
        return s
    if "_" in s:
        return s.replace("_", "-")
    return s


def norm_vol_bucket(v: Any) -> str:
    s = str(v or "").strip().upper()
    if not s:
        return "NONE"
    if s in ("LOW", "VOL_LOW", "PCT_LOW"):
        return "LOW"
    if s in ("MID", "MIDDLE", "VOL_MID", "PCT_MID"):
        return "MID"
    if s in ("HIGH", "VOL_HIGH", "PCT_HIGH"):
        return "HIGH"
    if s in ("NONE", "UNKNOWN", "N/A"):
        return "NONE"
    return s


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Evaluate Stage D shadow edge (D2-0 + D2-1 preview).")
    p.add_argument("--shadow-jsonl", action="append", required=True, help="repeatable; globs allowed")
    p.add_argument("--ohlcv-root", default="data/backtest_real")
    p.add_argument("--horizon-bars", type=int, default=5)
    p.add_argument("--label-cost-bps", type=float, default=12.0)
    p.add_argument("--out-dir", default="build/Release/logs/stageD_v2_shadow_eval")
    p.add_argument("--max-rows", type=int, default=0)
    p.add_argument("--config-path", default="build/Release/config/config.json")
    p.add_argument("--bundle-path", default="")
    p.add_argument("--market-top-n", type=int, default=10)
    p.add_argument("--cell-top-n", type=int, default=10)
    return p.parse_args(argv)


def expand_shadow_paths(raw_values: Iterable[str]) -> List[pathlib.Path]:
    out: List[pathlib.Path] = []
    seen = set()
    repo = resolve_repo_path(".")
    for token in raw_values:
        s = str(token or "").strip()
        if not s:
            continue
        if any(ch in s for ch in "*?[]"):
            for m in sorted(repo.glob(s)):
                if m.is_file():
                    k = str(m.resolve()).lower()
                    if k not in seen:
                        out.append(m.resolve())
                        seen.add(k)
            continue
        p = resolve_repo_path(s)
        if p.is_file():
            k = str(p.resolve()).lower()
            if k not in seen:
                out.append(p.resolve())
                seen.add(k)
    return out


def parse_market_from_csv_name(path: pathlib.Path) -> str:
    m = re.match(r"^upbit_([A-Za-z0-9]+)_([A-Za-z0-9]+)_1m_.*\.csv$", path.name, re.IGNORECASE)
    if m:
        return norm_market(f"{m.group(1)}-{m.group(2)}")
    parent = str(path.parent.name or "").strip()
    if "1m" in path.name.lower() and "-" in parent:
        return norm_market(parent)
    return ""


def discover_ohlcv_map(root: pathlib.Path) -> Dict[str, pathlib.Path]:
    out: Dict[str, pathlib.Path] = {}
    if not root.exists():
        return out
    for p in sorted(root.rglob("*.csv")):
        market = parse_market_from_csv_name(p)
        if not market:
            continue
        if market not in out:
            out[market] = p.resolve()
            continue
        cur = out[market]
        if "_1m_" in p.name.lower() and "_1m_" not in cur.name.lower():
            out[market] = p.resolve()
    return out


@dataclass
class OhlcvSeries:
    market: str
    ts: List[int]
    close: List[float]
    source_path: pathlib.Path


def load_ohlcv(path: pathlib.Path, market: str) -> OhlcvSeries:
    ts: List[int] = []
    cl: List[float] = []
    with path.open("r", encoding="utf-8-sig", newline="") as fp:
        rd = csv.DictReader(fp)
        for row in rd:
            if not isinstance(row, dict):
                continue
            t = to_int(row.get("timestamp", row.get("ts_ms", row.get("ts", 0))), 0)
            c = to_float(row.get("close", float("nan")), float("nan"))
            if t <= 0 or not math.isfinite(c) or c <= 0.0:
                continue
            ts.append(int(t))
            cl.append(float(c))
    if not ts:
        return OhlcvSeries(market=market, ts=[], close=[], source_path=path)
    idx = sorted(range(len(ts)), key=lambda i: ts[i])
    return OhlcvSeries(
        market=market,
        ts=[int(ts[i]) for i in idx],
        close=[float(cl[i]) for i in idx],
        source_path=path,
    )

def resolve_bundle_path(config_payload: Dict[str, Any], bundle_path_arg: str) -> str:
    ov = str(bundle_path_arg or "").strip()
    if ov:
        return str(resolve_repo_path(ov))
    trading = config_payload.get("trading", {}) if isinstance(config_payload, dict) else {}
    if not isinstance(trading, dict):
        trading = {}
    raw = str(trading.get("probabilistic_runtime_bundle_path", "")).strip()
    if not raw:
        return ""
    return str(resolve_repo_path(raw))


def pick_edge_bps(row: Dict[str, Any], key_bps: str, key_pct: str) -> float:
    if key_bps in row:
        return to_float(row.get(key_bps, 0.0))
    if key_pct in row:
        return to_float(row.get(key_pct, 0.0)) * 10000.0
    return 0.0


def bool_or_none(v: Any) -> Optional[bool]:
    if isinstance(v, bool):
        return bool(v)
    if v is None:
        return None
    s = str(v).strip().lower()
    if s in ("1", "true", "y", "yes"):
        return True
    if s in ("0", "false", "n", "no"):
        return False
    return None


def agg(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    edges = [to_float(x.get("shadow_edge_bps", 0.0)) for x in rows]
    cnt = len(edges)
    pos = sum(1 for v in edges if v > 0.0)
    return {
        "count_with_t5": int(cnt),
        "pnl_sum_bps": round(float(sum(edges)), 6),
        "mean_edge_bps": round(mean(edges), 6) if cnt else 0.0,
        "median_edge_bps": round(qtile(edges, 0.5), 6) if cnt else 0.0,
        "p10_edge_bps": round(qtile(edges, 0.1), 6) if cnt else 0.0,
        "p90_edge_bps": round(qtile(edges, 0.9), 6) if cnt else 0.0,
        "positive_rate": round(float(pos) / float(max(1, cnt)), 6),
    }


def build_decision_preview(report: Dict[str, Any], eval_rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    counts = report.get("counts", {}) if isinstance(report, dict) else {}
    overall = report.get("overall", {}) if isinstance(report, dict) else {}
    by_vol = report.get("by_vol_bucket_pct", {}) if isinstance(report, dict) else {}

    with_t5 = to_int(counts.get("with_t5", 0), 0)
    mean_edge = to_float(overall.get("mean_edge_bps", 0.0), 0.0)
    pos_rate = to_float(overall.get("positive_rate", 0.0), 0.0)

    exec_true = 0
    exec_total = 0
    for r in eval_rows:
        b = bool_or_none(r.get("would_pass_execution_guard"))
        if b is None:
            continue
        exec_total += 1
        if b:
            exec_true += 1
    exec_rate = float(exec_true) / float(exec_total) if exec_total > 0 else 0.0

    segs: List[Tuple[str, float, int, float]] = []
    if isinstance(by_vol, dict):
        for bucket, payload in by_vol.items():
            if not isinstance(payload, dict):
                continue
            segs.append(
                (
                    str(bucket),
                    to_float(payload.get("mean_edge_bps", 0.0)),
                    to_int(payload.get("count_with_t5", 0), 0),
                    to_float(payload.get("positive_rate", 0.0)),
                )
            )
    segs.sort(key=lambda x: (-x[1], -x[2], x[0]))

    decision = "GO_COLLECT_MORE_SHADOW"
    rationale: List[str] = []
    if with_t5 < 300:
        decision = "GO_COLLECT_MORE_SHADOW"
        rationale.append("with_t5 sample size is still limited for robust single-rule selection.")
    else:
        min_cnt = max(30, int(round(with_t5 * 0.03)))
        pos_segments = [x for x in segs if x[2] >= min_cnt and x[1] > 0.0 and x[3] >= 0.55]
        if pos_segments:
            top = pos_segments[0]
            if top[0] in ("LOW", "MID"):
                decision = "GO_MR_SINGLE_RULE"
                rationale.append(
                    f"positive cluster detected in vol bucket {top[0]} (mean_edge_bps={top[1]:.3f}, count={top[2]})."
                )
            else:
                decision = "GO_MICRO_SCALP_SINGLE_RULE"
                rationale.append(
                    f"positive cluster detected in bucket {top[0]}; choose one micro entry rule."
                )
        else:
            if mean_edge < 0.0 and pos_rate < 0.5:
                decision = "NO_GO_KEEP_OFF"
                rationale.append("overall shadow edge is negative with sub-50% positive rate.")
            elif exec_total > 0 and exec_rate >= 0.9 and -2.0 <= mean_edge <= 2.0:
                decision = "GO_MICRO_SCALP_SINGLE_RULE"
                rationale.append("execution guard pass rate is high and edge is near zero; micro-scalp probe is plausible.")
            else:
                decision = "GO_COLLECT_MORE_SHADOW"
                rationale.append("no stable positive segment with sufficient sample detected yet.")

    return {
        "decision": decision,
        "with_t5": int(with_t5),
        "overall_mean_edge_bps": round(mean_edge, 6),
        "overall_positive_rate": round(pos_rate, 6),
        "execution_guard_pass_rate": round(exec_rate, 6),
        "rationale": rationale,
    }


def build_next_action_plan(choice: Dict[str, Any]) -> str:
    decision = str(choice.get("decision", "")).strip()
    metrics = choice.get("metrics", {}) if isinstance(choice, dict) else {}
    if not isinstance(metrics, dict):
        metrics = {}

    def metric(name: str, fallback: Any) -> Any:
        if name in metrics:
            return metrics.get(name)
        return choice.get(name, fallback)

    lines: List[str] = []
    lines.append("# Stage D v2 Next Action Plan")
    lines.append("")
    lines.append(f"- decision: `{decision}`")
    lines.append("- rule: single-axis only in next round")
    lines.append("")
    lines.append("## Evidence")
    lines.append(f"- with_t5: {metric('with_t5', 0)}")
    lines.append(f"- overall_mean_edge_bps: {metric('overall_mean_edge_bps', 0.0)}")
    lines.append(f"- overall_positive_rate: {metric('overall_positive_rate', 0.0)}")
    lines.append(f"- execution_guard_pass_rate: {metric('execution_guard_pass_rate', 0.0)}")
    lines.append("")
    lines.append("## Next Round")
    if decision == "GO_MR_SINGLE_RULE":
        lines.append("- Add exactly one RANGING mean-reversion rule.")
        lines.append("- Keep all existing gates/thresholds unchanged.")
        lines.append("- Validate with: Quarantine x1 -> Paper 1h -> Paper 4h.")
    elif decision == "GO_MICRO_SCALP_SINGLE_RULE":
        lines.append("- Add exactly one micro-scalp rule for RANGING.")
        lines.append("- Keep risk budget and semantics lock unchanged.")
        lines.append("- Validate with: Quarantine x1 -> Paper 1h -> Paper 4h.")
    elif decision == "NO_GO_KEEP_OFF":
        lines.append("- Keep RANGING real entry OFF.")
        lines.append("- Continue shadow-only collection.")
        lines.append("- Re-evaluate after additional shadow accumulation.")
    else:
        lines.append("- Collect more shadow data without behavior change.")
        lines.append("- Re-run D2-0 evaluator after sample growth.")
    lines.append("")
    lines.append("## Constraints")
    lines.append("- No strategy proliferation.")
    lines.append("- No multi-axis tuning.")
    lines.append("- allow_live_orders=false remains fixed.")
    return "\n".join(lines) + "\n"

def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    out_dir = resolve_repo_path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    shadow_paths = expand_shadow_paths(args.shadow_jsonl or [])
    if not shadow_paths:
        raise RuntimeError("No shadow jsonl inputs resolved.")

    ohlcv_root = resolve_repo_path(args.ohlcv_root)
    ohlcv_map = discover_ohlcv_map(ohlcv_root)
    if not ohlcv_map:
        raise RuntimeError(f"No 1m ohlcv csv discovered under: {ohlcv_root}")

    config_path = resolve_repo_path(args.config_path)
    config_payload: Dict[str, Any] = {}
    if config_path.exists():
        loaded = json.loads(config_path.read_text(encoding="utf-8-sig"))
        if isinstance(loaded, dict):
            config_payload = loaded
    trading_cfg = config_payload.get("trading", {}) if isinstance(config_payload, dict) else {}
    if not isinstance(trading_cfg, dict):
        trading_cfg = {}
    bundle_path = resolve_bundle_path(config_payload, args.bundle_path)

    horizon = max(1, int(args.horizon_bars))
    label_cost_bps = float(args.label_cost_bps)
    max_rows = max(0, int(args.max_rows))

    cache: Dict[str, OhlcvSeries] = {}

    def series_for(market: str) -> Optional[OhlcvSeries]:
        key = norm_market(market)
        if not key:
            return None
        if key in cache:
            return cache[key]
        p = ohlcv_map.get(key)
        if p is None:
            return None
        cache[key] = load_ohlcv(p, key)
        return cache[key]

    total_rows = 0
    missing_market = 0
    missing_entry = 0
    missing_t5 = 0
    with_t5 = 0
    quality_selection_true = 0
    quality_selection_total = 0
    exec_true = 0
    exec_total = 0
    eval_rows: List[Dict[str, Any]] = []

    for shadow_path in shadow_paths:
        with shadow_path.open("r", encoding="utf-8", errors="ignore") as fp:
            for raw in fp:
                if max_rows > 0 and total_rows >= max_rows:
                    break
                line = str(raw).strip()
                if not line:
                    continue
                try:
                    payload = json.loads(line)
                except Exception:
                    continue
                if not isinstance(payload, dict):
                    continue
                total_rows += 1

                f = bool_or_none(payload.get("would_pass_quality_selection"))
                if f is not None:
                    quality_selection_total += 1
                    if f:
                        quality_selection_true += 1
                e = bool_or_none(payload.get("would_pass_execution_guard"))
                if e is not None:
                    exec_total += 1
                    if e:
                        exec_true += 1

                market = norm_market(payload.get("market", ""))
                ts_ms = to_int(payload.get("ts_ms", payload.get("ts", 0)), 0)
                ser = series_for(market)
                if ser is None or not ser.ts:
                    missing_market += 1
                    continue

                entry_idx = bisect.bisect_right(ser.ts, ts_ms) - 1
                if entry_idx < 0:
                    missing_entry += 1
                    continue
                exit_idx = entry_idx + horizon
                if exit_idx >= len(ser.ts):
                    missing_t5 += 1
                    continue

                entry_ts = int(ser.ts[entry_idx])
                exit_ts = int(ser.ts[exit_idx])
                entry_close = float(ser.close[entry_idx])
                exit_close = float(ser.close[exit_idx])
                if not (entry_close > 0.0 and exit_close > 0.0):
                    missing_t5 += 1
                    continue

                ret_bps = ((exit_close / entry_close) - 1.0) * 10000.0
                shadow_edge_bps = ret_bps - label_cost_bps

                edge_cal_bps = pick_edge_bps(payload, "expected_edge_calibrated_bps", "expected_edge_calibrated_pct")
                edge_after_cost_bps = pick_edge_bps(payload, "expected_edge_bps_after_cost", "expected_edge_after_cost_pct")
                if "expected_edge_used_for_gate_bps" in payload and "expected_edge_bps_after_cost" not in payload:
                    edge_after_cost_bps = to_float(payload.get("expected_edge_used_for_gate_bps", edge_after_cost_bps))

                eval_rows.append(
                    {
                        "shadow_ts": int(ts_ms),
                        "market": market,
                        "regime": str(payload.get("regime", "UNKNOWN")).strip().upper() or "UNKNOWN",
                        "vol_bucket_pct": norm_vol_bucket(payload.get("vol_bucket_pct", "NONE")),
                        "source": str(payload.get("source", "")).strip(),
                        "shadow_file": str(shadow_path),
                        "entry_ts": entry_ts,
                        "exit_ts": exit_ts,
                        "entry_close": round(entry_close, 10),
                        "exit_close": round(exit_close, 10),
                        "return_bps": round(ret_bps, 6),
                        "shadow_edge_bps": round(shadow_edge_bps, 6),
                        "p_h5": round(to_float(payload.get("p_h5_calibrated", payload.get("p_h5", 0.0))), 8),
                        "selection_threshold_h5": round(to_float(payload.get("selection_threshold_h5", payload.get("threshold", 0.0))), 8),
                        "margin": round(to_float(payload.get("margin", 0.0)), 8),
                        "edge_cal_bps": round(edge_cal_bps, 6),
                        "edge_after_cost_bps": round(edge_after_cost_bps, 6),
                        "signal_ev": round(to_float(payload.get("signal_expected_value", 0.0)), 10),
                        "signal_strength": round(to_float(payload.get("signal_strength", 0.0)), 8),
                        "disabled_reason": str(payload.get("disabled_reason", "")).strip(),
                        "would_pass_quality_selection": f,
                        "would_pass_manager": bool_or_none(payload.get("would_pass_manager")),
                        "would_pass_execution_guard": e,
                        "note": str(payload.get("note", "")).strip(),
                    }
                )
                with_t5 += 1

            if max_rows > 0 and total_rows >= max_rows:
                break

    edges = [to_float(r.get("shadow_edge_bps", 0.0)) for r in eval_rows]
    pos_cnt = sum(1 for x in edges if x > 0.0)

    by_market: Dict[str, List[Dict[str, Any]]] = {}
    by_vol: Dict[str, List[Dict[str, Any]]] = {}
    by_cell: Dict[Tuple[str, str, str], List[Dict[str, Any]]] = {}
    for r in eval_rows:
        mk = str(r.get("market", "")).strip() or "UNKNOWN"
        vb = str(r.get("vol_bucket_pct", "NONE")).strip() or "NONE"
        rg = str(r.get("regime", "UNKNOWN")).strip() or "UNKNOWN"
        by_market.setdefault(mk, []).append(r)
        by_vol.setdefault(vb, []).append(r)
        by_cell.setdefault((mk, vb, rg), []).append(r)

    market_rows: List[Dict[str, Any]] = []
    for mk, rows in by_market.items():
        a = agg(rows)
        f_true = f_tot = e_true = e_tot = 0
        for rr in rows:
            ff = bool_or_none(rr.get("would_pass_quality_selection"))
            if ff is not None:
                f_tot += 1
                if ff:
                    f_true += 1
            ee = bool_or_none(rr.get("would_pass_execution_guard"))
            if ee is not None:
                e_tot += 1
                if ee:
                    e_true += 1
        market_rows.append(
            {
                "market": mk,
                **a,
                "would_pass_quality_selection_rate": round(float(f_true) / float(max(1, f_tot)), 6) if f_tot > 0 else None,
                "would_pass_execution_guard_rate": round(float(e_true) / float(max(1, e_tot)), 6) if e_tot > 0 else None,
            }
        )
    market_rows.sort(key=lambda x: (-to_int(x.get("count_with_t5", 0), 0), str(x.get("market", ""))))
    market_top = market_rows[: max(1, int(args.market_top_n))]

    by_vol_payload: Dict[str, Dict[str, Any]] = {}
    for vb, rows in sorted(by_vol.items(), key=lambda kv: kv[0]):
        by_vol_payload[vb] = agg(rows)

    cells: List[Dict[str, Any]] = []
    for (mk, vb, rg), rows in by_cell.items():
        cells.append({"market": mk, "vol_bucket_pct": vb, "regime": rg, **agg(rows)})
    cells.sort(key=lambda x: (to_float(x.get("pnl_sum_bps", 0.0), 0.0), str(x.get("market", "")), str(x.get("vol_bucket_pct", "")), str(x.get("regime", ""))))
    top_n = max(1, int(args.cell_top_n))
    top_loss = cells[:top_n]
    top_gain = list(reversed(cells[-top_n:])) if cells else []

    report: Dict[str, Any] = {
        "generated_at": now_iso(),
        "hard_lock": {
            "runtime_config": str(config_path),
            "bundle": str(bundle_path),
            "allow_live_orders": bool(trading_cfg.get("allow_live_orders", False)),
            "live_paper_use_fixed_initial_capital": bool(trading_cfg.get("live_paper_use_fixed_initial_capital", False)),
            "live_paper_fixed_initial_capital_krw": to_int(trading_cfg.get("live_paper_fixed_initial_capital_krw", 0), 0),
            "edge_semantics": "net",
            "label_cost_bps": float(label_cost_bps),
            "horizon_bars": int(horizon),
        },
        "inputs": {
            "shadow_jsonl_paths": [str(p) for p in shadow_paths],
            "ohlcv_root": str(ohlcv_root),
            "market_ohlcv_map": {k: str(v) for k, v in sorted(ohlcv_map.items(), key=lambda kv: kv[0])},
            "max_rows": int(max_rows),
        },
        "counts": {
            "shadow_total": int(total_rows),
            "with_t5": int(with_t5),
            "missing_market": int(missing_market),
            "missing_entry": int(missing_entry),
            "missing_t5": int(missing_t5),
            "missing_t5_ratio": round(float(missing_t5) / float(max(1, total_rows)), 6),
        },
        "overall": {
            "mean_edge_bps": round(mean(edges), 6) if edges else 0.0,
            "median_edge_bps": round(qtile(edges, 0.5), 6) if edges else 0.0,
            "p10_edge_bps": round(qtile(edges, 0.1), 6) if edges else 0.0,
            "p90_edge_bps": round(qtile(edges, 0.9), 6) if edges else 0.0,
            "positive_rate": round(float(pos_cnt) / float(max(1, len(edges))), 6),
        },
        "by_market_top": market_top,
        "by_vol_bucket_pct": by_vol_payload,
        "would_pass_summary": {
            "shadow_would_pass_quality_selection_count": int(quality_selection_true),
            "shadow_would_pass_quality_selection_rate": round(float(quality_selection_true) / float(max(1, quality_selection_total)), 6) if quality_selection_total > 0 else None,
            "shadow_would_pass_execution_guard_count": int(exec_true),
            "shadow_would_pass_execution_guard_rate": round(float(exec_true) / float(max(1, exec_total)), 6) if exec_total > 0 else None,
            "would_pass_rate_by_market": [
                {
                    "market": r.get("market"),
                    "would_pass_quality_selection_rate": r.get("would_pass_quality_selection_rate"),
                    "would_pass_execution_guard_rate": r.get("would_pass_execution_guard_rate"),
                    "count_with_t5": r.get("count_with_t5"),
                }
                for r in market_top
            ],
        },
        "notes": [
            "live rows may have missing t+5 until enough time passes",
            "evaluation is diagnostic-only and does not include realized execution slippage",
            "shadow_edge_bps = return_bps(close_t->close_t+horizon) - label_cost_bps",
        ],
    }

    top_cells = {
        "generated_at": report["generated_at"],
        "top_loss_cells": top_loss,
        "top_gain_cells": top_gain,
    }

    preview = build_decision_preview(report, eval_rows)
    report["decision_preview"] = preview

    choice = {
        "generated_at": report["generated_at"],
        "decision": preview.get("decision", "GO_COLLECT_MORE_SHADOW"),
        "rationale": preview.get("rationale", []),
        "metrics": {
            "with_t5": preview.get("with_t5", 0),
            "overall_mean_edge_bps": preview.get("overall_mean_edge_bps", 0.0),
            "overall_positive_rate": preview.get("overall_positive_rate", 0.0),
            "execution_guard_pass_rate": preview.get("execution_guard_pass_rate", 0.0),
        },
        "single_axis_rule": "next round must change exactly one axis only",
    }

    rows_csv = out_dir / "shadow_edge_eval_rows.csv"
    fields = [
        "shadow_ts", "market", "regime", "vol_bucket_pct", "source", "shadow_file",
        "entry_ts", "exit_ts", "entry_close", "exit_close", "return_bps", "shadow_edge_bps",
        "p_h5", "selection_threshold_h5", "margin", "edge_cal_bps", "edge_after_cost_bps",
        "signal_ev", "signal_strength", "disabled_reason", "would_pass_quality_selection",
        "would_pass_manager", "would_pass_execution_guard", "note",
    ]
    with rows_csv.open("w", encoding="utf-8", newline="") as fp:
        w = csv.DictWriter(fp, fieldnames=fields)
        w.writeheader()
        for row in eval_rows:
            w.writerow({k: row.get(k) for k in fields})

    report_path = out_dir / "shadow_edge_eval_report.json"
    top_cells_path = out_dir / "shadow_edge_eval_top_cells.json"
    choice_path = out_dir / "stageD_v2_choice.json"
    plan_path = out_dir / "stageD_v2_next_action_plan.md"

    report["artifacts"] = {
        "shadow_edge_eval_report_json": str(report_path),
        "shadow_edge_eval_top_cells_json": str(top_cells_path),
        "shadow_edge_eval_rows_csv": str(rows_csv),
        "stageD_v2_choice_json": str(choice_path),
        "stageD_v2_next_action_plan_md": str(plan_path),
    }

    dump_json(report_path, report)
    dump_json(top_cells_path, top_cells)
    dump_json(choice_path, choice)
    plan_path.write_text(build_next_action_plan(choice), encoding="utf-8", newline="\n")

    print(
        "[ShadowEval] "
        f"shadow_total={total_rows} with_t5={with_t5} "
        f"missing_entry={missing_entry} missing_t5={missing_t5} "
        f"mean_edge_bps={report['overall']['mean_edge_bps']} "
        f"decision={choice['decision']}"
    )
    print(f"[ShadowEval] report_json={report_path}")
    print(f"[ShadowEval] top_cells_json={top_cells_path}")
    print(f"[ShadowEval] rows_csv={rows_csv}")
    print(f"[ShadowEval] choice_json={choice_path}")
    print(f"[ShadowEval] next_action_plan={plan_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
