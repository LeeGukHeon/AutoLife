#!/usr/bin/env python3
import math
from typing import Iterable, List, Tuple


def compute_prob_mean_std(probs: Iterable[float]) -> Tuple[float, float]:
    values: List[float] = []
    for p in probs:
        try:
            x = float(p)
        except Exception:
            continue
        if not math.isfinite(x):
            continue
        values.append(min(1.0, max(0.0, x)))
    if not values:
        return 0.5, 0.0
    mean = sum(values) / float(len(values))
    if len(values) < 2:
        return mean, 0.0
    var = sum((x - mean) ** 2 for x in values) / float(len(values))
    return mean, math.sqrt(max(0.0, var))


def uncertainty_size_multiplier(
    uncertainty_std: float,
    *,
    mode: str = "linear",
    u_max: float = 0.06,
    exp_k: float = 8.0,
    min_scale: float = 0.10,
) -> float:
    u = max(0.0, float(uncertainty_std) if math.isfinite(float(uncertainty_std)) else 0.0)
    u_max_safe = max(1e-6, float(u_max) if math.isfinite(float(u_max)) else 0.06)
    min_scale_safe = min(1.0, max(0.01, float(min_scale) if math.isfinite(float(min_scale)) else 0.10))
    m = str(mode or "linear").strip().lower()
    if m == "exp":
        k = max(0.0, float(exp_k) if math.isfinite(float(exp_k)) else 8.0)
        scale = math.exp(-k * u)
    else:
        scale = 1.0 - (u / u_max_safe)
    return min(1.0, max(min_scale_safe, scale))
