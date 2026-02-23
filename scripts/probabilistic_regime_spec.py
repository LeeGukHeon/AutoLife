#!/usr/bin/env python3
import math
from typing import Dict, List, Sequence


DEFAULT_REGIME_SPEC_CONFIG = {
    "enabled": False,
    "volatility_window": 48,
    "drawdown_window": 36,
    "volatile_zscore": 1.2,
    "hostile_zscore": 2.0,
    "volatile_drawdown_speed_bps": 3.0,
    "hostile_drawdown_speed_bps": 8.0,
    "enable_btc_correlation_shock": False,
    "correlation_window": 48,
    "correlation_shock_threshold": 1.2,
    "hostile_block_new_entries": True,
    "volatile_threshold_add": 0.010,
    "hostile_threshold_add": 0.030,
    "volatile_size_multiplier": 0.50,
    "hostile_size_multiplier": 0.20,
}


def _f(value: object, default: float) -> float:
    try:
        out = float(value)
    except Exception:
        out = float(default)
    if not math.isfinite(out):
        out = float(default)
    return float(out)


def _i(value: object, default: int, lo: int, hi: int) -> int:
    try:
        out = int(value)
    except Exception:
        out = int(default)
    return int(max(lo, min(hi, out)))


def normalize_regime_spec_config(raw: Dict[str, object] | None) -> Dict[str, object]:
    src = raw if isinstance(raw, dict) else {}
    volatile_z = max(0.0, _f(src.get("volatile_zscore"), DEFAULT_REGIME_SPEC_CONFIG["volatile_zscore"]))
    volatile_dd = max(
        0.0,
        _f(
            src.get("volatile_drawdown_speed_bps"),
            DEFAULT_REGIME_SPEC_CONFIG["volatile_drawdown_speed_bps"],
        ),
    )
    volatile_add = max(
        0.0,
        _f(src.get("volatile_threshold_add"), DEFAULT_REGIME_SPEC_CONFIG["volatile_threshold_add"]),
    )
    volatile_size = max(
        0.05,
        min(1.0, _f(src.get("volatile_size_multiplier"), DEFAULT_REGIME_SPEC_CONFIG["volatile_size_multiplier"])),
    )
    return {
        "enabled": bool(src.get("enabled", False)),
        "volatility_window": _i(src.get("volatility_window"), 48, 10, 720),
        "drawdown_window": _i(src.get("drawdown_window"), 36, 5, 720),
        "volatile_zscore": volatile_z,
        "hostile_zscore": max(volatile_z, _f(src.get("hostile_zscore"), 2.0)),
        "volatile_drawdown_speed_bps": volatile_dd,
        "hostile_drawdown_speed_bps": max(
            volatile_dd,
            _f(src.get("hostile_drawdown_speed_bps"), 8.0),
        ),
        "enable_btc_correlation_shock": bool(src.get("enable_btc_correlation_shock", False)),
        "correlation_window": _i(src.get("correlation_window"), 48, 10, 720),
        "correlation_shock_threshold": max(
            0.0,
            min(2.0, _f(src.get("correlation_shock_threshold"), 1.2)),
        ),
        "hostile_block_new_entries": bool(src.get("hostile_block_new_entries", True)),
        "volatile_threshold_add": volatile_add,
        "hostile_threshold_add": max(
            volatile_add,
            _f(src.get("hostile_threshold_add"), 0.030),
        ),
        "volatile_size_multiplier": volatile_size,
        "hostile_size_multiplier": max(
            0.01,
            min(volatile_size, _f(src.get("hostile_size_multiplier"), 0.20)),
        ),
    }


def _returns(closes: Sequence[float], window: int) -> List[float]:
    if len(closes) < 2:
        return []
    n = min(len(closes) - 1, int(window))
    start = len(closes) - n - 1
    out: List[float] = []
    for i in range(start + 1, len(closes)):
        p0 = _f(closes[i - 1], 0.0)
        p1 = _f(closes[i], 0.0)
        if p0 <= 0.0 or p1 <= 0.0:
            continue
        out.append((p1 / p0) - 1.0)
    return out


def _pearson(x: Sequence[float], y: Sequence[float]) -> float:
    if len(x) != len(y) or len(x) < 3:
        return math.nan
    mx = sum(x) / float(len(x))
    my = sum(y) / float(len(y))
    cov = 0.0
    vx = 0.0
    vy = 0.0
    for xi, yi in zip(x, y):
        dx = xi - mx
        dy = yi - my
        cov += dx * dy
        vx += dx * dx
        vy += dy * dy
    if vx <= 1e-12 or vy <= 1e-12:
        return math.nan
    corr = cov / math.sqrt(vx * vy)
    return max(-1.0, min(1.0, corr))


def analyze_regime_state(
    closes: Sequence[float],
    config: Dict[str, object] | None,
    btc_closes: Sequence[float] | None = None,
) -> Dict[str, object]:
    cfg = normalize_regime_spec_config(config)
    out = {
        "enabled": bool(cfg["enabled"]),
        "state": "normal",
        "sufficient_data": False,
        "volatility_zscore": 0.0,
        "drawdown_speed_bps": 0.0,
        "btc_correlation": 0.0,
        "correlation_shock": 0.0,
        "correlation_shock_triggered": False,
    }
    if not bool(cfg["enabled"]):
        return out

    rets = _returns(closes, int(cfg["volatility_window"]))
    if len(rets) >= 4:
        abs_rets = [abs(x) for x in rets]
        latest = abs_rets[-1]
        hist = abs_rets[:-1]
        if len(hist) >= 2:
            mean = sum(hist) / float(len(hist))
            var = sum((x - mean) ** 2 for x in hist) / float(len(hist))
            std = math.sqrt(max(0.0, var))
            if std > 1e-12:
                out["volatility_zscore"] = (latest - mean) / std
                out["sufficient_data"] = True

    if len(closes) >= 2:
        dd_window = min(int(cfg["drawdown_window"]), len(closes) - 1)
        if dd_window > 0:
            start = len(closes) - dd_window - 1
            window_closes = [max(0.0, _f(x, 0.0)) for x in closes[start:]]
            peak = max(window_closes) if window_closes else 0.0
            cur = max(0.0, _f(closes[-1], 0.0))
            if peak > 0.0 and cur > 0.0 and cur < peak:
                dd_bps = ((peak - cur) / peak) * 10000.0
                out["drawdown_speed_bps"] = dd_bps / float(dd_window)
            out["sufficient_data"] = True

    if bool(cfg["enable_btc_correlation_shock"]) and btc_closes is not None:
        m_ret = _returns(closes, int(cfg["correlation_window"]))
        b_ret = _returns(btc_closes, int(cfg["correlation_window"]))
        n = min(len(m_ret), len(b_ret))
        if n >= 6:
            corr = _pearson(m_ret[-n:], b_ret[-n:])
            if math.isfinite(corr):
                out["btc_correlation"] = float(corr)
                shock = max(0.0, 1.0 - corr)
                out["correlation_shock"] = float(shock)
                out["correlation_shock_triggered"] = bool(
                    shock >= float(cfg["correlation_shock_threshold"])
                )
                out["sufficient_data"] = True

    hostile = (
        float(out["volatility_zscore"]) >= float(cfg["hostile_zscore"])
        or float(out["drawdown_speed_bps"]) >= float(cfg["hostile_drawdown_speed_bps"])
        or bool(out["correlation_shock_triggered"])
    )
    volatile = (
        float(out["volatility_zscore"]) >= float(cfg["volatile_zscore"])
        or float(out["drawdown_speed_bps"]) >= float(cfg["volatile_drawdown_speed_bps"])
    )
    if hostile:
        out["state"] = "hostile"
    elif volatile:
        out["state"] = "volatile"
    else:
        out["state"] = "normal"
    return out


def regime_threshold_add(config: Dict[str, object] | None, state: str) -> float:
    cfg = normalize_regime_spec_config(config)
    if not bool(cfg["enabled"]):
        return 0.0
    if state == "hostile":
        return float(cfg["hostile_threshold_add"])
    if state == "volatile":
        return float(cfg["volatile_threshold_add"])
    return 0.0


def regime_size_multiplier(config: Dict[str, object] | None, state: str) -> float:
    cfg = normalize_regime_spec_config(config)
    if not bool(cfg["enabled"]):
        return 1.0
    if state == "hostile":
        return float(cfg["hostile_size_multiplier"])
    if state == "volatile":
        return float(cfg["volatile_size_multiplier"])
    return 1.0


def regime_blocks_entries(config: Dict[str, object] | None, state: str) -> bool:
    cfg = normalize_regime_spec_config(config)
    return bool(cfg["enabled"] and cfg["hostile_block_new_entries"] and state == "hostile")
