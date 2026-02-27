#!/usr/bin/env python3
import argparse
import copy
import json
import math
import pathlib
import sys
from datetime import datetime, timezone
from typing import Any, Dict, List

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def now_epoch() -> int:
    return int(datetime.now(tz=timezone.utc).timestamp())


def f64(v: Any, d: float = 0.0) -> float:
    try:
        x = float(v)
    except Exception:
        return float(d)
    return float(d) if not math.isfinite(x) else x


def i64(v: Any, d: int = 0) -> int:
    try:
        return int(float(v))
    except Exception:
        return int(d)


def ng(payload: Any, path: List[str], default: Any) -> Any:
    cur = payload
    for key in path:
        if not isinstance(cur, dict):
            return default
        cur = cur.get(key)
    return default if cur is None else cur


def clamp(value: float, low: float, high: float) -> float:
    return max(float(low), min(float(high), float(value)))


def safe_log10(value: float) -> float:
    return math.log10(max(1.0, float(value)))


def quantile(values: List[float], q: float) -> float:
    clean = sorted(float(x) for x in values if math.isfinite(float(x)))
    if not clean:
        return 0.0
    qv = clamp(float(q), 0.0, 1.0)
    if len(clean) == 1:
        return clean[0]
    pos = qv * float(len(clean) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return clean[lo]
    weight = pos - float(lo)
    return clean[lo] + (clean[hi] - clean[lo]) * weight


def as_path(path_value: Any) -> List[str]:
    if isinstance(path_value, list):
        return [str(x).strip() for x in path_value if str(x).strip()]
    token = str(path_value).strip()
    if not token:
        return []
    return [x.strip() for x in token.split(".") if x.strip()]


def pick_eval_payload(report: Dict[str, Any], prefer_core_direct: bool) -> Dict[str, Any]:
    if not isinstance(report, dict):
        return {}
    if prefer_core_direct:
        core = report.get("core_window_direct", {})
        if isinstance(core, dict) and bool(core.get("available", False)):
            return core
    return report


def dominant_label(rows: Any, default: str = "ANY") -> str:
    if not isinstance(rows, list):
        return default
    best_label = str(default)
    best_count = -1
    for row in rows:
        if not isinstance(row, dict):
            continue
        label = str(row.get("label", "")).strip()
        if not label:
            continue
        count = max(0, i64(row.get("candidate_total", 0), 0))
        if count > best_count or (count == best_count and label < best_label):
            best_label = label
            best_count = count
    return best_label if best_count >= 0 else default


def hostile_share(rows: Any, hostile_labels: List[str]) -> float:
    if not isinstance(rows, list):
        return 0.0
    hostile = {str(x).strip().upper() for x in hostile_labels if str(x).strip()}
    if not hostile:
        return 0.0
    total = 0
    hit = 0
    for row in rows:
        if not isinstance(row, dict):
            continue
        label = str(row.get("label", "")).strip().upper()
        count = max(0, i64(row.get("candidate_total", 0), 0))
        total += count
        if label in hostile:
            hit += count
    if total <= 0:
        return 0.0
    return float(hit) / float(total)


def match_bucket(value: str, pattern: str) -> bool:
    token = str(pattern).strip().upper()
    if token in ("", "ANY", "*"):
        return True
    return str(value).strip().upper() == token


def lookup_limit(
    rows: Any,
    *,
    regime: str,
    volatility_bucket: str,
    liquidity_bucket: str,
    default_limit: float,
) -> float:
    if not isinstance(rows, list):
        return float(default_limit)
    best_score = -1
    best_limits: List[float] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        rg = str(row.get("regime", "ANY")).strip()
        vb = str(row.get("volatility_bucket", row.get("vol_bucket", "ANY"))).strip()
        lb = str(row.get("liquidity_bucket", row.get("liq_bucket", "ANY"))).strip()
        if not match_bucket(regime, rg):
            continue
        if not match_bucket(volatility_bucket, vb):
            continue
        if not match_bucket(liquidity_bucket, lb):
            continue
        score = 0
        if str(rg).strip().upper() not in ("", "ANY", "*"):
            score += 1
        if str(vb).strip().upper() not in ("", "ANY", "*"):
            score += 1
        if str(lb).strip().upper() not in ("", "ANY", "*"):
            score += 1
        limit_value = f64(row.get("limit", default_limit), default_limit)
        if score > best_score:
            best_score = score
            best_limits = [float(limit_value)]
        elif score == best_score:
            best_limits.append(float(limit_value))
    if not best_limits:
        return float(default_limit)
    return float(min(best_limits))


def combine_limits(lookup_value: float, dist_value: float, mode: str) -> float:
    token = str(mode).strip().lower()
    if token == "max":
        return float(max(lookup_value, dist_value))
    if token == "mean":
        return float((lookup_value + dist_value) * 0.5)
    return float(min(lookup_value, dist_value))


def phase3_funnel(payload: Dict[str, Any]) -> Dict[str, int]:
    node = ng(payload, ["diagnostics", "aggregate", "phase3_diagnostics_v2", "funnel_breakdown"], {})
    if not isinstance(node, dict):
        node = {}
    return {
        "candidate_total": max(0, i64(node.get("candidate_total", 0), 0)),
        "pass_total": max(0, i64(node.get("pass_total", 0), 0)),
        "reject_margin_insufficient": max(0, i64(node.get("reject_margin_insufficient", 0), 0)),
        "reject_strength_fail": max(0, i64(node.get("reject_strength_fail", 0), 0)),
        "reject_expected_value_fail": max(0, i64(node.get("reject_expected_value_fail", 0), 0)),
        "reject_frontier_fail": max(0, i64(node.get("reject_frontier_fail", 0), 0)),
        "reject_ev_confidence_low": max(0, i64(node.get("reject_ev_confidence_low", 0), 0)),
        "reject_cost_tail_fail": max(0, i64(node.get("reject_cost_tail_fail", 0), 0)),
    }


def phase4_selection_breakdown(payload: Dict[str, Any]) -> Dict[str, float]:
    node = ng(payload, ["diagnostics", "aggregate", "phase4_portfolio_diagnostics", "selection_breakdown"], {})
    if not isinstance(node, dict):
        node = {}
    candidates_total = max(0, i64(node.get("candidates_total", 0), 0))
    selected_total = max(0, i64(node.get("selected_total", 0), 0))
    return {
        "candidates_total": float(candidates_total),
        "selected_total": float(selected_total),
        "selection_rate": float(selected_total) / float(max(1, candidates_total)),
        "rejected_by_budget": float(max(0, i64(node.get("rejected_by_budget", 0), 0))),
        "rejected_by_cluster_cap": float(max(0, i64(node.get("rejected_by_cluster_cap", 0), 0))),
        "rejected_by_correlation_penalty": float(max(0, i64(node.get("rejected_by_correlation_penalty", 0), 0))),
        "rejected_by_execution_cap": float(max(0, i64(node.get("rejected_by_execution_cap", 0), 0))),
        "rejected_by_drawdown_governor": float(max(0, i64(node.get("rejected_by_drawdown_governor", 0), 0))),
    }


def phase4_corr_applied_telemetry(payload: Dict[str, Any]) -> Dict[str, Any]:
    node = ng(
        payload,
        ["diagnostics", "aggregate", "phase4_portfolio_diagnostics", "correlation_applied_telemetry"],
        {},
    )
    if not isinstance(node, dict):
        node = {}
    cluster_rows = node.get("cluster_exposure_current", [])
    if not isinstance(cluster_rows, list):
        cluster_rows = []
    near_cap_rows = node.get("near_cap_candidates", [])
    if not isinstance(near_cap_rows, list):
        near_cap_rows = []
    score_rows = node.get("penalty_score_samples", [])
    if not isinstance(score_rows, list):
        score_rows = []
    stage = str(node.get("constraint_apply_stage", "")).strip()
    stage_norm = stage.lower()
    if "score" in stage_norm:
        stage_bucket = "candidate_selection_pre_score"
    elif "post" in stage_norm and "clamp" in stage_norm:
        stage_bucket = "post_selection_clamp"
    elif stage_norm:
        stage_bucket = "allocator_loop"
    else:
        stage_bucket = "unknown"
    return {
        "constraint_apply_stage": stage,
        "constraint_apply_stage_bucket": stage_bucket,
        "constraint_unit": str(node.get("constraint_unit", "")).strip(),
        "cluster_cap_checks_total": max(0, i64(node.get("cluster_cap_checks_total", 0), 0)),
        "cluster_cap_near_cap_count": max(0, i64(node.get("cluster_cap_near_cap_count", 0), 0)),
        "corr_penalty_applied_count": max(0, i64(node.get("corr_penalty_applied_count", 0), 0)),
        "corr_penalty_avg": round(f64(node.get("corr_penalty_avg", 0.0), 0.0), 8),
        "corr_penalty_max": round(f64(node.get("corr_penalty_max", 0.0), 0.0), 8),
        "cluster_exposure_current": cluster_rows,
        "near_cap_candidates": near_cap_rows[:8],
        "penalty_score_samples": score_rows[:8],
        "binding_detected": bool(node.get("binding_detected", False)),
    }


def phase3_context(payload: Dict[str, Any], hostile_labels: List[str]) -> Dict[str, Any]:
    diag = ng(payload, ["diagnostics", "aggregate", "phase3_diagnostics_v2"], {})
    if not isinstance(diag, dict):
        diag = {}
    regime_rows = diag.get("pass_rate_by_regime", [])
    vol_rows = diag.get("pass_rate_by_vol_bucket", [])
    liq_rows = diag.get("pass_rate_by_liquidity_bucket", [])
    return {
        "regime": dominant_label(regime_rows, "ANY"),
        "volatility_bucket": dominant_label(vol_rows, "ANY"),
        "liquidity_bucket": dominant_label(liq_rows, "ANY"),
        "hostile_share": hostile_share(regime_rows, hostile_labels),
    }


def extract_phase3_pass_ev_distribution(
    report: Dict[str, Any],
    *,
    prefer_core_direct: bool,
) -> Dict[str, Any]:
    payload = pick_eval_payload(report, prefer_core_direct)
    dist = ng(payload, ["diagnostics", "aggregate", "phase3_pass_ev_distribution"], {})
    if not isinstance(dist, dict):
        dist = {}
    samples_bps: List[float] = []
    raw_samples = dist.get("samples_bps", [])
    if isinstance(raw_samples, list):
        for x in raw_samples:
            v = f64(x, float("nan"))
            if math.isfinite(v):
                samples_bps.append(float(v))
    sample_size = max(0, i64(dist.get("sample_size_total", len(samples_bps)), len(samples_bps)))
    return {
        "enabled": bool(dist.get("enabled", False)) and sample_size > 0,
        "source": str(dist.get("source", "")).strip(),
        "sample_size_total": int(sample_size),
        "selected_sample_size_total": max(
            0,
            i64(dist.get("selected_sample_size_total", 0), 0),
        ),
        "samples_bps": samples_bps,
        "quantiles_bps": dist.get("quantiles_bps", {}),
        "quantiles_pct": dist.get("quantiles_pct", {}),
    }


def quantile_summary(values: List[float]) -> Dict[str, float]:
    clean = [float(x) for x in values if math.isfinite(float(x))]
    if not clean:
        return {
            "p10": 0.0,
            "p25": 0.0,
            "p50": 0.0,
            "p75": 0.0,
            "p90": 0.0,
        }
    return {
        "p10": float(quantile(clean, 0.10)),
        "p25": float(quantile(clean, 0.25)),
        "p50": float(quantile(clean, 0.50)),
        "p75": float(quantile(clean, 0.75)),
        "p90": float(quantile(clean, 0.90)),
    }


def metric_value(payload: Dict[str, Any], metric_name: str, metric_policy: Dict[str, Any]) -> float:
    source = str(metric_policy.get("source", metric_name)).strip().lower()
    if source in ("tail_concentration", "top3_loss_concentration"):
        return f64(
            ng(
                payload,
                ["adaptive_validation", "aggregates", "loss_tail_aggregate", "avg_top3_loss_concentration"],
                0.0,
            ),
            0.0,
        )
    if source in ("tail_exceed_rate", "tail_exceed_rate_proxy", "cost_tail_fail_rate"):
        funnel = phase3_funnel(payload)
        candidate_total = max(1, int(funnel["candidate_total"]))
        return float(funnel["reject_cost_tail_fail"]) / float(candidate_total)
    if source in ("avg_fee_krw", "fee_proxy"):
        return f64(ng(payload, ["aggregates", "avg_fee_krw"], 0.0), 0.0)
    if source == "path":
        p = as_path(metric_policy.get("path", []))
        return f64(ng(payload, p, 0.0), 0.0) if p else 0.0
    p = as_path(source)
    if p:
        return f64(ng(payload, p, 0.0), 0.0)
    return f64(metric_policy.get("fallback_value", 0.0), 0.0)


def metric_confidence(
    payload: Dict[str, Any],
    metric_name: str,
    metric_policy: Dict[str, Any],
) -> Dict[str, float]:
    source = str(metric_policy.get("source", metric_name)).strip().lower()
    loss_tail = ng(payload, ["adaptive_validation", "aggregates", "loss_tail_aggregate"], {})
    if not isinstance(loss_tail, dict):
        loss_tail = {}
    adaptive = ng(payload, ["adaptive_validation", "aggregates"], {})
    if not isinstance(adaptive, dict):
        adaptive = {}

    if source in ("tail_concentration", "top3_loss_concentration"):
        sample_size = max(
            0,
            i64(
                loss_tail.get(
                    "tail_metric_sample_size",
                    adaptive.get("dataset_count", 0),
                ),
                0,
            ),
        )
        effective_count = max(
            0.0,
            f64(
                loss_tail.get(
                    "tail_metric_effective_trades_count",
                    adaptive.get("avg_entries_executed", 0.0),
                ),
                0.0,
            ),
        )
        ess = max(
            0.0,
            f64(
                loss_tail.get(
                    "tail_metric_ess",
                    effective_count if effective_count > 0.0 else float(sample_size),
                ),
                0.0,
            ),
        )
        return {
            "sample_size": float(sample_size),
            "effective_count": float(effective_count),
            "ess": float(ess),
        }

    if source in ("tail_exceed_rate", "tail_exceed_rate_proxy", "cost_tail_fail_rate"):
        funnel = phase3_funnel(payload)
        candidate_total = max(0, int(funnel["candidate_total"]))
        return {
            "sample_size": float(candidate_total),
            "effective_count": float(candidate_total),
            "ess": float(candidate_total),
        }

    funnel = phase3_funnel(payload)
    candidate_total = max(0, int(funnel["candidate_total"]))
    return {
        "sample_size": float(candidate_total),
        "effective_count": float(candidate_total),
        "ess": float(candidate_total),
    }


def evaluate_dynamic_metric(
    *,
    metric_name: str,
    metric_policy: Dict[str, Any],
    context: Dict[str, Any],
    observed_value: float,
    history_values: List[float],
    observed_confidence: Dict[str, float],
    history_confidences: List[Dict[str, float]],
    global_combine_mode: str,
) -> Dict[str, Any]:
    default_limit = f64(metric_policy.get("lookup_limit_default", 1.0), 1.0)
    lookup_rows = metric_policy.get("lookup_limits", [])
    lookup = lookup_limit(
        lookup_rows,
        regime=str(context.get("regime", "ANY")),
        volatility_bucket=str(context.get("volatility_bucket", "ANY")),
        liquidity_bucket=str(context.get("liquidity_bucket", "ANY")),
        default_limit=default_limit,
    )
    min_samples = max(1, i64(metric_policy.get("dist_min_samples", 2), 2))
    qp = clamp(f64(metric_policy.get("dist_quantile_p", 0.8), 0.8), 0.0, 1.0)
    clean_history = [f64(x, 0.0) for x in history_values if math.isfinite(f64(x, 0.0))]
    dist = float(lookup)
    dist_available = len(clean_history) >= min_samples
    if dist_available:
        dist = quantile(clean_history, qp)
    combine_mode = str(metric_policy.get("combine_mode", global_combine_mode)).strip().lower() or str(global_combine_mode).strip().lower()
    final_limit = combine_limits(float(lookup), float(dist), combine_mode)
    excess = max(0.0, float(observed_value) - float(final_limit))
    risk_scale = max(1.0e-9, f64(metric_policy.get("risk_scale", max(abs(final_limit), 1.0e-6)), max(abs(final_limit), 1.0e-6)))
    risk_raw = excess / risk_scale
    risk_score = clamp(
        risk_raw,
        f64(metric_policy.get("risk_score_min", 0.0), 0.0),
        f64(metric_policy.get("risk_score_max", 1.0), 1.0),
    )
    coupling_k = max(0.0, f64(metric_policy.get("coupling_k", 0.0), 0.0))
    delta_required_ev_base = coupling_k * risk_score
    delta_required_ev = float(delta_required_ev_base)
    absorb_enabled = bool(metric_policy.get("coupling_absorb_enabled", False))
    absorb_max_excess = max(0.0, f64(metric_policy.get("coupling_absorb_max_excess", 0.0), 0.0))
    exceeded = observed_value > final_limit
    status = "pass"
    reason = f"{metric_name}_within_dynamic_limit"
    hard_fail = False
    soft_fail = False
    promotion_hold = False
    hard_fail_allowed = True
    coupling_soft_extra = 0.0

    confidence_policy = metric_policy.get("confidence", {})
    if not isinstance(confidence_policy, dict):
        confidence_policy = {}
    confidence_enabled = bool(confidence_policy.get("enabled", False))
    confidence_metric = str(confidence_policy.get("metric", "ess")).strip().lower() or "ess"

    def _confidence_value(row: Dict[str, float]) -> float:
        if not isinstance(row, dict):
            return 0.0
        if confidence_metric in ("ess", "effective_sample_size"):
            return max(0.0, f64(row.get("ess", 0.0), 0.0))
        if confidence_metric in ("effective_count", "effective_trades_count"):
            return max(0.0, f64(row.get("effective_count", 0.0), 0.0))
        return max(0.0, f64(row.get("sample_size", 0.0), 0.0))

    confidence_observed = _confidence_value(observed_confidence)
    confidence_history_values = [
        _confidence_value(x)
        for x in history_confidences
        if isinstance(x, dict)
    ]
    confidence_lookup = 0.0
    confidence_dist = 0.0
    confidence_dist_available = False
    confidence_qp = clamp(
        f64(confidence_policy.get("dist_quantile_p", 0.2), 0.2),
        0.0,
        1.0,
    )
    confidence_combine_mode = str(
        confidence_policy.get("combine_mode", "max")
    ).strip().lower() or "max"
    confidence_limit = 0.0
    confidence_sufficient = True
    confidence_low_action = "none"
    if confidence_enabled:
        confidence_lookup_default = max(
            0.0,
            f64(confidence_policy.get("lookup_limit_default", 0.0), 0.0),
        )
        confidence_lookup = lookup_limit(
            confidence_policy.get("lookup_limits", []),
            regime=str(context.get("regime", "ANY")),
            volatility_bucket=str(context.get("volatility_bucket", "ANY")),
            liquidity_bucket=str(context.get("liquidity_bucket", "ANY")),
            default_limit=confidence_lookup_default,
        )
        confidence_dist = float(confidence_lookup)
        confidence_min_samples = max(
            1,
            i64(confidence_policy.get("dist_min_samples", 2), 2),
        )
        clean_conf_history = [
            f64(x, 0.0)
            for x in confidence_history_values
            if math.isfinite(f64(x, 0.0))
        ]
        confidence_dist_available = len(clean_conf_history) >= confidence_min_samples
        if confidence_dist_available:
            confidence_dist = quantile(clean_conf_history, confidence_qp)
        confidence_limit = combine_limits(
            float(confidence_lookup),
            float(confidence_dist),
            confidence_combine_mode,
        )
        confidence_sufficient = confidence_observed >= confidence_limit

    if exceeded:
        status = "fail"
        reason = f"{metric_name}_limit_exceeded"
        hard_fail = True
        if confidence_enabled and not confidence_sufficient:
            hard_fail = False
            hard_fail_allowed = False
            soft_fail = True
            confidence_low_action = str(
                confidence_policy.get("low_conf_action", "coupling_only")
            ).strip().lower() or "coupling_only"
            if confidence_low_action not in ("coupling_only", "hold"):
                confidence_low_action = "coupling_only"
            coupling_multiplier = max(
                1.0,
                f64(confidence_policy.get("low_conf_coupling_multiplier", 1.0), 1.0),
            )
            coupling_floor = max(
                0.0,
                f64(confidence_policy.get("low_conf_coupling_floor", 0.0), 0.0),
            )
            delta_required_ev = max(
                float(delta_required_ev_base) * coupling_multiplier,
                coupling_floor,
            )
            coupling_soft_extra = max(
                0.0,
                float(delta_required_ev) - float(delta_required_ev_base),
            )
            if confidence_low_action == "hold":
                promotion_hold = True
                status = "soft_hold"
                reason = f"{metric_name}_limit_exceeded_low_confidence_promotion_hold"
            else:
                status = "soft_fail_coupling"
                reason = f"{metric_name}_limit_exceeded_low_confidence_soft_fail"
        elif absorb_enabled and excess <= absorb_max_excess:
            hard_fail = False
            status = "pass_with_coupling"
            reason = f"{metric_name}_absorbed_by_frontier_coupling"
    return {
        "metric": metric_name,
        "value": float(observed_value),
        "limit_lookup": float(lookup),
        "limit_dist": float(dist),
        "limit_dist_quantile_p": float(qp),
        "limit_dist_available": bool(dist_available),
        "limit_combine_mode": combine_mode,
        "dynamic_limit": float(final_limit),
        "limit_final": float(final_limit),
        "excess_over_limit": float(excess),
        "coupling_k": float(coupling_k),
        "risk_score": float(risk_score),
        "delta_required_ev": float(delta_required_ev),
        "delta_required_ev_base": float(delta_required_ev_base),
        "delta_required_ev_soft_extra": float(coupling_soft_extra),
        "hard_fail": bool(hard_fail),
        "soft_fail": bool(soft_fail),
        "promotion_hold": bool(promotion_hold),
        "status": status,
        "reason": reason,
        "metric_sample_size": float(f64(observed_confidence.get("sample_size", 0.0), 0.0)),
        "metric_effective_count": float(
            f64(observed_confidence.get("effective_count", 0.0), 0.0)
        ),
        "metric_ess": float(f64(observed_confidence.get("ess", 0.0), 0.0)),
        "metric_ess_limit": float(confidence_limit) if confidence_metric == "ess" else 0.0,
        "confidence": {
            "enabled": bool(confidence_enabled),
            "metric": confidence_metric,
            "sample_size": float(f64(observed_confidence.get("sample_size", 0.0), 0.0)),
            "effective_count": float(
                f64(observed_confidence.get("effective_count", 0.0), 0.0)
            ),
            "ess": float(f64(observed_confidence.get("ess", 0.0), 0.0)),
            "observed": float(confidence_observed),
            "history_values": [float(x) for x in confidence_history_values],
            "limit_lookup": float(confidence_lookup),
            "limit_dist": float(confidence_dist),
            "limit_dist_quantile_p": float(confidence_qp),
            "limit_dist_available": bool(confidence_dist_available),
            "limit_combine_mode": confidence_combine_mode,
            "limit_final": float(confidence_limit),
            "sufficient": bool(confidence_sufficient),
            "hard_fail_allowed": bool(hard_fail_allowed),
            "low_conf_action": confidence_low_action,
        },
    }


def compute_dynamic_band(
    *,
    band_policy: Dict[str, Any],
    candidate_total: int,
    market_count: int,
    hostile_share_value: float,
    dominant_regime: str,
    history_values: List[float],
    default_lower_combine: str,
    default_upper_combine: str,
    fallback_mode: bool = False,
) -> Dict[str, Any]:
    cand_log = safe_log10(float(max(1, int(candidate_total))))
    mkt_log = safe_log10(float(max(1, int(market_count))))
    regime_adds = band_policy.get("regime_additions", {})
    regime_add = 0.0
    if isinstance(regime_adds, dict):
        regime_add = f64(
            regime_adds.get(
                str(dominant_regime).strip(),
                regime_adds.get(str(dominant_regime).strip().upper(), 0.0),
            ),
            0.0,
        )
    lower_lookup = (
        f64(band_policy.get("lower_base", 0.0), 0.0)
        + f64(band_policy.get("lower_candidate_log10_scale", 0.0), 0.0) * cand_log
        + f64(band_policy.get("lower_market_log10_scale", 0.0), 0.0) * mkt_log
        + f64(band_policy.get("lower_hostile_share_scale", 0.0), 0.0) * float(hostile_share_value)
        + regime_add
    )
    upper_lookup = (
        f64(band_policy.get("upper_base", 1.0), 1.0)
        + f64(band_policy.get("upper_candidate_log10_scale", 0.0), 0.0) * cand_log
        + f64(band_policy.get("upper_market_log10_scale", 0.0), 0.0) * mkt_log
        + f64(band_policy.get("upper_hostile_share_scale", 0.0), 0.0) * float(hostile_share_value)
        + regime_add
    )
    lower_lookup = clamp(
        lower_lookup,
        f64(band_policy.get("lower_min", 0.0), 0.0),
        f64(band_policy.get("lower_max", 1.0), 1.0),
    )
    upper_lookup = clamp(
        upper_lookup,
        f64(band_policy.get("upper_min", 0.0), 0.0),
        f64(band_policy.get("upper_max", 1.0), 1.0),
    )
    clean_history = [f64(x, 0.0) for x in history_values if math.isfinite(f64(x, 0.0))]
    lower_dist = lower_lookup
    upper_dist = upper_lookup
    lower_dist_available = False
    upper_dist_available = False
    min_samples = max(1, i64(band_policy.get("dist_min_samples", 2), 2))
    if len(clean_history) >= min_samples:
        lower_q = clamp(f64(band_policy.get("lower_dist_quantile_p", 0.25), 0.25), 0.0, 1.0)
        upper_q = clamp(f64(band_policy.get("upper_dist_quantile_p", 0.75), 0.75), 0.0, 1.0)
        lower_dist = quantile(clean_history, lower_q)
        upper_dist = quantile(clean_history, upper_q)
        lower_dist_available = True
        upper_dist_available = True
    lower_combine = str(band_policy.get("lower_combine_mode", default_lower_combine)).strip().lower() or str(default_lower_combine).strip().lower()
    upper_combine = str(band_policy.get("upper_combine_mode", default_upper_combine)).strip().lower() or str(default_upper_combine).strip().lower()
    lower_final = combine_limits(float(lower_lookup), float(lower_dist), lower_combine)
    upper_final = combine_limits(float(upper_lookup), float(upper_dist), upper_combine)

    uncertainty_policy = band_policy.get("uncertainty_component", {})
    if not isinstance(uncertainty_policy, dict):
        uncertainty_policy = {}
    uncertainty_enabled = bool(uncertainty_policy.get("enabled", True))
    uncertainty_fallback_only = bool(uncertainty_policy.get("fallback_only", True))
    uncertainty_active = bool(uncertainty_enabled and (fallback_mode or not uncertainty_fallback_only))

    hist_count = len(clean_history)
    hist_median = quantile(clean_history, 0.5) if hist_count > 0 else 0.0
    hist_q25 = quantile(clean_history, 0.25) if hist_count > 0 else hist_median
    hist_q75 = quantile(clean_history, 0.75) if hist_count > 0 else hist_median
    hist_iqr = max(0.0, float(hist_q75) - float(hist_q25))

    lower_uncertainty_method = str(
        uncertainty_policy.get("lower_method", "median_iqr")
    ).strip().lower() or "median_iqr"
    upper_uncertainty_method = str(
        uncertainty_policy.get("upper_method", "median_iqr")
    ).strip().lower() or "median_iqr"
    lower_uncertainty_iqr_k = max(
        0.0,
        f64(uncertainty_policy.get("lower_iqr_k", 0.5), 0.5),
    )
    upper_uncertainty_iqr_k = max(
        0.0,
        f64(uncertainty_policy.get("upper_iqr_k", 1.0), 1.0),
    )
    lower_uncertainty_quantile = clamp(
        f64(uncertainty_policy.get("lower_quantile_p", 0.1), 0.1),
        0.0,
        1.0,
    )
    upper_uncertainty_quantile = clamp(
        f64(uncertainty_policy.get("upper_quantile_p", 0.9), 0.9),
        0.0,
        1.0,
    )

    lower_uncertainty = float(lower_lookup)
    upper_uncertainty = float(upper_lookup)
    if hist_count > 0:
        if lower_uncertainty_method == "quantile":
            lower_uncertainty = quantile(clean_history, lower_uncertainty_quantile)
        else:
            lower_uncertainty = float(hist_median) - (lower_uncertainty_iqr_k * float(hist_iqr))
        if upper_uncertainty_method == "quantile":
            upper_uncertainty = quantile(clean_history, upper_uncertainty_quantile)
        else:
            upper_uncertainty = float(hist_median) + (upper_uncertainty_iqr_k * float(hist_iqr))
    lower_uncertainty = clamp(
        lower_uncertainty,
        f64(band_policy.get("lower_min", 0.0), 0.0),
        f64(band_policy.get("lower_max", 1.0), 1.0),
    )
    upper_uncertainty = clamp(
        upper_uncertainty,
        f64(band_policy.get("upper_min", 0.0), 0.0),
        f64(band_policy.get("upper_max", 1.0), 1.0),
    )

    lower_uncertainty_combine_mode = str(
        uncertainty_policy.get("lower_combine_mode", lower_combine)
    ).strip().lower() or lower_combine
    upper_uncertainty_combine_mode = str(
        uncertainty_policy.get("upper_combine_mode", upper_combine)
    ).strip().lower() or upper_combine
    fallback_upper_override_applied = False
    if uncertainty_active:
        lower_final = combine_limits(
            float(lower_final),
            float(lower_uncertainty),
            lower_uncertainty_combine_mode,
        )
        if fallback_mode:
            # In core-zero fallback mode, avoid over-narrow upper band by using
            # the most permissive component among lookup/dist/uncertainty floor.
            upper_final = max(float(upper_lookup), float(upper_dist), float(upper_uncertainty))
            fallback_upper_override_applied = True
        else:
            upper_final = combine_limits(
                float(upper_final),
                float(upper_uncertainty),
                upper_uncertainty_combine_mode,
            )
    lower_final = clamp(
        lower_final,
        f64(band_policy.get("lower_min", 0.0), 0.0),
        f64(band_policy.get("lower_max", 1.0), 1.0),
    )
    upper_final = clamp(
        upper_final,
        f64(band_policy.get("upper_min", 0.0), 0.0),
        f64(band_policy.get("upper_max", 1.0), 1.0),
    )
    if upper_final < lower_final:
        upper_final = lower_final
    return {
        "lower_lookup": float(lower_lookup),
        "upper_lookup": float(upper_lookup),
        "lower_dist": float(lower_dist),
        "upper_dist": float(upper_dist),
        "lower_dist_available": bool(lower_dist_available),
        "upper_dist_available": bool(upper_dist_available),
        "lower_combine_mode": lower_combine,
        "upper_combine_mode": upper_combine,
        "lower_uncertainty": float(lower_uncertainty),
        "upper_uncertainty": float(upper_uncertainty),
        "lower_uncertainty_method": lower_uncertainty_method,
        "upper_uncertainty_method": upper_uncertainty_method,
        "lower_uncertainty_iqr_k": float(lower_uncertainty_iqr_k),
        "upper_uncertainty_iqr_k": float(upper_uncertainty_iqr_k),
        "lower_uncertainty_quantile_p": float(lower_uncertainty_quantile),
        "upper_uncertainty_quantile_p": float(upper_uncertainty_quantile),
        "lower_uncertainty_combine_mode": lower_uncertainty_combine_mode,
        "upper_uncertainty_combine_mode": upper_uncertainty_combine_mode,
        "uncertainty_history_count": int(hist_count),
        "uncertainty_history_median": float(hist_median),
        "uncertainty_history_q25": float(hist_q25),
        "uncertainty_history_q75": float(hist_q75),
        "uncertainty_history_iqr": float(hist_iqr),
        "fallback_mode": bool(fallback_mode),
        "uncertainty_component_active": bool(uncertainty_active),
        "fallback_upper_override_applied": bool(fallback_upper_override_applied),
        "band_components": {
            "lower": {
                "lookup": float(lower_lookup),
                "dist": float(lower_dist),
                "uncertainty": float(lower_uncertainty),
                "final": float(lower_final),
            },
            "upper": {
                "lookup": float(upper_lookup),
                "dist": float(upper_dist),
                "uncertainty": float(upper_uncertainty),
                "final": float(upper_final),
            },
        },
        "lower_final": float(lower_final),
        "upper_final": float(upper_final),
    }


def normalize_ops(ops: Dict[str, Any]) -> Dict[str, Any]:
    out = dict(ops)
    out["enabled"] = bool(out.get("enabled", False))
    out["mode"] = str(out.get("mode", "manual")).strip().lower() or "manual"
    out["required_ev_offset_min"] = f64(out.get("required_ev_offset_min", -0.003), -0.003)
    out["required_ev_offset_max"] = f64(out.get("required_ev_offset_max", 0.003), 0.003)
    if out["required_ev_offset_max"] < out["required_ev_offset_min"]:
        out["required_ev_offset_min"], out["required_ev_offset_max"] = out["required_ev_offset_max"], out["required_ev_offset_min"]
    out["required_ev_offset"] = min(out["required_ev_offset_max"], max(out["required_ev_offset_min"], f64(out.get("required_ev_offset", 0.0), 0.0)))

    out["k_margin_scale_min"] = max(0.0, f64(out.get("k_margin_scale_min", 0.5), 0.5))
    out["k_margin_scale_max"] = max(0.0, f64(out.get("k_margin_scale_max", 2.0), 2.0))
    if out["k_margin_scale_max"] < out["k_margin_scale_min"]:
        out["k_margin_scale_min"], out["k_margin_scale_max"] = out["k_margin_scale_max"], out["k_margin_scale_min"]
    out["k_margin_scale"] = min(out["k_margin_scale_max"], max(out["k_margin_scale_min"], f64(out.get("k_margin_scale", 1.0), 1.0)))

    out["ev_blend_scale_min"] = max(0.0, f64(out.get("ev_blend_scale_min", 0.5), 0.5))
    out["ev_blend_scale_max"] = max(0.0, f64(out.get("ev_blend_scale_max", 1.5), 1.5))
    if out["ev_blend_scale_max"] < out["ev_blend_scale_min"]:
        out["ev_blend_scale_min"], out["ev_blend_scale_max"] = out["ev_blend_scale_max"], out["ev_blend_scale_min"]
    out["ev_blend_scale"] = min(out["ev_blend_scale_max"], max(out["ev_blend_scale_min"], f64(out.get("ev_blend_scale", 1.0), 1.0)))

    out["max_step_per_update"] = min(1.0, max(0.0, f64(out.get("max_step_per_update", 0.05), 0.05)))
    out["min_update_interval_sec"] = max(0, i64(out.get("min_update_interval_sec", 3600), 3600))
    return out


def ensure_ops(bundle: Dict[str, Any]) -> Dict[str, Any]:
    phase3 = bundle.get("phase3", {})
    if not isinstance(phase3, dict):
        phase3 = {}
    ops = phase3.get("operations_control", {})
    if not isinstance(ops, dict):
        ops = {}
    defaults = {
        "enabled": False,
        "mode": "manual",
        "required_ev_offset": 0.0,
        "required_ev_offset_min": -0.003,
        "required_ev_offset_max": 0.003,
        "k_margin_scale": 1.0,
        "k_margin_scale_min": 0.5,
        "k_margin_scale_max": 2.0,
        "ev_blend_scale": 1.0,
        "ev_blend_scale_min": 0.5,
        "ev_blend_scale_max": 1.5,
        "max_step_per_update": 0.05,
        "min_update_interval_sec": 3600,
    }
    merged = dict(defaults)
    merged.update(ops)
    phase3["operations_control"] = normalize_ops(merged)
    bundle["phase3"] = phase3
    return bundle


def flatten(x: Any, p: str = "") -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    if isinstance(x, dict):
        for k in sorted(x.keys()):
            cp = f"{p}.{k}" if p else str(k)
            out.update(flatten(x[k], cp))
        return out
    if isinstance(x, list):
        for i, v in enumerate(x):
            out.update(flatten(v, f"{p}[{i}]"))
        return out
    out[p] = x
    return out


def bundle_diff(lhs: Dict[str, Any], rhs: Dict[str, Any]) -> List[Dict[str, Any]]:
    a = flatten(lhs)
    b = flatten(rhs)
    rows = []
    for k in sorted(set(a.keys()) | set(b.keys())):
        if a.get(k) != b.get(k):
            rows.append({"key": k, "before": a.get(k), "after": b.get(k)})
    return rows


def kpis(report: Dict[str, Any]) -> Dict[str, Any]:
    agg = report.get("aggregates", {}) if isinstance(report.get("aggregates"), dict) else {}
    protocol_split = (
        report.get("protocol_split", {})
        if isinstance(report.get("protocol_split"), dict)
        else {}
    )
    throughput_observed_legacy_avg = max(0.0, f64(agg.get("avg_total_trades", 0.0), 0.0))
    throughput_observed_core_filled = throughput_observed_legacy_avg
    throughput_source = "legacy_avg_fallback"
    if isinstance(protocol_split, dict):
        if "total_trades_core_effective" in protocol_split:
            throughput_observed_core_filled = max(
                0.0,
                f64(protocol_split.get("total_trades_core_effective", 0.0), 0.0),
            )
            throughput_source = "core_filled"
        elif "orders_filled_core_effective" in protocol_split:
            throughput_observed_core_filled = max(
                0.0,
                f64(protocol_split.get("orders_filled_core_effective", 0.0), 0.0),
            )
            throughput_source = "core_filled_orders_fallback"
    aad = ng(report, ["adaptive_validation", "aggregates"], {})
    if not isinstance(aad, dict):
        aad = {}
    lta = aad.get("loss_tail_aggregate", {})
    if not isinstance(lta, dict):
        lta = {}
    p3 = ng(report, ["diagnostics", "aggregate", "phase3_diagnostics_v2", "funnel_breakdown"], {})
    if not isinstance(p3, dict):
        p3 = {}
    p4 = ng(report, ["diagnostics", "aggregate", "phase4_portfolio_diagnostics", "selection_breakdown"], {})
    if not isinstance(p4, dict):
        p4 = {}
    return {
        "throughput_avg_trades": float(throughput_observed_core_filled),
        "throughput_observed_core_filled": float(throughput_observed_core_filled),
        "throughput_observed_legacy_avg": float(throughput_observed_legacy_avg),
        "throughput_source": str(throughput_source),
        "quality_risk_adjusted_score": f64(aad.get("avg_risk_adjusted_score", 0.0), 0.0),
        "quality_realized_edge_proxy_krw": f64(agg.get("avg_expectancy_krw", 0.0), 0.0),
        "risk_drawdown_pct": max(0.0, f64(agg.get("peak_max_drawdown_pct", 0.0), 0.0)),
        "risk_tail_loss_concentration": max(0.0, f64(lta.get("avg_top3_loss_concentration", 0.0), 0.0)),
        "risk_avg_fee_krw": max(0.0, f64(agg.get("avg_fee_krw", 0.0), 0.0)),
        "phase3_rejects": {
            "reject_margin_insufficient": max(0, i64(p3.get("reject_margin_insufficient", 0))),
            "reject_expected_value_fail": max(0, i64(p3.get("reject_expected_value_fail", 0))),
            "reject_frontier_fail": max(0, i64(p3.get("reject_frontier_fail", 0))),
            "reject_ev_confidence_low": max(0, i64(p3.get("reject_ev_confidence_low", 0))),
            "reject_cost_tail_fail": max(0, i64(p3.get("reject_cost_tail_fail", 0))),
        },
        "phase4_selection": {
            "selection_rate": max(0.0, f64(p4.get("selection_rate", 0.0), 0.0)),
            "rejected_by_drawdown_governor": max(0, i64(p4.get("rejected_by_drawdown_governor", 0))),
        },
    }


def parse_time_range_hours(payload: Dict[str, Any], key: str) -> float:
    if not isinstance(payload, dict):
        return 0.0
    value = payload.get(key)
    if not isinstance(value, list) or len(value) < 2:
        return 0.0
    start_ts = i64(value[0], 0)
    end_ts = i64(value[1], 0)
    if end_ts <= start_ts:
        return 0.0
    # split ranges are recorded in epoch milliseconds.
    return float(end_ts - start_ts) / 3600000.0


def throughput_gate_context(report: Dict[str, Any]) -> Dict[str, Any]:
    protocol_split = report.get("protocol_split", {}) if isinstance(report.get("protocol_split"), dict) else {}
    split_name = str(
        report.get(
            "split_name",
            protocol_split.get("split_name", protocol_split.get("name", protocol_split.get("split", "unknown"))),
        )
    ).strip() or "unknown"
    core_hours = parse_time_range_hours(protocol_split, "evaluation_range_effective")
    if core_hours <= 0.0:
        core_hours = parse_time_range_hours(protocol_split, "evaluation_range_used")
    if core_hours <= 0.0:
        core_hours = parse_time_range_hours(protocol_split, "split_range_used")
    market_count = max(1, i64(report.get("dataset_count", 0), 0))
    return {
        "split_name": split_name,
        "core_length_hours": float(core_hours),
        "markets_count": int(market_count),
    }


def lookup_throughput_limit(
    rows: Any,
    *,
    split_name: str,
    core_length_hours: float,
    markets_count: int,
    default_limit: float,
    limit_key: str = "limit",
) -> float:
    if not isinstance(rows, list):
        return float(default_limit)
    best_score = -1
    best_limits: List[float] = []
    split_token = str(split_name).strip().lower()
    for row in rows:
        if not isinstance(row, dict):
            continue
        row_split = str(row.get("split_name", row.get("split", "ANY"))).strip().lower()
        if row_split not in ("", "any", "*") and row_split != split_token:
            continue
        core_min = f64(row.get("core_length_hours_min", row.get("core_hours_min", -1.0)), -1.0)
        core_max = f64(row.get("core_length_hours_max", row.get("core_hours_max", -1.0)), -1.0)
        mk_min = i64(row.get("markets_count_min", row.get("market_count_min", -1)), -1)
        mk_max = i64(row.get("markets_count_max", row.get("market_count_max", -1)), -1)
        if core_min >= 0.0 and float(core_length_hours) < float(core_min):
            continue
        if core_max >= 0.0 and float(core_length_hours) > float(core_max):
            continue
        if mk_min >= 0 and int(markets_count) < int(mk_min):
            continue
        if mk_max >= 0 and int(markets_count) > int(mk_max):
            continue
        limit_value = f64(row.get(limit_key, default_limit), default_limit)
        score = 0
        if row_split not in ("", "any", "*"):
            score += 1
        if core_min >= 0.0:
            score += 1
        if core_max >= 0.0:
            score += 1
        if mk_min >= 0:
            score += 1
        if mk_max >= 0:
            score += 1
        if score > best_score:
            best_score = score
            best_limits = [float(limit_value)]
        elif score == best_score:
            best_limits.append(float(limit_value))
    if not best_limits:
        return float(default_limit)
    return float(min(best_limits))


def extract_throughput_history(
    report: Dict[str, Any],
    *,
    prefer_core_direct: bool,
    fallback_on_core_zero: bool,
) -> Dict[str, Any]:
    payload = pick_eval_payload(report, prefer_core_direct)
    source = "core_window_direct" if payload is not report else "report"
    if (
        fallback_on_core_zero
        and payload is not report
        and phase3_funnel(payload).get("candidate_total", 0) <= 0
    ):
        payload = report
        source = "report_fallback_core_zero"
    samples: List[float] = []
    agg = payload.get("aggregates", {}) if isinstance(payload.get("aggregates"), dict) else {}
    avg_total_trades = f64(agg.get("avg_total_trades", 0.0), float("nan"))
    if math.isfinite(avg_total_trades):
        samples.append(max(0.0, float(avg_total_trades)))
    per_dataset = ng(payload, ["adaptive_validation", "per_dataset"], [])
    if isinstance(per_dataset, list):
        for row in per_dataset:
            if not isinstance(row, dict):
                continue
            trades = f64(row.get("total_trades", 0.0), float("nan"))
            if math.isfinite(trades):
                samples.append(max(0.0, float(trades)))
    return {
        "samples": samples,
        "sample_size_total": int(len(samples)),
        "source": source,
        "observed_avg_trades": max(0.0, f64(agg.get("avg_total_trades", 0.0), 0.0)),
    }


def apply_dynamic_throughput_gate(
    *,
    report: Dict[str, Any],
    evaluation: Dict[str, Any],
    legacy_min_avg_trades: float,
    throughput_policy: Dict[str, Any],
    history_reports: List[Dict[str, Any]],
    history_report_paths: List[str],
) -> Dict[str, Any]:
    observed_core_filled = max(
        0.0,
        f64(ng(evaluation, ["kpis", "throughput_observed_core_filled"], 0.0), 0.0),
    )
    observed_legacy_avg = max(
        0.0,
        f64(ng(evaluation, ["kpis", "throughput_observed_legacy_avg"], 0.0), 0.0),
    )
    observed_source = str(
        ng(evaluation, ["kpis", "throughput_source"], "legacy_avg_fallback")
    ).strip() or "legacy_avg_fallback"
    observed = max(
        0.0,
        f64(
            ng(
                evaluation,
                ["kpis", "throughput_avg_trades"],
                observed_core_filled if observed_source.startswith("core_filled") else observed_legacy_avg,
            ),
            0.0,
        ),
    )
    if not isinstance(throughput_policy, dict) or not bool(throughput_policy.get("enabled", False)):
        return {
            "enabled": False,
            "throughput_observed": float(observed),
            "throughput_observed_core_filled": float(observed_core_filled),
            "throughput_observed_legacy_avg": float(observed_legacy_avg),
            "throughput_observed_source": str(observed_source),
            "lookup_limit": float(legacy_min_avg_trades),
            "dist_limit": float(legacy_min_avg_trades),
            "final_limit": float(legacy_min_avg_trades),
            "combine_mode": "legacy_absolute",
            "throughput_ok": bool(observed >= float(legacy_min_avg_trades)),
            "throughput_hold": False,
            "low_conf_action": "none",
            "low_conf": False,
            "sample_size": 0,
            "min_samples_required": 0,
            "history_sources": [],
        }

    prefer_core_direct = bool(throughput_policy.get("history_use_core_window_direct", True))
    fallback_on_core_zero = bool(
        throughput_policy.get("fallback_to_split_report_on_core_zero", True)
    )
    ctx = throughput_gate_context(report)
    split_name = str(ctx.get("split_name", "unknown"))
    core_length_hours = f64(ctx.get("core_length_hours", 0.0), 0.0)
    markets_count = max(1, i64(ctx.get("markets_count", 1), 1))

    default_lookup = max(
        0.0,
        f64(throughput_policy.get("lookup_default_limit", legacy_min_avg_trades), legacy_min_avg_trades),
    )
    lookup_limit = lookup_throughput_limit(
        throughput_policy.get("lookup_limits", []),
        split_name=split_name,
        core_length_hours=core_length_hours,
        markets_count=markets_count,
        default_limit=default_lookup,
        limit_key="limit",
    )

    dist_quantile_p = clamp(f64(throughput_policy.get("dist_quantile_p", 0.2), 0.2), 0.0, 1.0)
    dist_min_samples = max(1, i64(throughput_policy.get("dist_min_samples", 2), 2))
    combine_mode = str(throughput_policy.get("combine_mode", "min")).strip().lower() or "min"

    history_rows: List[Dict[str, Any]] = []
    dist_samples: List[float] = []
    sample_sizes_history: List[float] = []
    for idx, hist_report in enumerate(history_reports):
        if not isinstance(hist_report, dict):
            continue
        hist_row = extract_throughput_history(
            hist_report,
            prefer_core_direct=prefer_core_direct,
            fallback_on_core_zero=fallback_on_core_zero,
        )
        hist_path = history_report_paths[idx] if idx < len(history_report_paths) else ""
        history_rows.append(
            {
                "path": str(hist_path),
                "source": str(hist_row.get("source", "")),
                "sample_size_total": int(hist_row.get("sample_size_total", 0)),
                "observed_avg_trades": round(f64(hist_row.get("observed_avg_trades", 0.0), 0.0), 6),
            }
        )
        for x in hist_row.get("samples", []):
            vx = f64(x, float("nan"))
            if math.isfinite(vx):
                dist_samples.append(max(0.0, float(vx)))
        sample_sizes_history.append(float(max(0, i64(hist_row.get("sample_size_total", 0), 0))))

    sample_size = len(dist_samples)
    dist_available = sample_size >= dist_min_samples
    dist_limit = quantile(dist_samples, dist_quantile_p) if dist_available else float(lookup_limit)

    uncertainty = throughput_policy.get("uncertainty_component", {})
    if not isinstance(uncertainty, dict):
        uncertainty = {}
    uncertainty_enabled = bool(uncertainty.get("enabled", True))
    min_samples_lookup_default = max(
        1.0,
        f64(uncertainty.get("min_samples_lookup_default", 8.0), 8.0),
    )
    min_samples_lookup = lookup_throughput_limit(
        uncertainty.get("min_samples_lookup", []),
        split_name=split_name,
        core_length_hours=core_length_hours,
        markets_count=markets_count,
        default_limit=min_samples_lookup_default,
        limit_key="limit",
    )
    min_samples_dist_q = clamp(
        f64(uncertainty.get("min_samples_dist_quantile_p", 0.2), 0.2),
        0.0,
        1.0,
    )
    clean_sample_sizes = [x for x in sample_sizes_history if math.isfinite(x) and x > 0.0]
    min_samples_dist = (
        quantile(clean_sample_sizes, min_samples_dist_q)
        if clean_sample_sizes
        else float(min_samples_lookup_default)
    )
    min_samples_required = int(math.ceil(max(float(min_samples_lookup), float(min_samples_dist))))
    low_conf = bool(uncertainty_enabled and sample_size < min_samples_required)

    dist_weight_when_low_conf = clamp(
        f64(uncertainty.get("dist_weight_when_low_conf", 0.0), 0.0),
        0.0,
        1.0,
    )
    effective_dist_limit = float(dist_limit)
    if low_conf:
        effective_dist_limit = float(lookup_limit) + (
            (float(dist_limit) - float(lookup_limit)) * float(dist_weight_when_low_conf)
        )
    final_limit = combine_limits(float(lookup_limit), float(effective_dist_limit), combine_mode)

    low_conf_action = str(uncertainty.get("low_conf_action", "promotion_hold")).strip().lower() or "promotion_hold"
    if low_conf_action in ("hold", "promotion_hold"):
        low_conf_action = "promotion_hold"
    elif low_conf_action in ("fail", "strict_fail"):
        low_conf_action = "fail"
    else:
        low_conf_action = "none"

    throughput_hold = bool(low_conf and observed < final_limit and low_conf_action == "promotion_hold")
    throughput_ok = bool((observed >= final_limit) or throughput_hold)

    levels = evaluation.get("levels", {})
    if not isinstance(levels, dict):
        levels = {}
    p4 = levels.get("p4", {})
    if not isinstance(p4, dict):
        p4 = {}
    checks = p4.get("checks", {})
    if not isinstance(checks, dict):
        checks = {}
    checks["throughput_ok"] = bool(throughput_ok)
    checks["throughput_hold"] = bool(throughput_hold)
    p4["checks"] = checks
    p3_pass = bool(ng(levels, ["p3", "pass"], False))
    p4["pass"] = bool(p3_pass and throughput_ok)
    levels["p4"] = p4
    evaluation["levels"] = levels
    evaluation["overall_pass"] = bool(p4.get("pass", False))

    gate_payload = {
        "enabled": True,
        "mode": "dynamic_limit",
        "throughput_observed": round(float(observed), 6),
        "throughput_observed_core_filled": round(float(observed_core_filled), 6),
        "throughput_observed_legacy_avg": round(float(observed_legacy_avg), 6),
        "throughput_observed_source": str(observed_source),
        "lookup_limit": round(float(lookup_limit), 6),
        "dist_limit": round(float(dist_limit), 6),
        "final_limit": round(float(final_limit), 6),
        "combine_mode": combine_mode,
        "throughput_ok": bool(throughput_ok),
        "throughput_hold": bool(throughput_hold),
        "low_conf": bool(low_conf),
        "low_conf_action": low_conf_action,
        "sample_size": int(sample_size),
        "min_samples_required": int(min_samples_required),
        "dist_quantile_p": float(dist_quantile_p),
        "dist_available": bool(dist_available),
        "history_sources": history_rows,
        "context": {
            "split_name": split_name,
            "core_length_hours": round(float(core_length_hours), 6),
            "markets_count": int(markets_count),
        },
        "uncertainty_component": {
            "enabled": bool(uncertainty_enabled),
            "min_samples_lookup_component": round(float(min_samples_lookup), 6),
            "min_samples_dist_component": round(float(min_samples_dist), 6),
            "min_samples_dist_quantile_p": float(min_samples_dist_q),
            "dist_weight_when_low_conf": float(dist_weight_when_low_conf),
        },
    }
    evaluation["throughput_gate"] = gate_payload
    return gate_payload


def evaluate_strict_parity_gate(
    *,
    report: Dict[str, Any],
    args: argparse.Namespace,
    bundle_payload: Dict[str, Any],
) -> Dict[str, Any]:
    policy = ng(bundle_payload, ["phase3", "operations_dynamic_gate", "strict_parity_gate"], {})
    if not isinstance(policy, dict):
        policy = {}
    policy_enabled = bool(policy.get("enabled", False))
    mode = str(policy.get("mode", "block")).strip().lower() or "block"
    if mode not in ("block", "warning_only", "warning_hold", "hold_on_low_conf"):
        mode = "block"

    check_name = str(policy.get("check_name", "cli_strict_parity_pass_flag")).strip() or "cli_strict_parity_pass_flag"
    expected_value = bool(policy.get("expected_value", True))
    observed_value = bool(args.strict_parity_pass)
    mismatch = bool(observed_value != expected_value)

    sample_metric = str(policy.get("sample_metric", "avg_total_trades")).strip().lower() or "avg_total_trades"
    if sample_metric == "candidate_total_core_effective":
        sample_observed = max(
            0.0,
            f64(ng(report, ["protocol_split", "candidate_total_core_effective"], 0.0), 0.0),
        )
    else:
        sample_metric = "avg_total_trades"
        sample_observed = max(0.0, f64(ng(report, ["aggregates", "avg_total_trades"], 0.0), 0.0))
    min_samples_required = max(0, i64(policy.get("min_samples_required", 0), 0))
    low_conf = bool(min_samples_required > 0 and sample_observed < float(min_samples_required))

    if not policy_enabled:
        strict_ok = bool(observed_value)
        strict_hold = False
        strict_warn = False
        effective_mode = "legacy_block"
    else:
        effective_mode = mode
        strict_ok = True
        strict_hold = False
        strict_warn = False
        if mode == "block":
            strict_ok = not mismatch
        elif mode == "warning_only":
            strict_ok = True
            strict_warn = bool(mismatch)
        elif mode == "warning_hold":
            strict_ok = True
            strict_warn = bool(mismatch)
            strict_hold = bool(mismatch)
        elif mode == "hold_on_low_conf":
            if mismatch and low_conf:
                strict_ok = True
                strict_warn = True
                strict_hold = True
            else:
                strict_ok = not mismatch
                strict_warn = False
                strict_hold = False

    strict_fail = bool((not strict_ok) and (not strict_hold))
    diagnosis_recent_n = max(1, i64(policy.get("diagnosis_recent_windows", 5), 5))
    return {
        "enabled": bool(policy_enabled),
        "mode": effective_mode,
        "strict_parity_check_name": check_name,
        "expected_value": bool(expected_value),
        "observed_value": bool(observed_value),
        "mismatch_count": 1 if mismatch else 0,
        "mismatch_rate": 1.0 if mismatch else 0.0,
        "mismatch_items_top3": [
            {
                "name": check_name,
                "expected": bool(expected_value),
                "observed": bool(observed_value),
                "mismatch": bool(mismatch),
            }
        ],
        "fail_threshold": {
            "type": "boolean",
            "expected": bool(expected_value),
            "mode": effective_mode,
        },
        "sample_metric": sample_metric,
        "sample_observed": round(float(sample_observed), 6),
        "min_samples_required": int(min_samples_required),
        "low_conf": bool(low_conf),
        "strict_parity_ok": bool(strict_ok),
        "strict_parity_warning": bool(strict_warn),
        "strict_parity_hold": bool(strict_hold),
        "strict_parity_fail": bool(strict_fail),
        "diagnosis_recent_n": int(diagnosis_recent_n),
    }


def eval_lex(
    report: Dict[str, Any],
    args: argparse.Namespace,
    *,
    strict_parity_eval: Dict[str, Any] = None,
) -> Dict[str, Any]:
    ks = kpis(report)
    strict_parity_ok = bool(args.strict_parity_pass)
    if isinstance(strict_parity_eval, dict) and "strict_parity_ok" in strict_parity_eval:
        strict_parity_ok = bool(strict_parity_eval.get("strict_parity_ok", strict_parity_ok))
    l1 = {
        "drawdown_ok": f64(ks["risk_drawdown_pct"]) <= float(args.max_drawdown_pct),
        "tail_ok": f64(ks["risk_tail_loss_concentration"]) <= float(args.max_tail_concentration),
    }
    l2 = {
        "adaptive_verdict_not_fail": str(ng(report, ["adaptive_validation", "verdict", "verdict"], "fail")).strip().lower() != "fail",
        "avg_fee_ok": f64(ks["risk_avg_fee_krw"]) <= float(args.max_avg_fee_krw),
        "strict_parity_ok": bool(strict_parity_ok),
    }
    agg = report.get("aggregates", {}) if isinstance(report.get("aggregates"), dict) else {}
    l3 = {
        "profit_factor_ok": f64(agg.get("avg_profit_factor", 0.0)) >= float(args.min_profit_factor),
        "expectancy_ok": f64(agg.get("avg_expectancy_krw", 0.0)) >= float(args.min_expectancy_krw),
    }
    l4 = {"throughput_ok": f64(ks["throughput_avg_trades"]) >= float(args.min_avg_trades)}
    p1 = all(l1.values())
    p2 = p1 and all(l2.values())
    p3 = p2 and all(l3.values())
    p4 = p3 and all(l4.values())
    return {"kpis": ks, "levels": {"p1": {"pass": p1, "checks": l1}, "p2": {"pass": p2, "checks": l2}, "p3": {"pass": p3, "checks": l3}, "p4": {"pass": p4, "checks": l4}}, "overall_pass": p4}


def promotion_fail_rows(
    report: Dict[str, Any],
    evaluation: Dict[str, Any],
    args: argparse.Namespace,
    prev_scores: Dict[str, float],
) -> List[Dict[str, Any]]:
    if not isinstance(evaluation, dict):
        return []
    levels = evaluation.get("levels", {})
    if not isinstance(levels, dict):
        levels = {}
    checks_p1 = ng(levels, ["p1", "checks"], {})
    checks_p2 = ng(levels, ["p2", "checks"], {})
    checks_p3 = ng(levels, ["p3", "checks"], {})
    checks_p4 = ng(levels, ["p4", "checks"], {})
    if not isinstance(checks_p1, dict):
        checks_p1 = {}
    if not isinstance(checks_p2, dict):
        checks_p2 = {}
    if not isinstance(checks_p3, dict):
        checks_p3 = {}
    if not isinstance(checks_p4, dict):
        checks_p4 = {}

    k = evaluation.get("kpis", {})
    if not isinstance(k, dict):
        k = {}
    agg = report.get("aggregates", {}) if isinstance(report.get("aggregates"), dict) else {}
    aad = ng(report, ["adaptive_validation", "aggregates"], {})
    if not isinstance(aad, dict):
        aad = {}
    lta = aad.get("loss_tail_aggregate", {})
    if not isinstance(lta, dict):
        lta = {}

    max_drawdown = float(args.max_drawdown_pct)
    max_tail = float(args.max_tail_concentration)
    max_avg_fee = float(args.max_avg_fee_krw)
    min_profit_factor = float(args.min_profit_factor)
    min_expectancy = float(args.min_expectancy_krw)
    min_avg_trades = float(args.min_avg_trades)

    value_drawdown = f64(k.get("risk_drawdown_pct", agg.get("peak_max_drawdown_pct", 0.0)), 0.0)
    value_tail = f64(
        k.get("risk_tail_loss_concentration", lta.get("avg_top3_loss_concentration", 0.0)),
        0.0,
    )
    value_avg_fee = f64(k.get("risk_avg_fee_krw", agg.get("avg_fee_krw", 0.0)), 0.0)
    value_profit_factor = f64(agg.get("avg_profit_factor", 0.0), 0.0)
    value_expectancy = f64(k.get("quality_realized_edge_proxy_krw", agg.get("avg_expectancy_krw", 0.0)), 0.0)
    value_throughput = f64(
        k.get(
            "throughput_avg_trades",
            ng(report, ["protocol_split", "total_trades_core_effective"], agg.get("avg_total_trades", 0.0)),
        ),
        0.0,
    )

    rows: List[Dict[str, Any]] = []

    def push_row(
        reason_key: str,
        group: str,
        value: Any,
        threshold: Any,
        compare_mode: str,
        contribution_score: float,
    ) -> None:
        prev = f64(prev_scores.get(reason_key, 0.0), 0.0) if isinstance(prev_scores, dict) else 0.0
        rows.append(
            {
                "reason_key": str(reason_key),
                "group": str(group),
                "value": value,
                "threshold": threshold,
                "compare_mode": str(compare_mode),
                "contribution_score": round(float(max(0.0, contribution_score)), 6),
                "delta_vs_previous": round(float(max(0.0, contribution_score) - prev), 6),
            }
        )

    if not bool(checks_p1.get("drawdown_ok", True)):
        push_row(
            reason_key="drawdown_fail",
            group="p1_risk",
            value=round(value_drawdown, 6),
            threshold=round(max_drawdown, 6),
            compare_mode="<=",
            contribution_score=max(0.0, value_drawdown - max_drawdown),
        )
    if not bool(checks_p1.get("tail_ok", True)):
        push_row(
            reason_key="tail_concentration_fail",
            group="p1_risk",
            value=round(value_tail, 6),
            threshold=round(max_tail, 6),
            compare_mode="<=",
            contribution_score=max(0.0, value_tail - max_tail),
        )
    if not bool(checks_p2.get("adaptive_verdict_not_fail", True)):
        push_row(
            reason_key="adaptive_verdict_fail",
            group="p2_quality_alignment",
            value="fail",
            threshold="not_fail",
            compare_mode="boolean",
            contribution_score=1.0,
        )
    if not bool(checks_p2.get("avg_fee_ok", True)):
        push_row(
            reason_key="avg_fee_fail",
            group="p2_quality_alignment",
            value=round(value_avg_fee, 6),
            threshold=round(max_avg_fee, 6),
            compare_mode="<=",
            contribution_score=max(0.0, value_avg_fee - max_avg_fee),
        )
    if not bool(checks_p2.get("strict_parity_ok", True)):
        push_row(
            reason_key="strict_parity_fail",
            group="p2_quality_alignment",
            value="false",
            threshold="true",
            compare_mode="boolean",
            contribution_score=1.0,
        )
    if not bool(checks_p3.get("profit_factor_ok", True)):
        push_row(
            reason_key="profit_factor_fail",
            group="p3_profitability",
            value=round(value_profit_factor, 6),
            threshold=round(min_profit_factor, 6),
            compare_mode=">=",
            contribution_score=max(0.0, min_profit_factor - value_profit_factor),
        )
    if not bool(checks_p3.get("expectancy_ok", True)):
        push_row(
            reason_key="expectancy_proxy_fail",
            group="p3_profitability",
            value=round(value_expectancy, 6),
            threshold=round(min_expectancy, 6),
            compare_mode=">=",
            contribution_score=max(0.0, min_expectancy - value_expectancy),
        )
    throughput_gate_ctx = evaluation.get("throughput_gate", {}) if isinstance(evaluation.get("throughput_gate"), dict) else {}
    throughput_threshold = f64(throughput_gate_ctx.get("final_limit", min_avg_trades), min_avg_trades)
    throughput_hold = bool(throughput_gate_ctx.get("throughput_hold", False))
    if not bool(checks_p4.get("throughput_ok", True)) and not throughput_hold:
        push_row(
            reason_key="throughput_fail",
            group="p4_throughput",
            value=round(value_throughput, 6),
            threshold=round(throughput_threshold, 6),
            compare_mode=">=",
            contribution_score=max(0.0, throughput_threshold - value_throughput),
        )

    rows.sort(key=lambda item: (-f64(item.get("contribution_score", 0.0), 0.0), str(item.get("reason_key", ""))))
    return rows


def derive_promotion_state(
    *,
    rollback: bool,
    strict_parity_hold: bool,
    current_stage: str,
    next_stage: str,
) -> Dict[str, Any]:
    cur = str(current_stage).strip().lower()
    nxt = str(next_stage).strip().lower()
    if rollback:
        promotion_state = "S0"
    elif strict_parity_hold:
        promotion_state = "S1"
    elif cur == "live100" or nxt == "live100":
        promotion_state = "S3"
    elif cur in {"live10", "live30"} or nxt in {"live10", "live30"}:
        promotion_state = "S2"
    else:
        promotion_state = "S1"

    paper_allowed = promotion_state in {"S1", "S2", "S3"}
    live_allowed = (promotion_state in {"S2", "S3"}) and (not strict_parity_hold)
    return {
        "promotion_state": promotion_state,
        "paper_promotion_allowed": bool(paper_allowed),
        "live_promotion_allowed": bool(live_allowed),
    }


def cmd_control(args: argparse.Namespace) -> int:
    bpath = resolve_repo_path(args.bundle_json)
    rpath = resolve_repo_path(args.verification_report_json)
    opath = resolve_repo_path(args.output_bundle_json)
    repath = resolve_repo_path(args.output_report_json)
    spath = resolve_repo_path(args.state_json)
    bundle = load_json_or_none(bpath)
    report = load_json_or_none(rpath)
    if not isinstance(bundle, dict) or not isinstance(report, dict):
        raise RuntimeError("bundle/report load failed")
    state = load_json_or_none(spath)
    if not isinstance(state, dict):
        state = {}
    before = copy.deepcopy(bundle)
    bundle = ensure_ops(bundle)
    ops = bundle["phase3"]["operations_control"]
    ks = kpis(report)
    p3r = ks["phase3_rejects"]
    max_step = max(0.0, f64(ops.get("max_step_per_update", 0.05), 0.05))
    requested_step = f64(args.step_size, -1.0)
    if requested_step <= 0.0:
        # Default behavior uses policy max_step_per_update when CLI step is omitted.
        step = max_step
    else:
        step = max(0.0, min(requested_step, max_step))
    cs = state.get("control_mode_a", {}) if isinstance(state.get("control_mode_a"), dict) else {}
    elapsed = max(0, now_epoch() - i64(cs.get("last_update_ts", 0)))
    interval_ok = elapsed >= i64(ops.get("min_update_interval_sec", 3600))
    acts = []

    def apply_delta(key: str, delta: float) -> None:
        old = f64(ops.get(key, 0.0))
        new = old + delta
        if key == "required_ev_offset":
            new = min(f64(ops["required_ev_offset_max"]), max(f64(ops["required_ev_offset_min"]), new))
        elif key == "k_margin_scale":
            new = min(f64(ops["k_margin_scale_max"]), max(f64(ops["k_margin_scale_min"]), new))
        elif key == "ev_blend_scale":
            new = min(f64(ops["ev_blend_scale_max"]), max(f64(ops["ev_blend_scale_min"]), new))
        if abs(new - old) > 1e-12:
            ops[key] = new
            acts.append({"key": key, "old": old, "new": new, "delta": delta})

    if interval_ok and step > 0.0:
        t = f64(ks["throughput_avg_trades"])
        q = f64(ks["quality_risk_adjusted_score"])
        d = f64(ks["risk_drawdown_pct"])
        tc = f64(ks["risk_tail_loss_concentration"])
        evf = i64(p3r["reject_expected_value_fail"]) + i64(p3r["reject_frontier_fail"]) + i64(p3r["reject_ev_confidence_low"]) + i64(p3r["reject_cost_tail_fail"])
        mf = i64(p3r["reject_margin_insufficient"])
        if t < float(args.target_throughput_min):
            if evf >= mf:
                apply_delta("required_ev_offset", -step)
            else:
                apply_delta("k_margin_scale", +step)
        elif t > float(args.target_throughput_max):
            apply_delta("required_ev_offset", +step)
        if d > float(args.risk_drawdown_max_pct) or tc > float(args.risk_tail_concentration_max):
            apply_delta("required_ev_offset", +step)
            apply_delta("ev_blend_scale", -step)
        if q < float(args.target_quality_min_score):
            apply_delta("required_ev_offset", +(step * 0.5))
            apply_delta("ev_blend_scale", -(step * 0.5))

    bundle["phase3"]["operations_control"] = normalize_ops(ops)
    meta = bundle.get("release_meta", {}) if isinstance(bundle.get("release_meta"), dict) else {}
    parent = str(meta.get("version", bundle.get("version", ""))).strip()
    meta.update({"version": (str(args.release_version).strip() or f"phase34_ops_{now_epoch()}"), "created_at": now_iso(), "parent_version": parent, "mode": "phase34_operations_control_a", "change_summary": [f"{x['key']}:{x['old']}->{x['new']}" for x in acts]})
    bundle["release_meta"] = meta

    drows = bundle_diff(before, bundle)
    dump_json(opath, bundle)
    state["control_mode_a"] = {"last_update_ts": now_epoch(), "last_actions": acts, "last_kpis": ks, "current_knobs": {"required_ev_offset": bundle["phase3"]["operations_control"]["required_ev_offset"], "k_margin_scale": bundle["phase3"]["operations_control"]["k_margin_scale"], "ev_blend_scale": bundle["phase3"]["operations_control"]["ev_blend_scale"]}}
    dump_json(spath, state)
    dump_json(repath, {"mode": "control_mode_a", "generated_at": now_iso(), "interval_ok": interval_ok, "elapsed_sec": elapsed, "actions": acts, "kpis": ks, "bundle_diff_count": len(drows), "bundle_diff_top": drows[:50], "bundle_output": str(opath), "state_path": str(spath)})
    print(f"[Phase34Ops] mode=control_a actions={len(acts)} diff={len(drows)} interval_ok={str(interval_ok).lower()}", flush=True)
    return 0


def reward(report: Dict[str, Any], args: argparse.Namespace) -> float:
    ks = kpis(report)
    return (f64(ks["quality_realized_edge_proxy_krw"]) * float(args.reward_expectancy_weight) - f64(ks["risk_drawdown_pct"]) * float(args.reward_drawdown_weight) - f64(ks["risk_tail_loss_concentration"]) * float(args.reward_tail_weight) + f64(ks["throughput_avg_trades"]) * float(args.reward_throughput_weight))


def cmd_bandit(args: argparse.Namespace) -> int:
    spath = resolve_repo_path(args.state_json)
    opath = resolve_repo_path(args.output_json)
    state = load_json_or_none(spath)
    if not isinstance(state, dict):
        state = {}
    arms = [str(resolve_repo_path(x)) for x in args.candidate_bundle_jsons if str(x).strip()]
    if not arms:
        raise RuntimeError("candidate bundles required")
    b = state.get("bandit_mode_b", {}) if isinstance(state.get("bandit_mode_b"), dict) else {}
    bd = b.get("arms", {}) if isinstance(b.get("arms"), dict) else {}
    for arm in arms:
        cur = bd.get(arm, {}) if isinstance(bd.get(arm), dict) else {}
        bd[arm] = {"plays": max(0, i64(cur.get("plays", 0))), "total_reward": f64(cur.get("total_reward", 0.0)), "mean_reward": f64(cur.get("mean_reward", 0.0)), "last_reward": cur.get("last_reward", None)}
    last = str(b.get("last_selected", "")).strip()
    updated = None
    if str(args.update_reward_report_json).strip() and last in bd:
        r = load_json_or_none(resolve_repo_path(args.update_reward_report_json))
        if isinstance(r, dict):
            updated = reward(r, args)
            bd[last]["plays"] = int(bd[last]["plays"]) + 1
            bd[last]["total_reward"] = float(bd[last]["total_reward"]) + float(updated)
            bd[last]["mean_reward"] = float(bd[last]["total_reward"]) / float(max(1, bd[last]["plays"]))
            bd[last]["last_reward"] = float(updated)
    rnd = max(0, i64(b.get("round", 0))) + 1
    n = max(1, i64(args.explore_every_n, 4))
    explore = (rnd % n) == 0
    if explore:
        sel = sorted(arms, key=lambda x: (i64(bd[x]["plays"]), x))[0]
        why = "explore_least_played"
    else:
        sel = sorted(arms, key=lambda x: (-f64(bd[x]["mean_reward"]), i64(bd[x]["plays"]), x))[0]
        why = "exploit_best_mean_reward"
    rank = sorted([{"bundle": x, "plays": i64(bd[x]["plays"]), "mean_reward": f64(bd[x]["mean_reward"]), "last_reward": bd[x]["last_reward"]} for x in arms], key=lambda x: (-f64(x["mean_reward"]), i64(x["plays"]), str(x["bundle"])))
    b.update({"round": int(rnd), "last_selected": sel, "last_selection_reason": why, "last_updated_at": now_iso(), "arms": bd})
    state["bandit_mode_b"] = b
    dump_json(spath, state)
    out = {"mode": "bandit_mode_b", "generated_at": now_iso(), "round": int(rnd), "selected_bundle": sel, "selection_reason": why, "should_explore": bool(explore), "updated_reward": updated, "ranking": rank, "state_path": str(spath)}
    if str(args.output_selected_bundle_json).strip():
        sel_payload = load_json_or_none(pathlib.Path(sel))
        if isinstance(sel_payload, dict):
            dump_json(resolve_repo_path(args.output_selected_bundle_json), sel_payload)
            out["output_selected_bundle_json"] = str(resolve_repo_path(args.output_selected_bundle_json))
    dump_json(opath, out)
    print(f"[Phase34Ops] mode=bandit_b round={rnd} selected={sel} reason={why}", flush=True)
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    dev = load_json_or_none(resolve_repo_path(args.development_report_json))
    val = load_json_or_none(resolve_repo_path(args.validation_report_json))
    qua = load_json_or_none(resolve_repo_path(args.quarantine_report_json))
    if not isinstance(dev, dict) or not isinstance(val, dict) or not isinstance(qua, dict):
        raise RuntimeError("dev/val/quarantine reports required")
    de = eval_lex(dev, args)
    ve = eval_lex(val, args)
    qe = eval_lex(qua, args)
    ok = bool(de.get("overall_pass", False) and ve.get("overall_pass", False) and qe.get("overall_pass", False))
    dump_json(resolve_repo_path(args.output_json), {"mode": "validation_protocol", "generated_at": now_iso(), "purge_gap_minutes": int(args.purge_gap_minutes), "split_meta": {"development_tag": str(args.development_tag), "validation_tag": str(args.validation_tag), "quarantine_tag": str(args.quarantine_tag)}, "development": de, "validation": ve, "quarantine": qe, "overall_pass": ok})
    print(f"[Phase34Ops] mode=validate overall_pass={str(ok).lower()}", flush=True)
    return 0 if ok else 2


def cmd_promotion(args: argparse.Namespace) -> int:
    rep = load_json_or_none(resolve_repo_path(args.paper_report_json))
    if not isinstance(rep, dict):
        raise RuntimeError("paper report required")
    spath = resolve_repo_path(args.state_json)
    state = load_json_or_none(spath)
    if not isinstance(state, dict):
        state = {}
    ps = state.get("promotion_gate", {}) if isinstance(state.get("promotion_gate"), dict) else {}
    bundle_payload: Dict[str, Any] = {}
    if str(args.bundle_json).strip():
        bundle_payload = load_json_required(args.bundle_json, "bundle")
    strict_parity_eval = evaluate_strict_parity_gate(
        report=rep,
        args=args,
        bundle_payload=bundle_payload,
    )
    ev = eval_lex(rep, args, strict_parity_eval=strict_parity_eval)

    throughput_debug: Dict[str, Any] = {
        "enabled": False,
        "mode": "legacy_absolute",
        "throughput_observed": round(
            f64(ng(ev, ["kpis", "throughput_avg_trades"], 0.0), 0.0),
            6,
        ),
        "throughput_observed_core_filled": round(
            f64(ng(ev, ["kpis", "throughput_observed_core_filled"], 0.0), 0.0),
            6,
        ),
        "throughput_observed_legacy_avg": round(
            f64(ng(ev, ["kpis", "throughput_observed_legacy_avg"], 0.0), 0.0),
            6,
        ),
        "throughput_observed_source": str(
            ng(ev, ["kpis", "throughput_source"], "legacy_avg_fallback")
        ),
        "legacy_threshold": round(float(args.min_avg_trades), 6),
    }
    history_reports: List[Dict[str, Any]] = []
    history_paths: List[str] = []
    if str(args.bundle_json).strip():
        throughput_policy = ng(bundle_payload, ["phase3", "operations_dynamic_gate", "throughput_gate"], {})
        if isinstance(throughput_policy, dict) and bool(throughput_policy.get("enabled", False)):
            for path_value in args.history_report_jsons:
                path_text = str(path_value).strip()
                if not path_text:
                    continue
                rpath = resolve_repo_path(path_text)
                payload = load_json_or_none(rpath)
                if not isinstance(payload, dict):
                    raise RuntimeError(f"history report load failed: {path_text}")
                history_reports.append(payload)
                history_paths.append(str(rpath))
            throughput_debug = apply_dynamic_throughput_gate(
                report=rep,
                evaluation=ev,
                legacy_min_avg_trades=float(args.min_avg_trades),
                throughput_policy=throughput_policy,
                history_reports=history_reports,
                history_report_paths=history_paths,
            )

    prev_reason_scores = ps.get("last_failed_reason_scores", {}) if isinstance(ps.get("last_failed_reason_scores"), dict) else {}
    fail_rows = promotion_fail_rows(rep, ev, args, prev_reason_scores)
    top_fail_rows = fail_rows[:3]
    passed = bool(ev.get("overall_pass", False))
    strict_parity_hold = bool(strict_parity_eval.get("strict_parity_hold", False))
    stages = ["paper", "live10", "live30", "live100"]
    cur = str(args.current_stage).strip().lower()
    if cur not in stages:
        cur = "paper"
    c = max(0, i64(ps.get("consecutive_ok", 0)))
    c = c + 1 if passed else 0
    req = max(1, i64(args.min_consecutive_windows, 2))
    action = "hold"
    nxt = cur
    rollback = False
    if not passed:
        rollback = True
        nxt = "paper"
        action = "rollback_to_conservative_bundle"
    elif strict_parity_hold:
        rollback = False
        nxt = cur
        action = "hold_for_strict_parity"
        c = 0
    elif c >= req:
        idx = stages.index(cur)
        if idx < len(stages) - 1:
            nxt = stages[idx + 1]
            action = f"promote_{cur}_to_{nxt}"
            c = 0
        else:
            action = "hold_live100"
    strict_history = ps.get("strict_parity_fail_history", [])
    if not isinstance(strict_history, list):
        strict_history = []
    strict_history.append(bool(strict_parity_eval.get("strict_parity_fail", False)))
    strict_history = strict_history[-50:]
    recent_n = max(1, i64(strict_parity_eval.get("diagnosis_recent_n", 5), 5))
    recent_window = strict_history[-recent_n:]
    recent_fail_count = int(sum(1 for x in recent_window if bool(x)))
    always_fail_recent_n = bool(len(recent_window) == recent_n and recent_fail_count == recent_n)
    strict_parity_eval["recent_n"] = int(recent_n)
    strict_parity_eval["recent_fail_count"] = int(recent_fail_count)
    strict_parity_eval["always_fail_recent_n"] = bool(always_fail_recent_n)
    promotion_state = derive_promotion_state(
        rollback=bool(rollback),
        strict_parity_hold=bool(strict_parity_hold),
        current_stage=cur,
        next_stage=nxt,
    )

    ps.update({
        "last_updated_at": now_iso(),
        "current_stage": cur,
        "next_stage": nxt,
        "action": action,
        "rollback": rollback,
        "consecutive_ok": c,
        "min_consecutive_windows": req,
        "last_pass": passed,
        "last_failed_reason_scores": {
            str(row.get("reason_key", "")): f64(row.get("contribution_score", 0.0), 0.0)
            for row in fail_rows
            if str(row.get("reason_key", "")).strip()
        },
        "strict_parity_fail_history": strict_history,
        "promotion_state": str(promotion_state.get("promotion_state", "S0")),
        "strict_parity_hold": bool(strict_parity_hold),
        "paper_promotion_allowed": bool(promotion_state.get("paper_promotion_allowed", False)),
        "live_promotion_allowed": bool(promotion_state.get("live_promotion_allowed", False)),
    })
    state["promotion_gate"] = ps
    dump_json(spath, state)
    if str(args.strict_parity_diagnosis_json).strip():
        diagnosis_payload = dict(strict_parity_eval)
        diagnosis_payload.update(
            {
                "generated_at": now_iso(),
                "paper_report_json": str(resolve_repo_path(args.paper_report_json)),
                "bundle_json": str(resolve_repo_path(args.bundle_json)) if str(args.bundle_json).strip() else "",
                "history_report_jsons": history_paths,
            }
        )
        dump_json(resolve_repo_path(args.strict_parity_diagnosis_json), diagnosis_payload)
    if str(args.throughput_dynamic_debug_json).strip():
        debug_payload = dict(throughput_debug)
        debug_payload["generated_at"] = now_iso()
        debug_payload["paper_report_json"] = str(resolve_repo_path(args.paper_report_json))
        if str(args.bundle_json).strip():
            debug_payload["bundle_json"] = str(resolve_repo_path(args.bundle_json))
        debug_payload["history_report_jsons"] = history_paths
        dump_json(resolve_repo_path(args.throughput_dynamic_debug_json), debug_payload)
    if str(args.a21_throughput_source_debug_json).strip():
        a21_payload = {
            "generated_at": now_iso(),
            "source": str(throughput_debug.get("throughput_observed_source", "")),
            "throughput_observed_core_filled": round(
                f64(throughput_debug.get("throughput_observed_core_filled", 0.0), 0.0),
                6,
            ),
            "throughput_observed_legacy_avg": round(
                f64(throughput_debug.get("throughput_observed_legacy_avg", 0.0), 0.0),
                6,
            ),
            "lookup_limit": round(f64(throughput_debug.get("lookup_limit", args.min_avg_trades), 0.0), 6),
            "dist_limit": round(f64(throughput_debug.get("dist_limit", args.min_avg_trades), 0.0), 6),
            "final_limit": round(f64(throughput_debug.get("final_limit", args.min_avg_trades), 0.0), 6),
            "throughput_ok": bool(throughput_debug.get("throughput_ok", False)),
            "throughput_hold": bool(throughput_debug.get("throughput_hold", False)),
            "paper_report_json": str(resolve_repo_path(args.paper_report_json)),
            "bundle_json": str(resolve_repo_path(args.bundle_json)) if str(args.bundle_json).strip() else "",
            "history_report_jsons": history_paths,
        }
        dump_json(resolve_repo_path(args.a21_throughput_source_debug_json), a21_payload)
    dump_json(
        resolve_repo_path(args.output_json),
        {
            "mode": "promotion_gate",
            "generated_at": now_iso(),
            "current_stage": cur,
            "next_stage": nxt,
            "action": action,
            "rollback": rollback,
            "consecutive_ok": c,
            "promotion_state": str(promotion_state.get("promotion_state", "S0")),
            "strict_parity_hold": bool(strict_parity_hold),
            "paper_promotion_allowed": bool(promotion_state.get("paper_promotion_allowed", False)),
            "live_promotion_allowed": bool(promotion_state.get("live_promotion_allowed", False)),
            "evaluation": ev,
            "top_failed_reasons": top_fail_rows,
            "strict_parity_gate": strict_parity_eval,
            "throughput_dynamic_gate": throughput_debug,
            "state_path": str(spath),
        },
    )
    print(f"[Phase34Ops] mode=promotion current={cur} next={nxt} action={action}", flush=True)
    return 0 if not rollback else 2


def cmd_diff(args: argparse.Namespace) -> int:
    a = load_json_or_none(resolve_repo_path(args.base_bundle_json))
    b = load_json_or_none(resolve_repo_path(args.candidate_bundle_json))
    if not isinstance(a, dict) or not isinstance(b, dict):
        raise RuntimeError("base/candidate bundles required")
    rows = bundle_diff(a, b)
    dump_json(resolve_repo_path(args.output_json), {"mode": "bundle_diff", "generated_at": now_iso(), "base_bundle_json": str(resolve_repo_path(args.base_bundle_json)), "candidate_bundle_json": str(resolve_repo_path(args.candidate_bundle_json)), "change_count": len(rows), "changes": rows})
    print(f"[Phase34Ops] mode=bundle_diff changes={len(rows)}", flush=True)
    return 0


def cmd_a6_required_ev_step(args: argparse.Namespace) -> int:
    bundle = load_json_required(args.bundle_json, "bundle")
    dev_report = load_json_required(args.development_report_json, "development_report")
    val_report = load_json_required(args.validation_report_json, "validation_report")

    phase3 = bundle.get("phase3", {})
    if not isinstance(phase3, dict):
        phase3 = {}
    ops = phase3.get("operations_control", {})
    if not isinstance(ops, dict):
        ops = {}
    dynamic_gate = phase3.get("operations_dynamic_gate", {})
    if not isinstance(dynamic_gate, dict):
        dynamic_gate = {}
    step_policy = dynamic_gate.get("required_ev_offset_step_policy", {})
    if not isinstance(step_policy, dict):
        step_policy = {}

    prefer_core_direct = bool(step_policy.get("history_use_core_window_direct", True))
    fallback_on_core_zero = bool(
        step_policy.get("fallback_to_split_report_on_core_zero", True)
    )
    q_cut = clamp(
        f64(step_policy.get("ev_step_quantile", f64(args.ev_step_quantile, 0.15)), 0.15),
        0.01,
        0.49,
    )
    clamp_k = max(
        0.0,
        f64(step_policy.get("delta_clamp_median_k", f64(args.delta_clamp_median_k, 0.5)), 0.5),
    )
    clamp_enabled = bool(step_policy.get("delta_clamp_enabled", True))
    reference_mode = str(step_policy.get("ev_floor_reference_mode", "required_ev_offset")).strip().lower()
    direction = str(step_policy.get("conservative_direction", "increase")).strip().lower() or "increase"
    if direction not in ("increase", "decrease"):
        direction = "increase"

    dev_dist = extract_phase3_pass_ev_distribution(
        dev_report,
        prefer_core_direct=prefer_core_direct,
    )
    val_dist = extract_phase3_pass_ev_distribution(
        val_report,
        prefer_core_direct=prefer_core_direct,
    )
    if fallback_on_core_zero:
        if int(dev_dist.get("sample_size_total", 0)) <= 0:
            fallback_dev_dist = extract_phase3_pass_ev_distribution(
                dev_report,
                prefer_core_direct=False,
            )
            if int(fallback_dev_dist.get("sample_size_total", 0)) > 0:
                fallback_dev_dist["source"] = (
                    str(fallback_dev_dist.get("source", "")).strip()
                    or "report_fallback_core_zero"
                )
                dev_dist = fallback_dev_dist
        if int(val_dist.get("sample_size_total", 0)) <= 0:
            fallback_val_dist = extract_phase3_pass_ev_distribution(
                val_report,
                prefer_core_direct=False,
            )
            if int(fallback_val_dist.get("sample_size_total", 0)) > 0:
                fallback_val_dist["source"] = (
                    str(fallback_val_dist.get("source", "")).strip()
                    or "report_fallback_core_zero"
                )
                val_dist = fallback_val_dist
    dev_samples = dev_dist.get("samples_bps", []) if isinstance(dev_dist.get("samples_bps", []), list) else []
    val_samples = val_dist.get("samples_bps", []) if isinstance(val_dist.get("samples_bps", []), list) else []
    ev_samples_bps: List[float] = []
    for x in list(dev_samples) + list(val_samples):
        v = f64(x, float("nan"))
        if math.isfinite(v):
            ev_samples_bps.append(float(v))
    ev_samples_pct = [float(x) / 10000.0 for x in ev_samples_bps]

    sample_sizes_history = [
        float(dev_dist.get("sample_size_total", 0)),
        float(val_dist.get("sample_size_total", 0)),
    ]
    sample_sizes_history = [x for x in sample_sizes_history if math.isfinite(x) and x > 0.0]
    hostile_labels = dynamic_gate.get("hostile_regime_labels", [])
    if not isinstance(hostile_labels, list):
        hostile_labels = []
    context_payload = pick_eval_payload(val_report, prefer_core_direct)
    context = phase3_context(context_payload, [str(x) for x in hostile_labels])

    min_samples_lookup_default = max(
        1.0,
        f64(step_policy.get("min_samples_lookup_default", f64(args.min_samples_lookup_default, 32)), 32.0),
    )
    min_samples_lookup = lookup_limit(
        step_policy.get("min_samples_lookup", []),
        regime=str(context.get("regime", "ANY")),
        volatility_bucket=str(context.get("volatility_bucket", "ANY")),
        liquidity_bucket=str(context.get("liquidity_bucket", "ANY")),
        default_limit=min_samples_lookup_default,
    )
    min_samples_dist_p = clamp(
        f64(step_policy.get("min_samples_dist_quantile_p", f64(args.min_samples_dist_quantile_p, 0.2)), 0.2),
        0.0,
        1.0,
    )
    min_samples_dist = (
        quantile(sample_sizes_history, min_samples_dist_p)
        if sample_sizes_history
        else float(min_samples_lookup_default)
    )
    min_samples_required = int(math.ceil(max(float(min_samples_lookup), float(min_samples_dist))))

    sample_size = len(ev_samples_pct)
    ev_cut_pct = quantile(ev_samples_pct, q_cut) if ev_samples_pct else 0.0
    ev_median_pct = quantile(ev_samples_pct, 0.5) if ev_samples_pct else 0.0
    quantiles_pct = quantile_summary(ev_samples_pct)
    quantiles_bps = {k: (v * 10000.0) for k, v in quantiles_pct.items()}

    required_ev_offset_old = f64(ops.get("required_ev_offset", 0.0), 0.0)
    if reference_mode in ("required_ev_offset", "ops_required_ev_offset", "current_offset"):
        ev_floor_reference = float(required_ev_offset_old)
        ev_floor_reference_mode = "required_ev_offset"
    else:
        ev_floor_reference = 0.0
        ev_floor_reference_mode = "zero"

    notes: List[str] = []
    if sample_size < min_samples_required:
        raw_delta = 0.0
        clamped_delta = 0.0
        clamp_max = 0.0
        notes.append("insufficient_sample_size_delta_forced_zero")
    else:
        raw_delta = max(0.0, float(ev_cut_pct) - float(ev_floor_reference))
        clamp_max = max(0.0, abs(float(ev_median_pct)) * float(clamp_k))
        clamped_delta = min(float(raw_delta), float(clamp_max)) if clamp_enabled else float(raw_delta)
        if clamp_enabled and raw_delta > clamped_delta:
            notes.append("delta_clamped_by_median_k")

    if direction == "increase":
        required_ev_offset_new = float(required_ev_offset_old) + float(clamped_delta)
    else:
        required_ev_offset_new = float(required_ev_offset_old) - float(clamped_delta)
    conservative_direction_ok = required_ev_offset_new >= required_ev_offset_old
    if not conservative_direction_ok:
        notes.append("direction_not_conservative_check_failed")

    out_bundle = copy.deepcopy(bundle)
    out_phase3 = out_bundle.get("phase3", {})
    if not isinstance(out_phase3, dict):
        out_phase3 = {}
    out_ops = out_phase3.get("operations_control", {})
    if not isinstance(out_ops, dict):
        out_ops = {}
    out_ops["required_ev_offset"] = float(required_ev_offset_new)
    out_phase3["operations_control"] = normalize_ops(out_ops)
    out_bundle["phase3"] = out_phase3

    release_meta = out_bundle.get("release_meta", {})
    if not isinstance(release_meta, dict):
        release_meta = {}
    parent_version = str(release_meta.get("version", out_bundle.get("version", ""))).strip()
    release_meta.update(
        {
            "version": str(args.release_version).strip() or f"a6_required_ev_step_{now_epoch()}",
            "created_at": now_iso(),
            "parent_version": parent_version,
            "mode": "a6_required_ev_offset_distribution_step",
            "change_summary": [
                (
                    "required_ev_offset:"
                    f"{required_ev_offset_old}->{required_ev_offset_new}"
                )
            ],
        }
    )
    out_bundle["release_meta"] = release_meta

    dump_json(resolve_repo_path(args.output_bundle_json), out_bundle)

    calc_report = {
        "mode": "a6_required_ev_offset_distribution_step",
        "generated_at": now_iso(),
        "bundle_input": str(resolve_repo_path(args.bundle_json)),
        "bundle_output": str(resolve_repo_path(args.output_bundle_json)),
        "development_report_json": str(resolve_repo_path(args.development_report_json)),
        "validation_report_json": str(resolve_repo_path(args.validation_report_json)),
        "q_cut": float(q_cut),
        "sample_size": int(sample_size),
        "sample_size_required": int(min_samples_required),
        "sample_size_lookup_component": float(min_samples_lookup),
        "sample_size_dist_component": float(min_samples_dist),
        "sample_size_dist_quantile_p": float(min_samples_dist_p),
        "ev_distribution_quantiles_pct": {k: round(float(v), 10) for k, v in quantiles_pct.items()},
        "ev_distribution_quantiles_bps": {k: round(float(v), 6) for k, v in quantiles_bps.items()},
        "ev_cut_pct": round(float(ev_cut_pct), 10),
        "ev_cut_bps": round(float(ev_cut_pct) * 10000.0, 6),
        "ev_median_pct": round(float(ev_median_pct), 10),
        "ev_median_bps": round(float(ev_median_pct) * 10000.0, 6),
        "ev_floor_reference": {
            "mode": ev_floor_reference_mode,
            "value_pct": round(float(ev_floor_reference), 10),
            "value_bps": round(float(ev_floor_reference) * 10000.0, 6),
        },
        "raw_delta": {
            "pct": round(float(raw_delta), 10),
            "bps": round(float(raw_delta) * 10000.0, 6),
        },
        "delta_clamp": {
            "enabled": bool(clamp_enabled),
            "median_k": float(clamp_k),
            "clamp_max_pct": round(float(clamp_max), 10),
            "clamp_max_bps": round(float(clamp_max) * 10000.0, 6),
            "clamped_delta_pct": round(float(clamped_delta), 10),
            "clamped_delta_bps": round(float(clamped_delta) * 10000.0, 6),
        },
        "required_ev_offset_old": round(float(required_ev_offset_old), 10),
        "required_ev_offset_new": round(float(required_ev_offset_new), 10),
        "conservative_direction": direction,
        "required_ev_increased": bool(conservative_direction_ok),
        "context": context,
        "history_sources": {
            "development": {
                "enabled": bool(dev_dist.get("enabled", False)),
                "sample_size_total": int(dev_dist.get("sample_size_total", 0)),
                "source": str(dev_dist.get("source", "")),
            },
            "validation": {
                "enabled": bool(val_dist.get("enabled", False)),
                "sample_size_total": int(val_dist.get("sample_size_total", 0)),
                "source": str(val_dist.get("source", "")),
            },
            "prefer_core_window_direct": bool(prefer_core_direct),
            "fallback_to_split_report_on_core_zero": bool(fallback_on_core_zero),
        },
        "notes": notes,
    }
    dump_json(resolve_repo_path(args.output_json), calc_report)
    print(
        "[Phase34Ops] mode=a6_required_ev_step "
        f"sample_size={sample_size} required={min_samples_required} "
        f"delta_bps={round(float(clamped_delta) * 10000.0, 6)} "
        f"offset_old={round(required_ev_offset_old, 10)} "
        f"offset_new={round(required_ev_offset_new, 10)}",
        flush=True,
    )
    return 0


def set_nested_value(root: Dict[str, Any], path: List[str], value: Any) -> None:
    cur = root
    for key in path[:-1]:
        child = cur.get(key)
        if not isinstance(child, dict):
            child = {}
            cur[key] = child
        cur = child
    cur[path[-1]] = value


def get_nested_value(root: Dict[str, Any], path: List[str], default: Any) -> Any:
    cur: Any = root
    for key in path:
        if not isinstance(cur, dict):
            return default
        cur = cur.get(key)
    return default if cur is None else cur


def build_expectancy_proxy_breakdown(
    report: Dict[str, Any],
    bundle: Dict[str, Any],
    *,
    min_avg_trades: float,
    max_drawdown_pct: float,
) -> Dict[str, Any]:
    agg = report.get("aggregates", {}) if isinstance(report.get("aggregates"), dict) else {}
    aad = ng(report, ["adaptive_validation", "aggregates"], {})
    if not isinstance(aad, dict):
        aad = {}
    loss_tail = aad.get("loss_tail_aggregate", {})
    if not isinstance(loss_tail, dict):
        loss_tail = {}
    phase3_gate = ng(bundle, ["phase3", "operations_dynamic_gate"], {})
    if not isinstance(phase3_gate, dict):
        phase3_gate = {}
    prefer_core = bool(phase3_gate.get("prefer_core_window_direct", True))
    payload = pick_eval_payload(report, prefer_core)
    fallback_on_core_zero = bool(phase3_gate.get("fallback_to_split_report_on_core_zero", True))
    source_mode = "core_window_direct" if payload is not report else "report"
    if fallback_on_core_zero and payload is not report:
        funnel_probe = phase3_funnel(payload)
        if funnel_probe["candidate_total"] <= 0:
            payload = report
            source_mode = "report_fallback_core_zero"
    hostile_labels = phase3_gate.get("hostile_regime_labels", [])
    if not isinstance(hostile_labels, list):
        hostile_labels = []
    context = phase3_context(payload, [str(x) for x in hostile_labels])
    funnel = phase3_funnel(payload)

    avg_expectancy_krw = f64(agg.get("avg_expectancy_krw", 0.0), 0.0)
    avg_fee_krw = max(0.0, f64(agg.get("avg_fee_krw", 0.0), 0.0))
    avg_total_trades = max(0.0, f64(agg.get("avg_total_trades", 0.0), 0.0))
    peak_max_drawdown_pct = max(0.0, f64(agg.get("peak_max_drawdown_pct", 0.0), 0.0))
    tail_concentration = max(0.0, f64(loss_tail.get("avg_top3_loss_concentration", 0.0), 0.0))
    tail_exceed_rate = float(funnel.get("reject_cost_tail_fail", 0)) / float(
        max(1, int(funnel.get("candidate_total", 0)))
    )
    avg_downtrend_trade_share = max(0.0, f64(aad.get("avg_downtrend_trade_share", 0.0), 0.0))
    avg_downtrend_loss_per_trade_krw = max(
        0.0,
        f64(aad.get("avg_downtrend_loss_per_trade_krw", 0.0), 0.0),
    )
    hostile_share = max(0.0, f64(context.get("hostile_share", 0.0), 0.0))

    frontier_k_cost_tail = max(0.0, f64(ng(bundle, ["phase3", "frontier", "k_cost_tail"], 0.2), 0.2))
    tail_lambda = max(0.0, f64(ng(bundle, ["phase4", "portfolio_allocator", "lambda_tail"], 1.0), 1.0))
    gross_expectancy_proxy_krw = float(avg_expectancy_krw) + float(avg_fee_krw)
    cost_component = -float(avg_fee_krw) * max(0.0, frontier_k_cost_tail)
    tail_scale_krw = max(1.0, abs(float(avg_expectancy_krw))) * max(0.25, 0.5 * tail_lambda)
    tail_exceed_scale_krw = max(0.5, 0.25 * max(1.0, abs(float(avg_expectancy_krw))))
    tail_penalty_component = -(
        float(tail_concentration) * float(tail_scale_krw)
        + float(tail_exceed_rate) * float(tail_exceed_scale_krw)
    )
    drawdown_scale_krw = max(0.5, 0.35 * max(1.0, abs(float(avg_expectancy_krw))))
    drawdown_or_loss_streak_component = -(
        (float(peak_max_drawdown_pct) / max(1.0, float(max_drawdown_pct))) * float(drawdown_scale_krw)
    )
    regime_share_scale_krw = max(0.5, 0.25 * max(1.0, abs(float(avg_expectancy_krw))))
    regime_loss_scale_krw = 0.5
    regime_hostility_component = -(
        float(hostile_share) * float(regime_share_scale_krw)
        + max(0.0, float(avg_downtrend_trade_share) - 0.35) * float(regime_share_scale_krw)
        + float(avg_downtrend_loss_per_trade_krw) * float(regime_loss_scale_krw)
    )
    confidence_scale_krw = max(0.5, 0.5 * max(1.0, abs(float(avg_expectancy_krw))))
    confidence_shortfall_ratio = max(
        0.0,
        (float(min_avg_trades) - float(avg_total_trades)) / max(1.0, float(min_avg_trades)),
    )
    confidence_component = -float(confidence_shortfall_ratio) * float(confidence_scale_krw)
    expectancy_raw_component = float(avg_expectancy_krw)

    expectancy_proxy_total = (
        float(expectancy_raw_component)
        + float(cost_component)
        + float(tail_penalty_component)
        + float(drawdown_or_loss_streak_component)
        + float(regime_hostility_component)
        + float(confidence_component)
    )
    components = {
        "expectancy_raw_component": {
            "value": round(float(expectancy_raw_component), 6),
            "unit": "krw_per_trade",
            "source": ["aggregates.avg_expectancy_krw"],
            "scaling": {"scale": 1.0, "note": "net expectancy proxy"},
        },
        "cost_component": {
            "value": round(float(cost_component), 6),
            "unit": "krw_per_trade",
            "source": ["aggregates.avg_fee_krw", "phase3.frontier.k_cost_tail"],
            "scaling": {
                "avg_fee_krw": round(float(avg_fee_krw), 6),
                "k_cost_tail": round(float(frontier_k_cost_tail), 6),
                "formula": "-avg_fee_krw * k_cost_tail",
            },
        },
        "tail_penalty_component": {
            "value": round(float(tail_penalty_component), 6),
            "unit": "krw_proxy",
            "source": [
                "adaptive_validation.aggregates.loss_tail_aggregate.avg_top3_loss_concentration",
                "diagnostics.aggregate.phase3_diagnostics_v2.funnel_breakdown.reject_cost_tail_fail",
            ],
            "scaling": {
                "tail_concentration": round(float(tail_concentration), 6),
                "tail_exceed_rate": round(float(tail_exceed_rate), 6),
                "tail_scale_krw": round(float(tail_scale_krw), 6),
                "tail_exceed_scale_krw": round(float(tail_exceed_scale_krw), 6),
                "lambda_tail": round(float(tail_lambda), 6),
                "formula": "-(tail_concentration*tail_scale_krw + tail_exceed_rate*tail_exceed_scale_krw)",
            },
        },
        "drawdown_or_loss_streak_component": {
            "value": round(float(drawdown_or_loss_streak_component), 6),
            "unit": "krw_proxy",
            "source": ["aggregates.peak_max_drawdown_pct"],
            "scaling": {
                "peak_max_drawdown_pct": round(float(peak_max_drawdown_pct), 6),
                "max_drawdown_pct_limit": round(float(max_drawdown_pct), 6),
                "drawdown_scale_krw": round(float(drawdown_scale_krw), 6),
                "formula": "-(peak_max_drawdown_pct/max_drawdown_pct_limit)*drawdown_scale_krw",
            },
        },
        "regime_hostility_component": {
            "value": round(float(regime_hostility_component), 6),
            "unit": "krw_proxy",
            "source": [
                "adaptive_validation.aggregates.avg_downtrend_trade_share",
                "adaptive_validation.aggregates.avg_downtrend_loss_per_trade_krw",
                "diagnostics.aggregate.phase3_diagnostics_v2.pass_rate_by_regime",
            ],
            "scaling": {
                "hostile_share": round(float(hostile_share), 6),
                "avg_downtrend_trade_share": round(float(avg_downtrend_trade_share), 6),
                "avg_downtrend_loss_per_trade_krw": round(float(avg_downtrend_loss_per_trade_krw), 6),
                "regime_share_scale_krw": round(float(regime_share_scale_krw), 6),
                "regime_loss_scale_krw": round(float(regime_loss_scale_krw), 6),
                "formula": "-(hostile + downtrend_share_excess + downtrend_loss)",
            },
        },
        "confidence_component": {
            "value": round(float(confidence_component), 6),
            "unit": "krw_proxy",
            "source": ["aggregates.avg_total_trades"],
            "scaling": {
                "avg_total_trades": round(float(avg_total_trades), 6),
                "min_avg_trades": round(float(min_avg_trades), 6),
                "shortfall_ratio": round(float(confidence_shortfall_ratio), 6),
                "confidence_scale_krw": round(float(confidence_scale_krw), 6),
                "formula": "-shortfall_ratio * confidence_scale_krw",
            },
        },
    }
    component_rows: List[Dict[str, Any]] = []
    for name, payload in components.items():
        if not isinstance(payload, dict):
            continue
        component_rows.append(
            {
                "name": str(name),
                "value": round(float(f64(payload.get("value", 0.0), 0.0)), 6),
            }
        )
    component_rows.sort(key=lambda item: (f64(item.get("value", 0.0), 0.0), str(item.get("name", ""))))
    top3_negative_components = [
        row for row in component_rows if f64(row.get("value", 0.0), 0.0) < 0.0
    ][:3]
    top1 = top3_negative_components[0]["name"] if top3_negative_components else ""
    return {
        "expectancy_proxy_total": round(float(expectancy_proxy_total), 6),
        "expectancy_proxy_threshold": 0.0,
        "components": components,
        "component_values": component_rows,
        "top3_negative_components": top3_negative_components,
        "top1_negative_component": str(top1),
        "context": {
            "regime": str(context.get("regime", "ANY")),
            "volatility_bucket": str(context.get("volatility_bucket", "ANY")),
            "liquidity_bucket": str(context.get("liquidity_bucket", "ANY")),
            "hostile_share": round(float(hostile_share), 6),
            "source_mode": source_mode,
        },
    }


def apply_a7_single_axis_fix(
    bundle: Dict[str, Any],
    report: Dict[str, Any],
    breakdown: Dict[str, Any],
) -> Dict[str, Any]:
    top1 = str(breakdown.get("top1_negative_component", "")).strip()
    context = breakdown.get("context", {}) if isinstance(breakdown.get("context"), dict) else {}
    regime = str(context.get("regime", "ANY")).strip().upper()

    candidate_bundle = copy.deepcopy(bundle)
    case_id = "A7-4"
    action_label = "regime_cut_step1"
    notes: List[str] = []
    path: List[str] = []
    old_value = 0.0
    new_value = 0.0

    if top1 == "cost_component":
        case_id = "A7-1"
        action_label = "phase3_frontier_k_cost_tail_step_up"
        path = ["phase3", "frontier", "k_cost_tail"]
        old_value = f64(get_nested_value(candidate_bundle, path, 0.2), 0.2)
        step = max(0.02, abs(old_value) * 0.15)
        new_value = min(1.0, old_value + step)
    elif top1 == "tail_penalty_component":
        case_id = "A7-2"
        phase4_enabled = bool(get_nested_value(candidate_bundle, ["phase4", "correlation_control", "enabled"], False))
        has_cluster_caps = bool(get_nested_value(candidate_bundle, ["phase4", "correlation_control", "cluster_caps"], {}))
        if phase4_enabled and has_cluster_caps:
            action_label = "phase4_default_cluster_cap_tighten_step1"
            path = ["phase4", "correlation_control", "default_cluster_cap"]
            old_value = f64(get_nested_value(candidate_bundle, path, 1.0), 1.0)
            new_value = max(0.08, old_value - 0.02)
        else:
            action_label = "phase3_cost_tail_reject_threshold_tighten_step1_fallback"
            path = ["phase3", "frontier", "cost_tail_reject_threshold_pct"]
            old_value = f64(get_nested_value(candidate_bundle, path, 0.03), 0.03)
            new_value = max(0.005, old_value - 0.002)
            notes.append("phase4_cluster_cap_not_active_fallback_to_phase3_tail_threshold")
    elif top1 == "regime_hostility_component":
        case_id = "A7-3"
        action_label = "hostile_regime_budget_tighten_step1"
        path = ["phase3", "manager_filter", "scan_prefilter_margin_add_hostile"]
        old_value = f64(get_nested_value(candidate_bundle, path, 0.015), 0.015)
        new_value = min(0.15, old_value + 0.005)
    elif top1 == "expectancy_raw_component":
        case_id = "A7-4"
        action_label = "regime_cut_ranging_strength_step1"
        if regime == "RANGING":
            path = ["phase3", "manager_filter", "base_min_strength_ranging"]
            old_value = f64(get_nested_value(candidate_bundle, path, 0.43), 0.43)
            new_value = min(0.98, old_value + 0.01)
        else:
            path = ["phase3", "manager_filter", "base_min_strength_default"]
            old_value = f64(get_nested_value(candidate_bundle, path, 0.4), 0.4)
            new_value = min(0.98, old_value + 0.01)
            notes.append("dominant_regime_not_ranging_fallback_to_base_min_strength_default")
    else:
        case_id = "A7-3"
        action_label = "hostile_regime_budget_tighten_step1_fallback"
        path = ["phase3", "manager_filter", "scan_prefilter_margin_add_hostile"]
        old_value = f64(get_nested_value(candidate_bundle, path, 0.015), 0.015)
        new_value = min(0.15, old_value + 0.005)
        notes.append("top1_component_missing_fallback_applied")

    if not path:
        raise RuntimeError("A7 single-axis fix path resolution failed")
    if abs(new_value - old_value) <= 1e-12:
        notes.append("selected_axis_noop_after_clamp")

    set_nested_value(candidate_bundle, path, float(new_value))
    release_meta = candidate_bundle.get("release_meta", {})
    if not isinstance(release_meta, dict):
        release_meta = {}
    parent_version = str(release_meta.get("version", candidate_bundle.get("version", ""))).strip()
    release_meta.update(
        {
            "created_at": now_iso(),
            "parent_version": parent_version,
            "mode": "a7_expectancy_component_single_axis_fix",
            "change_summary": [
                f"{'.'.join(path)}:{old_value}->{new_value}",
            ],
        }
    )
    candidate_bundle["release_meta"] = release_meta
    return {
        "bundle": candidate_bundle,
        "selected_case": case_id,
        "selected_component": top1,
        "action_label": action_label,
        "changed_path": ".".join(path),
        "old_value": float(old_value),
        "new_value": float(new_value),
        "delta": float(new_value - old_value),
        "notes": notes,
    }


def cmd_a7_expectancy_component_fix(args: argparse.Namespace) -> int:
    bundle = load_json_required(args.bundle_json, "bundle")
    quarantine_report = load_json_required(args.quarantine_report_json, "quarantine_report")
    breakdown = build_expectancy_proxy_breakdown(
        quarantine_report,
        bundle,
        min_avg_trades=float(args.min_avg_trades),
        max_drawdown_pct=float(args.max_drawdown_pct),
    )
    fix = apply_a7_single_axis_fix(bundle, quarantine_report, breakdown)
    out_bundle = fix.get("bundle", {})
    if not isinstance(out_bundle, dict):
        raise RuntimeError("A7 failed to build output bundle")
    release_meta = out_bundle.get("release_meta", {})
    if not isinstance(release_meta, dict):
        release_meta = {}
    release_meta["version"] = str(args.release_version).strip() or f"a7_expectancy_component_fix_{now_epoch()}"
    out_bundle["release_meta"] = release_meta
    dump_json(resolve_repo_path(args.output_bundle_json), out_bundle)

    out_report = {
        "mode": "a7_expectancy_proxy_breakdown_single_axis_fix",
        "generated_at": now_iso(),
        "bundle_input": str(resolve_repo_path(args.bundle_json)),
        "bundle_output": str(resolve_repo_path(args.output_bundle_json)),
        "quarantine_report_json": str(resolve_repo_path(args.quarantine_report_json)),
        "expectancy_proxy_total": breakdown.get("expectancy_proxy_total", 0.0),
        "expectancy_proxy_threshold": breakdown.get("expectancy_proxy_threshold", 0.0),
        "components": breakdown.get("components", {}),
        "component_values": breakdown.get("component_values", []),
        "top3_negative_components": breakdown.get("top3_negative_components", []),
        "top1_negative_component": breakdown.get("top1_negative_component", ""),
        "normalization_scaling_info": {
            "component_units": {
                "expectancy_raw_component": "krw_per_trade",
                "cost_component": "krw_per_trade",
                "tail_penalty_component": "krw_proxy",
                "drawdown_or_loss_streak_component": "krw_proxy",
                "regime_hostility_component": "krw_proxy",
                "confidence_component": "krw_proxy",
            },
            "context": breakdown.get("context", {}),
            "thresholds": {
                "min_avg_trades": float(args.min_avg_trades),
                "max_drawdown_pct": float(args.max_drawdown_pct),
            },
        },
        "selected_fix": {
            "selected_case": fix.get("selected_case", ""),
            "selected_component": fix.get("selected_component", ""),
            "action_label": fix.get("action_label", ""),
            "changed_path": fix.get("changed_path", ""),
            "old_value": round(f64(fix.get("old_value", 0.0), 0.0), 10),
            "new_value": round(f64(fix.get("new_value", 0.0), 0.0), 10),
            "delta": round(f64(fix.get("delta", 0.0), 0.0), 10),
            "notes": fix.get("notes", []),
        },
    }
    dump_json(resolve_repo_path(args.output_json), out_report)
    print(
        "[Phase34Ops] mode=a7_expectancy_component_fix "
        f"top1={str(breakdown.get('top1_negative_component', ''))} "
        f"case={str(fix.get('selected_case', ''))} "
        f"path={str(fix.get('changed_path', ''))} "
        f"delta={round(f64(fix.get('delta', 0.0), 0.0), 10)}",
        flush=True,
    )
    return 0


def load_json_required(path_value: str, label: str) -> Dict[str, Any]:
    payload = load_json_or_none(resolve_repo_path(path_value))
    if not isinstance(payload, dict):
        raise RuntimeError(f"{label} load failed: {path_value}")
    return payload


def cmd_dynamic_phase3_gate(args: argparse.Namespace) -> int:
    bundle = load_json_required(args.bundle_json, "bundle")
    dev_report = load_json_required(args.development_report_json, "development_report")
    val_report = load_json_required(args.validation_report_json, "validation_report")
    qua_report = load_json_required(args.quarantine_report_json, "quarantine_report")
    phase3_gate = ng(bundle, ["phase3", "operations_dynamic_gate"], {})
    if not isinstance(phase3_gate, dict):
        raise RuntimeError("bundle missing phase3.operations_dynamic_gate")
    if not bool(phase3_gate.get("enabled", False)):
        raise RuntimeError("phase3.operations_dynamic_gate.enabled=false")

    prefer_core = bool(phase3_gate.get("prefer_core_window_direct", True))
    dev_payload = pick_eval_payload(dev_report, prefer_core)
    val_payload = pick_eval_payload(val_report, prefer_core)
    qua_payload = pick_eval_payload(qua_report, prefer_core)
    source_tags = {
        "development": "core_window_direct" if dev_payload is not dev_report else "report",
        "validation": "core_window_direct" if val_payload is not val_report else "report",
        "quarantine": "core_window_direct" if qua_payload is not qua_report else "report",
    }
    fallback_on_core_zero = bool(phase3_gate.get("fallback_to_split_report_on_core_zero", True))
    if fallback_on_core_zero:
        if dev_payload is not dev_report and phase3_funnel(dev_payload)["candidate_total"] <= 0:
            dev_payload = dev_report
            source_tags["development"] = "report_fallback_core_zero"
        if val_payload is not val_report and phase3_funnel(val_payload)["candidate_total"] <= 0:
            val_payload = val_report
            source_tags["validation"] = "report_fallback_core_zero"
        if qua_payload is not qua_report and phase3_funnel(qua_payload)["candidate_total"] <= 0:
            qua_payload = qua_report
            source_tags["quarantine"] = "report_fallback_core_zero"

    hostile_labels = phase3_gate.get("hostile_regime_labels", [])
    if not isinstance(hostile_labels, list):
        hostile_labels = []
    context = phase3_context(val_payload, [str(x) for x in hostile_labels])

    dev_funnel = phase3_funnel(dev_payload)
    val_funnel = phase3_funnel(val_payload)
    qua_funnel = phase3_funnel(qua_payload)
    dev_pass_rate = float(dev_funnel["pass_total"]) / float(max(1, dev_funnel["candidate_total"]))
    val_pass_rate = float(val_funnel["pass_total"]) / float(max(1, val_funnel["candidate_total"]))
    qua_pass_rate = float(qua_funnel["pass_total"]) / float(max(1, qua_funnel["candidate_total"]))
    market_count = max(1, i64(ng(val_payload, ["dataset_count"], 0), 0))

    metrics_policy = phase3_gate.get("metrics", {})
    if not isinstance(metrics_policy, dict):
        metrics_policy = {}
    required_metrics = phase3_gate.get("required_metric_keys", [])
    if not isinstance(required_metrics, list) or not required_metrics:
        required_metrics = ["tail_concentration", "tail_exceed_rate"]
    global_combine_mode = str(phase3_gate.get("limit_combine_mode", "min")).strip().lower() or "min"

    metric_rows: List[Dict[str, Any]] = []
    metric_reasons: List[str] = []
    metric_soft_fail_reasons: List[str] = []
    metric_hold_reasons: List[str] = []
    total_delta_required_ev = 0.0
    for metric_name in [str(x).strip() for x in required_metrics if str(x).strip()]:
        policy = metrics_policy.get(metric_name, {})
        if not isinstance(policy, dict):
            policy = {}
        if not bool(policy.get("enabled", True)):
            continue
        dev_metric = metric_value(dev_payload, metric_name, policy)
        val_metric = metric_value(val_payload, metric_name, policy)
        qua_metric = metric_value(qua_payload, metric_name, policy)
        dev_conf = metric_confidence(dev_payload, metric_name, policy)
        val_conf = metric_confidence(val_payload, metric_name, policy)
        qua_conf = metric_confidence(qua_payload, metric_name, policy)
        row = evaluate_dynamic_metric(
            metric_name=metric_name,
            metric_policy=policy,
            context=context,
            observed_value=qua_metric,
            history_values=[dev_metric, val_metric],
            observed_confidence=qua_conf,
            history_confidences=[dev_conf, val_conf],
            global_combine_mode=global_combine_mode,
        )
        row["history_values"] = [float(dev_metric), float(val_metric)]
        row["history_confidence"] = [dev_conf, val_conf]
        row["observed_confidence"] = qua_conf
        row["observed_split"] = "quarantine"
        metric_rows.append(row)
        total_delta_required_ev += float(row.get("delta_required_ev", 0.0))
        if bool(row.get("hard_fail", False)):
            metric_reasons.append(str(row.get("reason", f"{metric_name}_limit_exceeded")))
        elif bool(row.get("promotion_hold", False)):
            metric_hold_reasons.append(
                str(row.get("reason", f"{metric_name}_low_confidence_promotion_hold"))
            )
        elif bool(row.get("soft_fail", False)):
            metric_soft_fail_reasons.append(
                str(row.get("reason", f"{metric_name}_low_confidence_soft_fail"))
            )

    tail_gate_pass = not metric_reasons
    promotion_hold = bool(metric_hold_reasons)

    pass_band_policy = phase3_gate.get("pass_rate_band", {})
    if not isinstance(pass_band_policy, dict):
        pass_band_policy = {}
    val_band_policy = pass_band_policy.get("validation", {})
    if not isinstance(val_band_policy, dict):
        val_band_policy = {}
    qua_band_policy = pass_band_policy.get("quarantine", {})
    if not isinstance(qua_band_policy, dict):
        qua_band_policy = {}
    val_fallback_mode = "fallback_core_zero" in str(source_tags.get("validation", "")).lower()
    qua_fallback_mode = "fallback_core_zero" in str(source_tags.get("quarantine", "")).lower()
    val_band = compute_dynamic_band(
        band_policy=val_band_policy,
        candidate_total=val_funnel["candidate_total"],
        market_count=market_count,
        hostile_share_value=f64(context.get("hostile_share", 0.0), 0.0),
        dominant_regime=str(context.get("regime", "ANY")),
        history_values=[dev_pass_rate, val_pass_rate],
        default_lower_combine="max",
        default_upper_combine="min",
        fallback_mode=val_fallback_mode,
    )
    qua_band = compute_dynamic_band(
        band_policy=qua_band_policy,
        candidate_total=qua_funnel["candidate_total"],
        market_count=market_count,
        hostile_share_value=f64(context.get("hostile_share", 0.0), 0.0),
        dominant_regime=str(context.get("regime", "ANY")),
        history_values=[dev_pass_rate, val_pass_rate],
        default_lower_combine="max",
        default_upper_combine="min",
        fallback_mode=qua_fallback_mode,
    )
    val_pass = val_pass_rate >= float(val_band["lower_final"])
    qua_pass = (qua_pass_rate >= float(qua_band["lower_final"])) and (
        qua_pass_rate <= float(qua_band["upper_final"])
    )

    reason_codes: List[str] = []
    reason_codes.extend(metric_reasons)
    reason_codes.extend(metric_hold_reasons)
    if not val_pass:
        reason_codes.append("pass_rate_val_core_below_dynamic_lower")
    if not qua_pass:
        if qua_pass_rate < float(qua_band["lower_final"]):
            reason_codes.append("pass_rate_qua_core_below_dynamic_lower")
        if qua_pass_rate > float(qua_band["upper_final"]):
            reason_codes.append("pass_rate_qua_core_above_dynamic_upper")
    overall_pass = bool(tail_gate_pass and val_pass and qua_pass and not promotion_hold)
    if overall_pass:
        reason_codes.append("candidate_pass_dynamic_gate")

    reject_delta_keys = [
        "reject_margin_insufficient",
        "reject_strength_fail",
        "reject_expected_value_fail",
        "reject_frontier_fail",
        "reject_ev_confidence_low",
        "reject_cost_tail_fail",
    ]
    reject_breakdown_delta_qua_minus_val = {
        key: int(qua_funnel.get(key, 0)) - int(val_funnel.get(key, 0))
        for key in reject_delta_keys
    }

    out = {
        "mode": "dynamic_phase3_gate",
        "generated_at": now_iso(),
        "candidate_name": str(args.candidate_name),
        "prefer_core_window_direct": bool(prefer_core),
        "payload_source": source_tags,
        "context": {
            "regime": str(context.get("regime", "ANY")),
            "volatility_bucket": str(context.get("volatility_bucket", "ANY")),
            "liquidity_bucket": str(context.get("liquidity_bucket", "ANY")),
            "hostile_share": round(f64(context.get("hostile_share", 0.0), 0.0), 6),
            "market_count": int(market_count),
        },
        "phase3_pass_rate": {
            "development_core": round(dev_pass_rate, 6),
            "validation_core": round(val_pass_rate, 6),
            "quarantine_core": round(qua_pass_rate, 6),
            "development_funnel": dev_funnel,
            "validation_funnel": val_funnel,
            "quarantine_funnel": qua_funnel,
            "reject_breakdown_delta_qua_minus_val": reject_breakdown_delta_qua_minus_val,
        },
        "dynamic_tail_gate": {
            "pass": bool(tail_gate_pass),
            "metrics": metric_rows,
            "delta_required_ev_total": round(total_delta_required_ev, 8),
            "hard_fail_reasons": metric_reasons,
            "soft_fail_reasons": metric_soft_fail_reasons,
            "promotion_hold_reasons": metric_hold_reasons,
            "hard_fail_count": int(len(metric_reasons)),
            "soft_fail_count": int(len(metric_soft_fail_reasons)),
            "promotion_hold": bool(promotion_hold),
        },
        "dynamic_pass_band": {
            "validation": {
                **val_band,
                "observed_pass_rate": round(val_pass_rate, 6),
                "pass": bool(val_pass),
            },
            "quarantine": {
                **qua_band,
                "observed_pass_rate": round(qua_pass_rate, 6),
                "pass": bool(qua_pass),
            },
        },
        "lexicographic_result": {
            "level1_dynamic_tail_gate_pass": bool(tail_gate_pass),
            "level2_validation_pass_rate_pass": bool(val_pass),
            "level3_quarantine_pass_rate_pass": bool(qua_pass),
            "promotion_hold": bool(promotion_hold),
            "overall_pass": bool(overall_pass),
            "reason_codes": reason_codes,
        },
    }
    dump_json(resolve_repo_path(args.output_json), out)
    print(
        "[Phase34Ops] mode=dynamic_phase3_gate "
        f"candidate={args.candidate_name} pass={str(overall_pass).lower()} "
        f"tail_pass={str(tail_gate_pass).lower()} val_rate={round(val_pass_rate, 6)} qua_rate={round(qua_pass_rate, 6)}",
        flush=True,
    )
    if bool(args.fail_on_reject) and not overall_pass:
        return 2
    return 0


def cmd_phase4_exposure_summary(args: argparse.Namespace) -> int:
    bundle = load_json_required(args.bundle_json, "bundle")
    source_path = resolve_repo_path(args.candidate_artifact_jsonl)
    if not source_path.exists():
        raise RuntimeError(f"candidate artifact missing: {source_path}")
    cluster_map = ng(bundle, ["phase4", "correlation_control", "market_cluster_map"], {})
    if not isinstance(cluster_map, dict):
        cluster_map = {}
    exposure_by_cluster: Dict[str, float] = {}
    selected_by_cluster: Dict[str, int] = {}
    selected_total = 0
    candidate_total = 0
    source_mode = "jsonl"
    warnings: List[str] = []

    def _consume_candidate_rows(rows: Any, *, default_notional: float) -> None:
        nonlocal candidate_total
        nonlocal selected_total
        if not isinstance(rows, list):
            return
        for candidate in rows:
            if not isinstance(candidate, dict):
                continue
            candidate_total += 1
            if not bool(candidate.get("selected", False)):
                continue
            selected_total += 1
            market_id = str(candidate.get("market_id", candidate.get("market", ""))).strip()
            cluster = str(cluster_map.get(market_id, "unclustered")).strip() or "unclustered"
            execution_proxy = candidate.get("execution_proxy", {})
            if not isinstance(execution_proxy, dict):
                execution_proxy = {}
            notional = max(
                0.0,
                f64(
                    execution_proxy.get(
                        "position_size",
                        execution_proxy.get("notional", default_notional),
                    ),
                    default_notional,
                ),
            )
            exposure_by_cluster[cluster] = exposure_by_cluster.get(cluster, 0.0) + float(notional)
            selected_by_cluster[cluster] = selected_by_cluster.get(cluster, 0) + 1

    with source_path.open("r", encoding="utf-8") as fp:
        for line in fp:
            text = str(line).strip()
            if not text:
                continue
            try:
                row = json.loads(text)
            except Exception:
                continue
            if not isinstance(row, dict):
                continue
            _consume_candidate_rows(row.get("candidates", []), default_notional=1.0)

    fallback_report_payload: Dict[str, Any] = {}
    if (
        candidate_total <= 0
        and str(args.fallback_report_json).strip()
    ):
        fallback_report_payload = load_json_required(args.fallback_report_json, "fallback_report")
        phase4_gate = ng(bundle, ["phase4", "operations_dynamic_gate"], {})
        if not isinstance(phase4_gate, dict):
            phase4_gate = {}
        prefer_core = bool(phase4_gate.get("prefer_core_window_direct", True))
        fallback_core_zero = bool(phase4_gate.get("fallback_to_split_report_on_core_zero", True))
        fallback_payload = pick_eval_payload(fallback_report_payload, prefer_core)
        source_mode = "fallback_report"
        if fallback_payload is fallback_report_payload:
            source_mode = "fallback_report_split"
        else:
            source_mode = "fallback_report_core_window_direct"
        if (
            fallback_core_zero
            and fallback_payload is not fallback_report_payload
            and int(phase4_selection_breakdown(fallback_payload).get("candidates_total", 0.0)) <= 0
        ):
            fallback_payload = fallback_report_payload
            source_mode = "fallback_report_split_core_zero"

        # 1) Prefer explicit candidate snapshots when available.
        snapshot_candidates: List[Dict[str, Any]] = []
        for path in (
            ["phase4_candidate_snapshot_samples"],
            ["diagnostics", "aggregate", "phase4_candidate_snapshot_samples"],
            ["diagnostics", "aggregate", "phase4_portfolio_diagnostics", "phase4_candidate_snapshot_samples"],
        ):
            node = ng(fallback_payload, path, [])
            if isinstance(node, list) and node:
                snapshot_candidates = node
                break
        if snapshot_candidates:
            # Snapshot payload does not carry per-candidate notional; use unit proxy.
            _consume_candidate_rows(snapshot_candidates, default_notional=1.0)
            warnings.append("fallback_used_unit_notional_proxy_from_snapshot_samples")
        else:
            # 2) Prefer aggregate exposure rows if verification report already provides them.
            exposure_rows = ng(
                fallback_payload,
                ["diagnostics", "aggregate", "phase4_portfolio_diagnostics", "exposure_by_cluster"],
                [],
            )
            if isinstance(exposure_rows, list) and exposure_rows:
                selection = phase4_selection_breakdown(fallback_payload)
                candidate_total = max(0, int(selection.get("candidates_total", 0.0)))
                selected_total = max(0, int(selection.get("selected_total", 0.0)))
                for row in exposure_rows:
                    if not isinstance(row, dict):
                        continue
                    cluster = str(row.get("cluster", "unclustered")).strip() or "unclustered"
                    selected_count = max(0, i64(row.get("selected_count", 0), 0))
                    exposure_sum = max(0.0, f64(row.get("notional_exposure_sum", 0.0), 0.0))
                    selected_by_cluster[cluster] = selected_by_cluster.get(cluster, 0) + int(selected_count)
                    exposure_by_cluster[cluster] = (
                        exposure_by_cluster.get(cluster, 0.0) + float(exposure_sum)
                    )
                warnings.append("fallback_used_report_aggregate_exposure_rows")
            else:
                # 3) Last fallback: selection counters only (no cluster decomposition available).
                selection = phase4_selection_breakdown(fallback_payload)
                candidate_total = max(0, int(selection.get("candidates_total", 0.0)))
                selected_total = max(0, int(selection.get("selected_total", 0.0)))
                if selected_total > 0:
                    exposure_by_cluster["unclustered"] = float(selected_total)
                    selected_by_cluster["unclustered"] = int(selected_total)
                warnings.append("fallback_used_selection_breakdown_only_no_cluster_rows")

    total_exposure = sum(max(0.0, f64(v, 0.0)) for v in exposure_by_cluster.values())
    cluster_rows = []
    max_cluster_share = 0.0
    for cluster in sorted(exposure_by_cluster.keys()):
        exp_value = max(0.0, f64(exposure_by_cluster.get(cluster, 0.0), 0.0))
        share = exp_value / total_exposure if total_exposure > 0.0 else 0.0
        max_cluster_share = max(max_cluster_share, share)
        cluster_rows.append(
            {
                "cluster": cluster,
                "selected_count": int(selected_by_cluster.get(cluster, 0)),
                "notional_exposure_sum": round(exp_value, 8),
                "share": round(share, 6),
            }
        )
    out = {
        "mode": "phase4_exposure_summary",
        "generated_at": now_iso(),
        "candidate_name": str(args.candidate_name),
        "source_file": str(source_path),
        "source_mode": source_mode,
        "fallback_report_json": str(resolve_repo_path(args.fallback_report_json)) if str(args.fallback_report_json).strip() else "",
        "warnings": warnings,
        "candidate_total": int(candidate_total),
        "selected_total": int(selected_total),
        "selection_rate_from_artifact": round(float(selected_total) / float(max(1, candidate_total)), 6),
        "total_selected_notional_exposure": round(total_exposure, 8),
        "max_cluster_share": round(max_cluster_share, 6),
        "exposure_by_cluster": cluster_rows,
    }
    dump_json(resolve_repo_path(args.output_json), out)
    print(
        "[Phase34Ops] mode=phase4_exposure_summary "
        f"candidate={args.candidate_name} selected={selected_total}/{candidate_total} max_cluster_share={round(max_cluster_share, 6)}",
        flush=True,
    )
    return 0


def cmd_dynamic_phase4_gate(args: argparse.Namespace) -> int:
    bundle = load_json_required(args.bundle_json, "bundle")
    off_report = load_json_required(args.off_report_json, "off_report")
    cap_high_report = load_json_required(args.cap_high_report_json, "cap_high_report")
    corr_on_report = load_json_required(args.cap_high_corr_on_report_json, "cap_high_corr_on_report")
    off_exposure = load_json_required(args.off_exposure_json, "off_exposure")
    cap_high_exposure = load_json_required(args.cap_high_exposure_json, "cap_high_exposure")
    corr_on_exposure = load_json_required(args.cap_high_corr_on_exposure_json, "cap_high_corr_on_exposure")
    phase4_gate = ng(bundle, ["phase4", "operations_dynamic_gate"], {})
    if not isinstance(phase4_gate, dict):
        raise RuntimeError("bundle missing phase4.operations_dynamic_gate")
    if not bool(phase4_gate.get("enabled", False)):
        raise RuntimeError("phase4.operations_dynamic_gate.enabled=false")

    prefer_core = bool(phase4_gate.get("prefer_core_window_direct", True))
    off_payload = pick_eval_payload(off_report, prefer_core)
    cap_high_payload = pick_eval_payload(cap_high_report, prefer_core)
    corr_on_payload = pick_eval_payload(corr_on_report, prefer_core)
    source_tags = {
        "off": "core_window_direct" if off_payload is not off_report else "report",
        "cap_high": "core_window_direct" if cap_high_payload is not cap_high_report else "report",
        "cap_high_corr_on": "core_window_direct" if corr_on_payload is not corr_on_report else "report",
    }
    fallback_on_core_zero = bool(phase4_gate.get("fallback_to_split_report_on_core_zero", True))
    if fallback_on_core_zero:
        if off_payload is not off_report and int(phase4_selection_breakdown(off_payload).get("candidates_total", 0.0)) <= 0:
            off_payload = off_report
            source_tags["off"] = "report_fallback_core_zero"
        if cap_high_payload is not cap_high_report and int(phase4_selection_breakdown(cap_high_payload).get("candidates_total", 0.0)) <= 0:
            cap_high_payload = cap_high_report
            source_tags["cap_high"] = "report_fallback_core_zero"
        if corr_on_payload is not corr_on_report and int(phase4_selection_breakdown(corr_on_payload).get("candidates_total", 0.0)) <= 0:
            corr_on_payload = corr_on_report
            source_tags["cap_high_corr_on"] = "report_fallback_core_zero"
    selection_band_policy = phase4_gate.get("selection_rate_band", {})
    if not isinstance(selection_band_policy, dict):
        selection_band_policy = {}

    hostile_labels = phase4_gate.get("hostile_regime_labels", [])
    if not isinstance(hostile_labels, list):
        hostile_labels = []
    context = phase3_context(cap_high_payload, [str(x) for x in hostile_labels])
    market_count = max(1, i64(ng(cap_high_payload, ["dataset_count"], 0), 0))
    cap_high_sel = phase4_selection_breakdown(cap_high_payload)
    corr_on_sel = phase4_selection_breakdown(corr_on_payload)
    off_sel = phase4_selection_breakdown(off_payload)
    history_values: List[float] = []
    def _history_selection_rate(path_value: str, label: str) -> float:
        report_payload = load_json_required(path_value, label)
        payload = pick_eval_payload(report_payload, prefer_core)
        if (
            fallback_on_core_zero
            and payload is not report_payload
            and int(phase4_selection_breakdown(payload).get("candidates_total", 0.0)) <= 0
        ):
            payload = report_payload
        return phase4_selection_breakdown(payload).get("selection_rate", 0.0)

    if str(args.off_history_report_json).strip():
        history_values.append(_history_selection_rate(args.off_history_report_json, "off_history_report"))
    if str(args.cap_high_history_report_json).strip():
        history_values.append(
            _history_selection_rate(args.cap_high_history_report_json, "cap_high_history_report")
        )
    if str(args.cap_high_corr_on_history_report_json).strip():
        history_values.append(
            _history_selection_rate(
                args.cap_high_corr_on_history_report_json,
                "cap_high_corr_on_history_report",
            )
        )
    if not history_values:
        history_values = [float(off_sel["selection_rate"]), float(cap_high_sel["selection_rate"])]
    band = compute_dynamic_band(
        band_policy=selection_band_policy,
        candidate_total=int(cap_high_sel["candidates_total"]),
        market_count=market_count,
        hostile_share_value=f64(context.get("hostile_share", 0.0), 0.0),
        dominant_regime=str(context.get("regime", "ANY")),
        history_values=history_values,
        default_lower_combine="max",
        default_upper_combine="min",
    )

    off_corr_tel = phase4_corr_applied_telemetry(off_payload)
    cap_corr_tel = phase4_corr_applied_telemetry(cap_high_payload)
    corr_corr_tel = phase4_corr_applied_telemetry(corr_on_payload)

    def _profile_eval(
        name: str,
        sel: Dict[str, float],
        exposure: Dict[str, Any],
        corr_telemetry: Dict[str, Any],
    ) -> Dict[str, Any]:
        selection_rate = f64(sel.get("selection_rate", 0.0), 0.0)
        in_band = selection_rate >= float(band["lower_final"]) and selection_rate <= float(band["upper_final"])
        return {
            "name": name,
            "selection": {
                "candidates_total": int(sel.get("candidates_total", 0.0)),
                "selected_total": int(sel.get("selected_total", 0.0)),
                "selection_rate": round(selection_rate, 6),
                "rejected_by_budget": int(sel.get("rejected_by_budget", 0.0)),
                "rejected_by_cluster_cap": int(sel.get("rejected_by_cluster_cap", 0.0)),
                "rejected_by_correlation_penalty": int(sel.get("rejected_by_correlation_penalty", 0.0)),
                "rejected_by_execution_cap": int(sel.get("rejected_by_execution_cap", 0.0)),
                "rejected_by_drawdown_governor": int(sel.get("rejected_by_drawdown_governor", 0.0)),
            },
            "selection_rate_in_dynamic_band": bool(in_band),
            "max_cluster_share": round(f64(exposure.get("max_cluster_share", 0.0), 0.0), 6),
            "exposure_by_cluster": exposure.get("exposure_by_cluster", []),
            "corr_cluster_applied_telemetry": corr_telemetry,
        }

    off_eval = _profile_eval("off", off_sel, off_exposure, off_corr_tel)
    cap_eval = _profile_eval("cap_high", cap_high_sel, cap_high_exposure, cap_corr_tel)
    corr_eval = _profile_eval("cap_high_corr_on", corr_on_sel, corr_on_exposure, corr_corr_tel)

    corr_strength_delta = float(corr_eval["max_cluster_share"]) - float(cap_eval["max_cluster_share"])
    corr_effective = corr_strength_delta <= f64(phase4_gate.get("max_cluster_share_delta_for_effective_corr", 0.0), 0.0)
    corr_binding_reject_count = int(corr_eval["selection"]["rejected_by_cluster_cap"]) + int(
        corr_eval["selection"]["rejected_by_correlation_penalty"]
    )
    corr_checks_total = max(0, i64(corr_corr_tel.get("cluster_cap_checks_total", 0), 0))
    corr_penalty_applied_count = max(
        0,
        i64(corr_corr_tel.get("corr_penalty_applied_count", 0), 0),
    )
    corr_penalty_max = max(
        0.0,
        f64(corr_corr_tel.get("corr_penalty_max", 0.0), 0.0),
    )
    corr_effect_zero_reasons: List[str] = []
    if not str(corr_corr_tel.get("constraint_apply_stage", "")).strip():
        corr_effect_zero_reasons.append("corr_constraint_apply_stage_missing")
    if not str(corr_corr_tel.get("constraint_unit", "")).strip():
        corr_effect_zero_reasons.append("corr_constraint_unit_missing")
    if corr_checks_total <= 0:
        corr_effect_zero_reasons.append("corr_constraint_not_applied_in_allocator_loop")
    elif corr_binding_reject_count <= 0:
        corr_effect_zero_reasons.append("corr_constraint_applied_but_not_binding")
    if corr_penalty_applied_count <= 0:
        corr_effect_zero_reasons.append("correlation_penalty_not_triggered_or_unimplemented")
    elif corr_binding_reject_count <= 0:
        corr_effect_zero_reasons.append("correlation_penalty_triggered_but_strength_insufficient")
    corr_effect_zero = (abs(corr_strength_delta) <= 1.0e-12) and (corr_binding_reject_count <= 0)

    qua_tail_metric = f64(
        ng(corr_on_payload, ["adaptive_validation", "aggregates", "loss_tail_aggregate", "avg_top3_loss_concentration"], 0.0),
        0.0,
    )
    cap_tail_metric = f64(
        ng(cap_high_payload, ["adaptive_validation", "aggregates", "loss_tail_aggregate", "avg_top3_loss_concentration"], 0.0),
        0.0,
    )
    tail_not_worse = qua_tail_metric <= cap_tail_metric + f64(
        phase4_gate.get("tail_concentration_tolerance", 0.0), 0.0
    )

    reason_codes: List[str] = []
    if not bool(cap_eval["selection_rate_in_dynamic_band"]):
        reason_codes.append("cap_high_selection_rate_out_of_dynamic_band")
    if not bool(corr_eval["selection_rate_in_dynamic_band"]):
        reason_codes.append("corr_on_selection_rate_out_of_dynamic_band")
    if not corr_effective:
        reason_codes.append("corr_on_cluster_concentration_not_improved")
    if not tail_not_worse:
        reason_codes.append("corr_on_tail_concentration_worse_than_cap_high")
    overall_pass = len(reason_codes) == 0
    if overall_pass:
        reason_codes.append("phase4_dynamic_gate_pass")

    out = {
        "mode": "dynamic_phase4_gate",
        "generated_at": now_iso(),
        "prefer_core_window_direct": bool(prefer_core),
        "payload_source": source_tags,
        "context": {
            "regime": str(context.get("regime", "ANY")),
            "volatility_bucket": str(context.get("volatility_bucket", "ANY")),
            "liquidity_bucket": str(context.get("liquidity_bucket", "ANY")),
            "hostile_share": round(f64(context.get("hostile_share", 0.0), 0.0), 6),
            "market_count": int(market_count),
        },
        "selection_rate_band_dynamic": {
            **band,
            "history_values": [round(x, 6) for x in history_values],
        },
        "profiles": {
            "off": off_eval,
            "cap_high": cap_eval,
            "cap_high_corr_on": corr_eval,
        },
        "corr_effect": {
            "max_cluster_share_delta_corr_minus_cap_high": round(corr_strength_delta, 6),
            "corr_effective": bool(corr_effective),
            "corr_binding_reject_count": int(corr_binding_reject_count),
        },
        "corr_cluster_applied_telemetry": {
            "off": off_corr_tel,
            "cap_high": cap_corr_tel,
            "cap_high_corr_on": corr_corr_tel,
        },
        "corr_effect_zero_diagnosis": {
            "corr_effect_zero": bool(corr_effect_zero),
            "reason_codes": corr_effect_zero_reasons,
            "cluster_cap_checks_total": int(corr_checks_total),
            "corr_penalty_applied_count": int(corr_penalty_applied_count),
            "corr_penalty_max": round(corr_penalty_max, 8),
            "constraint_apply_stage": str(corr_corr_tel.get("constraint_apply_stage", "")),
            "constraint_apply_stage_bucket": str(
                corr_corr_tel.get("constraint_apply_stage_bucket", "unknown")
            ),
            "constraint_unit": str(corr_corr_tel.get("constraint_unit", "")),
            "penalty_score_samples": corr_corr_tel.get("penalty_score_samples", []),
            "near_cap_candidates": corr_corr_tel.get("near_cap_candidates", []),
            "cluster_exposure_current": corr_corr_tel.get("cluster_exposure_current", []),
        },
        "tail_compare": {
            "cap_high_qua_tail_concentration": round(cap_tail_metric, 6),
            "corr_on_qua_tail_concentration": round(qua_tail_metric, 6),
            "tail_not_worse": bool(tail_not_worse),
        },
        "overall_pass": bool(overall_pass),
        "reason_codes": reason_codes,
    }
    dump_json(resolve_repo_path(args.output_json), out)
    print(
        "[Phase34Ops] mode=dynamic_phase4_gate "
        f"pass={str(overall_pass).lower()} "
        f"sel_cap={round(cap_high_sel['selection_rate'], 6)} "
        f"sel_corr={round(corr_on_sel['selection_rate'], 6)} "
        f"max_cluster_delta={round(corr_strength_delta, 6)}",
        flush=True,
    )
    if bool(args.fail_on_reject) and not overall_pass:
        return 2
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Phase3/4 operations tuning and anti-overfit validation toolkit.")
    s = p.add_subparsers(dest="cmd", required=True)

    a = s.add_parser("control_a_update")
    a.add_argument("--bundle-json", default=r".\config\model\probabilistic_runtime_bundle_v2.json")
    a.add_argument("--verification-report-json", default=r".\build\Release\logs\verification_report.json")
    a.add_argument("--output-bundle-json", default=r".\config\model\probabilistic_runtime_bundle_v2_phase34_ops_next.json")
    a.add_argument("--output-report-json", default=r".\build\Release\logs\phase34_ops_control_update_report.json")
    a.add_argument("--state-json", default=r".\build\Release\logs\phase34_ops_state.json")
    a.add_argument("--release-version", default="")
    a.add_argument("--step-size", type=float, default=0.0)
    a.add_argument("--target-throughput-min", type=float, default=8.0)
    a.add_argument("--target-throughput-max", type=float, default=80.0)
    a.add_argument("--target-quality-min-score", type=float, default=-0.10)
    a.add_argument("--risk-drawdown-max-pct", type=float, default=12.0)
    a.add_argument("--risk-tail-concentration-max", type=float, default=0.70)

    b = s.add_parser("bandit_b_select")
    b.add_argument("--candidate-bundle-jsons", nargs="+", required=True)
    b.add_argument("--state-json", default=r".\build\Release\logs\phase34_ops_state.json")
    b.add_argument("--output-json", default=r".\build\Release\logs\phase34_ops_bandit_report.json")
    b.add_argument("--output-selected-bundle-json", default="")
    b.add_argument("--update-reward-report-json", default="")
    b.add_argument("--explore-every-n", type=int, default=4)
    b.add_argument("--reward-expectancy-weight", type=float, default=1.0)
    b.add_argument("--reward-drawdown-weight", type=float, default=2.0)
    b.add_argument("--reward-tail-weight", type=float, default=50.0)
    b.add_argument("--reward-throughput-weight", type=float, default=0.2)

    v = s.add_parser("validate_protocol")
    v.add_argument("--development-report-json", required=True)
    v.add_argument("--validation-report-json", required=True)
    v.add_argument("--quarantine-report-json", required=True)
    v.add_argument("--output-json", default=r".\build\Release\logs\phase34_ops_validation_report.json")
    v.add_argument("--development-tag", default="development")
    v.add_argument("--validation-tag", default="validation")
    v.add_argument("--quarantine-tag", default="quarantine")
    v.add_argument("--purge-gap-minutes", type=int, default=0)
    v.add_argument("--strict-parity-pass", action="store_true")
    v.add_argument("--max-drawdown-pct", type=float, default=12.0)
    v.add_argument("--max-tail-concentration", type=float, default=0.70)
    v.add_argument("--max-avg-fee-krw", type=float, default=5000.0)
    v.add_argument("--min-profit-factor", type=float, default=1.0)
    v.add_argument("--min-expectancy-krw", type=float, default=0.0)
    v.add_argument("--min-avg-trades", type=float, default=8.0)

    g = s.add_parser("promotion_gate")
    g.add_argument("--paper-report-json", required=True)
    g.add_argument("--state-json", default=r".\build\Release\logs\phase34_ops_state.json")
    g.add_argument("--output-json", default=r".\build\Release\logs\phase34_ops_promotion_gate_report.json")
    g.add_argument("--current-stage", default="paper")
    g.add_argument("--min-consecutive-windows", type=int, default=2)
    g.add_argument("--strict-parity-pass", action="store_true")
    g.add_argument("--max-drawdown-pct", type=float, default=12.0)
    g.add_argument("--max-tail-concentration", type=float, default=0.70)
    g.add_argument("--max-avg-fee-krw", type=float, default=5000.0)
    g.add_argument("--min-profit-factor", type=float, default=1.0)
    g.add_argument("--min-expectancy-krw", type=float, default=0.0)
    g.add_argument("--min-avg-trades", type=float, default=8.0)
    g.add_argument("--bundle-json", default="")
    g.add_argument("--history-report-jsons", nargs="*", default=[])
    g.add_argument("--throughput-dynamic-debug-json", default="")
    g.add_argument("--a21-throughput-source-debug-json", default="")
    g.add_argument("--strict-parity-diagnosis-json", default="")

    d = s.add_parser("bundle_diff")
    d.add_argument("--base-bundle-json", required=True)
    d.add_argument("--candidate-bundle-json", required=True)
    d.add_argument("--output-json", default=r".\build\Release\logs\phase34_ops_bundle_diff.json")

    a6 = s.add_parser("a6_required_ev_step")
    a6.add_argument("--bundle-json", required=True)
    a6.add_argument("--development-report-json", required=True)
    a6.add_argument("--validation-report-json", required=True)
    a6.add_argument("--output-bundle-json", required=True)
    a6.add_argument("--output-json", required=True)
    a6.add_argument("--release-version", default="")
    a6.add_argument("--ev-step-quantile", type=float, default=0.15)
    a6.add_argument("--delta-clamp-median-k", type=float, default=0.5)
    a6.add_argument("--min-samples-lookup-default", type=float, default=32.0)
    a6.add_argument("--min-samples-dist-quantile-p", type=float, default=0.2)

    a7 = s.add_parser("a7_expectancy_component_fix")
    a7.add_argument("--bundle-json", required=True)
    a7.add_argument("--quarantine-report-json", required=True)
    a7.add_argument("--output-bundle-json", required=True)
    a7.add_argument("--output-json", required=True)
    a7.add_argument("--release-version", default="")
    a7.add_argument("--min-avg-trades", type=float, default=8.0)
    a7.add_argument("--max-drawdown-pct", type=float, default=12.0)

    r3 = s.add_parser("dynamic_phase3_gate")
    r3.add_argument("--candidate-name", default="")
    r3.add_argument("--bundle-json", required=True)
    r3.add_argument("--development-report-json", required=True)
    r3.add_argument("--validation-report-json", required=True)
    r3.add_argument("--quarantine-report-json", required=True)
    r3.add_argument("--output-json", required=True)
    r3.add_argument("--fail-on-reject", action="store_true")

    exp = s.add_parser("phase4_exposure_summary")
    exp.add_argument("--candidate-name", default="")
    exp.add_argument("--bundle-json", required=True)
    exp.add_argument("--candidate-artifact-jsonl", required=True)
    exp.add_argument("--fallback-report-json", default="")
    exp.add_argument("--output-json", required=True)

    r4 = s.add_parser("dynamic_phase4_gate")
    r4.add_argument("--bundle-json", required=True)
    r4.add_argument("--off-report-json", required=True)
    r4.add_argument("--cap-high-report-json", required=True)
    r4.add_argument("--cap-high-corr-on-report-json", required=True)
    r4.add_argument("--off-exposure-json", required=True)
    r4.add_argument("--cap-high-exposure-json", required=True)
    r4.add_argument("--cap-high-corr-on-exposure-json", required=True)
    r4.add_argument("--off-history-report-json", default="")
    r4.add_argument("--cap-high-history-report-json", default="")
    r4.add_argument("--cap-high-corr-on-history-report-json", default="")
    r4.add_argument("--output-json", required=True)
    r4.add_argument("--fail-on-reject", action="store_true")
    return p


def main(argv: List[str] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.cmd == "control_a_update":
        return cmd_control(args)
    if args.cmd == "bandit_b_select":
        return cmd_bandit(args)
    if args.cmd == "validate_protocol":
        return cmd_validate(args)
    if args.cmd == "promotion_gate":
        return cmd_promotion(args)
    if args.cmd == "bundle_diff":
        return cmd_diff(args)
    if args.cmd == "a6_required_ev_step":
        return cmd_a6_required_ev_step(args)
    if args.cmd == "a7_expectancy_component_fix":
        return cmd_a7_expectancy_component_fix(args)
    if args.cmd == "dynamic_phase3_gate":
        return cmd_dynamic_phase3_gate(args)
    if args.cmd == "phase4_exposure_summary":
        return cmd_phase4_exposure_summary(args)
    if args.cmd == "dynamic_phase4_gate":
        return cmd_dynamic_phase4_gate(args)
    raise RuntimeError(f"unsupported cmd: {args.cmd}")


if __name__ == "__main__":
    sys.exit(main())
