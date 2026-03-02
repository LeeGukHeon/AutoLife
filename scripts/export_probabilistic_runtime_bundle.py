#!/usr/bin/env python3
import argparse
import pathlib
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import joblib
import numpy as np

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export trained probabilistic model artifacts to runtime-friendly JSON bundle."
    )
    parser.add_argument(
        "--train-summary-json",
        default=r".\build\Release\logs\probabilistic_model_train_summary_global_v2_gate4_20260224.json",
    )
    parser.add_argument(
        "--output-json",
        default=r".\config\model\probabilistic_runtime_bundle_v2.json",
    )
    parser.add_argument(
        "--export-mode",
        choices=("global_only", "hybrid", "per_market"),
        default="global_only",
        help=(
            "global_only: export only global fallback(default_model), "
            "hybrid: export global fallback + per-market entries, "
            "per_market: export per-market entries only."
        ),
    )
    parser.add_argument(
        "--fold-policy",
        choices=("latest", "best_h5_logloss"),
        default="latest",
        help="Which fold to export per market.",
    )
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("v2",),
        default="v2",
        help="MODE switch. v2 exports active runtime bundle contract.",
    )
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def infer_train_summary_pipeline_version(summary: Dict[str, Any]) -> str:
    explicit = str(summary.get("pipeline_version", "")).strip().lower()
    if explicit == "v2":
        return explicit
    summary_version = str(summary.get("version", "")).strip().lower()
    if "v2" in summary_version:
        return "v2"
    return "unknown"


def normalize_regime_entry_disable_key(raw_key: Any) -> str:
    token = str(raw_key or "").strip().lower().replace("-", "_").replace(" ", "_")
    if token in ("ranging", "range"):
        return "RANGING"
    if token in ("trending_up", "up"):
        return "TRENDING_UP"
    if token in ("trending_down", "down"):
        return "TRENDING_DOWN"
    if token in ("high_volatility", "volatile", "hostile"):
        return "HIGH_VOLATILITY"
    if token == "unknown":
        return "UNKNOWN"
    if token in ("any", "default"):
        return "ANY"
    return ""


def default_phase3_bundle_config() -> Dict[str, Any]:
    return {
        "disable_ranging_entry": False,
        "regime_entry_disable": {},
        "ev_calibration": {
            "enabled": True,
            "use_quantile_map": False,
            "min_bucket_samples": 32,
            "default_confidence": 0.90,
            "min_confidence": 0.10,
            "ood_penalty": 0.10,
            "buckets": [
                {
                    "name": "default_all",
                    "regime": "ANY",
                    "volatility_bucket": "ANY",
                    "liquidity_bucket": "ANY",
                    "slope": 0.95,
                    "intercept_bps": 1.0,
                    "sample_size": 500,
                    "confidence": 0.88,
                    "vol_bps_min": -1.0,
                    "vol_bps_max": -1.0,
                    "liquidity_ratio_min": -1.0,
                    "liquidity_ratio_max": -1.0,
                    "quantile_map": [],
                }
            ],
        },
        "cost_model": {
            "enabled": True,
            "mode": "hybrid_mode",
            "entry_multiplier": 0.55,
            "exit_multiplier": 0.45,
            "entry_add_bps": 0.0,
            "exit_add_bps": 0.0,
            "tail_markup_ratio": 0.35,
            "tail_add_bps": 0.0,
            "hybrid_lambda": 0.50,
        },
        "adaptive_ev_blend": {
            "enabled": True,
            "min": 0.05,
            "max": 0.40,
            "base": 0.20,
            "trend_bonus": 0.08,
            "ranging_penalty": 0.06,
            "hostile_penalty": 0.08,
            "high_confidence_threshold": 0.72,
            "high_confidence_bonus": 0.05,
            "low_confidence_penalty": 0.10,
            "cost_penalty": 0.06,
        },
        "operations_control": {
            "enabled": False,
            "mode": "manual",
            "required_ev_offset": 0.0,
            "required_ev_offset_min": -0.0030,
            "required_ev_offset_max": 0.0030,
            "k_margin_scale": 1.0,
            "k_margin_scale_min": 0.50,
            "k_margin_scale_max": 2.00,
            "ev_blend_scale": 1.0,
            "ev_blend_scale_min": 0.50,
            "ev_blend_scale_max": 1.50,
            "max_step_per_update": 0.05,
            "min_update_interval_sec": 3600,
        },
        "execution_guard": {
            "liq_vol_gate": {
                "enabled": False,
                "mode": "static",
                "window_minutes": 60,
                "quantile_q": 0.20,
                "min_samples_required": 30,
                "low_conf_action": "hold",
            }
        },
        "foundation_structure_gate": {
            "enabled": False,
            "mode": "static",
            "relax_delta": 0.0,
        },
        "bear_rebound_guard": {
            "enabled": False,
            "mode": "static",
            "window_minutes": 60,
            "quantile_q": 0.20,
            "min_samples_required": 30,
            "low_conf_action": "hold",
            "static_threshold": 1.0,
        },
        "primary_minimums": {
            "enabled": True,
            "min_h5_calibrated": 0.48,
            "min_h5_margin": -0.03,
            "min_liquidity_score": 42.0,
            "min_signal_strength": 0.34,
            "hostile_add_h5_calibrated": 0.06,
            "hostile_add_h5_margin": 0.03,
            "hostile_add_liquidity_score": 10.0,
            "hostile_add_signal_strength": 0.08,
            "ranging_add_h5_calibrated": 0.01,
            "ranging_add_h5_margin": 0.005,
            "ranging_add_liquidity_score": 2.0,
            "ranging_add_signal_strength": 0.02,
            "trending_up_add_h5_calibrated": -0.02,
            "trending_up_add_h5_margin": -0.01,
            "trending_up_add_liquidity_score": -5.0,
            "trending_up_add_signal_strength": -0.03,
            "regime_volatile_add_signal_strength": 0.03,
            "regime_hostile_add_liquidity_score": 8.0,
            "regime_hostile_add_signal_strength": 0.08,
            "clamp_h5_margin_min": -0.50,
            "clamp_h5_margin_max": 0.20,
            "clamp_liquidity_min": 0.0,
            "clamp_liquidity_max": 100.0,
            "clamp_strength_min": 0.0,
            "clamp_strength_max": 1.0,
        },
        "primary_priority": {
            "enabled": True,
            "margin_score_shift": 0.10,
            "margin_score_scale": 0.20,
            "edge_score_shift": 0.0005,
            "edge_score_scale": 0.0025,
            "prob_weight": 0.50,
            "margin_weight": 0.22,
            "liquidity_weight": 0.10,
            "strength_weight": 0.10,
            "edge_weight": 0.08,
            "hostile_prob_weight": 0.54,
            "hostile_margin_weight": 0.22,
            "hostile_liquidity_weight": 0.11,
            "hostile_strength_weight": 0.09,
            "hostile_edge_weight": 0.04,
            "strong_buy_bonus": 0.02,
            "margin_bonus_scale": 0.08,
            "margin_bonus_cap": 0.03,
            "range_penalty": 0.11,
            "range_bonus": 0.03,
            "range_penalty_strength_floor": 0.50,
            "range_penalty_margin_floor": 0.008,
            "range_penalty_prob_floor": 0.54,
            "range_bonus_margin_floor": 0.012,
            "range_bonus_prob_floor": 0.57,
            "uptrend_bonus": 0.03,
            "uptrend_bonus_margin_floor": 0.0,
            "uptrend_bonus_prob_floor": 0.52,
        },
        "primary_decision_profile": {
            "enabled": True,
            "confidence_prob_shift": 0.50,
            "confidence_prob_scale": 0.20,
            "confidence_margin_shift": 0.01,
            "confidence_margin_scale": 0.08,
            "confidence_prob_weight": 0.65,
            "confidence_margin_weight": 0.35,
            "target_strength_base": 0.22,
            "target_strength_prob_weight": 0.60,
            "target_strength_margin_weight": 1.80,
            "target_strength_min_hostile": 0.26,
            "target_strength_min_calm": 0.18,
            "target_strength_max": 0.98,
            "target_filter_base": 0.42,
            "target_filter_prob_weight": 0.40,
            "target_filter_margin_weight": 0.70,
            "target_filter_min": 0.20,
            "target_filter_max": 0.95,
            "implied_win_margin_weight": 0.45,
            "implied_win_threshold_gap_weight": 0.35,
            "implied_win_min_hostile": 0.44,
            "implied_win_min_calm": 0.40,
            "implied_win_max": 0.86,
            "probabilistic_edge_floor_margin_weight": 0.0012,
            "probabilistic_edge_floor_prob_weight": 0.0010,
            "probabilistic_edge_floor_penalty_hostile": 0.00045,
            "probabilistic_edge_floor_penalty_calm": 0.00030,
            "current_risk_min": 0.0010,
            "current_risk_max": 0.0500,
            "target_risk_base_hostile": 0.0026,
            "target_risk_base_calm": 0.0028,
            "target_risk_confidence_scale_hostile": 0.0036,
            "target_risk_confidence_scale_calm": 0.0048,
            "fragility_target_risk_multiplier": 0.78,
            "fragility_negative_margin_threshold": -0.006,
            "fragility_negative_margin_target_risk_multiplier": 0.90,
            "target_risk_min_hostile": 0.0022,
            "target_risk_min_calm": 0.0024,
            "target_risk_max_hostile": 0.0075,
            "target_risk_max_calm": 0.0105,
            "blended_risk_old_weight": 0.35,
            "blended_risk_target_weight": 0.65,
            "blended_risk_min_hostile": 0.0022,
            "blended_risk_min_calm": 0.0024,
            "blended_risk_max_hostile": 0.0080,
            "blended_risk_max_calm": 0.0120,
            "rr_base_hostile": 1.10,
            "rr_base_calm": 1.20,
            "rr_confidence_weight_hostile": 1.00,
            "rr_confidence_weight_calm": 1.30,
            "rr_margin_positive_weight_hostile": 2.0,
            "rr_margin_positive_weight_calm": 2.8,
            "fragility_rr_bonus": 0.18,
            "rr_min_hostile": 1.05,
            "rr_min_calm": 1.10,
            "rr_max_hostile": 2.40,
            "rr_max_calm": 3.20,
            "tp1_rr_min": 1.0,
            "tp1_rr_multiplier": 0.55,
            "breakeven_mult_fragility": 0.48,
            "breakeven_mult_default": 0.70,
            "trailing_mult_fragility": 0.82,
            "trailing_mult_default": 1.10,
            "size_base": 0.42,
            "size_confidence_weight": 0.80,
            "size_margin_positive_weight": 1.20,
            "size_negative_margin_multiplier": 0.86,
            "size_hostile_multiplier": 0.88,
            "size_min": 0.30,
            "size_max": 1.35,
            "score_margin_boost_cap": 0.12,
            "filter_margin_boost_scale": 0.10,
            "edge_clip_abs_pct": 0.05,
            "primary_nudge_base": 0.20,
            "primary_nudge_blend_scale": 0.25,
            "primary_filter_nudge_base": 0.10,
            "primary_filter_nudge_blend_scale": 0.10,
        },
        "diagnostics_v2": {"enabled": True},
    }


def merge_dicts(base: Dict[str, Any], override: Dict[str, Any]) -> Dict[str, Any]:
    out: Dict[str, Any] = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = merge_dicts(out[key], value)
        else:
            out[key] = value
    return out


def build_phase3_bundle_config(summary: Dict[str, Any]) -> Dict[str, Any]:
    defaults = default_phase3_bundle_config()
    raw = summary.get("phase3", {})
    if not isinstance(raw, dict):
        raw = {}
    merged = merge_dicts(defaults, raw)

    merged["ev_calibration"]["enabled"] = bool(merged["ev_calibration"].get("enabled", False))
    merged["cost_model"]["enabled"] = bool(merged["cost_model"].get("enabled", False))
    merged["adaptive_ev_blend"]["enabled"] = bool(merged["adaptive_ev_blend"].get("enabled", False))
    merged["operations_control"]["enabled"] = bool(merged["operations_control"].get("enabled", False))
    execution_guard = merged.get("execution_guard", {})
    if not isinstance(execution_guard, dict):
        execution_guard = {}
    liq_vol_gate = execution_guard.get("liq_vol_gate", {})
    if not isinstance(liq_vol_gate, dict):
        liq_vol_gate = {}
    mode = str(liq_vol_gate.get("mode", "static")).strip().lower()
    if mode != "quantile_dynamic":
        mode = "static"
    low_conf_action = str(liq_vol_gate.get("low_conf_action", "hold")).strip().lower()
    if low_conf_action != "fallback_static":
        low_conf_action = "hold"
    liq_vol_gate_clean = {
        "enabled": bool(liq_vol_gate.get("enabled", False) and mode == "quantile_dynamic"),
        "mode": mode,
        "window_minutes": int(np.clip(round(finite_float(liq_vol_gate.get("window_minutes", 60), 60.0)), 1, 1440)),
        "quantile_q": float(np.clip(finite_float(liq_vol_gate.get("quantile_q", 0.20), 0.20), 0.0, 1.0)),
        "min_samples_required": int(
            np.clip(round(finite_float(liq_vol_gate.get("min_samples_required", 30), 30.0)), 1, 200000)
        ),
        "low_conf_action": low_conf_action,
    }
    if not liq_vol_gate_clean["enabled"]:
        liq_vol_gate_clean["mode"] = "static"
    execution_guard["liq_vol_gate"] = liq_vol_gate_clean
    merged["execution_guard"] = execution_guard
    foundation_structure_gate = merged.get("foundation_structure_gate", {})
    if not isinstance(foundation_structure_gate, dict):
        foundation_structure_gate = {}
    structure_mode = str(foundation_structure_gate.get("mode", "static")).strip().lower()
    if structure_mode != "trend_only_relax":
        structure_mode = "static"
    foundation_structure_gate_clean = {
        "enabled": bool(
            foundation_structure_gate.get("enabled", False) and structure_mode == "trend_only_relax"
        ),
        "mode": structure_mode,
        "relax_delta": float(
            np.clip(finite_float(foundation_structure_gate.get("relax_delta", 0.0), 0.0), 0.0, 1.0)
        ),
    }
    if not foundation_structure_gate_clean["enabled"]:
        foundation_structure_gate_clean["mode"] = "static"
        foundation_structure_gate_clean["relax_delta"] = 0.0
    merged["foundation_structure_gate"] = foundation_structure_gate_clean
    bear_rebound_guard = merged.get("bear_rebound_guard", {})
    if not isinstance(bear_rebound_guard, dict):
        bear_rebound_guard = {}
    bear_mode = str(bear_rebound_guard.get("mode", "static")).strip().lower()
    if bear_mode != "quantile_dynamic":
        bear_mode = "static"
    bear_low_conf_action = str(bear_rebound_guard.get("low_conf_action", "hold")).strip().lower()
    if bear_low_conf_action != "fallback_static":
        bear_low_conf_action = "hold"
    bear_rebound_guard_clean = {
        "enabled": bool(bear_rebound_guard.get("enabled", False) and bear_mode == "quantile_dynamic"),
        "mode": bear_mode,
        "window_minutes": int(
            np.clip(round(finite_float(bear_rebound_guard.get("window_minutes", 60), 60.0)), 1, 1440)
        ),
        "quantile_q": float(
            np.clip(finite_float(bear_rebound_guard.get("quantile_q", 0.20), 0.20), 0.0, 1.0)
        ),
        "min_samples_required": int(
            np.clip(
                round(finite_float(bear_rebound_guard.get("min_samples_required", 30), 30.0)),
                1,
                200000,
            )
        ),
        "low_conf_action": bear_low_conf_action,
        "static_threshold": float(
            np.clip(finite_float(bear_rebound_guard.get("static_threshold", 1.0), 1.0), 0.0, 10.0)
        ),
    }
    if not bear_rebound_guard_clean["enabled"]:
        bear_rebound_guard_clean["mode"] = "static"
    merged["bear_rebound_guard"] = bear_rebound_guard_clean
    merged["diagnostics_v2"]["enabled"] = bool(merged["diagnostics_v2"].get("enabled", False))
    merged["primary_minimums"]["enabled"] = bool(merged["primary_minimums"].get("enabled", False))
    merged["primary_priority"]["enabled"] = bool(merged["primary_priority"].get("enabled", False))
    merged["primary_decision_profile"]["enabled"] = bool(
        merged["primary_decision_profile"].get("enabled", False)
    )
    merged["disable_ranging_entry"] = bool(merged.get("disable_ranging_entry", False))
    raw_regime_entry_disable = merged.get("regime_entry_disable", {})
    if not isinstance(raw_regime_entry_disable, dict):
        raw_regime_entry_disable = {}
    cleaned_regime_entry_disable: Dict[str, bool] = {}
    for key, value in raw_regime_entry_disable.items():
        regime_key = normalize_regime_entry_disable_key(key)
        if not regime_key:
            continue
        cleaned_regime_entry_disable[regime_key] = bool(value)
    if merged["disable_ranging_entry"]:
        cleaned_regime_entry_disable["RANGING"] = True
    merged["regime_entry_disable"] = cleaned_regime_entry_disable
    return merged


def default_phase4_bundle_config() -> Dict[str, Any]:
    return {
        "portfolio_allocator": {
            "enabled": False,
            "top_k": 1,
            "min_score": -1.0e6,
            "lambda_tail": 1.0,
            "lambda_cost": 1.0,
            "lambda_uncertainty": 1.0,
            "lambda_margin": 1.0,
            "uncertainty_prob_weight": 0.50,
            "uncertainty_ev_weight": 0.50,
        },
        "correlation_control": {
            "enabled": False,
            "default_cluster_cap": 1.0,
            "market_cluster_map": {},
            "cluster_caps": {},
        },
        "risk_budget": {
            "enabled": False,
            "per_market_cap": 1.0,
            "gross_cap": 1.0,
            "risk_budget_cap": 1.0,
            "risk_proxy_stop_pct": 0.03,
            "regime_budget_multipliers": {},
        },
        "drawdown_governor": {
            "enabled": False,
            "dd_threshold_soft": 0.05,
            "dd_threshold_hard": 0.10,
            "budget_multiplier_soft": 0.70,
            "budget_multiplier_hard": 0.40,
        },
        "execution_aware_sizing": {
            "enabled": False,
            "liquidity_low_threshold": 40.0,
            "liquidity_mid_threshold": 65.0,
            "liquidity_low_size_multiplier": 0.50,
            "liquidity_mid_size_multiplier": 0.75,
            "liquidity_high_size_multiplier": 1.00,
            "tail_cost_soft_pct": 0.0015,
            "tail_cost_hard_pct": 0.0030,
            "tail_soft_multiplier": 0.80,
            "tail_hard_multiplier": 0.60,
            "min_position_size": 0.01,
        },
        "portfolio_diagnostics": {"enabled": False},
    }


def build_phase4_bundle_config(summary: Dict[str, Any]) -> Dict[str, Any]:
    defaults = default_phase4_bundle_config()
    raw = summary.get("phase4", {})
    if not isinstance(raw, dict):
        raw = {}
    merged = merge_dicts(defaults, raw)
    allocator = merged["portfolio_allocator"]
    allocator["enabled"] = bool(allocator.get("enabled", False))
    allocator["top_k"] = int(np.clip(round(finite_float(allocator.get("top_k", 1), 1.0)), 1, 256))
    allocator["min_score"] = finite_float(allocator.get("min_score", -1.0e6), -1.0e6)
    allocator["lambda_tail"] = max(0.0, finite_float(allocator.get("lambda_tail", 1.0), 1.0))
    allocator["lambda_cost"] = max(0.0, finite_float(allocator.get("lambda_cost", 1.0), 1.0))
    allocator["lambda_uncertainty"] = max(
        0.0, finite_float(allocator.get("lambda_uncertainty", 1.0), 1.0)
    )
    allocator["lambda_margin"] = max(0.0, finite_float(allocator.get("lambda_margin", 1.0), 1.0))
    prob_w = max(0.0, finite_float(allocator.get("uncertainty_prob_weight", 0.50), 0.50))
    ev_w = max(0.0, finite_float(allocator.get("uncertainty_ev_weight", 0.50), 0.50))
    if prob_w + ev_w <= 1e-9:
        prob_w = 0.50
        ev_w = 0.50
    norm = prob_w + ev_w
    allocator["uncertainty_prob_weight"] = prob_w / norm
    allocator["uncertainty_ev_weight"] = ev_w / norm
    correlation_control = merged["correlation_control"]
    correlation_control["enabled"] = bool(correlation_control.get("enabled", False))
    correlation_control["default_cluster_cap"] = float(
        np.clip(
            finite_float(correlation_control.get("default_cluster_cap", 1.0), 1.0),
            0.0,
            10.0,
        )
    )
    raw_market_cluster_map = correlation_control.get("market_cluster_map", {})
    if not isinstance(raw_market_cluster_map, dict):
        raw_market_cluster_map = {}
    correlation_control["market_cluster_map"] = {
        str(k): str(v)
        for k, v in raw_market_cluster_map.items()
        if str(k).strip() and str(v).strip()
    }
    raw_cluster_caps = correlation_control.get("cluster_caps", {})
    if not isinstance(raw_cluster_caps, dict):
        raw_cluster_caps = {}
    correlation_control["cluster_caps"] = {
        str(k): float(np.clip(finite_float(v, correlation_control["default_cluster_cap"]), 0.0, 10.0))
        for k, v in raw_cluster_caps.items()
        if str(k).strip()
    }
    risk_budget = merged["risk_budget"]
    risk_budget["enabled"] = bool(risk_budget.get("enabled", False))
    risk_budget["per_market_cap"] = float(
        np.clip(finite_float(risk_budget.get("per_market_cap", 1.0), 1.0), 0.0, 1.0)
    )
    risk_budget["gross_cap"] = float(
        np.clip(finite_float(risk_budget.get("gross_cap", 1.0), 1.0), 0.0, 10.0)
    )
    risk_budget["risk_budget_cap"] = float(
        np.clip(finite_float(risk_budget.get("risk_budget_cap", 1.0), 1.0), 0.0, 10.0)
    )
    risk_budget["risk_proxy_stop_pct"] = float(
        np.clip(finite_float(risk_budget.get("risk_proxy_stop_pct", 0.03), 0.03), 0.0, 1.0)
    )
    raw_regime_budget_multipliers = risk_budget.get("regime_budget_multipliers", {})
    if not isinstance(raw_regime_budget_multipliers, dict):
        raw_regime_budget_multipliers = {}
    cleaned_regime_budget_multipliers: Dict[str, float] = {}
    for key, value in raw_regime_budget_multipliers.items():
        regime_key = str(key).strip().upper()
        if not regime_key:
            continue
        cleaned_regime_budget_multipliers[regime_key] = float(
            np.clip(finite_float(value, 1.0), 0.0, 1.0)
        )
    risk_budget["regime_budget_multipliers"] = cleaned_regime_budget_multipliers
    drawdown_governor = merged["drawdown_governor"]
    drawdown_governor["enabled"] = bool(drawdown_governor.get("enabled", False))
    drawdown_governor["dd_threshold_soft"] = float(
        np.clip(finite_float(drawdown_governor.get("dd_threshold_soft", 0.05), 0.05), 0.0, 1.0)
    )
    drawdown_governor["dd_threshold_hard"] = float(
        np.clip(finite_float(drawdown_governor.get("dd_threshold_hard", 0.10), 0.10), 0.0, 1.0)
    )
    if drawdown_governor["dd_threshold_hard"] < drawdown_governor["dd_threshold_soft"]:
        drawdown_governor["dd_threshold_soft"], drawdown_governor["dd_threshold_hard"] = (
            drawdown_governor["dd_threshold_hard"],
            drawdown_governor["dd_threshold_soft"],
        )
    drawdown_governor["budget_multiplier_soft"] = float(
        np.clip(
            finite_float(drawdown_governor.get("budget_multiplier_soft", 0.70), 0.70),
            0.0,
            1.0,
        )
    )
    drawdown_governor["budget_multiplier_hard"] = float(
        np.clip(
            finite_float(drawdown_governor.get("budget_multiplier_hard", 0.40), 0.40),
            0.0,
            1.0,
        )
    )
    if drawdown_governor["budget_multiplier_hard"] > drawdown_governor["budget_multiplier_soft"]:
        drawdown_governor["budget_multiplier_soft"], drawdown_governor["budget_multiplier_hard"] = (
            drawdown_governor["budget_multiplier_hard"],
            drawdown_governor["budget_multiplier_soft"],
        )
    execution_aware_sizing = merged["execution_aware_sizing"]
    execution_aware_sizing["enabled"] = bool(execution_aware_sizing.get("enabled", False))
    execution_aware_sizing["liquidity_low_threshold"] = float(
        np.clip(
            finite_float(execution_aware_sizing.get("liquidity_low_threshold", 40.0), 40.0),
            0.0,
            100.0,
        )
    )
    execution_aware_sizing["liquidity_mid_threshold"] = float(
        np.clip(
            finite_float(execution_aware_sizing.get("liquidity_mid_threshold", 65.0), 65.0),
            0.0,
            100.0,
        )
    )
    if execution_aware_sizing["liquidity_mid_threshold"] < execution_aware_sizing["liquidity_low_threshold"]:
        execution_aware_sizing["liquidity_low_threshold"], execution_aware_sizing["liquidity_mid_threshold"] = (
            execution_aware_sizing["liquidity_mid_threshold"],
            execution_aware_sizing["liquidity_low_threshold"],
        )
    execution_aware_sizing["liquidity_low_size_multiplier"] = float(
        np.clip(
            finite_float(
                execution_aware_sizing.get("liquidity_low_size_multiplier", 0.50),
                0.50,
            ),
            0.0,
            1.0,
        )
    )
    execution_aware_sizing["liquidity_mid_size_multiplier"] = float(
        np.clip(
            finite_float(
                execution_aware_sizing.get("liquidity_mid_size_multiplier", 0.75),
                0.75,
            ),
            0.0,
            1.0,
        )
    )
    execution_aware_sizing["liquidity_high_size_multiplier"] = float(
        np.clip(
            finite_float(
                execution_aware_sizing.get("liquidity_high_size_multiplier", 1.00),
                1.00,
            ),
            0.0,
            1.0,
        )
    )
    execution_aware_sizing["tail_cost_soft_pct"] = float(
        np.clip(
            finite_float(execution_aware_sizing.get("tail_cost_soft_pct", 0.0015), 0.0015),
            0.0,
            1.0,
        )
    )
    execution_aware_sizing["tail_cost_hard_pct"] = float(
        np.clip(
            finite_float(execution_aware_sizing.get("tail_cost_hard_pct", 0.0030), 0.0030),
            0.0,
            1.0,
        )
    )
    if execution_aware_sizing["tail_cost_hard_pct"] < execution_aware_sizing["tail_cost_soft_pct"]:
        execution_aware_sizing["tail_cost_soft_pct"], execution_aware_sizing["tail_cost_hard_pct"] = (
            execution_aware_sizing["tail_cost_hard_pct"],
            execution_aware_sizing["tail_cost_soft_pct"],
        )
    execution_aware_sizing["tail_soft_multiplier"] = float(
        np.clip(
            finite_float(execution_aware_sizing.get("tail_soft_multiplier", 0.80), 0.80),
            0.0,
            1.0,
        )
    )
    execution_aware_sizing["tail_hard_multiplier"] = float(
        np.clip(
            finite_float(execution_aware_sizing.get("tail_hard_multiplier", 0.60), 0.60),
            0.0,
            1.0,
        )
    )
    if execution_aware_sizing["tail_hard_multiplier"] > execution_aware_sizing["tail_soft_multiplier"]:
        execution_aware_sizing["tail_soft_multiplier"], execution_aware_sizing["tail_hard_multiplier"] = (
            execution_aware_sizing["tail_hard_multiplier"],
            execution_aware_sizing["tail_soft_multiplier"],
        )
    execution_aware_sizing["min_position_size"] = float(
        np.clip(
            finite_float(execution_aware_sizing.get("min_position_size", 0.01), 0.01),
            0.0,
            1.0,
        )
    )
    merged["portfolio_diagnostics"]["enabled"] = bool(
        merged["portfolio_diagnostics"].get("enabled", False)
    )
    return merged


def extract_linear(payload: Dict[str, Any]) -> Dict[str, Any]:
    model = payload.get("model")
    if model is None:
        raise RuntimeError("model missing in payload")
    coef = np.asarray(model.coef_, dtype=np.float64).reshape(-1).tolist()
    intercept = float(np.asarray(model.intercept_, dtype=np.float64).reshape(-1)[0])
    return {
        "coef": [float(x) for x in coef],
        "intercept": float(intercept),
    }


def finite_float(value: Any, default: float = 0.0) -> float:
    try:
        out = float(value)
    except Exception:
        return float(default)
    if not np.isfinite(out):
        return float(default)
    return float(out)


def extract_edge_regressor(payload: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    node = payload.get("edge_regressor")
    if not isinstance(node, dict):
        return None
    linear = node.get("linear")
    if not isinstance(linear, dict):
        return None
    coef_raw = linear.get("coef")
    if not isinstance(coef_raw, list) or not coef_raw:
        return None
    try:
        coef = [float(x) for x in coef_raw]
        intercept = float(linear.get("intercept", 0.0) or 0.0)
        clip_abs_bps = float(node.get("clip_abs_bps", 250.0) or 250.0)
    except Exception:
        return None
    if not np.isfinite(intercept):
        return None
    if not np.isfinite(clip_abs_bps):
        clip_abs_bps = 250.0
    clip_abs_bps = float(np.clip(clip_abs_bps, 10.0, 5000.0))
    return {
        "linear": {
            "coef": coef,
            "intercept": intercept,
        },
        "clip_abs_bps": clip_abs_bps,
    }


def pick_fold(folds: List[Dict[str, Any]], policy: str) -> Optional[Dict[str, Any]]:
    if not folds:
        return None
    if policy == "latest":
        return max(folds, key=lambda x: int(x.get("fold_id", 0) or 0))
    if policy == "best_h5_logloss":
        cand = []
        for f in folds:
            ll = (
                f.get("metrics", {})
                .get("h5", {})
                .get("test_calibrated", {})
                .get("logloss", None)
            )
            if ll is None:
                continue
            try:
                ll_f = float(ll)
            except Exception:
                continue
            cand.append((ll_f, f))
        if cand:
            cand.sort(key=lambda x: x[0])
            return cand[0][1]
    return max(folds, key=lambda x: int(x.get("fold_id", 0) or 0))


def make_runtime_entry(
    *,
    chosen: Dict[str, Any],
    h1_payload: Dict[str, Any],
    h5_payload: Dict[str, Any],
) -> Dict[str, Any]:
    h1_lin = extract_linear(h1_payload)
    h5_lin = extract_linear(h5_payload)
    h5_edge_regressor = extract_edge_regressor(h5_payload)

    h1_cal = chosen.get("calibration", {}).get("h1", {}) or {}
    h5_cal = chosen.get("calibration", {}).get("h5", {}) or {}
    h5_thr = (
        chosen.get("metrics", {})
        .get("h5", {})
        .get("threshold_selection", {})
        .get("threshold", 0.6)
    )
    h5_edge_profile = h5_payload.get("edge_profile", {}) if isinstance(h5_payload, dict) else {}
    if not isinstance(h5_edge_profile, dict):
        h5_edge_profile = {}
    return {
        "selected_fold_id": int(chosen.get("fold_id", 0) or 0),
        "h1_model": {
            "linear": h1_lin,
            "calibration": {
                "a": float(h1_cal.get("a", 1.0) or 1.0),
                "b": float(h1_cal.get("b", 0.0) or 0.0),
            },
        },
        "h5_model": {
            "linear": h5_lin,
            "calibration": {
                "a": float(h5_cal.get("a", 1.0) or 1.0),
                "b": float(h5_cal.get("b", 0.0) or 0.0),
            },
            "selection_threshold": float(h5_thr),
            "edge_regressor": h5_edge_regressor if h5_edge_regressor is not None else None,
            "edge_profile": {
                "sample_count": int(h5_edge_profile.get("sample_count", 0) or 0),
                "win_count": int(h5_edge_profile.get("win_count", 0) or 0),
                "loss_count": int(h5_edge_profile.get("loss_count", 0) or 0),
                "neutral_count": int(h5_edge_profile.get("neutral_count", 0) or 0),
                "win_mean_edge_bps": float(h5_edge_profile.get("win_mean_edge_bps", 0.0) or 0.0),
                "loss_mean_edge_bps": float(h5_edge_profile.get("loss_mean_edge_bps", 0.0) or 0.0),
                "neutral_mean_edge_bps": float(h5_edge_profile.get("neutral_mean_edge_bps", 0.0) or 0.0),
            },
        },
        "selected_fold_metrics": {
            "h5_test_calibrated_logloss": finite_float(
                chosen.get("metrics", {})
                .get("h5", {})
                .get("test_calibrated", {})
                .get("logloss", 0.0)
            ),
            "h5_test_calibrated_brier": finite_float(
                chosen.get("metrics", {})
                .get("h5", {})
                .get("test_calibrated", {})
                .get("brier", 0.0)
            ),
            "h5_test_trade_selected_coverage": finite_float(
                chosen.get("metrics", {})
                .get("h5", {})
                .get("test_trade_metrics", {})
                .get("coverage", 0.0)
            ),
            "h5_test_trade_selected_mean_edge_bps": finite_float(
                chosen.get("metrics", {})
                .get("h5", {})
                .get("test_trade_metrics", {})
                .get("mean_edge_bps", 0.0)
            ),
        },
    }


def build_global_default_model(
    summary: Dict[str, Any],
    fold_policy: str,
) -> Optional[Dict[str, Any]]:
    global_node = summary.get("global_model", {})
    if not isinstance(global_node, dict):
        return None
    folds = [f for f in (global_node.get("folds", []) or []) if isinstance(f, dict)]
    if not folds:
        return None
    chosen = pick_fold(folds, fold_policy)
    if chosen is None:
        return None
    h1_path = str(chosen.get("model_artifacts", {}).get("h1_model", "")).strip()
    h5_path = str(chosen.get("model_artifacts", {}).get("h5_model", "")).strip()
    if not h1_path or not h5_path:
        return None
    h1_payload = joblib.load(h1_path)
    h5_payload = joblib.load(h5_path)
    out = make_runtime_entry(chosen=chosen, h1_payload=h1_payload, h5_payload=h5_payload)
    out["source"] = "global_cross_market"
    return out


def build_global_ensemble_members(
    summary: Dict[str, Any],
    fold_policy: str,
) -> List[Dict[str, Any]]:
    ensemble_node = summary.get("ensemble", {})
    if not isinstance(ensemble_node, dict) or not bool(ensemble_node.get("enabled", False)):
        return []
    members_raw = [x for x in (ensemble_node.get("members", []) or []) if isinstance(x, dict)]
    out_members: List[Dict[str, Any]] = []
    for member in members_raw:
        global_model = member.get("global_model", {})
        if not isinstance(global_model, dict):
            continue
        folds = [f for f in (global_model.get("folds", []) or []) if isinstance(f, dict)]
        if not folds:
            continue
        chosen = pick_fold(folds, fold_policy)
        if chosen is None:
            continue
        h1_path = str(chosen.get("model_artifacts", {}).get("h1_model", "")).strip()
        h5_path = str(chosen.get("model_artifacts", {}).get("h5_model", "")).strip()
        if not h1_path or not h5_path:
            continue
        h1_payload = joblib.load(h1_path)
        h5_payload = joblib.load(h5_path)
        runtime_entry = make_runtime_entry(chosen=chosen, h1_payload=h1_payload, h5_payload=h5_payload)
        out_members.append(
            {
                "member_index": int(member.get("member_index", 0) or 0),
                "random_state": int(member.get("random_state", 0) or 0),
                "selected_fold_id": int(runtime_entry.get("selected_fold_id", 0) or 0),
                "model_artifacts": {
                    "h1_model": h1_path,
                    "h5_model": h5_path,
                },
                "h1_model": runtime_entry.get("h1_model", {}),
                "h5_model": runtime_entry.get("h5_model", {}),
                "selected_fold_metrics": runtime_entry.get("selected_fold_metrics", {}),
            }
        )
    return out_members


def main(argv=None) -> int:
    args = parse_args(argv)
    train_summary_path = resolve_repo_path(args.train_summary_json)
    output_path = resolve_repo_path(args.output_json)
    pipeline_version = str(args.pipeline_version).strip().lower()

    summary = load_json_or_none(train_summary_path)
    if not isinstance(summary, dict):
        raise RuntimeError(f"invalid training summary: {train_summary_path}")
    summary_pipeline_version = infer_train_summary_pipeline_version(summary)
    if summary_pipeline_version != pipeline_version:
        raise RuntimeError(
            "pipeline version mismatch: "
            f"export={pipeline_version} train_summary={summary_pipeline_version}"
        )

    feature_columns = list(summary.get("feature_columns", []) or [])
    if not feature_columns:
        raise RuntimeError("feature_columns missing in training summary")

    markets_out = []
    if str(args.export_mode) in ("hybrid", "per_market"):
        for ds in summary.get("datasets", []) or []:
            if not isinstance(ds, dict):
                continue
            market = str(ds.get("market", "")).strip()
            folds = [f for f in (ds.get("folds", []) or []) if isinstance(f, dict)]
            if not market or not folds:
                continue
            chosen = pick_fold(folds, str(args.fold_policy))
            if chosen is None:
                continue

            h1_path = str(chosen.get("model_artifacts", {}).get("h1_model", "")).strip()
            h5_path = str(chosen.get("model_artifacts", {}).get("h5_model", "")).strip()
            if not h1_path or not h5_path:
                continue
            h1_payload = joblib.load(h1_path)
            h5_payload = joblib.load(h5_path)

            row = make_runtime_entry(chosen=chosen, h1_payload=h1_payload, h5_payload=h5_payload)
            row["market"] = market
            markets_out.append(row)

    default_model = None
    ensemble_members: List[Dict[str, Any]] = []
    ensemble_summary_meta: Dict[str, Any] = {}
    if str(args.export_mode) in ("global_only", "hybrid"):
        default_model = build_global_default_model(summary, str(args.fold_policy))
        if default_model is None:
            raise RuntimeError(
                "export_mode requires global_model in train summary, but global fold artifacts were not found"
            )
        ensemble_members = build_global_ensemble_members(summary, str(args.fold_policy))
        ensemble_node = summary.get("ensemble", {})
        ensemble_enabled_in_summary = isinstance(ensemble_node, dict) and bool(ensemble_node.get("enabled", False))
        if ensemble_enabled_in_summary and not ensemble_members:
            raise RuntimeError("ensemble enabled in training summary, but no valid ensemble member artifacts were found")
        if ensemble_members:
            default_model["ensemble_members"] = ensemble_members
            default_model["ensemble_member_count"] = int(len(ensemble_members))
            default_model["ensemble_model_artifacts"] = [
                x.get("model_artifacts", {}) for x in ensemble_members
            ]
            ensemble_summary_meta = {
                "enabled": True,
                "ensemble_k": int(
                    (ensemble_node.get("ensemble_k", len(ensemble_members)) if isinstance(ensemble_node, dict) else len(ensemble_members))
                    or len(ensemble_members)
                ),
                "member_count": int(len(ensemble_members)),
                "seed_step": int(
                    (ensemble_node.get("seed_step", 0) if isinstance(ensemble_node, dict) else 0) or 0
                ),
            }

    if str(args.export_mode) == "per_market" and not markets_out:
        raise RuntimeError("per_market export requested, but no valid market fold artifacts found")
    if str(args.export_mode) in ("global_only", "hybrid") and default_model is None:
        raise RuntimeError("global export requested, but default_model could not be created")

    global_fallback_enabled = bool(default_model is not None)
    prefer_default_model = bool(default_model is not None)
    phase3_bundle = build_phase3_bundle_config(summary)
    phase4_bundle = build_phase4_bundle_config(summary)

    out = {
        "version": "probabilistic_runtime_bundle_v2_draft",
        "generated_at_utc": utc_now_iso(),
        "source_train_summary_json": str(train_summary_path),
        "selection_policy": str(args.fold_policy),
        "export_mode": str(args.export_mode),
        "feature_columns": feature_columns,
        "feature_transform_contract": {
            "rsi_center_scale": {"center": 50.0, "scale": 50.0},
            "age_min_cap": 240.0,
            "log_transform_columns": ["vol_ratio_20", "notional_ratio_20"],
            "percent_scale_rules": ["ret_*", "*_ret_*", "*atr_pct*", "*bb_width*", "*gap*"],
            "clip_abs": 8.0,
        },
        "global_fallback_enabled": bool(global_fallback_enabled),
        "prefer_default_model": bool(prefer_default_model),
        "default_model": default_model if default_model is not None else None,
        "markets": markets_out,
        "edge_semantics": "net",
        "cost_model": summary.get("cost_model", {}),
        "phase3": phase3_bundle,
        "phase4": phase4_bundle,
    }
    out["pipeline_version"] = str(pipeline_version)
    out["feature_contract_version"] = "v2_draft"
    out["runtime_bundle_contract_version"] = "v2_draft"
    if ensemble_summary_meta:
        out["ensemble"] = ensemble_summary_meta
    dump_json(output_path, out)

    print("[ExportRuntimeBundle] completed", flush=True)
    print(f"export_mode={args.export_mode}", flush=True)
    print(f"global_fallback_enabled={global_fallback_enabled}", flush=True)
    print(f"markets={len(markets_out)}", flush=True)
    print(f"pipeline_version={pipeline_version}", flush=True)
    print(f"output={output_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
