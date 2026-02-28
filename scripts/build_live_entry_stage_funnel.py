#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from _script_common import dump_json, resolve_repo_path


EXEC_RE = re.compile(
    r"Execution lifecycle:\s*source=([a-zA-Z0-9_]+),\s*event=([a-zA-Z0-9_]+),\s*order_id=([^,]+),\s*market=([A-Z0-9\-]+),\s*side=([A-Z]+),\s*status=([^,]+),\s*filled=([-+]?\d+(?:\.\d+)?),\s*volume=([-+]?\d+(?:\.\d+)?)"
)
TS_RE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]")
SELECT_RE = re.compile(
    r"^\[(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]\s+\[info\]\s+"
    r"(?P<market>[A-Z0-9\-]+)\s+probabilistic-primary best selected:\s+"
    r"p_h5=(?P<p_h5>[-+]?\d+(?:\.\d+)?),\s+"
    r"margin=(?P<margin>[-+]?\d+(?:\.\d+)?),\s+"
    r"liq=(?P<liq>[-+]?\d+(?:\.\d+)?),\s+"
    r"strength=(?P<strength>[-+]?\d+(?:\.\d+)?),.*?"
    r"edge_cal=(?P<edge_cal_pct>[-+]?\d+(?:\.\d+)?)%"
)


@dataclass(frozen=True)
class ReasonPattern:
    key: str
    pattern: re.Pattern[str]


REASON_PATTERNS: List[ReasonPattern] = [
    ReasonPattern(
        key="blocked_realtime_entry_veto_spread",
        pattern=re.compile(r"buy vetoed:\s*spread", re.IGNORECASE),
    ),
    ReasonPattern(
        key="blocked_realtime_entry_veto_imbalance",
        pattern=re.compile(r"buy vetoed:\s*orderbook imbalance", re.IGNORECASE),
    ),
    ReasonPattern(
        key="blocked_realtime_entry_veto_rapid_drop",
        pattern=re.compile(r"buy vetoed:\s*rapid drop", re.IGNORECASE),
    ),
    ReasonPattern(
        key="insufficient_balance",
        pattern=re.compile(r"insufficient available capital", re.IGNORECASE),
    ),
    ReasonPattern(
        key="risk_manager_canEnter_false",
        pattern=re.compile(r"buy skipped \(risk check rejected\)", re.IGNORECASE),
    ),
    ReasonPattern(
        key="invalid_price_level",
        pattern=re.compile(r"invalid TP/SL|invalid realtime orderbook snapshot", re.IGNORECASE),
    ),
    ReasonPattern(
        key="min_notional_fail",
        pattern=re.compile(
            r"below safe minimum|safe minimum order|risk budget too small",
            re.IGNORECASE,
        ),
    ),
    ReasonPattern(
        key="size_clamped_to_zero",
        pattern=re.compile(r"position_size clamped:\s*[-+]?\d+(?:\.\d+)?\s*->\s*0", re.IGNORECASE),
    ),
]


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Build Round5 diagnostic-only stage funnel artifacts.")
    p.add_argument(
        "--live-funnel-json",
        default="build/Release/logs/live_signal_funnel_taxonomy_report.json",
    )
    p.add_argument("--stdout-log", required=True)
    p.add_argument("--run-label", default="E2_round5")
    p.add_argument("--output-stage-funnel-json", required=True)
    p.add_argument("--output-block-reasons-json", required=True)
    p.add_argument("--output-selected-samples-json", required=True)
    p.add_argument("--topn", type=int, default=5)
    p.add_argument("--max-samples", type=int, default=5)
    return p.parse_args()


def read_json(path: Path) -> Dict[str, Any]:
    if not path.exists():
        return {}
    try:
        obj = json.loads(path.read_text(encoding="utf-8-sig"))
        if isinstance(obj, dict):
            return obj
    except Exception:
        pass
    return {}


def read_lines(path: Path) -> List[str]:
    if not path.exists():
        return []
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def to_int(v: Any, d: int = 0) -> int:
    try:
        return int(v)
    except Exception:
        try:
            return int(float(v))
        except Exception:
            return d


def to_float(v: Any, d: float = 0.0) -> float:
    try:
        return float(v)
    except Exception:
        return d


def ts_of_line(line: str) -> Optional[str]:
    m = TS_RE.match(line)
    if not m:
        return None
    return m.group(1)


def build_stage_funnel(live_funnel: Dict[str, Any], lines: List[str], run_label: str) -> Dict[str, Any]:
    phase4 = live_funnel.get("phase4_portfolio_diagnostics", {})
    if not isinstance(phase4, dict):
        phase4 = {}

    s0 = to_int(live_funnel.get("generated_signal_candidates", 0), 0)
    s1 = to_int(live_funnel.get("selected_signal_candidates", 0), 0)
    s2 = to_int(live_funnel.get("selection_scored_candidate_count", s1), s1)
    s3 = to_int(phase4.get("selected_total", s1), s1)

    submitted_buy = 0
    filled_buy = 0
    submitted_orders: Dict[str, Dict[str, Any]] = {}

    for raw in lines:
        m = EXEC_RE.search(raw)
        if not m:
            continue
        event = m.group(2).strip().lower()
        order_id = m.group(3).strip()
        market = m.group(4).strip()
        side = m.group(5).strip().upper()
        status = m.group(6).strip().upper()
        filled = to_float(m.group(7), 0.0)
        volume = to_float(m.group(8), 0.0)
        if side != "BUY":
            continue
        if event == "submitted" or status == "SUBMITTED":
            submitted_buy += 1
            submitted_orders[order_id] = {
                "market": market,
                "volume": volume,
                "filled": 0.0,
            }
        if event == "filled" or status == "FILLED":
            if filled > 0.0:
                filled_buy += 1
            if order_id in submitted_orders:
                submitted_orders[order_id]["filled"] = filled

    return {
        "run_label": run_label,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "stages": {
            "S0_candidates_generated_total": s0,
            "S1_candidates_selected_total": s1,
            "S2_after_manager_pass_total": s2,
            "S3_after_portfolio_pass_total": s3,
            "S4_orders_submitted_total": submitted_buy,
            "S5_orders_filled_total": filled_buy,
        },
        "notes": [
            "S0/S1 from live_signal_funnel_taxonomy_report",
            "S2 uses selection_scored_candidate_count",
            "S3 uses phase4_portfolio_diagnostics.selected_total",
            "S4/S5 from stdout Execution lifecycle BUY events",
        ],
    }


def build_block_reasons(
    live_funnel: Dict[str, Any],
    lines: List[str],
    run_label: str,
    topn: int,
) -> Dict[str, Any]:
    reason_counts: Dict[str, int] = {}
    by_market: Dict[str, Dict[str, int]] = {}

    for raw in lines:
        market = None
        ts = ts_of_line(raw)
        _ = ts
        after_info = raw.split("] [", 2)
        if len(after_info) >= 3:
            tail = after_info[2]
            market_m = re.match(r"(?:info|warn|error)\]\s+([A-Z0-9\-]+)\s+", tail, re.IGNORECASE)
            if market_m:
                market = market_m.group(1).strip().upper()
        matched = False
        for rp in REASON_PATTERNS:
            if rp.pattern.search(raw):
                reason_counts[rp.key] = reason_counts.get(rp.key, 0) + 1
                if market:
                    by_market.setdefault(market, {})
                    by_market[market][rp.key] = by_market[market].get(rp.key, 0) + 1
                matched = True
                break
        if not matched and (
            "buy skipped" in raw.lower()
            or "buy vetoed" in raw.lower()
            or "submit failed" in raw.lower()
        ):
            reason_counts["other"] = reason_counts.get("other", 0) + 1
            if market:
                by_market.setdefault(market, {})
                by_market[market]["other"] = by_market[market].get("other", 0) + 1

    rej = live_funnel.get("rejection_reason_counts", {})
    if isinstance(rej, dict):
        negative_ev = to_int(rej.get("reject_expected_edge_negative_count", 0), 0)
        if negative_ev > 0:
            reason_counts["reject_expected_edge_negative_count"] = (
                reason_counts.get("reject_expected_edge_negative_count", 0) + negative_ev
            )
        for key in (
            "blocked_realtime_entry_veto_spread",
            "blocked_realtime_entry_veto_imbalance",
            "blocked_realtime_entry_veto_rapid_drop",
            "blocked_realtime_entry_veto_drop_vs_signal",
            "blocked_realtime_entry_veto_drop_vs_recent",
        ):
            v = to_int(rej.get(key, 0), 0)
            if v > 0:
                reason_counts[key] = reason_counts.get(key, 0) + v

    sorted_items = sorted(reason_counts.items(), key=lambda x: (-x[1], x[0]))
    if not sorted_items:
        top = [{"reason": "no_entry_block_observed", "count": 0}]
    else:
        top = [{"reason": k, "count": v} for k, v in sorted_items[: max(1, topn)]]
    return {
        "run_label": run_label,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "top_block_reasons": top,
        "all_block_reasons": reason_counts,
        "block_reasons_by_market": by_market,
    }


def build_selected_samples(
    live_funnel: Dict[str, Any],
    lines: List[str],
    run_label: str,
    max_samples: int,
) -> Dict[str, Any]:
    phase4 = live_funnel.get("phase4_portfolio_diagnostics", {})
    if not isinstance(phase4, dict):
        phase4 = {}

    market_block_reason: Dict[str, str] = {}
    market_veto_obs: Dict[str, Dict[str, Optional[float]]] = {}
    for raw in lines:
        low = raw.lower()
        market_m = re.search(r"\]\s+\[(?:info|warn|error)\]\s+([A-Z0-9\-]+)\s+", raw, re.IGNORECASE)
        market = market_m.group(1).strip().upper() if market_m else None
        if market is None:
            continue
        if "buy vetoed: spread" in low:
            market_block_reason[market] = "blocked_realtime_entry_veto_spread"
        elif "buy vetoed: orderbook imbalance" in low:
            market_block_reason[market] = "blocked_realtime_entry_veto_imbalance"
        elif "buy vetoed: rapid drop" in low:
            market_block_reason[market] = "blocked_realtime_entry_veto_rapid_drop"
        elif "insufficient available capital" in low:
            market_block_reason[market] = "insufficient_balance"
        elif "risk check rejected" in low:
            market_block_reason[market] = "risk_manager_canEnter_false"
        elif "invalid tp/sl" in low or "invalid realtime orderbook snapshot" in low:
            market_block_reason[market] = "invalid_price_level"
        elif "safe minimum order" in low or "below safe minimum" in low or "min order" in low:
            market_block_reason[market] = "min_notional_fail"

        if "buy vetoed" in low:
            market_veto_obs.setdefault(
                market,
                {
                    "spread_pct": None,
                    "imbalance": None,
                    "drop_vs_signal_pct": None,
                    "drop_vs_recent_pct": None,
                },
            )
            spread_m = re.search(r"spread\s+([-+]?\d+(?:\.\d+)?)%\s*>\s*([-+]?\d+(?:\.\d+)?)%", raw)
            if spread_m:
                market_veto_obs[market]["spread_pct"] = to_float(spread_m.group(1), 0.0)
            imb_m = re.search(r"imbalance\s+([-+]?\d+(?:\.\d+)?)\s*<\s*([-+]?\d+(?:\.\d+)?)", raw)
            if imb_m:
                market_veto_obs[market]["imbalance"] = to_float(imb_m.group(1), 0.0)
            drop_m = re.search(r"rapid drop vs signal price\s+([-+]?\d+(?:\.\d+)?)%\s*>\s*([-+]?\d+(?:\.\d+)?)%", raw)
            if drop_m:
                market_veto_obs[market]["drop_vs_signal_pct"] = to_float(drop_m.group(1), 0.0)
            recent_m = re.search(
                r"rapid drop vs recent best ask\s+([-+]?\d+(?:\.\d+)?)%\s*>\s*([-+]?\d+(?:\.\d+)?)%",
                raw,
            )
            if recent_m:
                market_veto_obs[market]["drop_vs_recent_pct"] = to_float(recent_m.group(1), 0.0)

    submitted_markets = set()
    filled_markets = set()
    for raw in lines:
        m = EXEC_RE.search(raw)
        if not m:
            continue
        event = m.group(2).strip().lower()
        market = m.group(4).strip().upper()
        side = m.group(5).strip().upper()
        status = m.group(6).strip().upper()
        filled = to_float(m.group(7), 0.0)
        if side != "BUY":
            continue
        if event == "submitted" or status == "SUBMITTED":
            submitted_markets.add(market)
        if (event == "filled" or status == "FILLED") and filled > 0.0:
            filled_markets.add(market)

    samples: List[Dict[str, Any]] = []
    for raw in lines:
        m = SELECT_RE.search(raw)
        if not m:
            continue
        market = m.group("market").strip().upper()
        final_reason = market_block_reason.get(market)
        if market in filled_markets:
            final_reason = "order_filled"
        elif market in submitted_markets and not final_reason:
            final_reason = "order_submitted_not_filled"
        if not final_reason:
            final_reason = "unknown_or_not_executed"

        edge_cal_pct = to_float(m.group("edge_cal_pct"), 0.0)
        sample = {
            "ts": m.group("ts"),
            "market": market,
            "regime": None,
            "vol_bucket_pct": None,
            "margin": to_float(m.group("margin"), 0.0),
            "expected_value": None,
            "required_expected_value": None,
            "execution_guard_observed": market_veto_obs.get(
                market,
                {
                    "spread_pct": None,
                    "imbalance": None,
                    "drop_vs_signal_pct": None,
                    "drop_vs_recent_pct": None,
                },
            ),
            "p_h5_calibrated": to_float(m.group("p_h5"), 0.0),
            "signal_strength": to_float(m.group("strength"), 0.0),
            "expected_edge_calibrated_bps": round(edge_cal_pct * 100.0, 6),
            "after_manager_pass": True,
            "after_portfolio_pass": phase4.get("selected_total", 0) > 0,
            "final_block_reason": final_reason,
        }
        samples.append(sample)
        if len(samples) >= max(1, max_samples):
            break

    return {
        "run_label": run_label,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "selected_candidate_samples": samples,
        "sample_count": len(samples),
    }


def main() -> int:
    args = parse_args()
    live_funnel_path = resolve_repo_path(args.live_funnel_json)
    stdout_log_path = resolve_repo_path(args.stdout_log)
    out_stage = resolve_repo_path(args.output_stage_funnel_json)
    out_block = resolve_repo_path(args.output_block_reasons_json)
    out_samples = resolve_repo_path(args.output_selected_samples_json)
    out_stage.parent.mkdir(parents=True, exist_ok=True)
    out_block.parent.mkdir(parents=True, exist_ok=True)
    out_samples.parent.mkdir(parents=True, exist_ok=True)

    live_funnel = read_json(live_funnel_path)
    lines = read_lines(stdout_log_path)

    stage = build_stage_funnel(live_funnel, lines, str(args.run_label))
    blocks = build_block_reasons(live_funnel, lines, str(args.run_label), int(args.topn))
    samples = build_selected_samples(
        live_funnel,
        lines,
        str(args.run_label),
        int(args.max_samples),
    )

    dump_json(out_stage, stage)
    dump_json(out_block, blocks)
    dump_json(out_samples, samples)

    print(f"[StageFunnel] stage_json={out_stage}")
    print(f"[StageFunnel] block_json={out_block}")
    print(f"[StageFunnel] samples_json={out_samples}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
