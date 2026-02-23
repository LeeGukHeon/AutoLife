#!/usr/bin/env python3
from __future__ import annotations
import argparse
import csv
import math
import pathlib
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

try:
    import joblib
except Exception:  # pragma: no cover - optional dependency import guard
    joblib = None

try:
    import numpy as np
except Exception:  # pragma: no cover - optional dependency import guard
    np = None

try:
    from sklearn.linear_model import LogisticRegression, SGDClassifier, SGDRegressor
    from sklearn.metrics import accuracy_score, brier_score_loss, log_loss
except Exception:  # pragma: no cover - optional dependency import guard
    LogisticRegression = None
    SGDClassifier = None
    SGDRegressor = None
    accuracy_score = None
    brier_score_loss = None
    log_loss = None

from _script_common import dump_json, load_json_or_none, resolve_repo_path


FEATURE_COLUMNS = [
    "ret_1m",
    "ret_5m",
    "ret_20m",
    "ema_gap_12_26",
    "rsi_14",
    "atr_pct_14",
    "bb_width_20",
    "vol_ratio_20",
    "notional_ratio_20",
    "ctx_5m_age_min",
    "ctx_5m_ret_3",
    "ctx_5m_ret_12",
    "ctx_5m_ema_gap_20",
    "ctx_5m_rsi_14",
    "ctx_5m_atr_pct_14",
    "ctx_15m_age_min",
    "ctx_15m_ret_3",
    "ctx_15m_ret_12",
    "ctx_15m_ema_gap_20",
    "ctx_15m_rsi_14",
    "ctx_15m_atr_pct_14",
    "ctx_60m_age_min",
    "ctx_60m_ret_3",
    "ctx_60m_ret_12",
    "ctx_60m_ema_gap_20",
    "ctx_60m_rsi_14",
    "ctx_60m_atr_pct_14",
    "ctx_240m_age_min",
    "ctx_240m_ret_3",
    "ctx_240m_ret_12",
    "ctx_240m_ema_gap_20",
    "ctx_240m_rsi_14",
    "ctx_240m_atr_pct_14",
    "regime_trend_60_sign",
    "regime_trend_240_sign",
    "regime_vol_60_atr_pct",
]


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Train probabilistic pattern model on frozen feature split (streaming SGD + Platt calibration)."
    )
    parser.add_argument(
        "--split-manifest-json",
        default=r".\data\model_input\probabilistic_features_v1_full_20260220_181345\probabilistic_split_manifest_v1.json",
    )
    parser.add_argument(
        "--baseline-json",
        default=r".\build\Release\logs\probabilistic_baseline_summary_full_20260220.json",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\probabilistic_model_train_summary_full_20260220.json",
    )
    parser.add_argument("--model-dir", default=r".\build\Release\models\probabilistic_pattern_v1")
    parser.add_argument("--batch-size", type=int, default=4096)
    parser.add_argument("--infer-batch-size", type=int, default=8192)
    parser.add_argument("--alpha", type=float, default=1e-5)
    parser.add_argument("--l1-ratio", type=float, default=0.05)
    parser.add_argument("--clip-abs", type=float, default=8.0)
    parser.add_argument("--prob-eps", type=float, default=1e-6)
    parser.add_argument("--calib-max-iter", type=int, default=200)
    parser.add_argument("--threshold-min", type=float, default=0.50)
    parser.add_argument("--threshold-max", type=float, default=0.80)
    parser.add_argument("--threshold-step", type=float, default=0.01)
    parser.add_argument("--threshold-min-coverage", type=float, default=0.02)
    parser.add_argument("--threshold-min-selected", type=int, default=100)
    parser.add_argument(
        "--h1-target-column",
        default="label_up_h1",
        help="Binary target column for h1 head.",
    )
    parser.add_argument(
        "--h5-target-column",
        default="label_up_h5",
        help=(
            "Target column for h5 head. Supports binary (0/1) or signed "
            "values (e.g. -1/0/1 from triple-barrier dir)."
        ),
    )
    parser.add_argument(
        "--edge-column",
        default="label_edge_bps_h5",
        help="Edge/return column used for threshold optimization and selected-edge metrics.",
    )
    parser.add_argument(
        "--drop-neutral-target",
        action="store_true",
        default=True,
        help="Drop rows where signed target column equals 0.",
    )
    parser.add_argument(
        "--keep-neutral-target",
        dest="drop_neutral_target",
        action="store_false",
        help="Map signed target 0 to class 0 instead of dropping.",
    )
    parser.add_argument(
        "--enable-edge-regressor",
        action="store_true",
        default=True,
        help="Train an additional h5 edge regressor head (EV bridge).",
    )
    parser.add_argument(
        "--disable-edge-regressor",
        dest="enable_edge_regressor",
        action="store_false",
        help="Disable h5 edge regressor head.",
    )
    parser.add_argument(
        "--edge-target-clip-bps",
        type=float,
        default=250.0,
        help="Clip absolute edge target for regression stability.",
    )
    parser.add_argument("--max-datasets", type=int, default=0)
    parser.add_argument("--random-state", type=int, default=42)
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_iso_to_ts_ms(value: str) -> int:
    token = str(value or "").strip()
    if not token:
        return 0
    dt = datetime.fromisoformat(token.replace("Z", "+00:00")).astimezone(timezone.utc)
    return int(dt.timestamp() * 1000)


def safe_int01(raw: Any) -> Optional[int]:
    try:
        v = int(float(raw))
    except Exception:
        return None
    return v if v in (0, 1) else None


def safe_binary_target(raw: Any, *, drop_neutral: bool) -> Optional[int]:
    """
    Accept binary (0/1) and signed (-1/0/1) targets.
    - positive -> 1
    - negative -> 0
    - zero -> None(drop) or 0(keep) by policy
    """
    direct = safe_int01(raw)
    if direct is not None:
        return direct

    v = safe_float(raw)
    if v is None:
        return None
    if v > 0.0:
        return 1
    if v < 0.0:
        return 0
    return None if bool(drop_neutral) else 0


def safe_float(raw: Any) -> Optional[float]:
    try:
        v = float(raw)
    except Exception:
        return None
    if not math.isfinite(v):
        return None
    return v


def sigmoid_stable(x: np.ndarray) -> np.ndarray:
    z = np.clip(x, -40.0, 40.0)
    return 1.0 / (1.0 + np.exp(-z))


def clip_prob(p: np.ndarray, eps: float) -> np.ndarray:
    return np.clip(p, float(eps), 1.0 - float(eps))


def apply_platt(prob: np.ndarray, a: float, b: float, eps: float) -> np.ndarray:
    p = clip_prob(prob, eps)
    logit = np.log(p / (1.0 - p))
    z = (float(a) * logit) + float(b)
    out = sigmoid_stable(z)
    return clip_prob(out, eps)


def build_feature_vector(row: Dict[str, str], clip_abs: float) -> Optional[List[float]]:
    out: List[float] = []
    cabs = float(max(0.1, clip_abs))
    for key in FEATURE_COLUMNS:
        raw = row.get(key, "")
        v = safe_float(raw)
        if v is None:
            return None

        if key == "rsi_14" or key.endswith("_rsi_14"):
            x = (v - 50.0) / 50.0
        elif key.endswith("_age_min"):
            x = min(max(v, 0.0), 240.0) / 240.0
        elif key in ("vol_ratio_20", "notional_ratio_20"):
            x = math.log(max(v, 1e-9))
        elif key.endswith("_sign"):
            x = v
        elif (
            key.startswith("ret_")
            or "_ret_" in key
            or "atr_pct" in key
            or "bb_width" in key
            or "gap_" in key
            or "_gap_" in key
        ):
            x = v * 100.0
        else:
            x = v
        out.append(float(min(cabs, max(-cabs, x))))
    return out


def classification_metrics(y: np.ndarray, p: np.ndarray, eps: float) -> Dict[str, Any]:
    if y.size == 0:
        return {
            "n": 0,
            "logloss": math.nan,
            "brier": math.nan,
            "accuracy": math.nan,
            "positive_rate": math.nan,
        }
    p2 = clip_prob(np.asarray(p, dtype=np.float64), eps)
    y2 = np.asarray(y, dtype=np.int64)
    pred = (p2 >= 0.5).astype(np.int64)
    return {
        "n": int(y2.size),
        "logloss": float(log_loss(y2, p2, labels=[0, 1])),
        "brier": float(brier_score_loss(y2, p2)),
        "accuracy": float(accuracy_score(y2, pred)),
        "positive_rate": float(np.mean(y2)),
    }


def regression_metrics(y_true: np.ndarray, y_pred: np.ndarray) -> Dict[str, Any]:
    if y_true.size == 0 or y_pred.size == 0:
        return {
            "n": 0,
            "mae_bps": math.nan,
            "rmse_bps": math.nan,
            "corr": math.nan,
        }
    yt = np.asarray(y_true, dtype=np.float64)
    yp = np.asarray(y_pred, dtype=np.float64)
    err = yp - yt
    mae = float(np.mean(np.abs(err)))
    rmse = float(np.sqrt(np.mean(np.square(err))))
    corr = math.nan
    if yt.size >= 2:
        yt_std = float(np.std(yt))
        yp_std = float(np.std(yp))
        if yt_std > 1e-12 and yp_std > 1e-12:
            corr = float(np.corrcoef(yt, yp)[0, 1])
    return {
        "n": int(yt.size),
        "mae_bps": mae,
        "rmse_bps": rmse,
        "corr": corr,
    }


def optimize_h5_threshold(
    *,
    valid_prob_cal: np.ndarray,
    valid_edge: np.ndarray,
    threshold_min: float,
    threshold_max: float,
    threshold_step: float,
    min_coverage: float,
    min_selected: int,
) -> Dict[str, Any]:
    if valid_prob_cal.size == 0:
        return {
            "threshold": 0.60,
            "coverage": 0.0,
            "selected_count": 0,
            "mean_edge_bps": math.nan,
            "positive_edge_rate": math.nan,
            "score": -math.inf,
            "status": "empty",
        }

    best = None
    t = float(threshold_min)
    t_max = float(threshold_max)
    t_step = max(1e-4, float(threshold_step))
    n_total = int(valid_prob_cal.size)
    while t <= (t_max + 1e-9):
        mask = valid_prob_cal >= t
        n_sel = int(np.sum(mask))
        coverage = float(n_sel) / float(n_total) if n_total > 0 else 0.0
        if n_sel >= int(min_selected) and coverage >= float(min_coverage):
            edge_sel = valid_edge[mask]
            mean_edge = float(np.mean(edge_sel))
            pos_edge_rate = float(np.mean((edge_sel > 0.0).astype(np.float64)))
            score = mean_edge * math.sqrt(max(1e-9, coverage))
            cand = {
                "threshold": float(round(t, 4)),
                "coverage": coverage,
                "selected_count": n_sel,
                "mean_edge_bps": mean_edge,
                "positive_edge_rate": pos_edge_rate,
                "score": score,
            }
            if best is None or float(cand["score"]) > float(best["score"]):
                best = cand
        t += t_step

    if best is not None:
        best["status"] = "ok"
        return best

    # Relaxed fallback: only enforce a minimal selected-count.
    best_relaxed = None
    t = float(threshold_min)
    while t <= (t_max + 1e-9):
        mask = valid_prob_cal >= t
        n_sel = int(np.sum(mask))
        if n_sel >= max(20, int(min_selected // 4)):
            coverage = float(n_sel) / float(max(1, n_total))
            edge_sel = valid_edge[mask]
            mean_edge = float(np.mean(edge_sel))
            pos_edge_rate = float(np.mean((edge_sel > 0.0).astype(np.float64)))
            score = mean_edge * math.sqrt(max(1e-9, coverage))
            cand = {
                "threshold": float(round(t, 4)),
                "coverage": coverage,
                "selected_count": n_sel,
                "mean_edge_bps": mean_edge,
                "positive_edge_rate": pos_edge_rate,
                "score": score,
            }
            if best_relaxed is None or float(cand["score"]) > float(best_relaxed["score"]):
                best_relaxed = cand
        t += t_step
    if best_relaxed is not None:
        best_relaxed["status"] = "relaxed_fallback"
        return best_relaxed

    # Final fallback: top-quantile threshold.
    q = 0.90
    t = float(np.quantile(valid_prob_cal, q))
    mask = valid_prob_cal >= t
    n_sel = int(np.sum(mask))
    if n_sel > 0:
        edge_sel = valid_edge[mask]
        mean_edge = float(np.mean(edge_sel))
        pos_edge_rate = float(np.mean((edge_sel > 0.0).astype(np.float64)))
    else:
        mean_edge = math.nan
        pos_edge_rate = math.nan
    return {
        "threshold": float(t),
        "coverage": float(n_sel) / float(max(1, n_total)),
        "selected_count": n_sel,
        "mean_edge_bps": mean_edge,
        "positive_edge_rate": pos_edge_rate,
        "score": -math.inf,
        "status": "quantile_fallback",
    }


def threshold_metrics(prob_cal: np.ndarray, edge: np.ndarray, threshold: float) -> Dict[str, Any]:
    n_total = int(prob_cal.size)
    if n_total == 0:
        return {
            "n_total": 0,
            "n_selected": 0,
            "coverage": 0.0,
            "mean_edge_bps": math.nan,
            "positive_edge_rate": math.nan,
        }
    mask = prob_cal >= float(threshold)
    n_sel = int(np.sum(mask))
    if n_sel <= 0:
        return {
            "n_total": n_total,
            "n_selected": 0,
            "coverage": 0.0,
            "mean_edge_bps": math.nan,
            "positive_edge_rate": math.nan,
        }
    edge_sel = edge[mask]
    return {
        "n_total": n_total,
        "n_selected": n_sel,
        "coverage": float(n_sel) / float(n_total),
        "mean_edge_bps": float(np.mean(edge_sel)),
        "positive_edge_rate": float(np.mean((edge_sel > 0.0).astype(np.float64))),
    }


def edge_profile(y: np.ndarray, edge: np.ndarray) -> Dict[str, Any]:
    if y.size == 0 or edge.size == 0:
        return {
            "sample_count": 0,
            "win_count": 0,
            "loss_count": 0,
            "neutral_count": 0,
            "win_mean_edge_bps": 0.0,
            "loss_mean_edge_bps": 0.0,
            "neutral_mean_edge_bps": 0.0,
        }

    yv = np.asarray(y, dtype=np.int64)
    ev = np.asarray(edge, dtype=np.float64)
    win_mask = yv == 1
    loss_mask = yv == 0
    neutral_mask = ~(win_mask | loss_mask)

    def safe_mean(mask: np.ndarray) -> float:
        if not np.any(mask):
            return 0.0
        return float(np.mean(ev[mask]))

    return {
        "sample_count": int(yv.size),
        "win_count": int(np.sum(win_mask)),
        "loss_count": int(np.sum(loss_mask)),
        "neutral_count": int(np.sum(neutral_mask)),
        "win_mean_edge_bps": safe_mean(win_mask),
        "loss_mean_edge_bps": safe_mean(loss_mask),
        "neutral_mean_edge_bps": safe_mean(neutral_mask),
    }


def make_sgd_classifier(alpha: float, l1_ratio: float, random_state: int) -> SGDClassifier:
    return SGDClassifier(
        loss="log_loss",
        penalty="elasticnet",
        alpha=float(alpha),
        l1_ratio=float(l1_ratio),
        fit_intercept=True,
        max_iter=1,
        learning_rate="constant",
        eta0=0.005,
        average=True,
        random_state=int(random_state),
        tol=None,
        shuffle=False,
    )


def make_sgd_regressor(alpha: float, l1_ratio: float, random_state: int) -> SGDRegressor:
    return SGDRegressor(
        loss="huber",
        penalty="elasticnet",
        alpha=float(alpha),
        l1_ratio=float(l1_ratio),
        fit_intercept=True,
        max_iter=1,
        learning_rate="constant",
        eta0=0.0015,
        average=True,
        random_state=int(random_state),
        tol=None,
        shuffle=False,
    )


def split_name_for_ts(ts_ms: int, win: Dict[str, int]) -> Optional[str]:
    if win["train_start"] <= ts_ms <= win["train_end"]:
        return "train"
    if win["valid_start"] <= ts_ms <= win["valid_end"]:
        return "valid"
    if win["test_start"] <= ts_ms <= win["test_end"]:
        return "test"
    return None


def flush_train_buffer(state: Dict[str, Any]) -> None:
    if not state["train_x"]:
        return
    x = np.asarray(state["train_x"], dtype=np.float64)
    y_h1 = np.asarray(state["train_y_h1"], dtype=np.int64)
    y_h5 = np.asarray(state["train_y_h5"], dtype=np.int64)
    y_edge = np.asarray(state["train_edge"], dtype=np.float64)

    if not state["fitted_h1"]:
        state["model_h1"].partial_fit(x, y_h1, classes=np.array([0, 1], dtype=np.int64))
        state["fitted_h1"] = True
    else:
        state["model_h1"].partial_fit(x, y_h1)

    if not state["fitted_h5"]:
        state["model_h5"].partial_fit(x, y_h5, classes=np.array([0, 1], dtype=np.int64))
        state["fitted_h5"] = True
    else:
        state["model_h5"].partial_fit(x, y_h5)

    if state["model_edge"] is not None and y_edge.size == x.shape[0]:
        y_edge_clip = np.clip(
            y_edge,
            -float(state["edge_target_clip_bps"]),
            float(state["edge_target_clip_bps"]),
        )
        state["model_edge"].partial_fit(x, y_edge_clip)
        state["fitted_edge"] = True

    state["train_count"] += int(x.shape[0])
    state["train_x"].clear()
    state["train_y_h1"].clear()
    state["train_y_h5"].clear()
    state["train_edge"].clear()


def flush_infer_buffer(state: Dict[str, Any], split_name: str, prob_eps: float) -> None:
    buf = state["infer"][split_name]
    if not buf["x"]:
        return
    x = np.asarray(buf["x"], dtype=np.float64)
    y_h1 = np.asarray(buf["y_h1"], dtype=np.int64)
    y_h5 = np.asarray(buf["y_h5"], dtype=np.int64)
    edge = np.asarray(buf["edge"], dtype=np.float64)

    if state["fitted_h1"]:
        p_h1 = state["model_h1"].predict_proba(x)[:, 1]
    else:
        p_h1 = np.full(shape=(x.shape[0],), fill_value=0.5, dtype=np.float64)
    if state["fitted_h5"]:
        p_h5 = state["model_h5"].predict_proba(x)[:, 1]
    else:
        p_h5 = np.full(shape=(x.shape[0],), fill_value=0.5, dtype=np.float64)
    if state["fitted_edge"] and state["model_edge"] is not None:
        edge_pred = state["model_edge"].predict(x)
    else:
        edge_pred = np.full(shape=(x.shape[0],), fill_value=0.0, dtype=np.float64)

    p_h1 = clip_prob(p_h1, prob_eps)
    p_h5 = clip_prob(p_h5, prob_eps)
    edge_pred = np.clip(
        edge_pred,
        -float(state["edge_target_clip_bps"]),
        float(state["edge_target_clip_bps"]),
    )

    state[f"{split_name}_h1_prob_raw"].append(p_h1)
    state[f"{split_name}_h1_y"].append(y_h1)
    state[f"{split_name}_h5_prob_raw"].append(p_h5)
    state[f"{split_name}_h5_y"].append(y_h5)
    state[f"{split_name}_h5_edge"].append(edge)
    state[f"{split_name}_h5_edge_pred"].append(edge_pred)
    state[f"{split_name}_count"] += int(x.shape[0])

    buf["x"].clear()
    buf["y_h1"].clear()
    buf["y_h5"].clear()
    buf["edge"].clear()


def concat_or_empty(chunks: List[np.ndarray], dtype: Any = None) -> np.ndarray:
    if dtype is None:
        dtype = np.float64 if np is not None else float
    if not chunks:
        return np.empty((0,), dtype=dtype)
    return np.concatenate(chunks, axis=0)


def fit_platt(valid_prob: np.ndarray, valid_y: np.ndarray, max_iter: int, eps: float) -> Dict[str, Any]:
    if valid_prob.size < 100 or np.unique(valid_y).size < 2:
        return {
            "type": "identity",
            "a": 1.0,
            "b": 0.0,
            "status": "insufficient_validation",
        }
    p = clip_prob(valid_prob.astype(np.float64), eps)
    x = np.log(p / (1.0 - p)).reshape(-1, 1)
    y = valid_y.astype(np.int64)

    clf = LogisticRegression(
        solver="lbfgs",
        C=1.0,
        fit_intercept=True,
        max_iter=max(50, int(max_iter)),
        random_state=0,
    )
    clf.fit(x, y)
    a = float(clf.coef_[0][0])
    b = float(clf.intercept_[0])
    return {
        "type": "platt",
        "a": a,
        "b": b,
        "status": "ok",
    }


def evaluate_fold_state(
    *,
    state: Dict[str, Any],
    args: argparse.Namespace,
) -> Dict[str, Any]:
    valid_h1_prob_raw = concat_or_empty(state["valid_h1_prob_raw"], dtype=np.float64)
    valid_h1_y = concat_or_empty(state["valid_h1_y"], dtype=np.int64)
    test_h1_prob_raw = concat_or_empty(state["test_h1_prob_raw"], dtype=np.float64)
    test_h1_y = concat_or_empty(state["test_h1_y"], dtype=np.int64)

    valid_h5_prob_raw = concat_or_empty(state["valid_h5_prob_raw"], dtype=np.float64)
    valid_h5_y = concat_or_empty(state["valid_h5_y"], dtype=np.int64)
    valid_h5_edge = concat_or_empty(state["valid_h5_edge"], dtype=np.float64)
    valid_h5_edge_pred = concat_or_empty(state["valid_h5_edge_pred"], dtype=np.float64)
    test_h5_prob_raw = concat_or_empty(state["test_h5_prob_raw"], dtype=np.float64)
    test_h5_y = concat_or_empty(state["test_h5_y"], dtype=np.int64)
    test_h5_edge = concat_or_empty(state["test_h5_edge"], dtype=np.float64)
    test_h5_edge_pred = concat_or_empty(state["test_h5_edge_pred"], dtype=np.float64)

    calib_h1 = fit_platt(valid_h1_prob_raw, valid_h1_y, int(args.calib_max_iter), float(args.prob_eps))
    calib_h5 = fit_platt(valid_h5_prob_raw, valid_h5_y, int(args.calib_max_iter), float(args.prob_eps))

    valid_h1_prob_cal = apply_platt(valid_h1_prob_raw, calib_h1["a"], calib_h1["b"], float(args.prob_eps))
    test_h1_prob_cal = apply_platt(test_h1_prob_raw, calib_h1["a"], calib_h1["b"], float(args.prob_eps))
    valid_h5_prob_cal = apply_platt(valid_h5_prob_raw, calib_h5["a"], calib_h5["b"], float(args.prob_eps))
    test_h5_prob_cal = apply_platt(test_h5_prob_raw, calib_h5["a"], calib_h5["b"], float(args.prob_eps))

    h5_threshold = optimize_h5_threshold(
        valid_prob_cal=valid_h5_prob_cal,
        valid_edge=valid_h5_edge,
        threshold_min=float(args.threshold_min),
        threshold_max=float(args.threshold_max),
        threshold_step=float(args.threshold_step),
        min_coverage=float(args.threshold_min_coverage),
        min_selected=int(args.threshold_min_selected),
    )

    h5_test_trade_metrics = threshold_metrics(
        prob_cal=test_h5_prob_cal,
        edge=test_h5_edge,
        threshold=float(h5_threshold["threshold"]),
    )
    h5_valid_edge_reg = regression_metrics(valid_h5_edge, valid_h5_edge_pred)
    h5_test_edge_reg = regression_metrics(test_h5_edge, test_h5_edge_pred)
    h5_valid_edge_profile = edge_profile(valid_h5_y, valid_h5_edge)
    h5_test_edge_profile = edge_profile(test_h5_y, test_h5_edge)

    return {
        "fold_id": int(state["fold_id"]),
        "train_count": int(state["train_count"]),
        "valid_count": int(state["valid_count"]),
        "test_count": int(state["test_count"]),
        "calibration": {
            "h1": calib_h1,
            "h5": calib_h5,
        },
        "metrics": {
            "h1": {
                "valid_raw": classification_metrics(valid_h1_y, valid_h1_prob_raw, float(args.prob_eps)),
                "valid_calibrated": classification_metrics(valid_h1_y, valid_h1_prob_cal, float(args.prob_eps)),
                "test_raw": classification_metrics(test_h1_y, test_h1_prob_raw, float(args.prob_eps)),
                "test_calibrated": classification_metrics(test_h1_y, test_h1_prob_cal, float(args.prob_eps)),
            },
            "h5": {
                "valid_raw": classification_metrics(valid_h5_y, valid_h5_prob_raw, float(args.prob_eps)),
                "valid_calibrated": classification_metrics(valid_h5_y, valid_h5_prob_cal, float(args.prob_eps)),
                "test_raw": classification_metrics(test_h5_y, test_h5_prob_raw, float(args.prob_eps)),
                "test_calibrated": classification_metrics(test_h5_y, test_h5_prob_cal, float(args.prob_eps)),
                "threshold_selection": h5_threshold,
                "test_trade_metrics": h5_test_trade_metrics,
                "valid_edge_regression": h5_valid_edge_reg,
                "test_edge_regression": h5_test_edge_reg,
                "valid_edge_profile": h5_valid_edge_profile,
                "test_edge_profile": h5_test_edge_profile,
            },
        },
        "artifacts": {
            "valid_h1_n": int(valid_h1_y.size),
            "test_h1_n": int(test_h1_y.size),
            "valid_h5_n": int(valid_h5_y.size),
            "test_h5_n": int(test_h5_y.size),
        },
        "model_state": {
            "h1": state["model_h1"],
            "h5": state["model_h5"],
            "h5_edge": state["model_edge"] if state["fitted_edge"] else None,
        },
    }


def save_fold_model_artifacts(
    *,
    model_dir: pathlib.Path,
    market: str,
    fold_eval: Dict[str, Any],
    args: argparse.Namespace,
) -> Dict[str, str]:
    out_dir = model_dir / market
    out_dir.mkdir(parents=True, exist_ok=True)
    fold_id = int(fold_eval["fold_id"])

    h1_path = out_dir / f"fold_{fold_id}_h1.joblib"
    h5_path = out_dir / f"fold_{fold_id}_h5.joblib"

    h1_payload = {
        "target": "h1",
        "target_column": str(args.h1_target_column),
        "market": market,
        "fold_id": fold_id,
        "feature_columns": FEATURE_COLUMNS,
        "transform_version": "v1_clip_transform",
        "sgd_params": {
            "alpha": float(args.alpha),
            "l1_ratio": float(args.l1_ratio),
            "batch_size": int(args.batch_size),
        },
        "calibration": fold_eval["calibration"]["h1"],
        "model": fold_eval["model_state"]["h1"],
    }
    h5_payload = {
        "target": "h5",
        "target_column": str(args.h5_target_column),
        "edge_column": str(args.edge_column),
        "drop_neutral_target": bool(args.drop_neutral_target),
        "market": market,
        "fold_id": fold_id,
        "feature_columns": FEATURE_COLUMNS,
        "transform_version": "v1_clip_transform",
        "sgd_params": {
            "alpha": float(args.alpha),
            "l1_ratio": float(args.l1_ratio),
            "batch_size": int(args.batch_size),
        },
        "calibration": fold_eval["calibration"]["h5"],
        "threshold_selection": fold_eval["metrics"]["h5"]["threshold_selection"],
        "edge_profile": fold_eval["metrics"]["h5"]["test_edge_profile"],
        "edge_regressor": None,
        "model": fold_eval["model_state"]["h5"],
    }
    edge_model = fold_eval.get("model_state", {}).get("h5_edge")
    if edge_model is not None:
        coef = np.asarray(edge_model.coef_, dtype=np.float64).reshape(-1).tolist()
        intercept = float(np.asarray(edge_model.intercept_, dtype=np.float64).reshape(-1)[0])
        h5_payload["edge_regressor"] = {
            "linear": {
                "coef": [float(x) for x in coef],
                "intercept": float(intercept),
            },
            "clip_abs_bps": float(max(10.0, args.edge_target_clip_bps)),
        }
    joblib.dump(h1_payload, h1_path)
    joblib.dump(h5_payload, h5_path)
    return {
        "h1_model": str(h1_path),
        "h5_model": str(h5_path),
    }


def init_fold_state(fold: Dict[str, Any], args: argparse.Namespace) -> Dict[str, Any]:
    win = fold["win"]
    edge_model_enabled = bool(args.enable_edge_regressor)
    return {
        "fold_id": int(fold["fold_id"]),
        "win": win,
        "model_h1": make_sgd_classifier(float(args.alpha), float(args.l1_ratio), int(args.random_state) + int(fold["fold_id"])),
        "model_h5": make_sgd_classifier(float(args.alpha), float(args.l1_ratio), int(args.random_state) + 100 + int(fold["fold_id"])),
        "model_edge": (
            make_sgd_regressor(float(args.alpha), float(args.l1_ratio), int(args.random_state) + 200 + int(fold["fold_id"]))
            if edge_model_enabled else None
        ),
        "edge_target_clip_bps": float(max(10.0, args.edge_target_clip_bps)),
        "fitted_h1": False,
        "fitted_h5": False,
        "fitted_edge": False,
        "train_x": [],
        "train_y_h1": [],
        "train_y_h5": [],
        "train_edge": [],
        "infer": {
            "valid": {"x": [], "y_h1": [], "y_h5": [], "edge": []},
            "test": {"x": [], "y_h1": [], "y_h5": [], "edge": []},
        },
        "train_count": 0,
        "valid_count": 0,
        "test_count": 0,
        "valid_h1_prob_raw": [],
        "valid_h1_y": [],
        "test_h1_prob_raw": [],
        "test_h1_y": [],
        "valid_h5_prob_raw": [],
        "valid_h5_y": [],
        "valid_h5_edge": [],
        "valid_h5_edge_pred": [],
        "test_h5_prob_raw": [],
        "test_h5_y": [],
        "test_h5_edge": [],
        "test_h5_edge_pred": [],
    }


def run_dataset_training(
    *,
    dataset: Dict[str, Any],
    args: argparse.Namespace,
    model_dir: pathlib.Path,
) -> Dict[str, Any]:
    market = str(dataset.get("market", "")).strip()
    output_path = str(dataset.get("output_path", "")).strip()
    csv_path = pathlib.Path(output_path)
    if not market or not csv_path.exists():
        return {
            "market": market,
            "status": "invalid_dataset",
            "csv_path": str(csv_path),
            "pass": False,
            "errors": [f"missing dataset csv: {csv_path}"],
        }

    folds_raw = dataset.get("folds", [])
    if not isinstance(folds_raw, list) or not folds_raw:
        return {
            "market": market,
            "status": "invalid_folds",
            "csv_path": str(csv_path),
            "pass": False,
            "errors": ["folds missing"],
        }

    folds = []
    for fold in folds_raw:
        if not isinstance(fold, dict):
            continue
        fold_id = int(fold.get("fold_id", 0) or 0)
        win = {
            "train_start": parse_iso_to_ts_ms(str(fold.get("train_start_utc", ""))),
            "train_end": parse_iso_to_ts_ms(str(fold.get("train_end_utc", ""))),
            "valid_start": parse_iso_to_ts_ms(str(fold.get("valid_start_utc", ""))),
            "valid_end": parse_iso_to_ts_ms(str(fold.get("valid_end_utc", ""))),
            "test_start": parse_iso_to_ts_ms(str(fold.get("test_start_utc", ""))),
            "test_end": parse_iso_to_ts_ms(str(fold.get("test_end_utc", ""))),
        }
        if fold_id <= 0 or min(win.values()) <= 0:
            continue
        folds.append({"fold_id": fold_id, "win": win})
    if not folds:
        return {
            "market": market,
            "status": "invalid_fold_windows",
            "csv_path": str(csv_path),
            "pass": False,
            "errors": ["no valid fold windows"],
        }

    fold_states = [init_fold_state(f, args) for f in folds]

    rows_total = 0
    rows_used = 0
    rows_skipped = 0

    print(f"[TrainProbModel] stream market={market} file={csv_path.name}", flush=True)
    with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows_total += 1
            ts = safe_float(row.get("timestamp"))
            y1 = safe_binary_target(
                row.get(str(args.h1_target_column)),
                drop_neutral=bool(args.drop_neutral_target),
            )
            y5 = safe_binary_target(
                row.get(str(args.h5_target_column)),
                drop_neutral=bool(args.drop_neutral_target),
            )
            edge = safe_float(row.get(str(args.edge_column)))
            if edge is None and str(args.edge_column) != "label_edge_bps_h5":
                edge = safe_float(row.get("label_edge_bps_h5"))
            if ts is None or y1 is None or y5 is None or edge is None:
                rows_skipped += 1
                continue
            x = build_feature_vector(row, float(args.clip_abs))
            if x is None:
                rows_skipped += 1
                continue
            ts_i = int(ts)
            rows_used += 1

            for state in fold_states:
                split = split_name_for_ts(ts_i, state["win"])
                if split is None:
                    continue
                if split == "train":
                    state["train_x"].append(x)
                    state["train_y_h1"].append(int(y1))
                    state["train_y_h5"].append(int(y5))
                    state["train_edge"].append(float(edge))
                    if len(state["train_x"]) >= int(args.batch_size):
                        flush_train_buffer(state)
                else:
                    buf = state["infer"][split]
                    buf["x"].append(x)
                    buf["y_h1"].append(int(y1))
                    buf["y_h5"].append(int(y5))
                    buf["edge"].append(float(edge))
                    if len(buf["x"]) >= int(args.infer_batch_size):
                        flush_infer_buffer(state, split, float(args.prob_eps))

    for state in fold_states:
        flush_train_buffer(state)
        flush_infer_buffer(state, "valid", float(args.prob_eps))
        flush_infer_buffer(state, "test", float(args.prob_eps))

    fold_results = []
    errors = []
    for state in fold_states:
        fold_eval = evaluate_fold_state(state=state, args=args)
        model_paths = save_fold_model_artifacts(
            model_dir=model_dir,
            market=market,
            fold_eval=fold_eval,
            args=args,
        )
        fold_eval.pop("model_state", None)
        fold_eval["model_artifacts"] = model_paths
        fold_results.append(fold_eval)

        if fold_eval["train_count"] <= 0 or fold_eval["test_count"] <= 0:
            errors.append(f"fold_{fold_eval['fold_id']}_insufficient_samples")

    status = "ok" if len(errors) == 0 else "partial"
    passed = len(errors) == 0
    return {
        "market": market,
        "status": status,
        "pass": bool(passed),
        "csv_path": str(csv_path),
        "rows_total": int(rows_total),
        "rows_used": int(rows_used),
        "rows_skipped": int(rows_skipped),
        "folds": fold_results,
        "errors": errors,
    }


def weighted_from_folds(
    fold_rows: List[Dict[str, Any]],
    target: str,
    split: str,
    metric_key: str,
) -> float:
    num = 0.0
    den = 0.0
    for row in fold_rows:
        try:
            n = float(row["metrics"][target][split]["n"])
            v = float(row["metrics"][target][split][metric_key])
        except Exception:
            continue
        if not math.isfinite(n) or n <= 0.0 or not math.isfinite(v):
            continue
        num += (n * v)
        den += n
    return (num / den) if den > 0.0 else math.nan


def compare_with_baseline(
    *,
    baseline_json: pathlib.Path,
    weighted_summary: Dict[str, Any],
) -> Dict[str, Any]:
    payload = load_json_or_none(baseline_json)
    if not isinstance(payload, dict):
        return {
            "status": "baseline_missing",
            "baseline_json": str(baseline_json),
        }
    base = payload.get("weighted_overall", {})
    if not isinstance(base, dict):
        return {
            "status": "baseline_invalid",
            "baseline_json": str(baseline_json),
        }

    h5_base_logloss = safe_float(base.get("test_h5_logloss"))
    h5_base_brier = safe_float(base.get("test_h5_brier"))
    h5_base_acc = safe_float(base.get("test_h5_accuracy"))
    h5_base_edge = safe_float(base.get("test_h5_mean_edge_bps"))

    h5_model_logloss = safe_float(weighted_summary.get("h5_test_calibrated_logloss"))
    h5_model_brier = safe_float(weighted_summary.get("h5_test_calibrated_brier"))
    h5_model_acc = safe_float(weighted_summary.get("h5_test_calibrated_accuracy"))
    h5_model_selected_edge = safe_float(weighted_summary.get("h5_test_selected_mean_edge_bps"))

    return {
        "status": "ok",
        "baseline_json": str(baseline_json),
        "baseline_weighted": {
            "h5_logloss": h5_base_logloss,
            "h5_brier": h5_base_brier,
            "h5_accuracy": h5_base_acc,
            "h5_mean_edge_bps": h5_base_edge,
        },
        "delta_model_minus_baseline": {
            "h5_logloss": (h5_model_logloss - h5_base_logloss)
            if h5_model_logloss is not None and h5_base_logloss is not None
            else math.nan,
            "h5_brier": (h5_model_brier - h5_base_brier)
            if h5_model_brier is not None and h5_base_brier is not None
            else math.nan,
            "h5_accuracy": (h5_model_acc - h5_base_acc)
            if h5_model_acc is not None and h5_base_acc is not None
            else math.nan,
            "h5_selected_edge_minus_baseline_unconditional_edge": (h5_model_selected_edge - h5_base_edge)
            if h5_model_selected_edge is not None and h5_base_edge is not None
            else math.nan,
        },
    }


def main(argv=None) -> int:
    args = parse_args(argv)

    if joblib is None or np is None or LogisticRegression is None or SGDClassifier is None:
        missing = []
        if np is None:
            missing.append("numpy")
        if joblib is None:
            missing.append("joblib")
        if LogisticRegression is None or SGDClassifier is None:
            missing.append("scikit-learn")
        raise RuntimeError(
            "Missing training dependencies: "
            + ", ".join(missing)
            + ". Install required packages before running training."
        )
    if bool(args.enable_edge_regressor) and SGDRegressor is None:
        raise RuntimeError("Edge regressor enabled but SGDRegressor is unavailable in scikit-learn.")
    split_manifest_path = resolve_repo_path(args.split_manifest_json)
    baseline_json_path = resolve_repo_path(args.baseline_json)
    output_json_path = resolve_repo_path(args.output_json)
    model_dir = resolve_repo_path(args.model_dir)
    model_dir.mkdir(parents=True, exist_ok=True)

    split_manifest = load_json_or_none(split_manifest_path)
    if not isinstance(split_manifest, dict):
        raise RuntimeError(f"invalid split manifest: {split_manifest_path}")
    datasets = split_manifest.get("datasets", [])
    if not isinstance(datasets, list) or not datasets:
        raise RuntimeError(f"empty datasets in split manifest: {split_manifest_path}")

    if int(args.max_datasets) > 0:
        datasets = datasets[: int(args.max_datasets)]

    started_at = utc_now_iso()
    print("[TrainProbModel] start", flush=True)
    print(f"split_manifest={split_manifest_path}", flush=True)
    print(f"datasets={len(datasets)}", flush=True)
    print(f"model_dir={model_dir}", flush=True)

    dataset_results = []
    failed = []
    fold_pool = []
    for idx, dataset in enumerate(datasets, start=1):
        market = str(dataset.get("market", "")).strip()
        print(f"[TrainProbModel] [{idx}/{len(datasets)}] market={market}", flush=True)
        result = run_dataset_training(dataset=dataset, args=args, model_dir=model_dir)
        dataset_results.append(result)
        if not bool(result.get("pass", False)):
            failed.append({"market": market, "errors": result.get("errors", [])})
            print(f"[TrainProbModel] [{idx}] fail market={market} errors={result.get('errors', [])}", flush=True)
        else:
            print(
                f"[TrainProbModel] [{idx}] pass market={market} used={result.get('rows_used', 0)}",
                flush=True,
            )
        for fold in result.get("folds", []) or []:
            if isinstance(fold, dict):
                fold_pool.append(fold)

    weighted_summary = {
        "h1_test_raw_logloss": weighted_from_folds(fold_pool, "h1", "test_raw", "logloss"),
        "h1_test_calibrated_logloss": weighted_from_folds(fold_pool, "h1", "test_calibrated", "logloss"),
        "h1_test_raw_brier": weighted_from_folds(fold_pool, "h1", "test_raw", "brier"),
        "h1_test_calibrated_brier": weighted_from_folds(fold_pool, "h1", "test_calibrated", "brier"),
        "h1_test_raw_accuracy": weighted_from_folds(fold_pool, "h1", "test_raw", "accuracy"),
        "h1_test_calibrated_accuracy": weighted_from_folds(fold_pool, "h1", "test_calibrated", "accuracy"),
        "h5_test_raw_logloss": weighted_from_folds(fold_pool, "h5", "test_raw", "logloss"),
        "h5_test_calibrated_logloss": weighted_from_folds(fold_pool, "h5", "test_calibrated", "logloss"),
        "h5_test_raw_brier": weighted_from_folds(fold_pool, "h5", "test_raw", "brier"),
        "h5_test_calibrated_brier": weighted_from_folds(fold_pool, "h5", "test_calibrated", "brier"),
        "h5_test_raw_accuracy": weighted_from_folds(fold_pool, "h5", "test_raw", "accuracy"),
        "h5_test_calibrated_accuracy": weighted_from_folds(fold_pool, "h5", "test_calibrated", "accuracy"),
        "h5_valid_edge_reg_mae_bps": weighted_from_folds(fold_pool, "h5", "valid_edge_regression", "mae_bps"),
        "h5_valid_edge_reg_rmse_bps": weighted_from_folds(fold_pool, "h5", "valid_edge_regression", "rmse_bps"),
        "h5_test_edge_reg_mae_bps": weighted_from_folds(fold_pool, "h5", "test_edge_regression", "mae_bps"),
        "h5_test_edge_reg_rmse_bps": weighted_from_folds(fold_pool, "h5", "test_edge_regression", "rmse_bps"),
    }

    total_h5_test = 0
    total_h5_selected = 0
    sum_h5_selected_edge = 0.0
    sum_h5_selected_pos = 0
    for fold in fold_pool:
        h5_trade = fold.get("metrics", {}).get("h5", {}).get("test_trade_metrics", {})
        n_total = int(h5_trade.get("n_total", 0) or 0)
        n_sel = int(h5_trade.get("n_selected", 0) or 0)
        total_h5_test += n_total
        total_h5_selected += n_sel
        mean_edge = safe_float(h5_trade.get("mean_edge_bps"))
        pos_edge = safe_float(h5_trade.get("positive_edge_rate"))
        if mean_edge is not None and math.isfinite(mean_edge) and n_sel > 0:
            sum_h5_selected_edge += (mean_edge * n_sel)
        if pos_edge is not None and math.isfinite(pos_edge) and n_sel > 0:
            sum_h5_selected_pos += int(round(pos_edge * n_sel))

    weighted_summary["h5_test_selection_coverage"] = (
        float(total_h5_selected) / float(total_h5_test) if total_h5_test > 0 else 0.0
    )
    weighted_summary["h5_test_selected_mean_edge_bps"] = (
        float(sum_h5_selected_edge) / float(total_h5_selected) if total_h5_selected > 0 else math.nan
    )
    weighted_summary["h5_test_selected_positive_edge_rate"] = (
        float(sum_h5_selected_pos) / float(total_h5_selected) if total_h5_selected > 0 else math.nan
    )
    weighted_summary["h5_test_selected_count"] = int(total_h5_selected)
    weighted_summary["h5_test_total_count"] = int(total_h5_test)

    baseline_compare = compare_with_baseline(
        baseline_json=baseline_json_path,
        weighted_summary=weighted_summary,
    )

    ended_at = utc_now_iso()
    summary = {
        "version": "probabilistic_pattern_model_v1",
        "started_at_utc": started_at,
        "finished_at_utc": ended_at,
        "status": "pass" if len(failed) == 0 else "partial_fail",
        "split_manifest_json": str(split_manifest_path),
        "baseline_json": str(baseline_json_path),
        "model_dir": str(model_dir),
        "feature_columns": FEATURE_COLUMNS,
        "target_columns": {
            "h1": str(args.h1_target_column),
            "h5": str(args.h5_target_column),
            "edge": str(args.edge_column),
            "drop_neutral_target": bool(args.drop_neutral_target),
        },
        "sgd_config": {
            "alpha": float(args.alpha),
            "l1_ratio": float(args.l1_ratio),
            "batch_size": int(args.batch_size),
            "infer_batch_size": int(args.infer_batch_size),
            "random_state": int(args.random_state),
            "enable_edge_regressor": bool(args.enable_edge_regressor),
            "edge_target_clip_bps": float(max(10.0, args.edge_target_clip_bps)),
        },
        "calibration_config": {
            "method": "platt_logistic_regression",
            "max_iter": int(args.calib_max_iter),
            "prob_eps": float(args.prob_eps),
        },
        "threshold_config": {
            "min": float(args.threshold_min),
            "max": float(args.threshold_max),
            "step": float(args.threshold_step),
            "min_coverage": float(args.threshold_min_coverage),
            "min_selected": int(args.threshold_min_selected),
        },
        "dataset_count": len(dataset_results),
        "dataset_passed": int(sum(1 for x in dataset_results if bool(x.get("pass", False)))),
        "dataset_failed": int(len(failed)),
        "failed": failed,
        "weighted_summary": weighted_summary,
        "baseline_comparison": baseline_compare,
        "datasets": dataset_results,
    }
    dump_json(output_json_path, summary)

    print("[TrainProbModel] completed", flush=True)
    print(f"status={summary['status']}", flush=True)
    print(f"dataset_passed={summary['dataset_passed']} failed={summary['dataset_failed']}", flush=True)
    print(f"output={output_json_path}", flush=True)
    return 0 if summary["status"] == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
