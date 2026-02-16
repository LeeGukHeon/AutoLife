#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
import pathlib
import subprocess
import sys
from copy import deepcopy
from typing import Any, Dict, List, Tuple

from _script_common import verification_lock

TUNABLE_TRADING_KEYS = [
    "max_new_orders_per_scan",
    "min_expected_edge_pct",
    "min_reward_risk",
    "min_rr_weak_signal",
    "min_rr_strong_signal",
    "min_strategy_trades_for_ev",
    "min_strategy_expectancy_krw",
    "min_strategy_profit_factor",
    "avoid_high_volatility",
    "avoid_trending_down",
    "hostility_ewma_alpha",
    "hostility_hostile_threshold",
    "hostility_severe_threshold",
    "hostility_extreme_threshold",
    "hostility_pause_scans",
    "hostility_pause_scans_extreme",
    "hostility_pause_recent_sample_min",
    "hostility_pause_recent_expectancy_krw",
    "hostility_pause_recent_win_rate",
    "backtest_hostility_pause_candles",
    "backtest_hostility_pause_candles_extreme",
]
TUNABLE_STRATEGY_KEYS = {
    "scalping": ["min_signal_strength"],
    "momentum": ["min_signal_strength"],
    "breakout": ["min_signal_strength"],
    "mean_reversion": ["min_signal_strength"],
}


def read_live_signal_funnel_snapshot(path_value: pathlib.Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {
            "exists": False,
            "funnel_json": str(path_value),
            "scan_count": 0,
            "top_group": "",
            "top_group_count": 0,
            "total_rejections": 0,
            "signal_generation_share": 0.0,
            "manager_prefilter_share": 0.0,
            "position_state_share": 0.0,
            "no_trade_bias_active": False,
            "selection_call_count": 0,
            "selection_scored_candidate_count": 0,
            "selection_hint_adjusted_candidate_count": 0,
            "selection_hint_adjusted_ratio": 0.0,
            "selection_hint_adjustment_counts": {},
            "top_selection_hint_adjustment": "",
        }

    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    group_counts_raw = payload.get("rejection_group_counts") or {}
    group_counts: Dict[str, int] = {}
    if isinstance(group_counts_raw, dict):
        for key, value in group_counts_raw.items():
            try:
                group_counts[str(key)] = int(value or 0)
            except (TypeError, ValueError):
                continue

    total_rejections = int(sum(max(0, v) for v in group_counts.values()))
    top_group = ""
    top_group_count = 0
    if group_counts:
        sorted_groups = sorted(group_counts.items(), key=lambda kv: (-kv[1], kv[0]))
        top_group = str(sorted_groups[0][0])
        top_group_count = int(sorted_groups[0][1])

    def _share(name: str) -> float:
        if total_rejections <= 0:
            return 0.0
        return float(max(0, group_counts.get(name, 0))) / float(total_rejections)

    signal_generation_share = float(_share("signal_generation"))
    manager_prefilter_share = float(_share("manager_prefilter"))
    position_state_share = float(_share("position_state"))
    scan_count = int(payload.get("scan_count", 0) or 0)
    no_trade_bias_active = bool(
        scan_count >= 3
        and total_rejections >= 20
        and position_state_share < 0.40
        and (
            signal_generation_share >= 0.55
            or (signal_generation_share + manager_prefilter_share) >= 0.75
        )
    )

    return {
        "exists": True,
        "funnel_json": str(path_value),
        "scan_count": scan_count,
        "top_group": top_group,
        "top_group_count": top_group_count,
        "total_rejections": total_rejections,
        "signal_generation_share": signal_generation_share,
        "manager_prefilter_share": manager_prefilter_share,
        "position_state_share": position_state_share,
        "no_trade_bias_active": no_trade_bias_active,
        "selection_call_count": int(payload.get("selection_call_count", 0) or 0),
        "selection_scored_candidate_count": int(payload.get("selection_scored_candidate_count", 0) or 0),
        "selection_hint_adjusted_candidate_count": int(
            payload.get("selection_hint_adjusted_candidate_count", 0) or 0
        ),
        "selection_hint_adjusted_ratio": float(payload.get("selection_hint_adjusted_ratio", 0.0) or 0.0),
        "selection_hint_adjustment_counts": payload.get("selection_hint_adjustment_counts") or {},
        "top_selection_hint_adjustment": str(
            ((payload.get("top_selection_hint_adjustments") or [{}])[0]).get("adjustment", "")
            if isinstance(payload.get("top_selection_hint_adjustments"), list)
            and (payload.get("top_selection_hint_adjustments") or [])
            else ""
        ),
    }


def read_train_eval_holdout_context(path_value: pathlib.Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {
            "exists": False,
            "summary_json": str(path_value),
            "holdout_found": False,
            "stage_name": "",
            "overall_gate_pass": False,
            "avg_profit_factor": 0.0,
            "avg_expectancy_krw": 0.0,
            "avg_total_trades": 0.0,
            "profitable_ratio": 0.0,
            "top_rejection_group": "",
            "top_risk_gate_component_reason": "",
            "top_risk_gate_component_count": 0,
            "recommendation": "",
        }

    payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    stages = payload.get("stages") or []
    holdout_stage: Dict[str, Any] = {}
    if isinstance(stages, list):
        candidates: List[Dict[str, Any]] = []
        for row in stages:
            if not isinstance(row, dict):
                continue
            if str(row.get("split", "")).strip().lower() == "holdout":
                candidates.append(row)
        if candidates:
            # Prefer deterministic holdout snapshot for suppression logic.
            holdout_stage = next(
                (
                    row
                    for row in candidates
                    if "deterministic" in str(row.get("stage", "")).lower()
                ),
                candidates[0],
            )

    snapshot = (holdout_stage.get("snapshot") or {}) if isinstance(holdout_stage, dict) else {}
    selected = (snapshot.get("selected") or {}) if isinstance(snapshot, dict) else {}
    core_full = (selected.get("core_full") or {}) if isinstance(selected, dict) else {}
    taxonomy = (holdout_stage.get("entry_rejection_taxonomy") or {}) if isinstance(holdout_stage, dict) else {}
    verdict = payload.get("promotion_verdict") or {}
    holdout_reject_ctx = verdict.get("holdout_core_rejection_context") or {}
    validation_reject_ctx = verdict.get("validation_core_rejection_context") or {}
    top_risk_gate_component_reason = str(
        holdout_reject_ctx.get("top_entry_risk_gate_component_reason")
        or validation_reject_ctx.get("top_entry_risk_gate_component_reason")
        or ""
    )
    try:
        top_risk_gate_component_count = int(
            holdout_reject_ctx.get("top_entry_risk_gate_component_count")
            or validation_reject_ctx.get("top_entry_risk_gate_component_count")
            or 0
        )
    except (TypeError, ValueError):
        top_risk_gate_component_count = 0

    return {
        "exists": True,
        "summary_json": str(path_value),
        "holdout_found": bool(holdout_stage),
        "stage_name": str(holdout_stage.get("stage", "") if isinstance(holdout_stage, dict) else ""),
        "overall_gate_pass": bool(selected.get("overall_gate_pass", False)),
        "avg_profit_factor": float(core_full.get("avg_profit_factor", 0.0) or 0.0),
        "avg_expectancy_krw": float(core_full.get("avg_expectancy_krw", 0.0) or 0.0),
        "avg_total_trades": float(core_full.get("avg_total_trades", 0.0) or 0.0),
        "profitable_ratio": float(core_full.get("profitable_ratio", 0.0) or 0.0),
        "top_rejection_group": str(taxonomy.get("overall_top_group", "") or ""),
        "top_risk_gate_component_reason": top_risk_gate_component_reason,
        "top_risk_gate_component_count": int(top_risk_gate_component_count),
        "recommendation": str(verdict.get("recommendation", "") or ""),
    }


def build_effective_bottleneck_context(
    live_context: Dict[str, Any],
    holdout_context: Dict[str, Any],
) -> Dict[str, Any]:
    context = dict(live_context or {})
    top_group_live = str(context.get("top_group", "") or "").strip()
    top_group_holdout = str(holdout_context.get("top_rejection_group", "") or "").strip()
    holdout_recommendation = str(holdout_context.get("recommendation", "") or "").strip()
    top_risk_gate_component_reason = str(holdout_context.get("top_risk_gate_component_reason", "") or "").strip()
    risk_gate_focus = ""
    if top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_invalid_levels":
        risk_gate_focus = "entry_quality_invalid_levels"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_base":
        risk_gate_focus = "entry_quality_rr_base"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_adaptive_history":
        risk_gate_focus = "entry_quality_rr_adaptive_history"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_adaptive_regime":
        risk_gate_focus = "entry_quality_rr_adaptive_regime"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_adaptive_mixed":
        risk_gate_focus = "entry_quality_rr_adaptive_mixed"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_adaptive":
        risk_gate_focus = "entry_quality_rr_adaptive"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr":
        risk_gate_focus = "entry_quality_rr"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_edge_base":
        risk_gate_focus = "entry_quality_edge_base"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_edge_adaptive_history":
        risk_gate_focus = "entry_quality_edge_adaptive_history"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_edge_adaptive_regime":
        risk_gate_focus = "entry_quality_edge_adaptive_regime"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_edge_adaptive_mixed":
        risk_gate_focus = "entry_quality_edge_adaptive_mixed"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_edge_adaptive":
        risk_gate_focus = "entry_quality_edge_adaptive"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_edge":
        risk_gate_focus = "entry_quality_edge"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_edge_base":
        risk_gate_focus = "entry_quality_rr_edge_base"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_edge_adaptive_history":
        risk_gate_focus = "entry_quality_rr_edge_adaptive_history"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_edge_adaptive_regime":
        risk_gate_focus = "entry_quality_rr_edge_adaptive_regime"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_edge_adaptive_mixed":
        risk_gate_focus = "entry_quality_rr_edge_adaptive_mixed"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_edge_adaptive":
        risk_gate_focus = "entry_quality_rr_edge_adaptive"
    elif top_risk_gate_component_reason == "blocked_risk_gate_entry_quality_rr_edge":
        risk_gate_focus = "entry_quality_rr_edge"
    elif top_risk_gate_component_reason.startswith("blocked_risk_gate_entry_quality"):
        risk_gate_focus = "entry_quality"
    elif top_risk_gate_component_reason == "blocked_risk_gate_regime":
        risk_gate_focus = "regime"
    elif top_risk_gate_component_reason == "blocked_risk_gate_strategy_ev":
        risk_gate_focus = "strategy_ev"
    elif top_risk_gate_component_reason.startswith("blocked_second_stage_confirmation"):
        risk_gate_focus = "second_stage_confirmation"

    recommendation_focus_map = {
        "hold_candidate_fix_entry_quality_price_level_consistency": "entry_quality_invalid_levels",
        "hold_candidate_calibrate_risk_gate_rr_baseline_floor": "entry_quality_rr_base",
        "hold_candidate_calibrate_risk_gate_rr_adaptive_history_adders": "entry_quality_rr_adaptive_history",
        "hold_candidate_calibrate_risk_gate_rr_adaptive_regime_adders": "entry_quality_rr_adaptive_regime",
        "hold_candidate_calibrate_risk_gate_rr_adaptive_mixed_adders": "entry_quality_rr_adaptive_mixed",
        "hold_candidate_calibrate_risk_gate_rr_adaptive_adders": "entry_quality_rr_adaptive",
        "hold_candidate_calibrate_risk_gate_entry_quality_rr": "entry_quality_rr",
        "hold_candidate_calibrate_risk_gate_edge_baseline_floor": "entry_quality_edge_base",
        "hold_candidate_calibrate_risk_gate_edge_adaptive_history_adders": "entry_quality_edge_adaptive_history",
        "hold_candidate_calibrate_risk_gate_edge_adaptive_regime_adders": "entry_quality_edge_adaptive_regime",
        "hold_candidate_calibrate_risk_gate_edge_adaptive_mixed_adders": "entry_quality_edge_adaptive_mixed",
        "hold_candidate_calibrate_risk_gate_edge_adaptive_adders": "entry_quality_edge_adaptive",
        "hold_candidate_calibrate_risk_gate_entry_quality_edge": "entry_quality_edge",
        "hold_candidate_calibrate_risk_gate_entry_quality_rr_edge": "entry_quality_rr_edge",
        "hold_candidate_calibrate_risk_gate_entry_quality_ownership": "entry_quality",
    }
    recommendation_focus = recommendation_focus_map.get(holdout_recommendation, "")
    risk_gate_focus_source = "top_risk_component"
    if recommendation_focus:
        risk_gate_focus = recommendation_focus
        risk_gate_focus_source = "holdout_recommendation_override"

    context["top_group_live"] = top_group_live
    context["top_group_holdout"] = top_group_holdout
    context["holdout_recommendation"] = holdout_recommendation
    context["top_risk_gate_component_reason"] = top_risk_gate_component_reason
    context["risk_gate_focus"] = risk_gate_focus
    context["risk_gate_focus_source"] = risk_gate_focus_source
    context["top_group_source"] = "live"
    context["top_group_fallback_applied"] = False

    risk_gate_override = (
        holdout_recommendation.startswith("hold_candidate_calibrate_risk_gate_")
        or holdout_recommendation == "hold_candidate_fix_entry_quality_price_level_consistency"
    )
    if risk_gate_override:
        context["top_group"] = "risk_gate"
        context["top_group_source"] = "holdout_recommendation_override"
        context["top_group_fallback_applied"] = True
        return context

    if top_group_live:
        return context

    if top_group_holdout in {"signal_generation", "manager_prefilter", "position_state", "risk_gate"}:
        context["top_group"] = top_group_holdout
        context["top_group_source"] = "holdout_fallback"
        context["top_group_fallback_applied"] = True
        # When live funnel top-group is unavailable, inject a conservative synthetic
        # share to avoid neutral-only family routing during holdout-driven failures.
        if top_group_holdout == "signal_generation":
            context["signal_generation_share"] = max(
                float(context.get("signal_generation_share", 0.0) or 0.0),
                0.70,
            )
        elif top_group_holdout == "manager_prefilter":
            context["manager_prefilter_share"] = max(
                float(context.get("manager_prefilter_share", 0.0) or 0.0),
                0.60,
            )
    return context


def compute_combo_bottleneck_priority_score(combo: Dict[str, Any], context: Dict[str, Any]) -> float:
    top_group = str(context.get("top_group", ""))
    risk_gate_focus = str(context.get("risk_gate_focus", ""))
    holdout_recommendation = str(context.get("holdout_recommendation", ""))
    no_trade_bias_active = bool(context.get("no_trade_bias_active", False))
    signal_generation_share = float(context.get("signal_generation_share", 0.0) or 0.0)
    manager_prefilter_share = float(context.get("manager_prefilter_share", 0.0) or 0.0)
    position_state_share = float(context.get("position_state_share", 0.0) or 0.0)

    min_expected_edge_pct = float(combo.get("min_expected_edge_pct", 0.0010))
    min_reward_risk = float(combo.get("min_reward_risk", 1.20))
    avg_strength = (
        float(combo.get("scalping_min_signal_strength", 0.70))
        + float(combo.get("momentum_min_signal_strength", 0.72))
        + float(combo.get("breakout_min_signal_strength", 0.40))
        + float(combo.get("mean_reversion_min_signal_strength", 0.40))
    ) / 4.0
    max_new_orders = int(combo.get("max_new_orders_per_scan", 2))
    avoid_high_vol = bool(combo.get("avoid_high_volatility", True))
    avoid_down = bool(combo.get("avoid_trending_down", True))
    min_strategy_pf = float(combo.get("min_strategy_profit_factor", 0.95))
    min_strategy_ev = float(combo.get("min_strategy_expectancy_krw", -1.0))

    # Relaxation affinity: higher means easier to generate entries.
    relax_score = 0.0
    relax_score += max(0.0, (0.0018 - min_expected_edge_pct) * 12000.0)
    relax_score += max(0.0, (1.55 - min_reward_risk) * 55.0)
    relax_score += max(0.0, (0.80 - avg_strength) * 90.0)
    relax_score += max(0, max_new_orders - 1) * 8.0
    if not avoid_high_vol:
        relax_score += 6.0
    if not avoid_down:
        relax_score += 5.0
    relax_score += max(0.0, (1.15 - min_strategy_pf) * 40.0)
    relax_score += max(0.0, (0.0 - min_strategy_ev) * 2.0)

    # Quality affinity: higher means tighter quality preference.
    quality_score = 0.0
    quality_score += max(0.0, (min_expected_edge_pct - 0.0006) * 12000.0)
    quality_score += max(0.0, (min_reward_risk - 1.00) * 65.0)
    quality_score += max(0.0, (avg_strength - 0.60) * 80.0)
    quality_score += max(0.0, (min_strategy_pf - 0.90) * 55.0)
    quality_score += max(0.0, min_strategy_ev) * 2.0
    if avoid_high_vol:
        quality_score += 3.0
    if avoid_down:
        quality_score += 3.0

    if top_group == "position_state" and position_state_share >= 0.40:
        # Entries already happen; prioritize quality-preserving candidates.
        return round(quality_score, 6)

    if top_group == "manager_prefilter":
        # Emphasize modest relaxation around prefilter bottlenecks.
        return round((relax_score * 0.70) + (quality_score * 0.30), 6)

    if top_group == "signal_generation" or no_trade_bias_active:
        # Signal-generation bottleneck: prioritize candidates that can create more
        # valid opportunities while still carrying some quality term.
        boost = 1.0 + min(0.8, signal_generation_share + (manager_prefilter_share * 0.5))
        return round((relax_score * boost * 0.85) + (quality_score * 0.15), 6)

    if top_group == "risk_gate":
        if risk_gate_focus == "entry_quality_invalid_levels":
            # Price-level inconsistency is structural; keep exploration conservative.
            return round((quality_score * 0.92) + (relax_score * 0.08), 6)
        if (
            holdout_recommendation == "hold_candidate_calibrate_risk_gate_edge_baseline_floor"
            or risk_gate_focus == "entry_quality_edge_base"
        ):
            # Edge baseline bottleneck: allow controlled relaxation search while
            # retaining quality term to prevent holdout RR regression.
            return round((quality_score * 0.70) + (relax_score * 0.30), 6)
        if risk_gate_focus in {"entry_quality_rr_base", "entry_quality_rr_edge_base"}:
            return round((quality_score * 0.90) + (relax_score * 0.10), 6)
        if risk_gate_focus in {
            "entry_quality_rr_adaptive",
            "entry_quality_edge_adaptive",
            "entry_quality_rr_edge_adaptive",
            "entry_quality_rr_adaptive_history",
            "entry_quality_rr_adaptive_regime",
            "entry_quality_rr_adaptive_mixed",
            "entry_quality_edge_adaptive_history",
            "entry_quality_edge_adaptive_regime",
            "entry_quality_edge_adaptive_mixed",
            "entry_quality_rr_edge_adaptive_history",
            "entry_quality_rr_edge_adaptive_regime",
            "entry_quality_rr_edge_adaptive_mixed",
        }:
            return round((quality_score * 0.84) + (relax_score * 0.16), 6)
        if risk_gate_focus in {"entry_quality_rr", "entry_quality_edge", "entry_quality_rr_edge"}:
            return round((quality_score * 0.88) + (relax_score * 0.12), 6)
        if risk_gate_focus in {"entry_quality", "second_stage_confirmation"}:
            # Overfit-safe policy: keep quality-oriented candidates first.
            return round((quality_score * 0.85) + (relax_score * 0.15), 6)
        return round((quality_score * 0.75) + (relax_score * 0.25), 6)

    # Unknown/neutral bottleneck: keep balance.
    return round((relax_score * 0.45) + (quality_score * 0.55), 6)


def prioritize_combo_specs_for_bottleneck(
    combos: List[Dict[str, Any]],
    context: Dict[str, Any],
) -> Tuple[List[Dict[str, Any]], Dict[str, Dict[str, Any]]]:
    scored: List[Tuple[float, int, Dict[str, Any]]] = []
    for index, combo in enumerate(combos):
        score = compute_combo_bottleneck_priority_score(combo, context)
        scored.append((score, index, combo))

    scored_sorted = sorted(scored, key=lambda x: (x[0], -x[1]), reverse=True)
    ordered = [x[2] for x in scored_sorted]
    meta: Dict[str, Dict[str, Any]] = {}
    for rank, (score, _idx, combo) in enumerate(scored_sorted, start=1):
        combo_id = str(combo.get("combo_id", ""))
        meta[combo_id] = {
            "priority_rank": int(rank),
            "priority_score": float(score),
            "top_group": str(context.get("top_group", "")),
            "no_trade_bias_active": bool(context.get("no_trade_bias_active", False)),
        }
    return ordered, meta


def _clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def apply_hint_impact_guardrail(
    combo: Dict[str, Any],
    baseline: Dict[str, Any],
    tighten_scale: float,
) -> None:
    scale = _clamp(float(tighten_scale), 0.0, 1.0)
    if scale <= 0.0:
        return

    base_edge = float(baseline.get("min_expected_edge_pct", combo.get("min_expected_edge_pct", 0.0010)))
    base_rr = float(baseline.get("min_reward_risk", combo.get("min_reward_risk", 1.20)))
    base_pf = float(baseline.get("min_strategy_profit_factor", combo.get("min_strategy_profit_factor", 0.95)))
    base_ev = float(baseline.get("min_strategy_expectancy_krw", combo.get("min_strategy_expectancy_krw", -1.0)))
    base_orders = int(baseline.get("max_new_orders_per_scan", combo.get("max_new_orders_per_scan", 2)))

    cur_edge = float(combo.get("min_expected_edge_pct", base_edge))
    cur_rr = float(combo.get("min_reward_risk", base_rr))
    cur_pf = float(combo.get("min_strategy_profit_factor", base_pf))
    cur_ev = float(combo.get("min_strategy_expectancy_krw", base_ev))
    cur_orders = int(combo.get("max_new_orders_per_scan", base_orders))

    combo["min_expected_edge_pct"] = round(
        _clamp(cur_edge + (base_edge - cur_edge) * scale, 0.0005, 0.0019), 4
    )
    combo["min_reward_risk"] = round(
        _clamp(cur_rr + (base_rr - cur_rr) * scale, 1.00, 1.80), 2
    )
    combo["min_strategy_profit_factor"] = round(
        _clamp(cur_pf + (base_pf - cur_pf) * scale, 0.85, 1.30), 2
    )
    combo["min_strategy_expectancy_krw"] = round(cur_ev + (base_ev - cur_ev) * scale, 2)
    combo["max_new_orders_per_scan"] = int(
        max(1, min(4, round(cur_orders + (base_orders - cur_orders) * scale)))
    )

    for key, lo, hi in (
        ("scalping_min_signal_strength", 0.58, 0.90),
        ("momentum_min_signal_strength", 0.58, 0.90),
        ("breakout_min_signal_strength", 0.30, 0.55),
        ("mean_reversion_min_signal_strength", 0.30, 0.55),
    ):
        base_v = float(baseline.get(key, combo.get(key, lo)))
        cur_v = float(combo.get(key, base_v))
        combo[key] = round(_clamp(cur_v + (base_v - cur_v) * scale, lo, hi), 2)

    if bool(baseline.get("avoid_high_volatility", True)) and scale >= 0.34:
        combo["avoid_high_volatility"] = True
    if bool(baseline.get("avoid_trending_down", True)) and scale >= 0.66:
        combo["avoid_trending_down"] = True


def apply_holdout_failure_family_suppression(
    combos: List[Dict[str, Any]],
    live_context: Dict[str, Any],
    holdout_context: Dict[str, Any],
    enabled: bool,
    hint_ratio_threshold: float,
    require_both_pf_exp_fail: bool,
) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    base_meta: Dict[str, Any] = {
        "enabled": bool(enabled),
        "active": False,
        "reason": "",
        "hint_ratio_threshold": float(hint_ratio_threshold),
        "live_hint_adjusted_ratio": float(live_context.get("selection_hint_adjusted_ratio", 0.0) or 0.0),
        "require_both_pf_exp_fail": bool(require_both_pf_exp_fail),
        "holdout_context": holdout_context,
        "suppressed_families": [],
        "suppressed_combo_count": 0,
        "kept_combo_count": len(combos),
        "fallback_retained_combo_id": "",
        "fail_open_all_suppressed": False,
    }
    for combo in combos:
        combo["holdout_failure_suppression_active"] = False
        combo["holdout_failure_suppressed_family"] = False
        combo["holdout_failure_suppression_reason"] = ""

    if not bool(enabled):
        base_meta["reason"] = "disabled"
        return combos, base_meta
    if not bool(holdout_context.get("exists", False)):
        base_meta["reason"] = "missing_train_eval_summary"
        return combos, base_meta
    if not bool(holdout_context.get("holdout_found", False)):
        base_meta["reason"] = "holdout_stage_not_found"
        return combos, base_meta

    hint_ratio = float(base_meta["live_hint_adjusted_ratio"])
    if hint_ratio < float(hint_ratio_threshold):
        base_meta["reason"] = "hint_ratio_below_threshold"
        return combos, base_meta

    holdout_pf = float(holdout_context.get("avg_profit_factor", 0.0) or 0.0)
    holdout_exp = float(holdout_context.get("avg_expectancy_krw", 0.0) or 0.0)
    pf_fail = holdout_pf < 1.0
    exp_fail = holdout_exp < 0.0
    quality_fail = (pf_fail and exp_fail) if bool(require_both_pf_exp_fail) else (pf_fail or exp_fail)
    if not quality_fail:
        base_meta["reason"] = "holdout_quality_not_failed"
        return combos, base_meta

    holdout_top_group = str(holdout_context.get("top_rejection_group", "") or "")
    if holdout_top_group not in {"signal_generation", "risk_gate"}:
        base_meta["reason"] = "holdout_group_not_targeted"
        return combos, base_meta

    suppressed_families = {"signal_generation_boost", "manager_prefilter_relax"}
    kept: List[Dict[str, Any]] = []
    suppressed: List[Dict[str, Any]] = []
    for combo in combos:
        family = str(combo.get("bottleneck_scenario_family", ""))
        suppress = bool(family in suppressed_families and str(combo.get("combo_id", "")) != "baseline_current")
        if suppress:
            combo["holdout_failure_suppression_active"] = True
            combo["holdout_failure_suppressed_family"] = True
            combo["holdout_failure_suppression_reason"] = (
                f"holdout_{holdout_top_group}_quality_fail_high_hint_ratio"
            )
            suppressed.append(combo)
        else:
            combo["holdout_failure_suppression_active"] = True
            combo["holdout_failure_suppressed_family"] = False
            combo["holdout_failure_suppression_reason"] = "active_not_suppressed"
            kept.append(combo)

    fallback_retained_combo_id = ""
    fail_open_all_suppressed = False
    if not kept and suppressed:
        fail_open_all_suppressed = True
        retained = deepcopy(suppressed[0])
        retained["holdout_failure_suppressed_family"] = False
        retained["holdout_failure_suppression_reason"] = "fallback_retain_single_combo_after_suppression"
        if retained.get("bottleneck_scenario_family") in suppressed_families:
            apply_hint_impact_guardrail(
                combo=retained,
                baseline=retained,
                tighten_scale=0.85,
            )
        kept = [retained]
        fallback_retained_combo_id = str(retained.get("combo_id", ""))

    base_meta.update(
        {
            "active": True,
            "reason": f"activated_by_holdout_{holdout_top_group}_quality_fail",
            "suppressed_families": sorted(suppressed_families),
            "suppressed_combo_count": len(suppressed),
            "kept_combo_count": len(kept),
            "fallback_retained_combo_id": fallback_retained_combo_id,
            "fail_open_all_suppressed": bool(fail_open_all_suppressed),
        }
    )
    return kept, base_meta


def expand_post_suppression_quality_exit_candidates(
    combos: List[Dict[str, Any]],
    suppression_meta: Dict[str, Any],
    context: Dict[str, Any],
    enabled: bool,
    min_combo_count: int,
) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    min_count = max(1, int(min_combo_count))
    meta: Dict[str, Any] = {
        "enabled": bool(enabled),
        "applied": False,
        "reason": "",
        "target_min_combo_count": int(min_count),
        "input_combo_count": len(combos),
        "output_combo_count": len(combos),
        "injected_combo_count": 0,
        "injected_combo_ids": [],
    }
    if not bool(enabled):
        meta["reason"] = "disabled"
        return combos, meta
    if len(combos) >= min_count:
        meta["reason"] = "already_enough_combos"
        return combos, meta
    if not bool((suppression_meta or {}).get("active", False)):
        meta["reason"] = "suppression_inactive"
        return combos, meta
    suppressed_combo_count = int((suppression_meta or {}).get("suppressed_combo_count", 0) or 0)
    fail_open_all_suppressed = bool((suppression_meta or {}).get("fail_open_all_suppressed", False))
    if suppressed_combo_count <= 0 and not fail_open_all_suppressed:
        meta["reason"] = "suppression_not_effective"
        return combos, meta
    if not combos:
        meta["reason"] = "no_kept_combo_for_expansion"
        return combos, meta

    out = [deepcopy(x) for x in combos]
    base = deepcopy(out[0])
    base_id = str(base.get("combo_id", "combo"))
    top_group = str(context.get("top_group", "") or "")
    needed = max(0, min_count - len(out))
    injected_ids: List[str] = []

    for idx in range(1, needed + 1):
        clone = deepcopy(base)
        clone["combo_id"] = f"{base_id}_quality_exit_{idx:02d}"
        edge = float(base.get("min_expected_edge_pct", 0.0010))
        rr = float(base.get("min_reward_risk", 1.20))
        rr_weak = float(base.get("min_rr_weak_signal", 1.80))
        rr_strong = float(base.get("min_rr_strong_signal", 1.20))
        min_pf = float(base.get("min_strategy_profit_factor", 0.95))
        min_ev = float(base.get("min_strategy_expectancy_krw", -1.0))
        orders = int(base.get("max_new_orders_per_scan", 2))

        clone["min_expected_edge_pct"] = round(_clamp(edge * (1.0 + (0.12 * idx)), 0.0006, 0.0020), 4)
        clone["min_reward_risk"] = round(_clamp(rr + (0.10 * idx), 1.05, 1.90), 2)
        clone["min_rr_weak_signal"] = round(_clamp(rr_weak + (0.16 * idx), 1.20, 2.60), 2)
        clone["min_rr_strong_signal"] = round(_clamp(rr_strong + (0.10 * idx), 0.95, 1.95), 2)
        clone["min_strategy_profit_factor"] = round(_clamp(min_pf + (0.04 * idx), 0.90, 1.35), 2)
        clone["min_strategy_expectancy_krw"] = round(min_ev + (0.80 * idx), 2)
        clone["max_new_orders_per_scan"] = int(max(1, min(4, orders - idx)))

        clone["scalping_min_signal_strength"] = round(
            _clamp(float(base.get("scalping_min_signal_strength", 0.70)) + (0.03 * idx), 0.60, 0.92), 2
        )
        clone["momentum_min_signal_strength"] = round(
            _clamp(float(base.get("momentum_min_signal_strength", 0.72)) + (0.03 * idx), 0.60, 0.92), 2
        )
        clone["breakout_min_signal_strength"] = round(
            _clamp(float(base.get("breakout_min_signal_strength", 0.40)) + (0.02 * idx), 0.30, 0.60), 2
        )
        clone["mean_reversion_min_signal_strength"] = round(
            _clamp(float(base.get("mean_reversion_min_signal_strength", 0.40)) + (0.02 * idx), 0.30, 0.60), 2
        )
        clone["avoid_high_volatility"] = True
        clone["avoid_trending_down"] = True

        clone["bottleneck_scenario_family"] = "quality_exit_rebalance"
        clone["bottleneck_scenario_family_source_group"] = top_group
        clone["bottleneck_scenario_family_adapted"] = True
        clone["bottleneck_hint_guardrail_active"] = False

        clone["holdout_failure_suppression_active"] = bool((suppression_meta or {}).get("active", False))
        clone["holdout_failure_suppressed_family"] = False
        clone["holdout_failure_suppression_reason"] = "post_suppression_quality_exit_injected"

        out.append(clone)
        injected_ids.append(str(clone["combo_id"]))

    meta.update(
        {
            "applied": bool(injected_ids),
            "reason": "expanded_quality_exit_after_effective_suppression" if injected_ids else "no_injection_needed",
            "output_combo_count": len(out),
            "injected_combo_count": len(injected_ids),
            "injected_combo_ids": injected_ids,
        }
    )
    return out, meta


def adapt_combo_specs_for_bottleneck(
    combos: List[Dict[str, Any]],
    context: Dict[str, Any],
    scenario_mode: str,
    enable_hint_impact_guardrail: bool,
    hint_impact_guardrail_ratio: float,
    hint_impact_guardrail_tighten_scale: float,
) -> Tuple[List[Dict[str, Any]], Dict[str, int]]:
    top_group = str(context.get("top_group", ""))
    risk_gate_focus = str(context.get("risk_gate_focus", ""))
    holdout_recommendation = str(context.get("holdout_recommendation", ""))
    no_trade_bias_active = bool(context.get("no_trade_bias_active", False))
    signal_generation_share = float(context.get("signal_generation_share", 0.0) or 0.0)
    selection_hint_adjusted_ratio = float(context.get("selection_hint_adjusted_ratio", 0.0) or 0.0)
    guardrail_active = bool(enable_hint_impact_guardrail) and (
        selection_hint_adjusted_ratio >= float(hint_impact_guardrail_ratio)
    )

    if scenario_mode == "legacy_only":
        out: List[Dict[str, Any]] = []
        family_counts: Dict[str, int] = {}
        for combo in combos:
            clone = deepcopy(combo)
            clone["bottleneck_scenario_family"] = "legacy_baseline"
            clone["bottleneck_scenario_family_source_group"] = top_group
            clone["bottleneck_scenario_family_adapted"] = False
            clone["bottleneck_hint_guardrail_active"] = False
            clone["bottleneck_hint_guardrail_ratio"] = round(selection_hint_adjusted_ratio, 6)
            clone["bottleneck_hint_guardrail_threshold"] = float(hint_impact_guardrail_ratio)
            clone["bottleneck_hint_guardrail_tighten_scale"] = float(hint_impact_guardrail_tighten_scale)
            out.append(clone)
            family_counts["legacy_baseline"] = family_counts.get("legacy_baseline", 0) + 1
        return out, family_counts

    if top_group == "position_state":
        family = "position_turnover_quality"
    elif top_group == "risk_gate":
        if holdout_recommendation == "hold_candidate_fix_entry_quality_price_level_consistency":
            family = "risk_gate_structure_guard"
        elif (
            holdout_recommendation == "hold_candidate_calibrate_risk_gate_edge_baseline_floor"
            or risk_gate_focus == "entry_quality_edge_base"
        ):
            family = "risk_gate_edge_baseline_floor_sweep"
        elif risk_gate_focus in {
            "entry_quality_rr_adaptive",
            "entry_quality_edge_adaptive",
            "entry_quality_rr_edge_adaptive",
            "entry_quality_rr_adaptive_history",
            "entry_quality_rr_adaptive_regime",
            "entry_quality_rr_adaptive_mixed",
            "entry_quality_edge_adaptive_history",
            "entry_quality_edge_adaptive_regime",
            "entry_quality_edge_adaptive_mixed",
            "entry_quality_rr_edge_adaptive_history",
            "entry_quality_rr_edge_adaptive_regime",
            "entry_quality_rr_edge_adaptive_mixed",
        }:
            family = "risk_gate_adaptive_rebalance"
        else:
            family = "risk_gate_quality_rebalance"
    elif top_group == "manager_prefilter":
        family = "manager_prefilter_relax"
    elif top_group == "signal_generation" or no_trade_bias_active:
        family = "signal_generation_boost"
    else:
        family = "neutral_balance"

    out: List[Dict[str, Any]] = []
    family_counts: Dict[str, int] = {}
    for combo in combos:
        clone = deepcopy(combo)
        combo_id = str(clone.get("combo_id", ""))
        adapted = combo_id != "baseline_current"
        guardrail_applied = False

        if adapted and family == "signal_generation_boost":
            clone["min_expected_edge_pct"] = round(
                _clamp(float(clone.get("min_expected_edge_pct", 0.0010)) * 0.88, 0.0005, 0.0016), 4
            )
            clone["min_reward_risk"] = round(
                _clamp(float(clone.get("min_reward_risk", 1.20)) - 0.08, 1.00, 1.60), 2
            )
            clone["min_rr_weak_signal"] = round(
                _clamp(float(clone.get("min_rr_weak_signal", 1.80)) - 0.15, 1.10, 2.20), 2
            )
            clone["min_rr_strong_signal"] = round(
                _clamp(float(clone.get("min_rr_strong_signal", 1.20)) - 0.08, 0.85, 1.60), 2
            )
            clone["min_strategy_trades_for_ev"] = int(
                max(10, round(float(clone.get("min_strategy_trades_for_ev", 30)) * 0.80))
            )
            clone["min_strategy_profit_factor"] = round(
                _clamp(float(clone.get("min_strategy_profit_factor", 0.95)) - 0.03, 0.85, 1.20), 2
            )
            clone["min_strategy_expectancy_krw"] = round(
                float(clone.get("min_strategy_expectancy_krw", -1.0)) - 0.50, 2
            )
            clone["max_new_orders_per_scan"] = int(max(1, min(4, int(clone.get("max_new_orders_per_scan", 2)) + 1)))
            clone["scalping_min_signal_strength"] = round(
                _clamp(float(clone.get("scalping_min_signal_strength", 0.70)) - 0.04, 0.58, 0.84), 2
            )
            clone["momentum_min_signal_strength"] = round(
                _clamp(float(clone.get("momentum_min_signal_strength", 0.72)) - 0.05, 0.58, 0.84), 2
            )
            clone["breakout_min_signal_strength"] = round(
                _clamp(float(clone.get("breakout_min_signal_strength", 0.40)) - 0.02, 0.30, 0.52), 2
            )
            clone["mean_reversion_min_signal_strength"] = round(
                _clamp(float(clone.get("mean_reversion_min_signal_strength", 0.40)) - 0.02, 0.30, 0.52), 2
            )
            clone["hostility_pause_scans"] = int(max(1, int(clone.get("hostility_pause_scans", 4)) - 1))
            clone["backtest_hostility_pause_candles"] = int(
                max(8, int(clone.get("backtest_hostility_pause_candles", 36)) - 6)
            )
            if no_trade_bias_active and signal_generation_share >= 0.60:
                clone["avoid_high_volatility"] = False

        elif adapted and family == "manager_prefilter_relax":
            clone["min_expected_edge_pct"] = round(
                _clamp(float(clone.get("min_expected_edge_pct", 0.0010)) * 0.93, 0.0006, 0.0017), 4
            )
            clone["min_reward_risk"] = round(
                _clamp(float(clone.get("min_reward_risk", 1.20)) - 0.05, 1.00, 1.70), 2
            )
            clone["min_strategy_trades_for_ev"] = int(max(12, int(clone.get("min_strategy_trades_for_ev", 30)) - 5))
            clone["min_strategy_profit_factor"] = round(
                _clamp(float(clone.get("min_strategy_profit_factor", 0.95)) - 0.02, 0.88, 1.20), 2
            )
            clone["scalping_min_signal_strength"] = round(
                _clamp(float(clone.get("scalping_min_signal_strength", 0.70)) - 0.03, 0.60, 0.84), 2
            )
            clone["momentum_min_signal_strength"] = round(
                _clamp(float(clone.get("momentum_min_signal_strength", 0.72)) - 0.03, 0.60, 0.84), 2
            )
            clone["breakout_min_signal_strength"] = round(
                _clamp(float(clone.get("breakout_min_signal_strength", 0.40)) - 0.02, 0.30, 0.52), 2
            )
            clone["mean_reversion_min_signal_strength"] = round(
                _clamp(float(clone.get("mean_reversion_min_signal_strength", 0.40)) - 0.02, 0.30, 0.52), 2
            )

        elif adapted and family == "position_turnover_quality":
            clone["min_expected_edge_pct"] = round(
                _clamp(float(clone.get("min_expected_edge_pct", 0.0010)) * 1.08, 0.0007, 0.0018), 4
            )
            clone["min_reward_risk"] = round(
                _clamp(float(clone.get("min_reward_risk", 1.20)) + 0.07, 1.05, 1.80), 2
            )
            clone["min_strategy_trades_for_ev"] = int(min(70, int(clone.get("min_strategy_trades_for_ev", 30)) + 5))
            clone["min_strategy_profit_factor"] = round(
                _clamp(float(clone.get("min_strategy_profit_factor", 0.95)) + 0.03, 0.90, 1.30), 2
            )
            clone["max_new_orders_per_scan"] = int(max(1, int(clone.get("max_new_orders_per_scan", 2)) - 1))
            clone["scalping_min_signal_strength"] = round(
                _clamp(float(clone.get("scalping_min_signal_strength", 0.70)) + 0.02, 0.60, 0.90), 2
            )
            clone["momentum_min_signal_strength"] = round(
                _clamp(float(clone.get("momentum_min_signal_strength", 0.72)) + 0.02, 0.60, 0.90), 2
            )
            clone["breakout_min_signal_strength"] = round(
                _clamp(float(clone.get("breakout_min_signal_strength", 0.40)) + 0.01, 0.30, 0.55), 2
            )
            clone["mean_reversion_min_signal_strength"] = round(
                _clamp(float(clone.get("mean_reversion_min_signal_strength", 0.40)) + 0.01, 0.30, 0.55), 2
            )
            clone["avoid_high_volatility"] = True
            clone["avoid_trending_down"] = True

        elif adapted and family == "risk_gate_edge_baseline_floor_sweep":
            variant = (sum(ord(c) for c in combo_id) % 3)
            edge_mul_grid = (0.86, 0.90, 0.94)
            rr_add_grid = (0.05, 0.03, 0.02)
            weak_add_grid = (0.10, 0.07, 0.05)
            strong_add_grid = (0.07, 0.05, 0.03)
            pf_add_grid = (0.03, 0.02, 0.01)
            ev_add_grid = (0.35, 0.22, 0.12)
            strength_add_grid = (0.02, 0.01, 0.01)

            clone["min_expected_edge_pct"] = round(
                _clamp(
                    float(clone.get("min_expected_edge_pct", 0.0010)) * edge_mul_grid[variant],
                    0.0005,
                    0.0016,
                ),
                4,
            )
            clone["min_reward_risk"] = round(
                _clamp(float(clone.get("min_reward_risk", 1.20)) + rr_add_grid[variant], 1.05, 1.85),
                2,
            )
            clone["min_rr_weak_signal"] = round(
                _clamp(float(clone.get("min_rr_weak_signal", 1.80)) + weak_add_grid[variant], 1.20, 2.50),
                2,
            )
            clone["min_rr_strong_signal"] = round(
                _clamp(float(clone.get("min_rr_strong_signal", 1.20)) + strong_add_grid[variant], 0.95, 1.85),
                2,
            )
            clone["min_strategy_trades_for_ev"] = int(
                min(85, max(24, int(clone.get("min_strategy_trades_for_ev", 30)) + (2 + variant)))
            )
            clone["min_strategy_profit_factor"] = round(
                _clamp(float(clone.get("min_strategy_profit_factor", 0.95)) + pf_add_grid[variant], 0.90, 1.35),
                2,
            )
            clone["min_strategy_expectancy_krw"] = round(
                float(clone.get("min_strategy_expectancy_krw", -1.0)) + ev_add_grid[variant],
                2,
            )
            clone["max_new_orders_per_scan"] = int(
                max(1, min(3, int(clone.get("max_new_orders_per_scan", 2)) - (1 if variant == 0 else 0)))
            )
            clone["scalping_min_signal_strength"] = round(
                _clamp(float(clone.get("scalping_min_signal_strength", 0.70)) + strength_add_grid[variant], 0.60, 0.90),
                2,
            )
            clone["momentum_min_signal_strength"] = round(
                _clamp(float(clone.get("momentum_min_signal_strength", 0.72)) + strength_add_grid[variant], 0.60, 0.90),
                2,
            )
            clone["breakout_min_signal_strength"] = round(
                _clamp(float(clone.get("breakout_min_signal_strength", 0.40)) + (0.01 * (1 if variant != 2 else 0)), 0.32, 0.58),
                2,
            )
            clone["mean_reversion_min_signal_strength"] = round(
                _clamp(float(clone.get("mean_reversion_min_signal_strength", 0.40)) + (0.01 * (1 if variant != 2 else 0)), 0.32, 0.58),
                2,
            )
            clone["avoid_high_volatility"] = True
            clone["avoid_trending_down"] = True
            clone["hostility_pause_scans"] = int(min(12, int(clone.get("hostility_pause_scans", 4)) + (1 if variant == 0 else 0)))
            clone["backtest_hostility_pause_candles"] = int(
                min(180, int(clone.get("backtest_hostility_pause_candles", 36)) + (6 if variant == 0 else 2))
            )
            clone["bottleneck_edge_baseline_variant"] = int(variant)

        elif adapted and family == "risk_gate_structure_guard":
            clone["min_expected_edge_pct"] = round(
                _clamp(float(clone.get("min_expected_edge_pct", 0.0010)) * 1.15, 0.0008, 0.0020), 4
            )
            clone["min_reward_risk"] = round(
                _clamp(float(clone.get("min_reward_risk", 1.20)) + 0.14, 1.10, 2.00), 2
            )
            clone["min_rr_weak_signal"] = round(
                _clamp(float(clone.get("min_rr_weak_signal", 1.80)) + 0.12, 1.25, 2.40), 2
            )
            clone["min_rr_strong_signal"] = round(
                _clamp(float(clone.get("min_rr_strong_signal", 1.20)) + 0.10, 1.00, 1.80), 2
            )
            clone["min_strategy_trades_for_ev"] = int(min(90, int(clone.get("min_strategy_trades_for_ev", 30)) + 10))
            clone["min_strategy_profit_factor"] = round(
                _clamp(float(clone.get("min_strategy_profit_factor", 0.95)) + 0.05, 0.92, 1.40), 2
            )
            clone["min_strategy_expectancy_krw"] = round(
                float(clone.get("min_strategy_expectancy_krw", -1.0)) + 0.70, 2
            )
            clone["max_new_orders_per_scan"] = int(max(1, int(clone.get("max_new_orders_per_scan", 2)) - 1))
            clone["scalping_min_signal_strength"] = round(
                _clamp(float(clone.get("scalping_min_signal_strength", 0.70)) + 0.04, 0.62, 0.94), 2
            )
            clone["momentum_min_signal_strength"] = round(
                _clamp(float(clone.get("momentum_min_signal_strength", 0.72)) + 0.04, 0.62, 0.94), 2
            )
            clone["breakout_min_signal_strength"] = round(
                _clamp(float(clone.get("breakout_min_signal_strength", 0.40)) + 0.03, 0.32, 0.60), 2
            )
            clone["mean_reversion_min_signal_strength"] = round(
                _clamp(float(clone.get("mean_reversion_min_signal_strength", 0.40)) + 0.03, 0.32, 0.60), 2
            )
            clone["avoid_high_volatility"] = True
            clone["avoid_trending_down"] = True
            clone["hostility_pause_scans"] = int(min(14, int(clone.get("hostility_pause_scans", 4)) + 2))
            clone["backtest_hostility_pause_candles"] = int(
                min(210, int(clone.get("backtest_hostility_pause_candles", 36)) + 12)
            )

        elif adapted and family == "risk_gate_adaptive_rebalance":
            # Adaptive RR adders suspected too strict: keep quality, but avoid aggressive tightening.
            clone["min_expected_edge_pct"] = round(
                _clamp(float(clone.get("min_expected_edge_pct", 0.0010)) * 1.03, 0.0006, 0.0017), 4
            )
            clone["min_reward_risk"] = round(
                _clamp(float(clone.get("min_reward_risk", 1.20)) + 0.02, 1.00, 1.70), 2
            )
            clone["min_rr_weak_signal"] = round(
                _clamp(float(clone.get("min_rr_weak_signal", 1.80)) + 0.03, 1.10, 2.20), 2
            )
            clone["min_rr_strong_signal"] = round(
                _clamp(float(clone.get("min_rr_strong_signal", 1.20)) + 0.02, 0.90, 1.60), 2
            )
            clone["min_strategy_trades_for_ev"] = int(min(72, int(clone.get("min_strategy_trades_for_ev", 30)) + 4))
            clone["min_strategy_profit_factor"] = round(
                _clamp(float(clone.get("min_strategy_profit_factor", 0.95)) + 0.01, 0.88, 1.25), 2
            )
            clone["min_strategy_expectancy_krw"] = round(
                float(clone.get("min_strategy_expectancy_krw", -1.0)) + 0.15, 2
            )
            clone["max_new_orders_per_scan"] = int(max(1, int(clone.get("max_new_orders_per_scan", 2)) - 1))
            clone["scalping_min_signal_strength"] = round(
                _clamp(float(clone.get("scalping_min_signal_strength", 0.70)) + 0.01, 0.60, 0.90), 2
            )
            clone["momentum_min_signal_strength"] = round(
                _clamp(float(clone.get("momentum_min_signal_strength", 0.72)) + 0.01, 0.60, 0.90), 2
            )
            clone["breakout_min_signal_strength"] = round(
                _clamp(float(clone.get("breakout_min_signal_strength", 0.40)) + 0.01, 0.30, 0.56), 2
            )
            clone["mean_reversion_min_signal_strength"] = round(
                _clamp(float(clone.get("mean_reversion_min_signal_strength", 0.40)) + 0.01, 0.30, 0.56), 2
            )
            clone["avoid_high_volatility"] = True
            clone["avoid_trending_down"] = True

        elif adapted and family == "risk_gate_quality_rebalance":
            clone["min_expected_edge_pct"] = round(
                _clamp(float(clone.get("min_expected_edge_pct", 0.0010)) * 1.10, 0.0007, 0.0019), 4
            )
            clone["min_reward_risk"] = round(
                _clamp(float(clone.get("min_reward_risk", 1.20)) + 0.10, 1.05, 1.90), 2
            )
            clone["min_rr_weak_signal"] = round(
                _clamp(float(clone.get("min_rr_weak_signal", 1.80)) + 0.08, 1.20, 2.30), 2
            )
            clone["min_rr_strong_signal"] = round(
                _clamp(float(clone.get("min_rr_strong_signal", 1.20)) + 0.06, 0.95, 1.70), 2
            )
            clone["min_strategy_trades_for_ev"] = int(min(80, int(clone.get("min_strategy_trades_for_ev", 30)) + 8))
            clone["min_strategy_profit_factor"] = round(
                _clamp(float(clone.get("min_strategy_profit_factor", 0.95)) + 0.04, 0.90, 1.35), 2
            )
            clone["min_strategy_expectancy_krw"] = round(
                float(clone.get("min_strategy_expectancy_krw", -1.0)) + 0.40, 2
            )
            clone["max_new_orders_per_scan"] = int(max(1, int(clone.get("max_new_orders_per_scan", 2)) - 1))
            clone["scalping_min_signal_strength"] = round(
                _clamp(float(clone.get("scalping_min_signal_strength", 0.70)) + 0.03, 0.60, 0.92), 2
            )
            clone["momentum_min_signal_strength"] = round(
                _clamp(float(clone.get("momentum_min_signal_strength", 0.72)) + 0.03, 0.60, 0.92), 2
            )
            clone["breakout_min_signal_strength"] = round(
                _clamp(float(clone.get("breakout_min_signal_strength", 0.40)) + 0.02, 0.30, 0.58), 2
            )
            clone["mean_reversion_min_signal_strength"] = round(
                _clamp(float(clone.get("mean_reversion_min_signal_strength", 0.40)) + 0.02, 0.30, 0.58), 2
            )
            clone["avoid_high_volatility"] = True
            clone["avoid_trending_down"] = True
            clone["hostility_pause_scans"] = int(min(12, int(clone.get("hostility_pause_scans", 4)) + 1))
            clone["backtest_hostility_pause_candles"] = int(
                min(180, int(clone.get("backtest_hostility_pause_candles", 36)) + 8)
            )

        if adapted and guardrail_active and family in ("signal_generation_boost", "manager_prefilter_relax"):
            apply_hint_impact_guardrail(
                combo=clone,
                baseline=combo,
                tighten_scale=hint_impact_guardrail_tighten_scale,
            )
            guardrail_applied = True

        clone["bottleneck_scenario_family"] = family
        clone["bottleneck_scenario_family_source_group"] = top_group
        clone["bottleneck_scenario_family_adapted"] = bool(adapted and family != "neutral_balance")
        clone["bottleneck_hint_guardrail_active"] = bool(guardrail_applied)
        clone["bottleneck_hint_guardrail_ratio"] = round(selection_hint_adjusted_ratio, 6)
        clone["bottleneck_hint_guardrail_threshold"] = float(hint_impact_guardrail_ratio)
        clone["bottleneck_hint_guardrail_tighten_scale"] = float(hint_impact_guardrail_tighten_scale)
        out.append(clone)
        fam = str(clone.get("bottleneck_scenario_family", "unknown"))
        family_counts[fam] = family_counts.get(fam, 0) + 1

    return out, family_counts


def resolve_or_throw(path_value: str, label: str) -> pathlib.Path:
    p = pathlib.Path(path_value)
    if not p.is_absolute():
        p = (pathlib.Path.cwd() / p).resolve()
    if not p.exists():
        raise FileNotFoundError(f"{label} not found: {path_value}")
    return p


def ensure_parent_directory(path_value: pathlib.Path) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)


def set_or_add_property(obj: Dict[str, Any], name: str, value: Any) -> None:
    obj[name] = value


def ensure_strategy_node(cfg: Dict[str, Any], strategy_name: str) -> None:
    if "strategies" not in cfg or not isinstance(cfg["strategies"], dict):
        cfg["strategies"] = {}
    if strategy_name not in cfg["strategies"] or not isinstance(cfg["strategies"][strategy_name], dict):
        cfg["strategies"][strategy_name] = {}


def apply_candidate_combo_to_config(cfg: Dict[str, Any], combo: Dict[str, Any]) -> None:
    if "trading" not in cfg or not isinstance(cfg["trading"], dict):
        cfg["trading"] = {}
    t = cfg["trading"]
    for k in (
        "max_new_orders_per_scan",
        "min_expected_edge_pct",
        "min_reward_risk",
        "min_rr_weak_signal",
        "min_rr_strong_signal",
        "min_strategy_trades_for_ev",
        "min_strategy_expectancy_krw",
        "min_strategy_profit_factor",
        "avoid_high_volatility",
        "avoid_trending_down",
        "hostility_ewma_alpha",
        "hostility_hostile_threshold",
        "hostility_severe_threshold",
        "hostility_extreme_threshold",
        "hostility_pause_scans",
        "hostility_pause_scans_extreme",
        "hostility_pause_recent_sample_min",
        "hostility_pause_recent_expectancy_krw",
        "hostility_pause_recent_win_rate",
        "backtest_hostility_pause_candles",
        "backtest_hostility_pause_candles_extreme",
    ):
        set_or_add_property(t, k, combo[k])

    for strategy in ("scalping", "momentum", "breakout", "mean_reversion"):
        ensure_strategy_node(cfg, strategy)
    cfg["strategies"]["scalping"]["min_signal_strength"] = combo["scalping_min_signal_strength"]
    cfg["strategies"]["momentum"]["min_signal_strength"] = combo["momentum_min_signal_strength"]
    cfg["strategies"]["breakout"]["min_signal_strength"] = combo["breakout_min_signal_strength"]
    cfg["strategies"]["mean_reversion"]["min_signal_strength"] = combo["mean_reversion_min_signal_strength"]


def has_higher_tf_companions(primary_path: pathlib.Path) -> bool:
    stem = primary_path.stem.lower()
    if not stem.startswith("upbit_") or "_1m_" not in stem:
        return False
    pivot = stem.index("_1m_")
    if pivot <= 6:
        return False
    market_token = stem[6:pivot]
    for tf in ("5m", "60m", "240m"):
        if not list(primary_path.parent.glob(f"upbit_{market_token}_{tf}_*.csv")):
            return False
    return True


def get_dataset_list(dirs: List[pathlib.Path], only_real_data: bool, require_higher_tf: bool) -> List[pathlib.Path]:
    all_items: List[pathlib.Path] = []
    for dir_path in dirs:
        if not dir_path.exists():
            continue
        is_real = "backtest_real" in str(dir_path).lower()
        if only_real_data and not is_real:
            continue
        for f in sorted(dir_path.glob("*.csv"), key=lambda x: x.name.lower()):
            if is_real and "_1m_" not in f.name.lower():
                continue
            if only_real_data and (not is_real):
                continue
            if require_higher_tf and is_real and not has_higher_tf_companions(f):
                continue
            all_items.append(f.resolve())
    return sorted(set(all_items), key=lambda x: str(x).lower())


def resolve_explicit_dataset_list(
    dataset_names: List[str],
    dirs: List[pathlib.Path],
    only_real_data: bool,
    require_higher_tf: bool,
) -> List[pathlib.Path]:
    search_dirs = [pathlib.Path.cwd().resolve(), *dirs]
    resolved: List[pathlib.Path] = []
    for raw in dataset_names:
        token = str(raw).strip()
        if not token:
            continue
        cand = pathlib.Path(token)
        found = None
        if cand.is_absolute() and cand.exists():
            found = cand.resolve()
        else:
            for base in search_dirs:
                probe = (base / cand).resolve()
                if probe.exists():
                    found = probe
                    break
        if found is None:
            raise FileNotFoundError(f"Dataset not found: {token}")

        is_real = "backtest_real" in str(found.parent).lower() or "_1m_" in found.name.lower()
        if only_real_data and not is_real:
            continue
        if is_real and "_1m_" not in found.name.lower():
            continue
        if require_higher_tf and is_real and not has_higher_tf_companions(found):
            raise RuntimeError(f"Missing higher TF companions for dataset: {found}")
        resolved.append(found)

    return sorted(set(resolved), key=lambda x: str(x).lower())


def run_dataset_quality_gate(
    parity_script: pathlib.Path,
    datasets: List[pathlib.Path],
    output_json: pathlib.Path,
) -> Dict[str, Any]:
    ensure_parent_directory(output_json)
    cmd = [
        sys.executable,
        str(parity_script),
        "--dataset-names",
        *[str(x) for x in datasets],
        "--output-json",
        str(output_json),
    ]
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"generate_parity_invariant_report.py failed (exit={proc.returncode})")

    report = json.loads(output_json.read_text(encoding="utf-8-sig"))
    rows = report.get("datasets") or []
    row_map: Dict[str, Dict[str, Any]] = {}
    for row in rows:
        path = str(pathlib.Path(str(row.get("dataset", ""))).resolve())
        row_map[path.lower()] = row

    passed: List[pathlib.Path] = []
    failed: List[pathlib.Path] = []
    failed_reasons: List[Dict[str, Any]] = []
    for ds in datasets:
        key = str(ds.resolve()).lower()
        row = row_map.get(key)
        if row is None:
            failed.append(ds)
            failed_reasons.append(
                {
                    "dataset": str(ds),
                    "reason": "missing_from_parity_report",
                }
            )
            continue
        if bool(row.get("invariant_pass", False)):
            passed.append(ds)
            continue
        failed.append(ds)
        checks = row.get("checks") or {}
        failed_reasons.append(
            {
                "dataset": str(ds),
                "reason": "invariant_fail",
                "failed_checks": [k for k, v in checks.items() if not bool(v)],
            }
        )

    return {
        "report_path": str(output_json),
        "summary": report.get("summary") or {},
        "passed_datasets": [str(x) for x in passed],
        "failed_datasets": [str(x) for x in failed],
        "failed_reasons": failed_reasons,
    }


def new_combo_variant(base: Dict[str, Any], combo_id: str, description: str, overrides: Dict[str, Any]) -> Dict[str, Any]:
    clone = deepcopy(base)
    clone["combo_id"] = combo_id
    clone["description"] = description
    clone.update(overrides)
    return clone


def build_combo_specs(scenario_mode: str, include_legacy: bool, max_scenarios: int) -> List[Dict[str, Any]]:
    legacy = [
        {
            "combo_id": "baseline_current",
            "description": "Current baseline in build config.",
            "max_new_orders_per_scan": 2,
            "min_expected_edge_pct": 0.0010,
            "min_reward_risk": 1.20,
            "min_rr_weak_signal": 1.80,
            "min_rr_strong_signal": 1.20,
            "min_strategy_trades_for_ev": 30,
            "min_strategy_expectancy_krw": -2.0,
            "min_strategy_profit_factor": 0.95,
            "avoid_high_volatility": True,
            "avoid_trending_down": True,
            "hostility_ewma_alpha": 0.14,
            "hostility_hostile_threshold": 0.62,
            "hostility_severe_threshold": 0.82,
            "hostility_extreme_threshold": 0.88,
            "hostility_pause_scans": 4,
            "hostility_pause_scans_extreme": 6,
            "hostility_pause_recent_sample_min": 10,
            "hostility_pause_recent_expectancy_krw": 0.0,
            "hostility_pause_recent_win_rate": 0.40,
            "backtest_hostility_pause_candles": 36,
            "backtest_hostility_pause_candles_extreme": 60,
            "scalping_min_signal_strength": 0.70,
            "momentum_min_signal_strength": 0.72,
            "breakout_min_signal_strength": 0.40,
            "mean_reversion_min_signal_strength": 0.40,
        }
    ]
    if scenario_mode == "legacy_only":
        combos = legacy
    else:
        base_balanced = legacy[0]
        generated: List[Dict[str, Any]] = []
        if scenario_mode in ("diverse_light", "diverse_wide"):
            edge_grid = [0.0006, 0.0008, 0.0010, 0.0012, 0.0014, 0.0016] if scenario_mode == "diverse_wide" else [0.0008, 0.0010, 0.0012, 0.0014]
            rr_grid = [1.05, 1.15, 1.25, 1.35] if scenario_mode == "diverse_wide" else [1.10, 1.20, 1.30]
            scalp_grid = [0.62, 0.66, 0.70, 0.74] if scenario_mode == "diverse_wide" else [0.64, 0.68, 0.72]
            mom_grid = [0.60, 0.64, 0.68, 0.72, 0.76] if scenario_mode == "diverse_wide" else [0.62, 0.68, 0.74]
            breakout_grid = [0.35, 0.40, 0.45] if scenario_mode == "diverse_wide" else [0.36, 0.42]
            mrev_grid = [0.35, 0.40, 0.45] if scenario_mode == "diverse_wide" else [0.36, 0.42]
            i = 0
            for edge in edge_grid:
                for rr in rr_grid:
                    weak = round(min(2.20, rr + 0.45), 2)
                    strong = round(max(0.80, rr - 0.10), 2)
                    ev_trades = 35 if rr >= 1.30 else (25 if rr >= 1.20 else 18)
                    ev_expect = 0.0 if edge >= 0.0014 else (-1.0 if edge >= 0.0010 else -3.0)
                    ev_pf = 1.00 if rr >= 1.30 else (0.95 if rr >= 1.20 else 0.90)
                    generated.append(
                        new_combo_variant(
                            base_balanced,
                            f"scenario_{scenario_mode}_{i:03d}",
                            f"Auto-generated {scenario_mode} scenario",
                            {
                                "max_new_orders_per_scan": 2 if rr >= 1.25 else 3,
                                "min_expected_edge_pct": edge,
                                "min_reward_risk": rr,
                                "min_rr_weak_signal": weak,
                                "min_rr_strong_signal": strong,
                                "min_strategy_trades_for_ev": ev_trades,
                                "min_strategy_expectancy_krw": ev_expect,
                                "min_strategy_profit_factor": ev_pf,
                                "avoid_high_volatility": edge >= 0.0010,
                                "avoid_trending_down": rr >= 1.20,
                                "hostility_ewma_alpha": 0.16 if rr >= 1.25 else 0.12,
                                "hostility_hostile_threshold": 0.64 if rr >= 1.30 else 0.60,
                                "hostility_severe_threshold": 0.84 if rr >= 1.30 else 0.80,
                                "hostility_extreme_threshold": 0.90 if rr >= 1.30 else 0.86,
                                "hostility_pause_scans": 5 if rr >= 1.30 else 3,
                                "hostility_pause_scans_extreme": 8 if rr >= 1.30 else 5,
                                "hostility_pause_recent_sample_min": 10,
                                "hostility_pause_recent_expectancy_krw": 0.0,
                                "hostility_pause_recent_win_rate": 0.42 if rr >= 1.30 else 0.38,
                                "backtest_hostility_pause_candles": 45 if rr >= 1.30 else 28,
                                "backtest_hostility_pause_candles_extreme": 72 if rr >= 1.30 else 48,
                                "scalping_min_signal_strength": scalp_grid[i % len(scalp_grid)],
                                "momentum_min_signal_strength": mom_grid[i % len(mom_grid)],
                                "breakout_min_signal_strength": breakout_grid[i % len(breakout_grid)],
                                "mean_reversion_min_signal_strength": mrev_grid[i % len(mrev_grid)],
                            },
                        )
                    )
                    i += 1
        elif scenario_mode == "quality_focus":
            quality_profiles = [
                {
                    "min_expected_edge_pct": 0.0010,
                    "min_reward_risk": 1.30,
                    "min_rr_weak_signal": 1.75,
                    "min_rr_strong_signal": 1.20,
                    "min_strategy_trades_for_ev": 35,
                    "min_strategy_expectancy_krw": -1.0,
                    "min_strategy_profit_factor": 1.00,
                    "scalping_min_signal_strength": 0.72,
                    "momentum_min_signal_strength": 0.74,
                    "breakout_min_signal_strength": 0.42,
                    "mean_reversion_min_signal_strength": 0.42,
                },
                {
                    "min_expected_edge_pct": 0.0012,
                    "min_reward_risk": 1.35,
                    "min_rr_weak_signal": 1.85,
                    "min_rr_strong_signal": 1.25,
                    "min_strategy_trades_for_ev": 40,
                    "min_strategy_expectancy_krw": -0.5,
                    "min_strategy_profit_factor": 1.05,
                    "scalping_min_signal_strength": 0.74,
                    "momentum_min_signal_strength": 0.76,
                    "breakout_min_signal_strength": 0.44,
                    "mean_reversion_min_signal_strength": 0.44,
                },
                {
                    "min_expected_edge_pct": 0.0014,
                    "min_reward_risk": 1.40,
                    "min_rr_weak_signal": 1.95,
                    "min_rr_strong_signal": 1.30,
                    "min_strategy_trades_for_ev": 45,
                    "min_strategy_expectancy_krw": 0.0,
                    "min_strategy_profit_factor": 1.08,
                    "scalping_min_signal_strength": 0.76,
                    "momentum_min_signal_strength": 0.78,
                    "breakout_min_signal_strength": 0.46,
                    "mean_reversion_min_signal_strength": 0.45,
                },
                {
                    "min_expected_edge_pct": 0.0011,
                    "min_reward_risk": 1.32,
                    "min_rr_weak_signal": 1.80,
                    "min_rr_strong_signal": 1.22,
                    "min_strategy_trades_for_ev": 38,
                    "min_strategy_expectancy_krw": -0.7,
                    "min_strategy_profit_factor": 1.03,
                    "scalping_min_signal_strength": 0.73,
                    "momentum_min_signal_strength": 0.75,
                    "breakout_min_signal_strength": 0.43,
                    "mean_reversion_min_signal_strength": 0.43,
                },
            ]
            for i, profile in enumerate(quality_profiles):
                generated.append(
                    new_combo_variant(
                        base_balanced,
                        f"scenario_{scenario_mode}_{i:03d}",
                        "Auto-generated quality-focused scenario",
                        {
                            "max_new_orders_per_scan": 2,
                            "avoid_high_volatility": True,
                            "avoid_trending_down": True,
                            "hostility_ewma_alpha": 0.16,
                            "hostility_hostile_threshold": 0.64,
                            "hostility_severe_threshold": 0.84,
                            "hostility_extreme_threshold": 0.90,
                            "hostility_pause_scans": 5,
                            "hostility_pause_scans_extreme": 8,
                            "hostility_pause_recent_sample_min": 10,
                            "hostility_pause_recent_expectancy_krw": 0.0,
                            "hostility_pause_recent_win_rate": 0.42,
                            "backtest_hostility_pause_candles": 45,
                            "backtest_hostility_pause_candles_extreme": 72,
                            **profile,
                        },
                    )
                )
            target_count = max(int(max_scenarios), 0) if int(max_scenarios) > 0 else 24
            idx = len(generated)
            quality_base = [deepcopy(x) for x in generated]
            for base in quality_base:
                if idx >= target_count:
                    break
                base_profile = {
                    "min_expected_edge_pct": float(base["min_expected_edge_pct"]),
                    "min_reward_risk": float(base["min_reward_risk"]),
                    "min_rr_weak_signal": float(base["min_rr_weak_signal"]),
                    "min_rr_strong_signal": float(base["min_rr_strong_signal"]),
                    "min_strategy_trades_for_ev": int(base["min_strategy_trades_for_ev"]),
                    "min_strategy_expectancy_krw": float(base["min_strategy_expectancy_krw"]),
                    "min_strategy_profit_factor": float(base["min_strategy_profit_factor"]),
                    "scalping_min_signal_strength": float(base["scalping_min_signal_strength"]),
                    "momentum_min_signal_strength": float(base["momentum_min_signal_strength"]),
                    "breakout_min_signal_strength": float(base["breakout_min_signal_strength"]),
                    "mean_reversion_min_signal_strength": float(base["mean_reversion_min_signal_strength"]),
                }
                perturbations: List[Tuple[float, float, float]] = [
                    (-0.0001, -0.05, -0.01),
                    (-0.0001, 0.00, -0.01),
                    (0.0000, 0.05, 0.00),
                    (0.0001, 0.00, 0.01),
                    (0.0001, 0.05, 0.01),
                ]
                for d_edge, d_rr, d_sig in perturbations:
                    if idx >= target_count:
                        break
                    min_rr = round(max(1.05, base_profile["min_reward_risk"] + d_rr), 2)
                    generated.append(
                        new_combo_variant(
                            base_balanced,
                            f"scenario_{scenario_mode}_{idx:03d}",
                            "Auto-generated quality-focused perturbation",
                            {
                                "max_new_orders_per_scan": 2 if min_rr >= 1.25 else 3,
                                "avoid_high_volatility": True,
                                "avoid_trending_down": min_rr >= 1.20,
                                "hostility_ewma_alpha": round(min(0.30, max(0.06, 0.16 + (0.02 if d_rr > 0 else -0.02))), 2),
                                "hostility_hostile_threshold": round(min(0.78, max(0.50, 0.64 + (0.02 if d_rr > 0 else -0.02))), 2),
                                "hostility_severe_threshold": round(min(0.90, max(0.65, 0.84 + (0.02 if d_rr > 0 else -0.02))), 2),
                                "hostility_extreme_threshold": round(min(0.95, max(0.70, 0.90 + (0.02 if d_rr > 0 else -0.02))), 2),
                                "hostility_pause_scans": int(max(2, min(12, 5 + (1 if d_rr > 0 else -1)))),
                                "hostility_pause_scans_extreme": int(max(3, min(16, 8 + (2 if d_rr > 0 else -2)))),
                                "hostility_pause_recent_sample_min": 10,
                                "hostility_pause_recent_expectancy_krw": 0.0,
                                "hostility_pause_recent_win_rate": round(min(0.55, max(0.30, 0.42 + (0.02 if d_rr > 0 else -0.02))), 2),
                                "backtest_hostility_pause_candles": int(max(12, min(180, 45 + (6 if d_rr > 0 else -6)))),
                                "backtest_hostility_pause_candles_extreme": int(max(24, min(240, 72 + (8 if d_rr > 0 else -8)))),
                                "min_expected_edge_pct": round(
                                    min(0.0018, max(0.0006, base_profile["min_expected_edge_pct"] + d_edge)),
                                    4,
                                ),
                                "min_reward_risk": min_rr,
                                "min_rr_weak_signal": round(min(2.20, min_rr + 0.50), 2),
                                "min_rr_strong_signal": round(max(0.90, min_rr - 0.10), 2),
                                "min_strategy_trades_for_ev": int(
                                    max(20, min(55, base_profile["min_strategy_trades_for_ev"] + (2 if d_rr > 0 else -2)))
                                ),
                                "min_strategy_expectancy_krw": round(
                                    min(0.8, max(-2.5, base_profile["min_strategy_expectancy_krw"] + (0.3 if d_rr > 0 else -0.2))),
                                    2,
                                ),
                                "min_strategy_profit_factor": round(
                                    min(1.12, max(0.92, base_profile["min_strategy_profit_factor"] + (0.02 if d_rr > 0 else -0.01))),
                                    2,
                                ),
                                "scalping_min_signal_strength": round(
                                    min(0.80, max(0.62, base_profile["scalping_min_signal_strength"] + d_sig)),
                                    2,
                                ),
                                "momentum_min_signal_strength": round(
                                    min(0.82, max(0.60, base_profile["momentum_min_signal_strength"] + d_sig)),
                                    2,
                                ),
                                "breakout_min_signal_strength": round(
                                    min(0.50, max(0.34, base_profile["breakout_min_signal_strength"] + (d_sig * 0.6))),
                                    2,
                                ),
                                "mean_reversion_min_signal_strength": round(
                                    min(0.50, max(0.34, base_profile["mean_reversion_min_signal_strength"] + (d_sig * 0.6))),
                                    2,
                                ),
                            },
                        )
                    )
                    idx += 1
        combos = (legacy + generated) if include_legacy else generated
    combos = dedupe_combos(combos)
    if max_scenarios > 0 and len(combos) > max_scenarios:
        combos = combos[:max_scenarios]
    if not combos:
        raise RuntimeError("No tuning combos selected. Check --scenario-mode/--max-scenarios.")
    return combos


def select_evenly_spaced_datasets(datasets: List[pathlib.Path], limit: int) -> List[pathlib.Path]:
    if limit <= 0 or len(datasets) <= limit:
        return datasets
    if limit == 1:
        return [datasets[len(datasets) // 2]]
    step = (len(datasets) - 1) / float(limit - 1)
    indices = sorted({int(round(i * step)) for i in range(limit)})
    if len(indices) < limit:
        existing = set(indices)
        for i in range(len(datasets)):
            if i in existing:
                continue
            indices.append(i)
            if len(indices) >= limit:
                break
        indices = sorted(indices[:limit])
    return [datasets[i] for i in indices]


def combo_fingerprint(combo: Dict[str, Any]) -> str:
    material = {
        "max_new_orders_per_scan": combo.get("max_new_orders_per_scan"),
        "min_expected_edge_pct": combo.get("min_expected_edge_pct"),
        "min_reward_risk": combo.get("min_reward_risk"),
        "min_rr_weak_signal": combo.get("min_rr_weak_signal"),
        "min_rr_strong_signal": combo.get("min_rr_strong_signal"),
        "min_strategy_trades_for_ev": combo.get("min_strategy_trades_for_ev"),
        "min_strategy_expectancy_krw": combo.get("min_strategy_expectancy_krw"),
        "min_strategy_profit_factor": combo.get("min_strategy_profit_factor"),
        "avoid_high_volatility": combo.get("avoid_high_volatility"),
        "avoid_trending_down": combo.get("avoid_trending_down"),
        "hostility_ewma_alpha": combo.get("hostility_ewma_alpha"),
        "hostility_hostile_threshold": combo.get("hostility_hostile_threshold"),
        "hostility_severe_threshold": combo.get("hostility_severe_threshold"),
        "hostility_extreme_threshold": combo.get("hostility_extreme_threshold"),
        "hostility_pause_scans": combo.get("hostility_pause_scans"),
        "hostility_pause_scans_extreme": combo.get("hostility_pause_scans_extreme"),
        "hostility_pause_recent_sample_min": combo.get("hostility_pause_recent_sample_min"),
        "hostility_pause_recent_expectancy_krw": combo.get("hostility_pause_recent_expectancy_krw"),
        "hostility_pause_recent_win_rate": combo.get("hostility_pause_recent_win_rate"),
        "backtest_hostility_pause_candles": combo.get("backtest_hostility_pause_candles"),
        "backtest_hostility_pause_candles_extreme": combo.get("backtest_hostility_pause_candles_extreme"),
        "scalping_min_signal_strength": combo.get("scalping_min_signal_strength"),
        "momentum_min_signal_strength": combo.get("momentum_min_signal_strength"),
        "breakout_min_signal_strength": combo.get("breakout_min_signal_strength"),
        "mean_reversion_min_signal_strength": combo.get("mean_reversion_min_signal_strength"),
    }
    encoded = json.dumps(material, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def dedupe_combos(combos: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    seen = set()
    unique: List[Dict[str, Any]] = []
    for combo in combos:
        fp = combo_fingerprint(combo)
        if fp in seen:
            continue
        seen.add(fp)
        unique.append(combo)
    return unique


def dataset_signature(datasets: List[pathlib.Path]) -> List[Dict[str, Any]]:
    signature: List[Dict[str, Any]] = []
    for ds in datasets:
        st = ds.stat()
        signature.append(
            {
                "path": str(ds.resolve()),
                "size": int(st.st_size),
                "mtime_ns": int(st.st_mtime_ns),
            }
        )
    return signature


def stable_json_hash(payload: Any) -> str:
    encoded = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def stable_base_config_hash(config_raw: str) -> str:
    try:
        cfg = json.loads(config_raw)
    except Exception:
        return hashlib.sha256(config_raw.encode("utf-8")).hexdigest()
    trading = cfg.get("trading")
    if isinstance(trading, dict):
        for key in TUNABLE_TRADING_KEYS:
            trading.pop(key, None)
    strategies = cfg.get("strategies")
    if isinstance(strategies, dict):
        for strategy_name, keys in TUNABLE_STRATEGY_KEYS.items():
            node = strategies.get(strategy_name)
            if not isinstance(node, dict):
                continue
            for key in keys:
                node.pop(key, None)
    return stable_json_hash(cfg)


def load_eval_cache(cache_path: pathlib.Path) -> Dict[str, Any]:
    if not cache_path.exists():
        return {"schema_version": 1, "entries": {}}
    try:
        payload = json.loads(cache_path.read_text(encoding="utf-8-sig"))
        if isinstance(payload, dict) and isinstance(payload.get("entries"), dict):
            return payload
    except Exception:
        pass
    return {"schema_version": 1, "entries": {}}


def save_eval_cache(cache_path: pathlib.Path, cache_payload: Dict[str, Any]) -> None:
    ensure_parent_directory(cache_path)
    cache_path.write_text(json.dumps(cache_payload, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")


def compute_combo_objective(
    avg_profit_factor: float,
    avg_expectancy_krw: float,
    profitable_ratio: float,
    avg_total_trades: float,
    avg_win_rate_pct: float,
    min_avg_trades: float,
    min_profitable_ratio: float,
    min_avg_win_rate_pct: float,
    min_expectancy_krw: float,
    objective_mode: str,
) -> float:
    penalty = 0.0
    if objective_mode == "profitable_ratio_priority":
        if avg_total_trades < min_avg_trades:
            penalty += 2200.0 + ((min_avg_trades - avg_total_trades) * 420.0)
        if profitable_ratio < min_profitable_ratio:
            penalty += 12000.0 + ((min_profitable_ratio - profitable_ratio) * 22000.0)
    else:
        if avg_total_trades < min_avg_trades:
            penalty += 6000.0 + ((min_avg_trades - avg_total_trades) * 800.0)
        if profitable_ratio < min_profitable_ratio:
            penalty += 6000.0 + ((min_profitable_ratio - profitable_ratio) * 9000.0)
    if avg_win_rate_pct < min_avg_win_rate_pct:
        penalty += 4000.0 + ((min_avg_win_rate_pct - avg_win_rate_pct) * 180.0)
    if avg_expectancy_krw < min_expectancy_krw:
        penalty += 6000.0 + ((min_expectancy_krw - avg_expectancy_krw) * 120.0)
    if avg_profit_factor < 1.0:
        penalty += (1.0 - avg_profit_factor) * 2500.0

    if penalty > 0.0:
        # Keep all infeasible combos below feasible ones while preserving ordering.
        return round(-penalty + (avg_profit_factor * 10.0), 6)

    if objective_mode == "profitable_ratio_priority":
        score = 0.0
        score += (profitable_ratio * 9000.0)
        score += (avg_expectancy_krw * 32.0)
        score += (avg_win_rate_pct * 42.0)
        score += ((avg_profit_factor - 1.0) * 220.0)
        score += (min(avg_total_trades, 20.0) * 12.0)
    else:
        score = 0.0
        score += (avg_expectancy_krw * 25.0)
        score += (profitable_ratio * 4000.0)
        score += (avg_win_rate_pct * 40.0)
        score += ((avg_profit_factor - 1.0) * 300.0)
        score += (min(avg_total_trades, 30.0) * 40.0)
    return round(score, 6)


def get_effective_objective_thresholds(row: Dict[str, Any], args) -> Dict[str, float]:
    base = {
        "min_avg_trades": float(args.objective_min_avg_trades),
        "min_profitable_ratio": float(args.objective_min_profitable_ratio),
        "min_avg_win_rate_pct": float(args.objective_min_avg_win_rate_pct),
        "min_expectancy_krw": float(args.objective_min_expectancy_krw),
    }
    if not bool(args.use_effective_thresholds_for_objective):
        return base
    effective = {
        "min_avg_trades": float(row.get("effective_min_avg_trades", base["min_avg_trades"])),
        "min_profitable_ratio": float(row.get("effective_min_profitable_ratio", base["min_profitable_ratio"])),
        "min_avg_win_rate_pct": float(row.get("effective_min_avg_win_rate_pct", base["min_avg_win_rate_pct"])),
        "min_expectancy_krw": float(row.get("effective_min_expectancy_krw", base["min_expectancy_krw"])),
    }
    # Keep user-requested objective floors as a hard lower bound even when
    # hostility-adaptive effective thresholds are enabled.
    return {
        # Trade-count floor may be relaxed in hostile regimes.
        "min_avg_trades": min(base["min_avg_trades"], effective["min_avg_trades"]),
        "min_profitable_ratio": max(base["min_profitable_ratio"], effective["min_profitable_ratio"]),
        "min_avg_win_rate_pct": max(base["min_avg_win_rate_pct"], effective["min_avg_win_rate_pct"]),
        "min_expectancy_krw": max(base["min_expectancy_krw"], effective["min_expectancy_krw"]),
    }


def evaluate_combo(
    matrix_script: pathlib.Path,
    build_config: pathlib.Path,
    original_build_raw: str,
    combo: Dict[str, Any],
    datasets: List[pathlib.Path],
    output_dir: pathlib.Path,
    stage_name: str,
    profile_ids: List[str],
    gate_min_avg_trades: int,
    require_higher_tf_companions: bool,
    enable_hostility_adaptive_thresholds: bool,
    enable_hostility_adaptive_trades_only: bool,
    matrix_max_workers: int,
    matrix_backtest_retry_count: int,
    skip_core_vs_legacy_gate: bool,
    eval_cache: Dict[str, Any],
    cache_enabled: bool,
    base_config_hash: str,
    datasets_sig_hash: str,
) -> Dict[str, Any]:
    cache_schema_version = 4
    cache_material = {
        "cache_schema_version": cache_schema_version,
        "base_config_hash": base_config_hash,
        "combo_fingerprint": combo_fingerprint(combo),
        "stage_name": stage_name,
        "profile_ids": list(profile_ids),
        "gate_min_avg_trades": int(gate_min_avg_trades),
        "require_higher_tf_companions": bool(require_higher_tf_companions),
        "enable_hostility_adaptive_thresholds": bool(enable_hostility_adaptive_thresholds),
        "enable_hostility_adaptive_trades_only": bool(enable_hostility_adaptive_trades_only),
        "skip_core_vs_legacy_gate": bool(skip_core_vs_legacy_gate),
        "matrix_max_workers": int(matrix_max_workers),
        "matrix_backtest_retry_count": int(matrix_backtest_retry_count),
        "datasets_sig_hash": datasets_sig_hash,
    }
    cache_key = stable_json_hash(cache_material)
    entries = eval_cache.setdefault("entries", {})
    if cache_enabled and cache_key in entries:
        cached = deepcopy(entries[cache_key])
        report_path = pathlib.Path(str(cached.get("report_json", "")))
        profile_path = pathlib.Path(str(cached.get("profile_csv", "")))
        matrix_path = pathlib.Path(str(cached.get("matrix_csv", "")))
        if report_path.exists() and profile_path.exists() and matrix_path.exists():
            cached["from_cache"] = True
            return cached
        entries.pop(cache_key, None)

    cfg = json.loads(original_build_raw)
    apply_candidate_combo_to_config(cfg, combo)
    build_config.write_text(json.dumps(cfg, ensure_ascii=False, indent=4), encoding="utf-8", newline="\n")

    suffix = f"{combo['combo_id']}_{stage_name}"
    matrix_csv_rel = output_dir / f"profitability_matrix_{suffix}.csv"
    profile_csv_rel = output_dir / f"profitability_profile_summary_{suffix}.csv"
    report_json_rel = output_dir / f"profitability_gate_report_{suffix}.json"

    cmd = [
        sys.executable,
        str(matrix_script),
        "--dataset-names",
        *[str(x) for x in datasets],
        "--profile-ids",
        *profile_ids,
        "--exclude-low-trade-runs-for-gate",
        "--min-trades-per-run-for-gate",
        "1",
        "--min-avg-trades",
        str(int(gate_min_avg_trades)),
        "--output-csv",
        str(matrix_csv_rel),
        "--output-profile-csv",
        str(profile_csv_rel),
        "--output-json",
        str(report_json_rel),
    ]
    if require_higher_tf_companions:
        cmd.append("--require-higher-tf-companions")
    if enable_hostility_adaptive_thresholds:
        cmd.append("--enable-hostility-adaptive-thresholds")
    if enable_hostility_adaptive_trades_only:
        cmd.append("--enable-hostility-adaptive-trades-only")
    if skip_core_vs_legacy_gate:
        cmd.append("--skip-core-vs-legacy-gate")
    cmd.extend(["--max-workers", str(max(1, int(matrix_max_workers)))])
    cmd.extend(["--backtest-retry-count", str(max(1, int(matrix_backtest_retry_count)))])
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"run_profitability_matrix.py failed for combo={combo['combo_id']} stage={stage_name}")

    report = json.loads(report_json_rel.read_text(encoding="utf-8-sig"))
    target_profile = "core_full" if "core_full" in profile_ids else profile_ids[0]
    summary = next((x for x in report.get("profile_summaries", []) if x.get("profile_id") == target_profile), None)
    if summary is None:
        raise RuntimeError(f"{target_profile} profile summary not found for combo={combo['combo_id']} stage={stage_name}")
    report_thresholds = report.get("thresholds") or {}
    threshold_bundle = report_thresholds.get("hostility_adaptive") or {}
    effective_thresholds = threshold_bundle.get("effective") or {}
    requested_thresholds = threshold_bundle.get("requested") or report_thresholds
    hostility = threshold_bundle.get("hostility") or {}
    quality = threshold_bundle.get("quality") or {}
    blended = threshold_bundle.get("blended_context") or {}

    row = {
        "combo_id": combo["combo_id"],
        "description": combo["description"],
        "stage": stage_name,
        "target_profile": target_profile,
        "overall_gate_pass": bool(report.get("overall_gate_pass", False)),
        "profile_gate_pass": bool(report.get("profile_gate_pass", False)),
        "runs_used_for_gate": int(summary.get("runs_used_for_gate", 0)),
        "excluded_low_trade_runs": int(summary.get("excluded_low_trade_runs", 0)),
        "avg_profit_factor": float(summary.get("avg_profit_factor", 0.0)),
        "avg_expectancy_krw": float(summary.get("avg_expectancy_krw", 0.0)),
        "avg_total_trades": float(summary.get("avg_total_trades", 0.0)),
        "avg_win_rate_pct": float(summary.get("avg_win_rate_pct", 0.0)),
        "profitable_ratio": float(summary.get("profitable_ratio", 0.0)),
        "gate_profit_factor_pass": bool(summary.get("gate_profit_factor_pass", False)),
        "gate_trades_pass": bool(summary.get("gate_trades_pass", False)),
        "gate_profitable_ratio_pass": bool(summary.get("gate_profitable_ratio_pass", False)),
        "gate_expectancy_pass": bool(summary.get("gate_expectancy_pass", False)),
        "effective_min_profit_factor": float(
            effective_thresholds.get("min_profit_factor", requested_thresholds.get("min_profit_factor", 1.0))
        ),
        "effective_min_expectancy_krw": float(
            effective_thresholds.get("min_expectancy_krw", requested_thresholds.get("min_expectancy_krw", 0.0))
        ),
        "effective_min_profitable_ratio": float(
            effective_thresholds.get("min_profitable_ratio", requested_thresholds.get("min_profitable_ratio", 0.5))
        ),
        "effective_min_avg_win_rate_pct": float(
            effective_thresholds.get("min_avg_win_rate_pct", requested_thresholds.get("min_avg_win_rate_pct", 48.0))
        ),
        "effective_min_avg_trades": float(
            effective_thresholds.get("min_avg_trades", requested_thresholds.get("min_avg_trades", gate_min_avg_trades))
        ),
        "hostility_level": str(hostility.get("hostility_level", "unknown")),
        "hostility_avg_score": float(hostility.get("avg_adversarial_score", 0.0)),
        "quality_level": str(quality.get("quality_level", "unknown")),
        "quality_avg_score": float(quality.get("avg_quality_risk_score", 0.0)),
        "hostility_blended_level": str(
            blended.get("blended_hostility_level", hostility.get("hostility_level", "unknown"))
        ),
        "hostility_blended_score": float(
            blended.get("blended_adversarial_score", hostility.get("avg_adversarial_score", 0.0))
        ),
        "report_json": str(report_json_rel.resolve()),
        "profile_csv": str(profile_csv_rel.resolve()),
        "matrix_csv": str(matrix_csv_rel.resolve()),
        "from_cache": False,
    }
    if cache_enabled:
        entries[cache_key] = deepcopy(row)
    return row


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--matrix-script", "-MatrixScript", default=r".\scripts\run_profitability_matrix.py")
    parser.add_argument(
        "--parity-invariant-script",
        "-ParityInvariantScript",
        default=r".\scripts\generate_parity_invariant_report.py",
    )
    parser.add_argument("--build-config-path", "-BuildConfigPath", default=r".\build\Release\config\config.json")
    parser.add_argument("--data-dir", "-DataDir", default=r".\data\backtest")
    parser.add_argument("--curated-data-dir", "-CuratedDataDir", default=r".\data\backtest_curated")
    parser.add_argument("--extra-data-dirs", "-ExtraDataDirs", nargs="*", default=[r".\data\backtest_real"])
    parser.add_argument(
        "--dataset-names",
        "-DatasetNames",
        nargs="*",
        default=[],
        help="Explicit primary datasets (.csv). When provided, auto-discovery is bypassed.",
    )
    parser.add_argument("--output-dir", "-OutputDir", default=r".\build\Release\logs")
    parser.add_argument("--summary-csv", "-SummaryCsv", default=r".\build\Release\logs\candidate_trade_density_tuning_summary.csv")
    parser.add_argument("--summary-json", "-SummaryJson", default=r".\build\Release\logs\candidate_trade_density_tuning_summary.json")
    parser.add_argument(
        "--scenario-mode",
        "-ScenarioMode",
        choices=["legacy_only", "diverse_light", "diverse_wide", "quality_focus"],
        default="quality_focus",
    )
    parser.add_argument("--max-scenarios", "-MaxScenarios", type=int, default=0)
    parser.add_argument("--include-legacy-scenarios", "-IncludeLegacyScenarios", action="store_true")
    parser.add_argument("--real-data-only", "-RealDataOnly", action="store_true")
    parser.add_argument(
        "--require-higher-tf-companions",
        "-RequireHigherTfCompanions",
        dest="require_higher_tf_companions",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--allow-missing-higher-tf-companions",
        dest="require_higher_tf_companions",
        action="store_false",
    )
    parser.add_argument(
        "--enable-dataset-quality-gate",
        "-EnableDatasetQualityGate",
        dest="enable_dataset_quality_gate",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-dataset-quality-gate",
        dest="enable_dataset_quality_gate",
        action="store_false",
    )
    parser.add_argument(
        "--dataset-quality-report-json",
        "-DatasetQualityReportJson",
        default=r".\build\Release\logs\dataset_quality_gate_summary.json",
    )
    parser.add_argument(
        "--dataset-quality-gate-fail-closed",
        "-DatasetQualityGateFailClosed",
        action="store_true",
        help="If quality gate excludes all datasets, fail instead of fail-open fallback.",
    )
    parser.add_argument("--screen-dataset-limit", "-ScreenDatasetLimit", type=int, default=8)
    parser.add_argument("--screen-top-k", "-ScreenTopK", type=int, default=6)
    parser.add_argument("--screen-profile-ids", "-ScreenProfileIds", nargs="*", default=["core_full"])
    parser.add_argument("--final-profile-ids", "-FinalProfileIds", nargs="*", default=["core_full"])
    parser.add_argument("--gate-min-avg-trades", "-GateMinAvgTrades", type=int, default=8)
    parser.add_argument("--objective-min-avg-trades", "-ObjectiveMinAvgTrades", type=float, default=8.0)
    parser.add_argument("--objective-min-profitable-ratio", "-ObjectiveMinProfitableRatio", type=float, default=0.50)
    parser.add_argument("--objective-min-avg-win-rate-pct", "-ObjectiveMinAvgWinRatePct", type=float, default=48.0)
    parser.add_argument("--objective-min-expectancy-krw", "-ObjectiveMinExpectancyKrw", type=float, default=0.0)
    parser.add_argument(
        "--objective-mode",
        "-ObjectiveMode",
        choices=["balanced", "profitable_ratio_priority"],
        default="balanced",
    )
    parser.add_argument(
        "--enable-hostility-adaptive-thresholds",
        "-EnableHostilityAdaptiveThresholds",
        dest="enable_hostility_adaptive_thresholds",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-hostility-adaptive-thresholds",
        dest="enable_hostility_adaptive_thresholds",
        action="store_false",
    )
    parser.add_argument(
        "--enable-hostility-adaptive-trades-only",
        "-EnableHostilityAdaptiveTradesOnly",
        dest="enable_hostility_adaptive_trades_only",
        action="store_true",
        default=False,
    )
    parser.add_argument(
        "--disable-hostility-adaptive-trades-only",
        dest="enable_hostility_adaptive_trades_only",
        action="store_false",
    )
    parser.add_argument(
        "--use-effective-thresholds-for-objective",
        "-UseEffectiveThresholdsForObjective",
        dest="use_effective_thresholds_for_objective",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-effective-thresholds-for-objective",
        dest="use_effective_thresholds_for_objective",
        action="store_false",
    )
    parser.add_argument("--eval-cache-json", "-EvalCacheJson", default=r".\build\Release\logs\candidate_trade_density_tuning_cache.json")
    parser.add_argument("--disable-eval-cache", "-DisableEvalCache", action="store_true")
    parser.add_argument(
        "--live-signal-funnel-taxonomy-json",
        "-LiveSignalFunnelTaxonomyJson",
        default=r".\build\Release\logs\live_signal_funnel_taxonomy_report.json",
    )
    parser.add_argument(
        "--train-eval-summary-json",
        "-TrainEvalSummaryJson",
        default=r".\build\Release\logs\candidate_train_eval_cycle_summary.json",
    )
    parser.add_argument(
        "--enable-bottleneck-priority",
        "-EnableBottleneckPriority",
        dest="enable_bottleneck_priority",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-bottleneck-priority",
        dest="enable_bottleneck_priority",
        action="store_false",
    )
    parser.add_argument(
        "--enable-bottleneck-adapted-scenarios",
        "-EnableBottleneckAdaptedScenarios",
        dest="enable_bottleneck_adapted_scenarios",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-bottleneck-adapted-scenarios",
        dest="enable_bottleneck_adapted_scenarios",
        action="store_false",
    )
    parser.add_argument(
        "--enable-hint-impact-guardrail",
        "-EnableHintImpactGuardrail",
        dest="enable_hint_impact_guardrail",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-hint-impact-guardrail",
        dest="enable_hint_impact_guardrail",
        action="store_false",
    )
    parser.add_argument(
        "--enable-holdout-failure-family-suppression",
        "-EnableHoldoutFailureFamilySuppression",
        dest="enable_holdout_failure_family_suppression",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-holdout-failure-family-suppression",
        dest="enable_holdout_failure_family_suppression",
        action="store_false",
    )
    parser.add_argument(
        "--holdout-suppression-hint-ratio-threshold",
        "-HoldoutSuppressionHintRatioThreshold",
        type=float,
        default=0.60,
    )
    parser.add_argument(
        "--holdout-suppression-require-both-pf-exp-fail",
        "-HoldoutSuppressionRequireBothPfExpFail",
        dest="holdout_suppression_require_both_pf_exp_fail",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--holdout-suppression-allow-either-pf-or-exp-fail",
        dest="holdout_suppression_require_both_pf_exp_fail",
        action="store_false",
    )
    parser.add_argument(
        "--enable-post-suppression-quality-expansion",
        "-EnablePostSuppressionQualityExpansion",
        dest="enable_post_suppression_quality_expansion",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-post-suppression-quality-expansion",
        dest="enable_post_suppression_quality_expansion",
        action="store_false",
    )
    parser.add_argument(
        "--post-suppression-min-combo-count",
        "-PostSuppressionMinComboCount",
        type=int,
        default=3,
    )
    parser.add_argument(
        "--hint-impact-guardrail-ratio",
        "-HintImpactGuardrailRatio",
        type=float,
        default=0.65,
    )
    parser.add_argument(
        "--hint-impact-guardrail-tighten-scale",
        "-HintImpactGuardrailTightenScale",
        type=float,
        default=0.55,
    )
    parser.add_argument("--matrix-max-workers", "-MatrixMaxWorkers", type=int, default=1)
    parser.add_argument("--matrix-backtest-retry-count", "-MatrixBacktestRetryCount", type=int, default=2)
    parser.add_argument(
        "--skip-core-vs-legacy-gate",
        "-SkipCoreVsLegacyGate",
        action="store_true",
        help="Skip legacy comparison gate while tuning (migration mode).",
    )
    parser.add_argument("--verification-lock-path", "-VerificationLockPath", default=r".\build\Release\logs\verification_run.lock")
    parser.add_argument("--verification-lock-timeout-sec", "-VerificationLockTimeoutSec", type=int, default=1800)
    parser.add_argument("--verification-lock-stale-sec", "-VerificationLockStaleSec", type=int, default=14400)
    args = parser.parse_args(argv)

    matrix_script = resolve_or_throw(args.matrix_script, "Matrix script")
    parity_invariant_script = resolve_or_throw(args.parity_invariant_script, "Parity invariant script")
    build_config = resolve_or_throw(args.build_config_path, "Build config")
    output_dir = pathlib.Path(args.output_dir).resolve()
    summary_csv = pathlib.Path(args.summary_csv).resolve()
    summary_json = pathlib.Path(args.summary_json).resolve()
    dataset_quality_report_json = pathlib.Path(args.dataset_quality_report_json).resolve()
    eval_cache_json = pathlib.Path(args.eval_cache_json).resolve()
    live_signal_funnel_taxonomy_json = pathlib.Path(args.live_signal_funnel_taxonomy_json).resolve()
    train_eval_summary_json = pathlib.Path(args.train_eval_summary_json).resolve()
    lock_path = pathlib.Path(args.verification_lock_path).resolve()
    cache_enabled = not bool(args.disable_eval_cache)
    ensure_parent_directory(summary_csv)
    ensure_parent_directory(summary_json)
    ensure_parent_directory(dataset_quality_report_json)
    ensure_parent_directory(eval_cache_json)
    output_dir.mkdir(parents=True, exist_ok=True)

    scan_dirs = [pathlib.Path(args.data_dir), pathlib.Path(args.curated_data_dir)]
    scan_dirs.extend(pathlib.Path(x) for x in args.extra_data_dirs if x and x.strip())
    scan_dirs = [p.resolve() for p in scan_dirs]
    if args.dataset_names:
        datasets = resolve_explicit_dataset_list(
            dataset_names=list(args.dataset_names),
            dirs=scan_dirs,
            only_real_data=bool(args.real_data_only),
            require_higher_tf=bool(args.require_higher_tf_companions),
        )
    else:
        datasets = get_dataset_list(scan_dirs, args.real_data_only, args.require_higher_tf_companions)
    if not datasets:
        raise RuntimeError("No datasets found under DataDir/CuratedDataDir/ExtraDataDirs with current filters.")

    dataset_quality_gate: Dict[str, Any] = {
        "enabled": bool(args.enable_dataset_quality_gate),
        "fail_closed": bool(args.dataset_quality_gate_fail_closed),
        "report_json": str(dataset_quality_report_json),
        "input_dataset_count": len(datasets),
        "input_datasets": [str(x) for x in datasets],
    }
    if bool(args.enable_dataset_quality_gate):
        quality = run_dataset_quality_gate(
            parity_script=parity_invariant_script,
            datasets=datasets,
            output_json=dataset_quality_report_json,
        )
        passed = [pathlib.Path(x).resolve() for x in (quality.get("passed_datasets") or [])]
        failed = [pathlib.Path(x).resolve() for x in (quality.get("failed_datasets") or [])]
        dataset_quality_gate.update(
            {
                "summary": quality.get("summary") or {},
                "passed_dataset_count": len(passed),
                "failed_dataset_count": len(failed),
                "failed_reasons": quality.get("failed_reasons") or [],
            }
        )
        if passed:
            datasets = sorted(set(passed), key=lambda x: str(x).lower())
            dataset_quality_gate["status"] = "pass"
            dataset_quality_gate["effective_dataset_count"] = len(datasets)
        else:
            if bool(args.dataset_quality_gate_fail_closed):
                raise RuntimeError("Dataset quality gate excluded all datasets (fail-closed).")
            dataset_quality_gate["status"] = "fail_open_no_passed_dataset"
            dataset_quality_gate["effective_dataset_count"] = len(datasets)
    else:
        dataset_quality_gate["status"] = "disabled"
        dataset_quality_gate["effective_dataset_count"] = len(datasets)

    print(
        f"[TuneCandidate] dataset_mode={'realdata_only' if args.real_data_only else 'mixed'}, "
        f"require_higher_tf={bool(args.require_higher_tf_companions)}, dataset_count={len(datasets)}"
    )
    if bool(args.enable_dataset_quality_gate):
        print(
            f"[TuneCandidate] dataset_quality_gate="
            f"status={dataset_quality_gate.get('status')}, "
            f"passed={dataset_quality_gate.get('passed_dataset_count')}, "
            f"failed={dataset_quality_gate.get('failed_dataset_count')}"
        )

    combo_specs = build_combo_specs(args.scenario_mode, args.include_legacy_scenarios, args.max_scenarios)
    live_bottleneck_context = read_live_signal_funnel_snapshot(live_signal_funnel_taxonomy_json)
    holdout_context = read_train_eval_holdout_context(train_eval_summary_json)
    bottleneck_context = build_effective_bottleneck_context(live_bottleneck_context, holdout_context)
    scenario_family_counts: Dict[str, int] = {}
    if bool(args.enable_bottleneck_adapted_scenarios):
        combo_specs, scenario_family_counts = adapt_combo_specs_for_bottleneck(
            combos=combo_specs,
            context=bottleneck_context,
            scenario_mode=str(args.scenario_mode),
            enable_hint_impact_guardrail=bool(args.enable_hint_impact_guardrail),
            hint_impact_guardrail_ratio=float(args.hint_impact_guardrail_ratio),
            hint_impact_guardrail_tighten_scale=float(args.hint_impact_guardrail_tighten_scale),
        )
        combo_specs = dedupe_combos(combo_specs)
    else:
        for combo in combo_specs:
            combo["bottleneck_scenario_family"] = "base_unadapted"
            combo["bottleneck_scenario_family_source_group"] = str(bottleneck_context.get("top_group", ""))
            combo["bottleneck_scenario_family_adapted"] = False
            combo["bottleneck_hint_guardrail_active"] = False
            combo["bottleneck_hint_guardrail_ratio"] = float(
                bottleneck_context.get("selection_hint_adjusted_ratio", 0.0) or 0.0
            )
            combo["bottleneck_hint_guardrail_threshold"] = float(args.hint_impact_guardrail_ratio)
            combo["bottleneck_hint_guardrail_tighten_scale"] = float(args.hint_impact_guardrail_tighten_scale)
            fam = str(combo["bottleneck_scenario_family"])
            scenario_family_counts[fam] = scenario_family_counts.get(fam, 0) + 1

    combo_specs, holdout_family_suppression_meta = apply_holdout_failure_family_suppression(
        combos=combo_specs,
        live_context=bottleneck_context,
        holdout_context=holdout_context,
        enabled=bool(args.enable_holdout_failure_family_suppression),
        hint_ratio_threshold=float(args.holdout_suppression_hint_ratio_threshold),
        require_both_pf_exp_fail=bool(args.holdout_suppression_require_both_pf_exp_fail),
    )
    combo_specs, post_suppression_quality_expansion_meta = expand_post_suppression_quality_exit_candidates(
        combos=combo_specs,
        suppression_meta=holdout_family_suppression_meta,
        context=bottleneck_context,
        enabled=bool(args.enable_post_suppression_quality_expansion),
        min_combo_count=int(args.post_suppression_min_combo_count),
    )
    combo_specs = dedupe_combos(combo_specs)
    if not combo_specs:
        raise RuntimeError("No tuning combos left after holdout family suppression.")

    bottleneck_priority_meta: Dict[str, Dict[str, Any]] = {}
    if bool(args.enable_bottleneck_priority):
        combo_specs, bottleneck_priority_meta = prioritize_combo_specs_for_bottleneck(combo_specs, bottleneck_context)
    else:
        for rank, combo in enumerate(combo_specs, start=1):
            combo_id = str(combo.get("combo_id", ""))
            bottleneck_priority_meta[combo_id] = {
                "priority_rank": int(rank),
                "priority_score": 0.0,
                "top_group": str(bottleneck_context.get("top_group", "")),
                "no_trade_bias_active": bool(bottleneck_context.get("no_trade_bias_active", False)),
            }
    scenario_family_counts = {}
    for combo in combo_specs:
        fam = str(combo.get("bottleneck_scenario_family", "unknown"))
        scenario_family_counts[fam] = scenario_family_counts.get(fam, 0) + 1

    if args.scenario_mode == "legacy_only":
        print("[TuneCandidate] scenario_mode=legacy_only (rollback/comparison mode)")
    print(f"[TuneCandidate] scenario_mode={args.scenario_mode}, combo_count={len(combo_specs)}")
    print(
        f"[TuneCandidate] bottleneck_priority={'on' if bool(args.enable_bottleneck_priority) else 'off'}, "
        f"top_group={bottleneck_context.get('top_group', '')}, "
        f"source={bottleneck_context.get('top_group_source', 'live')}, "
        f"risk_gate_focus={bottleneck_context.get('risk_gate_focus', '')}, "
        f"no_trade_bias_active={bool(bottleneck_context.get('no_trade_bias_active', False))}"
    )
    print(
        f"[TuneCandidate] bottleneck_adapted_scenarios="
        f"{'on' if bool(args.enable_bottleneck_adapted_scenarios) else 'off'}, "
        f"scenario_family_counts={scenario_family_counts}"
    )
    print(
        f"[TuneCandidate] holdout_family_suppression="
        f"{'on' if bool(args.enable_holdout_failure_family_suppression) else 'off'}, "
        f"active={bool(holdout_family_suppression_meta.get('active', False))}, "
        f"reason={holdout_family_suppression_meta.get('reason', '')}, "
        f"kept={holdout_family_suppression_meta.get('kept_combo_count', len(combo_specs))}, "
        f"suppressed={holdout_family_suppression_meta.get('suppressed_combo_count', 0)}"
    )
    print(
        f"[TuneCandidate] post_suppression_quality_expansion="
        f"{'on' if bool(args.enable_post_suppression_quality_expansion) else 'off'}, "
        f"applied={bool(post_suppression_quality_expansion_meta.get('applied', False))}, "
        f"reason={post_suppression_quality_expansion_meta.get('reason', '')}, "
        f"injected={post_suppression_quality_expansion_meta.get('injected_combo_count', 0)}, "
        f"combo_count={len(combo_specs)}"
    )
    print(
        f"[TuneCandidate] hint_impact_guardrail={'on' if bool(args.enable_hint_impact_guardrail) else 'off'}, "
        f"ratio={float(bottleneck_context.get('selection_hint_adjusted_ratio', 0.0) or 0.0):.4f}, "
        f"threshold={float(args.hint_impact_guardrail_ratio):.4f}, "
        f"tighten_scale={float(args.hint_impact_guardrail_tighten_scale):.2f}"
    )

    original_build_raw = build_config.read_text(encoding="utf-8-sig")
    base_config_hash = stable_base_config_hash(original_build_raw)
    eval_cache = load_eval_cache(eval_cache_json) if cache_enabled else {"schema_version": 1, "entries": {}}
    screen_profile_ids = [str(x).strip() for x in args.screen_profile_ids if str(x).strip()]
    final_profile_ids = [str(x).strip() for x in args.final_profile_ids if str(x).strip()]
    if not screen_profile_ids:
        screen_profile_ids = ["core_full"]
    if not final_profile_ids:
        final_profile_ids = ["core_full"]

    screen_datasets = select_evenly_spaced_datasets(datasets, int(args.screen_dataset_limit))
    screen_dataset_sig_hash = stable_json_hash(dataset_signature(screen_datasets))
    final_dataset_sig_hash = stable_json_hash(dataset_signature(datasets))
    do_screening = int(args.screen_dataset_limit) > 0 and len(screen_datasets) < len(datasets)
    print(
        f"[TuneCandidate] screening={'on' if do_screening else 'off'}, "
        f"screen_dataset_count={len(screen_datasets)}, final_dataset_count={len(datasets)}"
    )
    print(f"[TuneCandidate] eval_cache={'on' if cache_enabled else 'off'} path={eval_cache_json}")

    screen_rows: List[Dict[str, Any]] = []
    rows: List[Dict[str, Any]] = []
    with verification_lock(
        lock_path,
        timeout_sec=int(args.verification_lock_timeout_sec),
        stale_sec=int(args.verification_lock_stale_sec),
    ):
        try:
            for combo in combo_specs:
                print(f"[TuneCandidate][Screen] Running combo: {combo['combo_id']}")
                screen_row = evaluate_combo(
                    matrix_script=matrix_script,
                    build_config=build_config,
                    original_build_raw=original_build_raw,
                    combo=combo,
                    datasets=screen_datasets if do_screening else datasets,
                    output_dir=output_dir,
                    stage_name="screen",
                    profile_ids=screen_profile_ids,
                    gate_min_avg_trades=int(args.gate_min_avg_trades),
                    require_higher_tf_companions=bool(args.require_higher_tf_companions),
                    enable_hostility_adaptive_thresholds=bool(args.enable_hostility_adaptive_thresholds),
                    enable_hostility_adaptive_trades_only=bool(args.enable_hostility_adaptive_trades_only),
                    matrix_max_workers=int(args.matrix_max_workers),
                    matrix_backtest_retry_count=int(args.matrix_backtest_retry_count),
                    skip_core_vs_legacy_gate=bool(args.skip_core_vs_legacy_gate),
                    eval_cache=eval_cache,
                    cache_enabled=cache_enabled,
                    base_config_hash=base_config_hash,
                    datasets_sig_hash=screen_dataset_sig_hash if do_screening else final_dataset_sig_hash,
                )
                screen_effective = get_effective_objective_thresholds(screen_row, args)
                screen_objective = compute_combo_objective(
                    avg_profit_factor=float(screen_row.get("avg_profit_factor", 0.0)),
                    avg_expectancy_krw=float(screen_row.get("avg_expectancy_krw", 0.0)),
                    profitable_ratio=float(screen_row.get("profitable_ratio", 0.0)),
                    avg_total_trades=float(screen_row.get("avg_total_trades", 0.0)),
                    avg_win_rate_pct=float(screen_row.get("avg_win_rate_pct", 0.0)),
                    min_avg_trades=float(screen_effective["min_avg_trades"]),
                    min_profitable_ratio=float(screen_effective["min_profitable_ratio"]),
                    min_avg_win_rate_pct=float(screen_effective["min_avg_win_rate_pct"]),
                    min_expectancy_krw=float(screen_effective["min_expectancy_krw"]),
                    objective_mode=str(args.objective_mode),
                )
                screen_row["objective_score"] = screen_objective
                screen_row["objective_effective_min_avg_trades"] = float(screen_effective["min_avg_trades"])
                screen_row["objective_effective_min_profitable_ratio"] = float(screen_effective["min_profitable_ratio"])
                screen_row["objective_effective_min_avg_win_rate_pct"] = float(screen_effective["min_avg_win_rate_pct"])
                screen_row["objective_effective_min_expectancy_krw"] = float(screen_effective["min_expectancy_krw"])
                combo_meta = bottleneck_priority_meta.get(str(combo["combo_id"]), {})
                screen_row["bottleneck_priority_rank"] = int(combo_meta.get("priority_rank", 0) or 0)
                screen_row["bottleneck_priority_score"] = float(combo_meta.get("priority_score", 0.0) or 0.0)
                screen_row["bottleneck_top_group"] = str(combo_meta.get("top_group", ""))
                screen_row["bottleneck_no_trade_bias_active"] = bool(
                    combo_meta.get("no_trade_bias_active", False)
                )
                screen_row["bottleneck_scenario_family"] = str(combo.get("bottleneck_scenario_family", ""))
                screen_row["bottleneck_scenario_family_adapted"] = bool(
                    combo.get("bottleneck_scenario_family_adapted", False)
                )
                screen_row["holdout_failure_suppression_active"] = bool(
                    combo.get("holdout_failure_suppression_active", False)
                )
                screen_row["holdout_failure_suppressed_family"] = bool(
                    combo.get("holdout_failure_suppressed_family", False)
                )
                screen_row["holdout_failure_suppression_reason"] = str(
                    combo.get("holdout_failure_suppression_reason", "")
                )
                screen_row["bottleneck_hint_guardrail_active"] = bool(
                    combo.get("bottleneck_hint_guardrail_active", False)
                )
                screen_row["bottleneck_hint_guardrail_ratio"] = float(
                    combo.get("bottleneck_hint_guardrail_ratio", 0.0) or 0.0
                )
                screen_row["bottleneck_hint_guardrail_threshold"] = float(
                    combo.get("bottleneck_hint_guardrail_threshold", 0.0) or 0.0
                )
                screen_row["constraint_pass"] = (
                    float(screen_row.get("avg_total_trades", 0.0)) >= float(screen_effective["min_avg_trades"])
                    and float(screen_row.get("profitable_ratio", 0.0)) >= float(screen_effective["min_profitable_ratio"])
                    and float(screen_row.get("avg_win_rate_pct", 0.0)) >= float(screen_effective["min_avg_win_rate_pct"])
                    and float(screen_row.get("avg_expectancy_krw", 0.0)) >= float(screen_effective["min_expectancy_krw"])
                )
                screen_rows.append(screen_row)

            selected_combo_ids = [x["combo_id"] for x in combo_specs]
            if do_screening:
                screen_sorted = sorted(
                    screen_rows,
                    key=lambda x: (
                        bool(x.get("constraint_pass", False)),
                        float(x.get("objective_score", 0.0)),
                        float(x.get("avg_expectancy_krw", 0.0)),
                        float(x.get("avg_win_rate_pct", 0.0)),
                        float(x.get("profitable_ratio", 0.0)),
                        float(x.get("avg_total_trades", 0.0)),
                    ),
                    reverse=True,
                )
                selected_combo_ids = [x["combo_id"] for x in screen_sorted[: max(1, int(args.screen_top_k))]]
                print(f"[TuneCandidate] screened_top_k={len(selected_combo_ids)}")

            screen_map = {str(x["combo_id"]): x for x in screen_rows}
            combos_for_final = [x for x in combo_specs if x["combo_id"] in set(selected_combo_ids)]
            for combo in combos_for_final:
                print(f"[TuneCandidate][Final] Running combo: {combo['combo_id']}")
                final_row = evaluate_combo(
                    matrix_script=matrix_script,
                    build_config=build_config,
                    original_build_raw=original_build_raw,
                    combo=combo,
                    datasets=datasets,
                    output_dir=output_dir,
                    stage_name="final",
                    profile_ids=final_profile_ids,
                    gate_min_avg_trades=int(args.gate_min_avg_trades),
                    require_higher_tf_companions=bool(args.require_higher_tf_companions),
                    enable_hostility_adaptive_thresholds=bool(args.enable_hostility_adaptive_thresholds),
                    enable_hostility_adaptive_trades_only=bool(args.enable_hostility_adaptive_trades_only),
                    matrix_max_workers=int(args.matrix_max_workers),
                    matrix_backtest_retry_count=int(args.matrix_backtest_retry_count),
                    skip_core_vs_legacy_gate=bool(args.skip_core_vs_legacy_gate),
                    eval_cache=eval_cache,
                    cache_enabled=cache_enabled,
                    base_config_hash=base_config_hash,
                    datasets_sig_hash=final_dataset_sig_hash,
                )
                final_effective = get_effective_objective_thresholds(final_row, args)
                final_objective = compute_combo_objective(
                    avg_profit_factor=float(final_row.get("avg_profit_factor", 0.0)),
                    avg_expectancy_krw=float(final_row.get("avg_expectancy_krw", 0.0)),
                    profitable_ratio=float(final_row.get("profitable_ratio", 0.0)),
                    avg_total_trades=float(final_row.get("avg_total_trades", 0.0)),
                    avg_win_rate_pct=float(final_row.get("avg_win_rate_pct", 0.0)),
                    min_avg_trades=float(final_effective["min_avg_trades"]),
                    min_profitable_ratio=float(final_effective["min_profitable_ratio"]),
                    min_avg_win_rate_pct=float(final_effective["min_avg_win_rate_pct"]),
                    min_expectancy_krw=float(final_effective["min_expectancy_krw"]),
                    objective_mode=str(args.objective_mode),
                )
                final_row["objective_score"] = final_objective
                final_row["objective_effective_min_avg_trades"] = float(final_effective["min_avg_trades"])
                final_row["objective_effective_min_profitable_ratio"] = float(final_effective["min_profitable_ratio"])
                final_row["objective_effective_min_avg_win_rate_pct"] = float(final_effective["min_avg_win_rate_pct"])
                final_row["objective_effective_min_expectancy_krw"] = float(final_effective["min_expectancy_krw"])
                linked_screen = screen_map.get(str(combo["combo_id"]), {})
                final_row["screen_objective_score"] = float(linked_screen.get("objective_score", 0.0))
                final_row["screen_avg_total_trades"] = float(linked_screen.get("avg_total_trades", 0.0))
                final_row["screen_profitable_ratio"] = float(linked_screen.get("profitable_ratio", 0.0))
                final_row["screen_avg_win_rate_pct"] = float(linked_screen.get("avg_win_rate_pct", 0.0))
                combo_meta = bottleneck_priority_meta.get(str(combo["combo_id"]), {})
                final_row["bottleneck_priority_rank"] = int(combo_meta.get("priority_rank", 0) or 0)
                final_row["bottleneck_priority_score"] = float(combo_meta.get("priority_score", 0.0) or 0.0)
                final_row["bottleneck_top_group"] = str(combo_meta.get("top_group", ""))
                final_row["bottleneck_no_trade_bias_active"] = bool(
                    combo_meta.get("no_trade_bias_active", False)
                )
                final_row["bottleneck_scenario_family"] = str(combo.get("bottleneck_scenario_family", ""))
                final_row["bottleneck_scenario_family_adapted"] = bool(
                    combo.get("bottleneck_scenario_family_adapted", False)
                )
                final_row["holdout_failure_suppression_active"] = bool(
                    combo.get("holdout_failure_suppression_active", False)
                )
                final_row["holdout_failure_suppressed_family"] = bool(
                    combo.get("holdout_failure_suppressed_family", False)
                )
                final_row["holdout_failure_suppression_reason"] = str(
                    combo.get("holdout_failure_suppression_reason", "")
                )
                final_row["bottleneck_hint_guardrail_active"] = bool(
                    combo.get("bottleneck_hint_guardrail_active", False)
                )
                final_row["bottleneck_hint_guardrail_ratio"] = float(
                    combo.get("bottleneck_hint_guardrail_ratio", 0.0) or 0.0
                )
                final_row["bottleneck_hint_guardrail_threshold"] = float(
                    combo.get("bottleneck_hint_guardrail_threshold", 0.0) or 0.0
                )
                rows.append(final_row)
        finally:
            build_config.write_text(original_build_raw, encoding="utf-8", newline="\n")

    if not rows:
        raise RuntimeError("No tuning rows generated.")

    sorted_rows = sorted(
        rows,
        key=lambda x: (
            float(x.get("objective_score", 0.0)),
            float(x.get("avg_expectancy_krw", 0.0)),
            float(x.get("avg_win_rate_pct", 0.0)),
            float(x.get("profitable_ratio", 0.0)),
            float(x.get("avg_total_trades", 0.0)),
            float(x.get("avg_profit_factor", 0.0)),
        ),
        reverse=True,
    )
    with summary_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(sorted_rows[0].keys()))
        writer.writeheader()
        writer.writerows(sorted_rows)

    report_out = {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "dataset_mode": "realdata_only" if args.real_data_only else "mixed",
        "require_higher_tf_companions": bool(args.require_higher_tf_companions),
        "dataset_quality_gate": dataset_quality_gate,
        "dataset_dirs": [str(x) for x in scan_dirs],
        "dataset_count": len(datasets),
        "datasets": [str(x) for x in datasets],
        "screening": {
            "enabled": bool(do_screening),
            "eval_cache_enabled": cache_enabled,
            "eval_cache_json": str(eval_cache_json),
            "screen_dataset_limit": int(args.screen_dataset_limit),
            "screen_dataset_count": len(screen_datasets),
            "screen_top_k": int(args.screen_top_k),
            "screen_profile_ids": screen_profile_ids,
            "final_profile_ids": final_profile_ids,
            "objective_min_avg_trades": float(args.objective_min_avg_trades),
            "gate_min_avg_trades": int(args.gate_min_avg_trades),
            "objective_min_profitable_ratio": float(args.objective_min_profitable_ratio),
            "objective_min_avg_win_rate_pct": float(args.objective_min_avg_win_rate_pct),
            "objective_min_expectancy_krw": float(args.objective_min_expectancy_krw),
            "objective_mode": str(args.objective_mode),
            "enable_hostility_adaptive_thresholds": bool(args.enable_hostility_adaptive_thresholds),
            "enable_hostility_adaptive_trades_only": bool(args.enable_hostility_adaptive_trades_only),
            "skip_core_vs_legacy_gate": bool(args.skip_core_vs_legacy_gate),
            "use_effective_thresholds_for_objective": bool(args.use_effective_thresholds_for_objective),
            "enable_bottleneck_adapted_scenarios": bool(args.enable_bottleneck_adapted_scenarios),
            "enable_hint_impact_guardrail": bool(args.enable_hint_impact_guardrail),
            "hint_impact_guardrail_ratio": float(args.hint_impact_guardrail_ratio),
            "hint_impact_guardrail_tighten_scale": float(args.hint_impact_guardrail_tighten_scale),
            "enable_holdout_failure_family_suppression": bool(args.enable_holdout_failure_family_suppression),
            "holdout_suppression_hint_ratio_threshold": float(args.holdout_suppression_hint_ratio_threshold),
            "holdout_suppression_require_both_pf_exp_fail": bool(
                args.holdout_suppression_require_both_pf_exp_fail
            ),
            "enable_post_suppression_quality_expansion": bool(args.enable_post_suppression_quality_expansion),
            "post_suppression_min_combo_count": int(max(1, int(args.post_suppression_min_combo_count))),
            "train_eval_summary_json": str(train_eval_summary_json),
        },
        "bottleneck_priority": {
            "enabled": bool(args.enable_bottleneck_priority),
            "live_signal_funnel_taxonomy_json": str(live_signal_funnel_taxonomy_json),
            "live_context_raw": live_bottleneck_context,
            "context": bottleneck_context,
            "scenario_family_counts": scenario_family_counts,
            "holdout_failure_suppression": holdout_family_suppression_meta,
            "post_suppression_quality_expansion": post_suppression_quality_expansion_meta,
            "hint_impact_guardrail": {
                "enabled": bool(args.enable_hint_impact_guardrail),
                "ratio": float(args.hint_impact_guardrail_ratio),
                "tighten_scale": float(args.hint_impact_guardrail_tighten_scale),
            },
            "combo_priority_order": [
                {
                    "combo_id": str(c.get("combo_id", "")),
                    "priority_rank": int((bottleneck_priority_meta.get(str(c.get("combo_id", "")), {}) or {}).get("priority_rank", 0)),
                    "priority_score": float((bottleneck_priority_meta.get(str(c.get("combo_id", "")), {}) or {}).get("priority_score", 0.0)),
                    "scenario_family": str(c.get("bottleneck_scenario_family", "")),
                    "scenario_family_adapted": bool(c.get("bottleneck_scenario_family_adapted", False)),
                    "holdout_failure_suppression_active": bool(c.get("holdout_failure_suppression_active", False)),
                    "holdout_failure_suppressed_family": bool(c.get("holdout_failure_suppressed_family", False)),
                    "holdout_failure_suppression_reason": str(c.get("holdout_failure_suppression_reason", "")),
                    "hint_guardrail_active": bool(c.get("bottleneck_hint_guardrail_active", False)),
                    "hint_guardrail_ratio": float(c.get("bottleneck_hint_guardrail_ratio", 0.0) or 0.0),
                }
                for c in combo_specs
            ],
        },
        "combos": combo_specs,
        "screen_summary": screen_rows,
        "summary": sorted_rows,
    }
    ensure_parent_directory(summary_json)
    summary_json.write_text(json.dumps(report_out, ensure_ascii=False, indent=4), encoding="utf-8", newline="\n")
    if cache_enabled:
        save_eval_cache(eval_cache_json, eval_cache)

    print("[TuneCandidate] Completed")
    print(f"summary_csv={summary_csv}")
    print(f"summary_json={summary_json}")
    print(f"best_combo={sorted_rows[0]['combo_id']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
