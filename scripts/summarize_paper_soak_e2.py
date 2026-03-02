#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import re
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

from _script_common import dump_json, resolve_repo_path


TS_PREFIX_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]")
EXIT_RE = re.compile(
    r"Position exited:\s*([A-Z0-9\-]+)\s*\|\s*pnl\s*([-+]?\d+(?:\.\d+)?)\s*\(([-+]?\d+(?:\.\d+)?)%\)\s*\|\s*reason=([A-Za-z0-9_]+)"
)
EXEC_RE = re.compile(
    r"Execution lifecycle:\s*source=([a-zA-Z0-9_]+),\s*event=([a-zA-Z0-9_]+),\s*order_id=([^,]+),\s*market=([A-Z0-9\-]+),\s*side=([A-Z]+),\s*status=([^,]+)"
)
SCAN_RE = re.compile(r"Starting market scan")
REGIME_DISABLED_RE = re.compile(
    r"([A-Z0-9\-]+)\s+regime entry disabled:\s*regime=([A-Z_]+),\s*skipped_candidates=(\d+)"
)
SEVERE_ERROR_RE = re.compile(
    r"(fatal|assert|segmentation fault|stack trace|unhandled exception|critical)",
    re.IGNORECASE,
)
MILD_ERROR_RE = re.compile(r"(error|exception)", re.IGNORECASE)
VETO_KEY_RE = re.compile(r"^blocked_realtime_entry_veto_", re.IGNORECASE)


REGIME_BY_CODE = {
    0: "UNKNOWN",
    1: "TRENDING_UP",
    2: "TRENDING_DOWN",
    3: "RANGING",
    4: "HIGH_VOLATILITY",
}


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Summarize E2 paper soak run artifacts.")
    p.add_argument("--probe-summary-json", required=True)
    p.add_argument("--stdout-log", required=True)
    p.add_argument("--live-funnel-json", default="build/Release/logs/live_signal_funnel_taxonomy_report.json")
    p.add_argument("--state-json", default="build/Release/state/snapshot_state.json")
    p.add_argument("--output-summary-json", required=True)
    p.add_argument("--output-gate-json", required=True)
    p.add_argument("--output-timeseries-jsonl", required=True)
    p.add_argument("--output-toploss-json", required=True)
    p.add_argument("--output-liqvol-dynamic-json", default="")
    p.add_argument("--output-bear-rebound-debug-json", default="")
    p.add_argument("--run-label", default="E2")
    return p.parse_args(argv)


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
        if math.isfinite(out):
            return out
        return float(d)
    except Exception:
        return float(d)


def parse_utc_iso(value: str) -> datetime:
    token = str(value or "").strip()
    if not token:
        return datetime.now(timezone.utc)
    return datetime.fromisoformat(token.replace("Z", "+00:00")).astimezone(timezone.utc)


def parse_local_ts_from_line(raw: str, fallback_tz: timezone) -> Optional[datetime]:
    m = TS_PREFIX_RE.match(raw)
    if not m:
        return None
    try:
        dt = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
    except Exception:
        return None
    return dt.replace(tzinfo=fallback_tz)


def normalize_exit_reason(reason: str) -> str:
    token = str(reason or "").strip().lower()
    if "stop" in token:
        return "stop_loss"
    if "takeprofit" in token or token.startswith("tp"):
        return "take_profit"
    if "strategy" in token or "time" in token or "manual" in token:
        return "strategy_exit"
    return "other"


def safe_div(a: float, b: float) -> float:
    if b <= 0.0:
        return 0.0
    return float(a) / float(b)


def read_json(path_value: Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {}
    try:
        loaded = json.loads(path_value.read_text(encoding="utf-8-sig"))
        if isinstance(loaded, dict):
            return loaded
    except Exception:
        pass
    return {}


def parse_stdout_events(stdout_path: Path, started_at_utc: datetime, ended_at_utc: datetime) -> Dict[str, Any]:
    lines: List[str] = []
    if stdout_path.exists():
        lines = stdout_path.read_text(encoding="utf-8", errors="replace").splitlines()

    fallback_tz = started_at_utc.astimezone().tzinfo or timezone.utc

    scan_count = 0
    severe_error_count = 0
    mild_error_count = 0

    exec_events: List[Dict[str, Any]] = []
    exit_rows: List[Dict[str, Any]] = []
    regime_disable_rows: List[Dict[str, Any]] = []
    event_rows: List[Tuple[datetime, str, Dict[str, Any]]] = []

    for raw in lines:
        line = str(raw)
        if SEVERE_ERROR_RE.search(line):
            severe_error_count += 1
        if MILD_ERROR_RE.search(line):
            mild_error_count += 1

        ts_local = parse_local_ts_from_line(line, fallback_tz)
        ts_utc = ts_local.astimezone(timezone.utc) if ts_local is not None else None

        if SCAN_RE.search(line):
            scan_count += 1
            if ts_utc is not None:
                event_rows.append((ts_utc, "scan", {}))

        m_exec = EXEC_RE.search(line)
        if m_exec:
            row = {
                "source": m_exec.group(1),
                "event": m_exec.group(2).lower(),
                "order_id": m_exec.group(3).strip(),
                "market": m_exec.group(4).strip(),
                "side": m_exec.group(5).strip(),
                "status": m_exec.group(6).strip(),
                "ts_utc": ts_utc.isoformat() if ts_utc is not None else "",
            }
            exec_events.append(row)
            if ts_utc is not None and row["source"].lower() == "live":
                event_rows.append((ts_utc, f"exec_{row['event']}", row))
            continue

        m_exit = EXIT_RE.search(line)
        if m_exit:
            pnl_krw = to_float(m_exit.group(2), 0.0)
            pnl_pct = to_float(m_exit.group(3), 0.0)
            reason_raw = m_exit.group(4).strip()
            row = {
                "market": m_exit.group(1).strip(),
                "pnl_krw": pnl_krw,
                "pnl_pct": pnl_pct,
                "reason_raw": reason_raw,
                "reason_norm": normalize_exit_reason(reason_raw),
                "ts_utc": ts_utc.isoformat() if ts_utc is not None else "",
            }
            exit_rows.append(row)
            if ts_utc is not None:
                event_rows.append((ts_utc, "exit", row))
            continue

        m_regime = REGIME_DISABLED_RE.search(line)
        if m_regime:
            row = {
                "market": m_regime.group(1).strip(),
                "regime": m_regime.group(2).strip(),
                "skipped_candidates": to_int(m_regime.group(3), 0),
                "ts_utc": ts_utc.isoformat() if ts_utc is not None else "",
            }
            regime_disable_rows.append(row)
            if ts_utc is not None:
                event_rows.append((ts_utc, "regime_disabled", row))

    live_exec_submitted = sum(
        1 for x in exec_events if str(x.get("source", "")).lower() == "live" and str(x.get("event", "")) == "submitted"
    )
    live_exec_filled = sum(
        1 for x in exec_events if str(x.get("source", "")).lower() == "live" and str(x.get("event", "")) == "filled"
    )

    run_exit_rows = []
    for row in exit_rows:
        ts_token = str(row.get("ts_utc", ""))
        if not ts_token:
            continue
        ts = parse_utc_iso(ts_token)
        if started_at_utc <= ts <= ended_at_utc + timedelta(seconds=90):
            run_exit_rows.append(row)

    return {
        "line_count": len(lines),
        "scan_count_stdout": int(scan_count),
        "severe_error_count": int(severe_error_count),
        "mild_error_count": int(mild_error_count),
        "live_exec_submitted_stdout": int(live_exec_submitted),
        "live_exec_filled_stdout": int(live_exec_filled),
        "exit_rows_run_window": run_exit_rows,
        "regime_disable_rows": regime_disable_rows,
        "event_rows": event_rows,
    }


def collect_trade_rows_from_state(
    state_path: Path,
    started_at_utc: datetime,
    ended_at_utc: datetime,
) -> List[Dict[str, Any]]:
    state = read_json(state_path)
    raw_history = state.get("trade_history", []) if isinstance(state, dict) else []
    if not isinstance(raw_history, list):
        return []

    start_ms = int(started_at_utc.timestamp() * 1000.0)
    end_ms = int((ended_at_utc + timedelta(seconds=120)).timestamp() * 1000.0)
    out: List[Dict[str, Any]] = []
    for item in raw_history:
        if not isinstance(item, dict):
            continue
        exit_ms = to_int(item.get("exit_time", 0), 0)
        if exit_ms <= 0 or exit_ms < start_ms or exit_ms > end_ms:
            continue
        regime_code = to_int(item.get("market_regime", 0), 0)
        regime = REGIME_BY_CODE.get(regime_code, "UNKNOWN")
        out.append(
            {
                "market": str(item.get("market", "")).strip().upper(),
                "profit_loss": to_float(item.get("profit_loss", 0.0), 0.0),
                "profit_loss_pct": to_float(item.get("profit_loss_pct", 0.0), 0.0),
                "entry_time": to_int(item.get("entry_time", 0), 0),
                "exit_time": exit_ms,
                "exit_reason_raw": str(item.get("exit_reason", "")).strip(),
                "exit_reason_norm": normalize_exit_reason(str(item.get("exit_reason", ""))),
                "market_regime": regime,
                "strategy_name": str(item.get("strategy_name", "")).strip(),
            }
        )
    return out


def build_time_series_rows(
    started_at_utc: datetime,
    ended_at_utc: datetime,
    event_rows: List[Tuple[datetime, str, Dict[str, Any]]],
) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    scans = 0
    exits = 0
    exit_pnl_sum = 0.0
    exec_submitted = 0
    exec_filled = 0
    regime_disabled_ranging = 0

    sorted_events = sorted(event_rows, key=lambda x: x[0])
    for ts, etype, payload in sorted_events:
        if ts < started_at_utc or ts > ended_at_utc + timedelta(seconds=90):
            continue
        if etype == "scan":
            scans += 1
        elif etype == "exec_submitted":
            exec_submitted += 1
        elif etype == "exec_filled":
            exec_filled += 1
        elif etype == "exit":
            exits += 1
            exit_pnl_sum += to_float(payload.get("pnl_krw", 0.0), 0.0)
        elif etype == "regime_disabled":
            if str(payload.get("regime", "")).strip().upper() == "RANGING":
                regime_disabled_ranging += to_int(payload.get("skipped_candidates", 0), 0)
        rows.append(
            {
                "ts_utc": ts.isoformat(),
                "scan_count_cum": int(scans),
                "orders_submitted_cum": int(exec_submitted),
                "orders_filled_cum": int(exec_filled),
                "exits_cum": int(exits),
                "realized_pnl_sum_krw_cum": round(exit_pnl_sum, 6),
                "ranging_regime_disabled_cum": int(regime_disabled_ranging),
            }
        )

    if not rows:
        rows.append(
            {
                "ts_utc": ended_at_utc.isoformat(),
                "scan_count_cum": 0,
                "orders_submitted_cum": 0,
                "orders_filled_cum": 0,
                "exits_cum": 0,
                "realized_pnl_sum_krw_cum": 0.0,
                "ranging_regime_disabled_cum": 0,
            }
        )
    return rows


def aggregate_trade_kpis(
    trades: List[Dict[str, Any]],
    fallback_exits: List[Dict[str, Any]],
    duration_hours: float,
) -> Dict[str, Any]:
    use_rows = trades if trades else [
        {
            "market": str(x.get("market", "")).strip().upper(),
            "profit_loss": to_float(x.get("pnl_krw", 0.0), 0.0),
            "profit_loss_pct": to_float(x.get("pnl_pct", 0.0), 0.0),
            "entry_time": 0,
            "exit_time": 0,
            "exit_reason_raw": str(x.get("reason_raw", "")),
            "exit_reason_norm": str(x.get("reason_norm", "other")),
            "market_regime": "UNKNOWN",
            "strategy_name": "",
        }
        for x in fallback_exits
    ]

    pnl_values = [to_float(x.get("profit_loss", 0.0), 0.0) for x in use_rows]
    trade_count = len(use_rows)
    pnl_sum = float(sum(pnl_values))
    expectancy = float(pnl_sum / trade_count) if trade_count > 0 else 0.0
    wins = [x for x in pnl_values if x > 0]
    losses = [x for x in pnl_values if x < 0]
    draws = [x for x in pnl_values if abs(x) <= 1e-12]

    win_count = len(wins)
    loss_count = len(losses)
    draw_count = len(draws)
    win_rate = safe_div(win_count, max(1, trade_count))
    avg_win = float(sum(wins) / len(wins)) if wins else 0.0
    avg_loss = float(sum(losses) / len(losses)) if losses else 0.0

    hold_values_sec: List[float] = []
    for row in use_rows:
        entry_ms = to_int(row.get("entry_time", 0), 0)
        exit_ms = to_int(row.get("exit_time", 0), 0)
        if entry_ms > 0 and exit_ms > 0 and exit_ms >= entry_ms:
            hold_values_sec.append(float(exit_ms - entry_ms) / 1000.0)
    avg_hold = float(sum(hold_values_sec) / len(hold_values_sec)) if hold_values_sec else 0.0

    exit_reason_counter: Counter = Counter()
    regime_counter: Counter = Counter()
    market_pnl: Dict[str, float] = defaultdict(float)
    cell_pnl: Dict[str, float] = defaultdict(float)
    for row in use_rows:
        reason_norm = normalize_exit_reason(str(row.get("exit_reason_raw", row.get("exit_reason_norm", "other"))))
        exit_reason_counter[reason_norm] += 1
        regime = str(row.get("market_regime", "UNKNOWN")).strip().upper() or "UNKNOWN"
        regime_counter[regime] += 1
        market = str(row.get("market", "")).strip().upper() or "UNKNOWN"
        pnl = to_float(row.get("profit_loss", 0.0), 0.0)
        market_pnl[market] += pnl
        cell_pnl[f"{market}|{regime}"] += pnl

    trades_per_hour = safe_div(trade_count, max(1e-9, duration_hours))

    trending_count = int(regime_counter.get("TRENDING_UP", 0) + regime_counter.get("TRENDING_DOWN", 0))
    ranging_count = int(regime_counter.get("RANGING", 0))

    top_loss_markets = [
        {"market": k, "realized_pnl_sum_krw": round(v, 6)}
        for k, v in sorted(market_pnl.items(), key=lambda kv: (kv[1], kv[0]))[:5]
    ]
    top_loss_cells = []
    for key, value in sorted(cell_pnl.items(), key=lambda kv: (kv[1], kv[0]))[:8]:
        market, regime = key.split("|", 1)
        if regime not in ("TRENDING_UP", "TRENDING_DOWN"):
            continue
        top_loss_cells.append(
            {
                "cell_key": key,
                "market": market,
                "regime": regime,
                "realized_pnl_sum_krw": round(value, 6),
            }
        )

    return {
        "trade_count": int(trade_count),
        "trades_per_hour": round(trades_per_hour, 6),
        "realized_pnl_sum_krw": round(pnl_sum, 6),
        "realized_expectancy_krw_per_trade": round(expectancy, 6),
        "win_count": int(win_count),
        "loss_count": int(loss_count),
        "draw_count": int(draw_count),
        "win_rate": round(win_rate, 6),
        "avg_win_krw": round(avg_win, 6),
        "avg_loss_krw": round(avg_loss, 6),
        "avg_holding_time_seconds": round(avg_hold, 6),
        "exit_reason_breakdown": {
            "stop_loss": int(exit_reason_counter.get("stop_loss", 0)),
            "take_profit": int(exit_reason_counter.get("take_profit", 0)),
            "strategy_exit": int(exit_reason_counter.get("strategy_exit", 0)),
            "other": int(exit_reason_counter.get("other", 0)),
        },
        "regime_distribution_filled_trades": {
            "TRENDING_UP": int(regime_counter.get("TRENDING_UP", 0)),
            "TRENDING_DOWN": int(regime_counter.get("TRENDING_DOWN", 0)),
            "RANGING": int(ranging_count),
            "HIGH_VOLATILITY": int(regime_counter.get("HIGH_VOLATILITY", 0)),
            "UNKNOWN": int(regime_counter.get("UNKNOWN", 0)),
            "trending_total": int(trending_count),
            "trending_up_share_within_trending": round(
                safe_div(regime_counter.get("TRENDING_UP", 0), max(1, trending_count)), 6
            ),
            "trending_down_share_within_trending": round(
                safe_div(regime_counter.get("TRENDING_DOWN", 0), max(1, trending_count)), 6
            ),
        },
        "top_loss_markets": top_loss_markets,
        "top_loss_cells_trending_only": top_loss_cells,
        "source": "state_trade_history_window" if trades else "stdout_exit_fallback",
    }


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)

    probe_summary_path = resolve_repo_path(args.probe_summary_json)
    stdout_log_path = resolve_repo_path(args.stdout_log)
    live_funnel_path = resolve_repo_path(args.live_funnel_json)
    state_path = resolve_repo_path(args.state_json)

    summary_out = resolve_repo_path(args.output_summary_json)
    gate_out = resolve_repo_path(args.output_gate_json)
    ts_out = resolve_repo_path(args.output_timeseries_jsonl)
    top_loss_out = resolve_repo_path(args.output_toploss_json)
    liq_vol_dynamic_out: Optional[Path] = None
    if str(args.output_liqvol_dynamic_json).strip():
        liq_vol_dynamic_out = resolve_repo_path(args.output_liqvol_dynamic_json)
    bear_rebound_debug_out: Optional[Path] = None
    if str(args.output_bear_rebound_debug_json).strip():
        bear_rebound_debug_out = resolve_repo_path(args.output_bear_rebound_debug_json)
    output_paths = [summary_out, gate_out, ts_out, top_loss_out]
    if liq_vol_dynamic_out is not None:
        output_paths.append(liq_vol_dynamic_out)
    if bear_rebound_debug_out is not None:
        output_paths.append(bear_rebound_debug_out)
    for p in output_paths:
        p.parent.mkdir(parents=True, exist_ok=True)

    probe_summary = read_json(probe_summary_path)
    live_funnel = read_json(live_funnel_path)

    started_at_utc = parse_utc_iso(str(probe_summary.get("started_at_utc", "")))
    ended_at_utc = parse_utc_iso(str(probe_summary.get("ended_at_utc", "")))
    duration_hours = max(1e-9, (ended_at_utc - started_at_utc).total_seconds() / 3600.0)

    runtime_inputs = probe_summary.get("inputs", {}) if isinstance(probe_summary, dict) else {}
    config_path = Path(str(runtime_inputs.get("config_path", "")))
    if not config_path.is_absolute():
        config_path = resolve_repo_path(str(config_path))
    config_payload = read_json(config_path)
    trading_cfg = config_payload.get("trading", {}) if isinstance(config_payload, dict) else {}
    if not isinstance(trading_cfg, dict):
        trading_cfg = {}

    stdout_stats = parse_stdout_events(stdout_log_path, started_at_utc, ended_at_utc)
    state_trades = collect_trade_rows_from_state(state_path, started_at_utc, ended_at_utc)
    trade_kpis = aggregate_trade_kpis(
        state_trades,
        stdout_stats.get("exit_rows_run_window", []),
        duration_hours,
    )

    rejection_counts = live_funnel.get("rejection_reason_counts", {}) if isinstance(live_funnel, dict) else {}
    if not isinstance(rejection_counts, dict):
        rejection_counts = {}
    veto_counts = {
        str(k): max(0, to_int(v))
        for k, v in rejection_counts.items()
        if VETO_KEY_RE.match(str(k))
    }
    total_veto_count = int(sum(veto_counts.values()))
    spread_veto_count = int(veto_counts.get("blocked_realtime_entry_veto_spread", 0))
    spread_veto_share = safe_div(spread_veto_count, max(1, total_veto_count))

    reject_expected_edge_negative_count = max(
        0,
        to_int(rejection_counts.get("reject_expected_edge_negative_count", 0)),
    )
    candidate_total = max(1, to_int(rejection_counts.get("candidate_total", 0)))
    reject_expected_edge_negative_share = safe_div(reject_expected_edge_negative_count, candidate_total)

    run_result = probe_summary.get("run_result", {}) if isinstance(probe_summary, dict) else {}
    return_code = to_int(run_result.get("return_code", 0), 0)
    timed_out = bool(run_result.get("timed_out", False))
    severe_error_count = to_int(stdout_stats.get("severe_error_count", 0), 0)
    mild_error_count = to_int(stdout_stats.get("mild_error_count", 0), 0)

    g1_pass = bool(not bool(trading_cfg.get("allow_live_orders", False)))
    trending_filled = to_int(trade_kpis.get("regime_distribution_filled_trades", {}).get("trending_total", 0), 0)
    ranging_filled = to_int(trade_kpis.get("regime_distribution_filled_trades", {}).get("RANGING", 0), 0)
    shadow_delta = max(0, to_int(live_funnel.get("shadow_count_total", 0), 0))
    g2_pass = bool(trending_filled > 0)
    g3_pass = bool(ranging_filled == 0 and shadow_delta > 0)
    g4_pass = bool(spread_veto_share < 0.80)
    g5_pass = bool(reject_expected_edge_negative_share < 0.80)
    g6_pass = bool((timed_out or return_code == 0) and severe_error_count == 0 and mild_error_count < 500)

    liq_vol_dynamic = (
        live_funnel.get("liq_vol_gate_dynamic", {})
        if isinstance(live_funnel.get("liq_vol_gate_dynamic", {}), dict)
        else {}
    )
    structure_gate = (
        live_funnel.get("foundation_structure_gate", {})
        if isinstance(live_funnel.get("foundation_structure_gate", {}), dict)
        else {}
    )
    bear_rebound_guard = (
        live_funnel.get("bear_rebound_guard", {})
        if isinstance(live_funnel.get("bear_rebound_guard", {}), dict)
        else {}
    )
    liq_samples_raw = liq_vol_dynamic.get("samples", []) if isinstance(liq_vol_dynamic, dict) else []
    liq_samples: List[Dict[str, Any]] = []
    if isinstance(liq_samples_raw, list):
        for item in liq_samples_raw:
            if not isinstance(item, dict):
                continue
            liq_samples.append(
                {
                    "market": str(item.get("market", "")).strip().upper(),
                    "observed": round(to_float(item.get("observed", 0.0), 0.0), 8),
                    "threshold_dynamic": round(to_float(item.get("threshold_dynamic", 0.0), 0.0), 8),
                    "history_count": max(0, to_int(item.get("history_count", 0), 0)),
                    "pass": bool(item.get("pass", False)),
                    "low_conf": bool(item.get("low_conf", False)),
                }
            )
    liq_observation_count = max(0, to_int(liq_vol_dynamic.get("observation_count", 0), 0))
    liq_pass_count = max(0, to_int(liq_vol_dynamic.get("pass_count", 0), 0))
    liq_pass_rate = safe_div(liq_pass_count, max(1, liq_observation_count))
    bear_samples_raw = (
        bear_rebound_guard.get("samples", [])
        if isinstance(bear_rebound_guard, dict)
        else []
    )
    bear_samples: List[Dict[str, Any]] = []
    if isinstance(bear_samples_raw, list):
        for item in bear_samples_raw:
            if not isinstance(item, dict):
                continue
            bear_samples.append(
                {
                    "market": str(item.get("market", "")).strip().upper(),
                    "regime": str(item.get("regime", "")).strip().upper(),
                    "observed": round(to_float(item.get("observed", 0.0), 0.0), 8),
                    "threshold_dynamic": round(
                        to_float(item.get("threshold_dynamic", 0.0), 0.0),
                        8,
                    ),
                    "history_count": max(0, to_int(item.get("history_count", 0), 0)),
                    "pass": bool(item.get("pass", False)),
                    "low_conf": bool(item.get("low_conf", False)),
                }
            )
    bear_observation_count = max(0, to_int(bear_rebound_guard.get("observation_count", 0), 0))
    bear_pass_count = max(0, to_int(bear_rebound_guard.get("pass_count", 0), 0))
    bear_pass_rate = safe_div(bear_pass_count, max(1, bear_observation_count))

    gate = {
        "run_label": str(args.run_label),
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "checks": [
            {
                "id": "G1_allow_live_orders_false",
                "pass": g1_pass,
                "observed": bool(trading_cfg.get("allow_live_orders", False)),
                "expected": False,
            },
            {
                "id": "G2_trending_orders_filled_gt_0",
                "pass": g2_pass,
                "observed_trending_filled": int(trending_filled),
            },
            {
                "id": "G3_ranging_filled_eq_0_and_shadow_increase",
                "pass": g3_pass,
                "observed_ranging_filled": int(ranging_filled),
                "observed_shadow_count_total": int(shadow_delta),
            },
            {
                "id": "G4_spread_veto_share_lt_0_80",
                "pass": g4_pass,
                "observed_spread_veto_count": int(spread_veto_count),
                "observed_total_veto_count": int(total_veto_count),
                "observed_spread_veto_share": round(spread_veto_share, 6),
            },
            {
                "id": "G5_reject_expected_edge_negative_not_explosive",
                "pass": g5_pass,
                "observed_reject_expected_edge_negative_count": int(reject_expected_edge_negative_count),
                "observed_candidate_total": int(candidate_total),
                "observed_share": round(reject_expected_edge_negative_share, 6),
            },
            {
                "id": "G6_no_crash_or_log_explosion",
                "pass": g6_pass,
                "observed_return_code": int(return_code),
                "observed_timed_out": bool(timed_out),
                "observed_severe_error_count": int(severe_error_count),
                "observed_mild_error_count": int(mild_error_count),
            },
        ],
    }
    gate["all_pass"] = bool(all(bool(x.get("pass", False)) for x in gate["checks"]))
    gate["recommended_next"] = "extend_to_4h" if gate["all_pass"] else "stop_and_single_axis_fix"

    run_summary = {
        "run_label": str(args.run_label),
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "hard_lock": {
            "runtime_config_path": str(config_path),
            "bundle_path": str(trading_cfg.get("probabilistic_runtime_bundle_path", "")),
            "allow_live_orders": bool(trading_cfg.get("allow_live_orders", False)),
            "live_paper_use_fixed_initial_capital": bool(
                trading_cfg.get("live_paper_use_fixed_initial_capital", False)
            ),
            "live_paper_fixed_initial_capital_krw": to_int(
                trading_cfg.get("live_paper_fixed_initial_capital_krw", 0),
                0,
            ),
            "edge_semantics": "net",
            "strict_parity_mode": "warning_hold",
        },
        "window": {
            "started_at_utc": started_at_utc.isoformat(),
            "ended_at_utc": ended_at_utc.isoformat(),
            "duration_hours": round(duration_hours, 6),
        },
        "kpi": {
            "trades_total": int(trade_kpis.get("trade_count", 0)),
            "trades_per_hour": trade_kpis.get("trades_per_hour", 0.0),
            "realized_pnl_sum_krw": trade_kpis.get("realized_pnl_sum_krw", 0.0),
            "realized_expectancy_krw_per_trade": trade_kpis.get(
                "realized_expectancy_krw_per_trade",
                0.0,
            ),
            "win_count": int(trade_kpis.get("win_count", 0)),
            "loss_count": int(trade_kpis.get("loss_count", 0)),
            "draw_count": int(trade_kpis.get("draw_count", 0)),
            "win_rate": trade_kpis.get("win_rate", 0.0),
            "avg_win_krw": trade_kpis.get("avg_win_krw", 0.0),
            "avg_loss_krw": trade_kpis.get("avg_loss_krw", 0.0),
            "avg_holding_time_seconds": trade_kpis.get("avg_holding_time_seconds", 0.0),
            "exit_reason_breakdown": trade_kpis.get("exit_reason_breakdown", {}),
            "regime_distribution_filled_trades": trade_kpis.get(
                "regime_distribution_filled_trades",
                {},
            ),
            "spread_veto": {
                "count": int(spread_veto_count),
                "total_veto_count": int(total_veto_count),
                "share": round(spread_veto_share, 6),
            },
            "shadow": {
                "shadow_count_total": int(shadow_delta),
                "shadow_count_by_regime": live_funnel.get("shadow_count_by_regime", {}),
                "shadow_count_by_market": live_funnel.get("shadow_count_by_market", {}),
            },
            "liq_vol_gate_dynamic": {
                "mode": str(liq_vol_dynamic.get("mode", "static")),
                "quantile_q": round(to_float(liq_vol_dynamic.get("quantile_q", 0.0), 0.0), 8),
                "window_minutes": max(0, to_int(liq_vol_dynamic.get("window_minutes", 0), 0)),
                "min_samples_required": max(
                    0, to_int(liq_vol_dynamic.get("min_samples_required", 0), 0)
                ),
                "low_conf_action": str(liq_vol_dynamic.get("low_conf_action", "hold")),
                "observation_count": liq_observation_count,
                "pass_count": liq_pass_count,
                "fail_count": max(0, to_int(liq_vol_dynamic.get("fail_count", 0), 0)),
                "low_conf_triggered_count": max(
                    0, to_int(liq_vol_dynamic.get("low_conf_triggered_count", 0), 0)
                ),
                "pass_rate": round(liq_pass_rate, 6),
                "observed_mean": round(to_float(liq_vol_dynamic.get("observed_mean", 0.0), 0.0), 8),
                "threshold_dynamic_mean": round(
                    to_float(liq_vol_dynamic.get("threshold_dynamic_mean", 0.0), 0.0), 8
                ),
                "samples": liq_samples[:10],
            },
            "foundation_structure_gate": {
                "mode": str(structure_gate.get("mode", "static")),
                "relax_delta": round(to_float(structure_gate.get("relax_delta", 0.0), 0.0), 8),
                "observation_count": max(
                    0, to_int(structure_gate.get("observation_count", 0), 0)
                ),
                "fail_count_total": max(
                    0, to_int(structure_gate.get("fail_count_total", 0), 0)
                ),
                "fail_count_by_regime": (
                    structure_gate.get("fail_count_by_regime", {})
                    if isinstance(structure_gate.get("fail_count_by_regime", {}), dict)
                    else {}
                ),
                "pass_count_by_regime": (
                    structure_gate.get("pass_count_by_regime", {})
                    if isinstance(structure_gate.get("pass_count_by_regime", {}), dict)
                    else {}
                ),
                "observed_score_mean": round(
                    to_float(structure_gate.get("observed_score_mean", 0.0), 0.0), 8
                ),
                "threshold_before_mean": round(
                    to_float(structure_gate.get("threshold_before_mean", 0.0), 0.0), 8
                ),
                "threshold_after_mean": round(
                    to_float(structure_gate.get("threshold_after_mean", 0.0), 0.0), 8
                ),
                "samples": (
                    structure_gate.get("samples", [])
                    if isinstance(structure_gate.get("samples", []), list)
                    else []
                )[:10],
            },
            "bear_rebound_guard": {
                "mode": str(bear_rebound_guard.get("mode", "static")),
                "quantile_q": round(to_float(bear_rebound_guard.get("quantile_q", 0.0), 0.0), 8),
                "window_minutes": max(0, to_int(bear_rebound_guard.get("window_minutes", 0), 0)),
                "min_samples_required": max(
                    0, to_int(bear_rebound_guard.get("min_samples_required", 0), 0)
                ),
                "low_conf_action": str(bear_rebound_guard.get("low_conf_action", "hold")),
                "observation_count": bear_observation_count,
                "pass_count": bear_pass_count,
                "fail_count": max(0, to_int(bear_rebound_guard.get("fail_count", 0), 0)),
                "low_conf_triggered_count": max(
                    0, to_int(bear_rebound_guard.get("low_conf_triggered_count", 0), 0)
                ),
                "pass_rate": round(bear_pass_rate, 6),
                "observed_mean": round(to_float(bear_rebound_guard.get("observed_mean", 0.0), 0.0), 8),
                "threshold_dynamic_mean": round(
                    to_float(bear_rebound_guard.get("threshold_dynamic_mean", 0.0), 0.0),
                    8,
                ),
                "pass_count_by_regime": (
                    bear_rebound_guard.get("pass_count_by_regime", {})
                    if isinstance(bear_rebound_guard.get("pass_count_by_regime", {}), dict)
                    else {}
                ),
                "fail_count_by_regime": (
                    bear_rebound_guard.get("fail_count_by_regime", {})
                    if isinstance(bear_rebound_guard.get("fail_count_by_regime", {}), dict)
                    else {}
                ),
                "samples": bear_samples[:10],
            },
        },
        "gate_result": {
            "all_pass": bool(gate["all_pass"]),
            "recommended_next": gate["recommended_next"],
        },
        "sources": {
            "probe_summary_json": str(probe_summary_path),
            "stdout_log": str(stdout_log_path),
            "live_funnel_json": str(live_funnel_path),
            "state_json": str(state_path),
            "trade_kpi_source": str(trade_kpis.get("source", "")),
        },
    }

    gate_funnel_breakdown = {
        "run_label": str(args.run_label),
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "scan_count": max(
            to_int(live_funnel.get("scan_count", 0), 0),
            to_int(stdout_stats.get("scan_count_stdout", 0), 0),
        ),
        "markets_considered": to_int(live_funnel.get("markets_considered", 0), 0),
        "generated_signal_candidates": to_int(live_funnel.get("generated_signal_candidates", 0), 0),
        "selected_signal_candidates": to_int(live_funnel.get("selected_signal_candidates", 0), 0),
        "filtered_out_by_sizing": to_int(live_funnel.get("filtered_out_by_sizing", 0), 0),
        "no_signal_generated": to_int(live_funnel.get("no_signal_generated", 0), 0),
        "rejection_reason_counts": rejection_counts,
        "top_rejections": live_funnel.get("top_rejections", []),
        "realtime_entry_veto_counts": veto_counts,
        "shadow": {
            "shadow_count_total": int(shadow_delta),
            "shadow_count_by_regime": live_funnel.get("shadow_count_by_regime", {}),
            "shadow_count_by_market": live_funnel.get("shadow_count_by_market", {}),
            "shadow_would_pass_quality_selection_count": to_int(
                live_funnel.get("shadow_would_pass_quality_selection_count", 0),
                0,
            ),
            "shadow_would_pass_execution_guard_count": to_int(
                live_funnel.get("shadow_would_pass_execution_guard_count", 0),
                0,
            ),
        },
        "liq_vol_gate_dynamic": {
            "mode": str(liq_vol_dynamic.get("mode", "static")),
            "quantile_q": round(to_float(liq_vol_dynamic.get("quantile_q", 0.0), 0.0), 8),
            "window_minutes": max(0, to_int(liq_vol_dynamic.get("window_minutes", 0), 0)),
            "min_samples_required": max(
                0, to_int(liq_vol_dynamic.get("min_samples_required", 0), 0)
            ),
            "low_conf_action": str(liq_vol_dynamic.get("low_conf_action", "hold")),
            "observation_count": liq_observation_count,
            "pass_count": liq_pass_count,
            "fail_count": max(0, to_int(liq_vol_dynamic.get("fail_count", 0), 0)),
            "low_conf_triggered_count": max(
                0, to_int(liq_vol_dynamic.get("low_conf_triggered_count", 0), 0)
            ),
            "pass_rate": round(liq_pass_rate, 6),
            "observed_mean": round(to_float(liq_vol_dynamic.get("observed_mean", 0.0), 0.0), 8),
            "threshold_dynamic_mean": round(
                to_float(liq_vol_dynamic.get("threshold_dynamic_mean", 0.0), 0.0), 8
            ),
            "samples": liq_samples[:10],
        },
        "foundation_structure_gate": {
            "mode": str(structure_gate.get("mode", "static")),
            "relax_delta": round(to_float(structure_gate.get("relax_delta", 0.0), 0.0), 8),
            "observation_count": max(0, to_int(structure_gate.get("observation_count", 0), 0)),
            "fail_count_total": max(0, to_int(structure_gate.get("fail_count_total", 0), 0)),
            "fail_count_by_regime": (
                structure_gate.get("fail_count_by_regime", {})
                if isinstance(structure_gate.get("fail_count_by_regime", {}), dict)
                else {}
            ),
            "pass_count_by_regime": (
                structure_gate.get("pass_count_by_regime", {})
                if isinstance(structure_gate.get("pass_count_by_regime", {}), dict)
                else {}
            ),
            "observed_score_mean": round(
                to_float(structure_gate.get("observed_score_mean", 0.0), 0.0), 8
            ),
            "threshold_before_mean": round(
                to_float(structure_gate.get("threshold_before_mean", 0.0), 0.0), 8
            ),
            "threshold_after_mean": round(
                to_float(structure_gate.get("threshold_after_mean", 0.0), 0.0), 8
            ),
            "samples": (
                structure_gate.get("samples", [])
                if isinstance(structure_gate.get("samples", []), list)
                else []
            )[:10],
        },
        "bear_rebound_guard": {
            "mode": str(bear_rebound_guard.get("mode", "static")),
            "quantile_q": round(to_float(bear_rebound_guard.get("quantile_q", 0.0), 0.0), 8),
            "window_minutes": max(0, to_int(bear_rebound_guard.get("window_minutes", 0), 0)),
            "min_samples_required": max(
                0, to_int(bear_rebound_guard.get("min_samples_required", 0), 0)
            ),
            "low_conf_action": str(bear_rebound_guard.get("low_conf_action", "hold")),
            "observation_count": bear_observation_count,
            "pass_count": bear_pass_count,
            "fail_count": max(0, to_int(bear_rebound_guard.get("fail_count", 0), 0)),
            "low_conf_triggered_count": max(
                0, to_int(bear_rebound_guard.get("low_conf_triggered_count", 0), 0)
            ),
            "pass_rate": round(bear_pass_rate, 6),
            "observed_mean": round(to_float(bear_rebound_guard.get("observed_mean", 0.0), 0.0), 8),
            "threshold_dynamic_mean": round(
                to_float(bear_rebound_guard.get("threshold_dynamic_mean", 0.0), 0.0),
                8,
            ),
            "pass_count_by_regime": (
                bear_rebound_guard.get("pass_count_by_regime", {})
                if isinstance(bear_rebound_guard.get("pass_count_by_regime", {}), dict)
                else {}
            ),
            "fail_count_by_regime": (
                bear_rebound_guard.get("fail_count_by_regime", {})
                if isinstance(bear_rebound_guard.get("fail_count_by_regime", {}), dict)
                else {}
            ),
            "samples": bear_samples[:10],
        },
        "gate_checks": gate,
    }

    top_loss_payload = {
        "run_label": str(args.run_label),
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "top_loss_markets": trade_kpis.get("top_loss_markets", []),
        "top_loss_cells_trending_only": trade_kpis.get("top_loss_cells_trending_only", []),
        "trade_count": int(trade_kpis.get("trade_count", 0)),
    }

    ts_rows = build_time_series_rows(
        started_at_utc,
        ended_at_utc,
        stdout_stats.get("event_rows", []),
    )

    dump_json(summary_out, run_summary)
    dump_json(gate_out, gate_funnel_breakdown)
    dump_json(top_loss_out, top_loss_payload)
    if liq_vol_dynamic_out is not None:
        liq_vol_payload = {
            "run_label": str(args.run_label),
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "generated_signal_candidates": to_int(
                gate_funnel_breakdown.get("generated_signal_candidates", 0),
                0,
            ),
            "no_signal_generated": to_int(gate_funnel_breakdown.get("no_signal_generated", 0), 0),
            "liq_vol_gate_pass_rate": round(liq_pass_rate, 6),
            "liq_vol_gate_observation_count": liq_observation_count,
            "liq_vol_gate_mode": str(liq_vol_dynamic.get("mode", "static")),
            "liq_vol_gate_quantile_q": round(to_float(liq_vol_dynamic.get("quantile_q", 0.0), 0.0), 8),
            "liq_vol_gate_window_minutes": max(0, to_int(liq_vol_dynamic.get("window_minutes", 0), 0)),
            "liq_vol_gate_min_samples_required": max(
                0, to_int(liq_vol_dynamic.get("min_samples_required", 0), 0)
            ),
            "liq_vol_gate_low_conf_action": str(liq_vol_dynamic.get("low_conf_action", "hold")),
            "liq_vol_gate_low_conf_triggered_count": max(
                0, to_int(liq_vol_dynamic.get("low_conf_triggered_count", 0), 0)
            ),
            "liq_vol_gate_observed_threshold_samples": liq_samples[:10],
        }
        dump_json(liq_vol_dynamic_out, liq_vol_payload)
    if bear_rebound_debug_out is not None:
        bear_rebound_payload = {
            "run_label": str(args.run_label),
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "generated_signal_candidates": to_int(
                gate_funnel_breakdown.get("generated_signal_candidates", 0),
                0,
            ),
            "no_signal_generated": to_int(gate_funnel_breakdown.get("no_signal_generated", 0), 0),
            "bear_rebound_guard_mode": str(bear_rebound_guard.get("mode", "static")),
            "bear_rebound_guard_quantile_q": round(
                to_float(bear_rebound_guard.get("quantile_q", 0.0), 0.0),
                8,
            ),
            "bear_rebound_guard_window_minutes": max(
                0, to_int(bear_rebound_guard.get("window_minutes", 0), 0)
            ),
            "bear_rebound_guard_min_samples_required": max(
                0, to_int(bear_rebound_guard.get("min_samples_required", 0), 0)
            ),
            "bear_rebound_guard_low_conf_action": str(
                bear_rebound_guard.get("low_conf_action", "hold")
            ),
            "bear_rebound_guard_observation_count": bear_observation_count,
            "bear_rebound_guard_pass_count": bear_pass_count,
            "bear_rebound_guard_fail_count": max(
                0, to_int(bear_rebound_guard.get("fail_count", 0), 0)
            ),
            "bear_rebound_guard_low_conf_triggered_count": max(
                0, to_int(bear_rebound_guard.get("low_conf_triggered_count", 0), 0)
            ),
            "bear_rebound_guard_pass_rate": round(bear_pass_rate, 6),
            "bear_rebound_guard_pass_count_by_regime": (
                bear_rebound_guard.get("pass_count_by_regime", {})
                if isinstance(bear_rebound_guard.get("pass_count_by_regime", {}), dict)
                else {}
            ),
            "bear_rebound_guard_fail_count_by_regime": (
                bear_rebound_guard.get("fail_count_by_regime", {})
                if isinstance(bear_rebound_guard.get("fail_count_by_regime", {}), dict)
                else {}
            ),
            "bear_rebound_guard_observed_threshold_samples": bear_samples[:10],
        }
        dump_json(bear_rebound_debug_out, bear_rebound_payload)
    with ts_out.open("w", encoding="utf-8", newline="\n") as fp:
        for row in ts_rows:
            fp.write(json.dumps(row, ensure_ascii=False) + "\n")

    print(f"[E2Summary] summary_json={summary_out}")
    print(f"[E2Summary] gate_json={gate_out}")
    print(f"[E2Summary] timeseries_jsonl={ts_out}")
    print(f"[E2Summary] toploss_json={top_loss_out}")
    if liq_vol_dynamic_out is not None:
        print(f"[E2Summary] liqvol_dynamic_json={liq_vol_dynamic_out}")
    if bear_rebound_debug_out is not None:
        print(f"[E2Summary] bear_rebound_debug_json={bear_rebound_debug_out}")
    print(
        "[E2Summary] "
        f"trades_total={trade_kpis.get('trade_count', 0)} "
        f"trending_filled={trending_filled} ranging_filled={ranging_filled} "
        f"spread_veto_share={round(spread_veto_share, 6)} "
        f"gate_all_pass={str(gate['all_pass']).lower()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
