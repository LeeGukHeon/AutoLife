#!/usr/bin/env python3
import math
from typing import Dict


DEFAULT_COST_MODEL_CONFIG = {
    "enabled": False,
    "fee_floor_bps": 6.0,
    "volatility_weight": 3.0,
    "range_weight": 1.5,
    "liquidity_weight": 2.5,
    "volatility_norm_bps": 50.0,
    "range_norm_bps": 80.0,
    "liquidity_ref_ratio": 1.0,
    "liquidity_penalty_cap": 8.0,
    "cost_cap_bps": 200.0,
}


def _f(value: object, default: float) -> float:
    try:
        out = float(value)
    except Exception:
        out = float(default)
    if not math.isfinite(out):
        out = float(default)
    return float(out)


def normalize_cost_model_config(raw: Dict[str, object] | None) -> Dict[str, float | bool]:
    src = raw if isinstance(raw, dict) else {}
    enabled = bool(src.get("enabled", False))
    fee_floor_bps = max(0.0, _f(src.get("fee_floor_bps"), DEFAULT_COST_MODEL_CONFIG["fee_floor_bps"]))
    volatility_weight = max(0.0, _f(src.get("volatility_weight"), DEFAULT_COST_MODEL_CONFIG["volatility_weight"]))
    range_weight = max(0.0, _f(src.get("range_weight"), DEFAULT_COST_MODEL_CONFIG["range_weight"]))
    liquidity_weight = max(0.0, _f(src.get("liquidity_weight"), DEFAULT_COST_MODEL_CONFIG["liquidity_weight"]))
    volatility_norm_bps = max(1e-6, _f(src.get("volatility_norm_bps"), DEFAULT_COST_MODEL_CONFIG["volatility_norm_bps"]))
    range_norm_bps = max(1e-6, _f(src.get("range_norm_bps"), DEFAULT_COST_MODEL_CONFIG["range_norm_bps"]))
    liquidity_ref_ratio = max(1e-6, _f(src.get("liquidity_ref_ratio"), DEFAULT_COST_MODEL_CONFIG["liquidity_ref_ratio"]))
    liquidity_penalty_cap = max(
        1.0,
        _f(src.get("liquidity_penalty_cap"), DEFAULT_COST_MODEL_CONFIG["liquidity_penalty_cap"]),
    )
    cost_cap_bps = max(
        fee_floor_bps,
        _f(src.get("cost_cap_bps"), DEFAULT_COST_MODEL_CONFIG["cost_cap_bps"]),
    )
    return {
        "enabled": bool(enabled),
        "fee_floor_bps": float(fee_floor_bps),
        "volatility_weight": float(volatility_weight),
        "range_weight": float(range_weight),
        "liquidity_weight": float(liquidity_weight),
        "volatility_norm_bps": float(volatility_norm_bps),
        "range_norm_bps": float(range_norm_bps),
        "liquidity_ref_ratio": float(liquidity_ref_ratio),
        "liquidity_penalty_cap": float(liquidity_penalty_cap),
        "cost_cap_bps": float(cost_cap_bps),
    }


def estimate_conditional_cost_bps(
    *,
    atr_pct_14: float,
    bb_width_20: float,
    vol_ratio_20: float,
    notional_ratio_20: float,
    config: Dict[str, object] | None,
) -> float:
    cfg = normalize_cost_model_config(config)
    fee_floor_bps = float(cfg["fee_floor_bps"])
    atr_pct = max(0.0, _f(atr_pct_14, 0.0))
    range_pct = max(0.0, _f(bb_width_20, 0.0))
    vol_ratio = max(1e-9, _f(vol_ratio_20, 1.0))
    notional_ratio = max(1e-9, _f(notional_ratio_20, 1.0))

    atr_bps = atr_pct * 10000.0
    range_bps = range_pct * 10000.0
    vol_component = atr_bps / float(cfg["volatility_norm_bps"])
    range_component = range_bps / float(cfg["range_norm_bps"])
    liquidity_ratio = math.sqrt(vol_ratio * notional_ratio)
    illiquidity = min(float(cfg["liquidity_penalty_cap"]), float(cfg["liquidity_ref_ratio"]) / liquidity_ratio)

    cost_bps = (
        fee_floor_bps
        + (float(cfg["volatility_weight"]) * vol_component)
        + (float(cfg["range_weight"]) * range_component)
        + (float(cfg["liquidity_weight"]) * illiquidity)
    )
    return float(min(float(cfg["cost_cap_bps"]), max(fee_floor_bps, cost_bps)))


def resolve_label_cost_bps(
    *,
    roundtrip_cost_bps: float,
    cost_model_config: Dict[str, object] | None,
    atr_pct_14: float,
    bb_width_20: float,
    vol_ratio_20: float,
    notional_ratio_20: float,
) -> float:
    cfg = normalize_cost_model_config(cost_model_config)
    if not bool(cfg["enabled"]):
        return max(0.0, _f(roundtrip_cost_bps, 0.0))
    return estimate_conditional_cost_bps(
        atr_pct_14=atr_pct_14,
        bb_width_20=bb_width_20,
        vol_ratio_20=vol_ratio_20,
        notional_ratio_20=notional_ratio_20,
        config=cfg,
    )
