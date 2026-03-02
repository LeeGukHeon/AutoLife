
#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import gc
import math
import pathlib
import time
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

import joblib
import lightgbm as lgb
import numpy as np
from sklearn.metrics import accuracy_score, brier_score_loss, log_loss, roc_auc_score

from _script_common import dump_json, load_json_or_none, resolve_repo_path
from train_probabilistic_pattern_model import (
    FEATURE_COLUMNS,
    apply_platt,
    build_feature_vector,
    fit_platt,
    parse_iso_to_ts_ms,
    safe_binary_target,
    safe_float,
)

try:
    import psutil
except Exception:  # pragma: no cover
    psutil = None


TARGET_TP1_PRIMARY = "label_tp1_hit_h20m"
TARGET_TP1_DEPRECATED_ALIAS = "label_tp1_hit_h5m"


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Train global LightGBM probabilistic model on fixed walk-forward split.")
    p.add_argument(
        "--split-manifest-json",
        default=r".\data\model_input\probabilistic_features_v3_full_label_tp1H20_20260301\probabilistic_split_manifest_v2_draft.json",
    )
    p.add_argument(
        "--output-json",
        default=r".\build\Release\logs\train_summary_global_label_tp1H20_lgbm.json",
    )
    p.add_argument(
        "--fold-auc-json",
        default=r".\build\Release\logs\stageG3_fold_auc_report.json",
    )
    p.add_argument(
        "--feature-importance-json",
        default=r".\build\Release\logs\stageG3_feature_importance.json",
    )
    p.add_argument(
        "--model-dir",
        default=r".\build\Release\models\probabilistic_pattern_global_v3_lgbm_tp1H20",
    )
    p.add_argument(
        "--target-column",
        "--h5-target-column",
        dest="h5_target_column",
        default=TARGET_TP1_PRIMARY,
        help="Primary binary target column (falls back to deprecated alias if missing).",
    )
    p.add_argument("--edge-column", default="label_edge_bps_h5")
    p.add_argument("--drop-neutral-target", action="store_true", default=True)
    p.add_argument("--keep-neutral-target", dest="drop_neutral_target", action="store_false")
    p.add_argument("--clip-abs", type=float, default=8.0)
    p.add_argument("--prob-eps", type=float, default=1e-6)
    p.add_argument("--calibration-mode", choices=("none", "platt"), default="none")

    p.add_argument("--learning-rate", type=float, default=0.05)
    p.add_argument("--num-leaves", type=int, default=63)
    p.add_argument("--max-depth", type=int, default=-1)
    p.add_argument("--min-data-in-leaf", type=int, default=200)
    p.add_argument("--feature-fraction", type=float, default=0.8)
    p.add_argument("--bagging-fraction", type=float, default=0.8)
    p.add_argument("--bagging-freq", type=int, default=1)
    p.add_argument("--lambda-l1", type=float, default=0.0)
    p.add_argument("--lambda-l2", type=float, default=1.0)
    p.add_argument("--n-estimators", type=int, default=2000)
    p.add_argument("--early-stopping-rounds", type=int, default=100)
    p.add_argument("--random-state", type=int, default=42)
    p.add_argument("--num-threads", type=int, default=6)
    p.add_argument("--max-bin", type=int, default=127)
    p.add_argument("--min-data-in-bin", type=int, default=10)
    p.add_argument("--n-jobs", type=int, default=-1)
    p.add_argument(
        "--fold-ids",
        default="",
        help="Optional comma-separated fold ids to run (default: all available folds).",
    )
    p.add_argument(
        "--resume-existing-folds",
        action="store_true",
        help="Reuse existing fold model artifacts (load+evaluate only) when present.",
    )
    return p.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def safe_auc(y_true: np.ndarray, prob: np.ndarray) -> float:
    if y_true.size == 0 or prob.size == 0:
        return math.nan
    if int(np.min(y_true)) == int(np.max(y_true)):
        return math.nan
    try:
        return float(roc_auc_score(y_true, prob))
    except Exception:
        return math.nan


def classification_metrics(y_true: np.ndarray, prob: np.ndarray, eps: float) -> Dict[str, Any]:
    if y_true.size == 0 or prob.size == 0:
        return {
            "n": 0,
            "auc": math.nan,
            "logloss": math.nan,
            "brier": math.nan,
            "accuracy": math.nan,
            "positive_rate": math.nan,
        }
    p = np.clip(prob.astype(np.float64), float(eps), 1.0 - float(eps))
    y = y_true.astype(np.int64)
    pred = (p >= 0.5).astype(np.int64)
    return {
        "n": int(y.size),
        "auc": safe_auc(y, p),
        "logloss": float(log_loss(y, p, labels=[0, 1])),
        "brier": float(brier_score_loss(y, p)),
        "accuracy": float(accuracy_score(y, pred)),
        "positive_rate": float(np.mean(y)),
    }


class BoosterBinaryWrapper:
    """Minimal predictor wrapper to keep joblib package interface stable."""

    def __init__(self, booster: lgb.Booster):
        self.booster_ = booster
        self.best_iteration_ = int(getattr(booster, "best_iteration", 0) or 0)

    def predict_proba(self, x: np.ndarray) -> np.ndarray:
        x_in = np.asarray(x, dtype=np.float32, order="C")
        num_iteration = int(self.best_iteration_) if int(self.best_iteration_) > 0 else None
        p = self.booster_.predict(x_in, num_iteration=num_iteration)
        p = np.clip(np.asarray(p, dtype=np.float64), 0.0, 1.0).reshape(-1)
        return np.column_stack((1.0 - p, p))


def resolve_num_threads(args: argparse.Namespace) -> int:
    # Keep backward compatibility with --n-jobs while preferring explicit --num-threads.
    if int(getattr(args, "n_jobs", -1)) > 0:
        return max(1, int(args.n_jobs))
    return max(1, int(getattr(args, "num_threads", 6)))


def get_positive_prob(model_obj: Any, x: np.ndarray) -> np.ndarray:
    x_in = np.asarray(x, dtype=np.float32, order="C")
    if hasattr(model_obj, "predict_proba"):
        prob = model_obj.predict_proba(x_in)
        prob_arr = np.asarray(prob, dtype=np.float64)
        if prob_arr.ndim == 2 and prob_arr.shape[1] >= 2:
            return prob_arr[:, 1].reshape(-1)
        return prob_arr.reshape(-1)
    if isinstance(model_obj, lgb.Booster):
        return np.asarray(model_obj.predict(x_in), dtype=np.float64).reshape(-1)
    booster = getattr(model_obj, "booster_", None)
    if isinstance(booster, lgb.Booster):
        return np.asarray(booster.predict(x_in), dtype=np.float64).reshape(-1)
    raise RuntimeError(f"unsupported model type for probability inference: {type(model_obj)}")


def get_booster_and_best_iteration(model_obj: Any, package_best_iteration: int) -> Tuple[lgb.Booster, int]:
    booster: Optional[lgb.Booster] = None
    best_iter = int(package_best_iteration)
    if isinstance(model_obj, lgb.Booster):
        booster = model_obj
    else:
        maybe_booster = getattr(model_obj, "booster_", None)
        if isinstance(maybe_booster, lgb.Booster):
            booster = maybe_booster
    if booster is None:
        raise RuntimeError(f"unable to resolve lightgbm booster from model type: {type(model_obj)}")
    if best_iter <= 0:
        best_iter = int(getattr(model_obj, "best_iteration_", 0) or 0)
    if best_iter <= 0:
        best_iter = int(getattr(booster, "best_iteration", 0) or 0)
    return booster, int(best_iter)


def split_name_for_ts(ts_ms: int, win: Dict[str, int]) -> Optional[str]:
    if int(win["train_start"]) <= ts_ms <= int(win["train_end"]):
        return "train"
    if int(win["valid_start"]) <= ts_ms <= int(win["valid_end"]):
        return "valid"
    if int(win["test_start"]) <= ts_ms <= int(win["test_end"]):
        return "test"
    return None


def infer_pipeline_version(split_manifest: Dict[str, Any]) -> str:
    explicit = str(split_manifest.get("pipeline_version", "")).strip().lower()
    if explicit:
        return explicit
    source = str(split_manifest.get("source_manifest_version", "")).strip().lower()
    if "v2" in source:
        return "v2"
    return "v2"


def parse_requested_fold_ids(raw: str, available_fold_ids: List[int]) -> List[int]:
    available = sorted({int(x) for x in available_fold_ids if int(x) > 0})
    raw_text = str(raw or "").strip()
    if not raw_text:
        return available
    out: List[int] = []
    seen = set()
    for token in raw_text.split(","):
        text = token.strip()
        if not text:
            continue
        try:
            value = int(text)
        except Exception:
            continue
        if value <= 0 or value not in available or value in seen:
            continue
        seen.add(value)
        out.append(value)
    return out


def parse_dataset_windows(dataset: Dict[str, Any]) -> Dict[int, Dict[str, int]]:
    out: Dict[int, Dict[str, int]] = {}
    for fold in dataset.get("folds", []) or []:
        if not isinstance(fold, dict):
            continue
        fid = int(fold.get("fold_id", 0) or 0)
        if fid <= 0:
            continue
        win = {
            "train_start": parse_iso_to_ts_ms(str(fold.get("train_start_utc", ""))),
            "train_end": parse_iso_to_ts_ms(str(fold.get("train_end_utc", ""))),
            "valid_start": parse_iso_to_ts_ms(str(fold.get("valid_start_utc", ""))),
            "valid_end": parse_iso_to_ts_ms(str(fold.get("valid_end_utc", ""))),
            "test_start": parse_iso_to_ts_ms(str(fold.get("test_start_utc", ""))),
            "test_end": parse_iso_to_ts_ms(str(fold.get("test_end_utc", ""))),
        }
        if min(win.values()) <= 0:
            continue
        out[fid] = win
    return out

def collect_fold_data(
    *,
    datasets: List[Dict[str, Any]],
    fold_id: int,
    clip_abs: float,
    h5_target_column: str,
    edge_column: str,
    drop_neutral_target: bool,
) -> Dict[str, Any]:
    train_x: List[List[float]] = []
    train_y: List[int] = []
    train_edge: List[float] = []
    valid_x: List[List[float]] = []
    valid_y: List[int] = []
    valid_edge: List[float] = []
    test_x: List[List[float]] = []
    test_y: List[int] = []
    test_edge: List[float] = []

    rows_total = 0
    rows_used = 0
    rows_skipped = 0
    target_source_counts = {"primary": 0, "fallback_static": 0, "missing": 0}

    for ds in datasets:
        market = str(ds.get("market", "")).strip()
        csv_path = pathlib.Path(str(ds.get("output_path", "")).strip())
        if not market or not csv_path.exists():
            continue
        windows = parse_dataset_windows(ds)
        win = windows.get(int(fold_id))
        if not win:
            continue

        with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                rows_total += 1
                ts = safe_float(row.get("timestamp"))
                y_raw = row.get(str(h5_target_column))
                target_source = "primary"
                if (y_raw is None or str(y_raw).strip() == "") and str(h5_target_column) != TARGET_TP1_DEPRECATED_ALIAS:
                    y_raw = row.get(TARGET_TP1_DEPRECATED_ALIAS)
                    target_source = "fallback_static"
                y = safe_binary_target(y_raw, drop_neutral=bool(drop_neutral_target))
                edge = safe_float(row.get(str(edge_column)))
                if edge is None and str(edge_column) != "label_edge_bps_h5":
                    edge = safe_float(row.get("label_edge_bps_h5"))
                if ts is None or y is None or edge is None:
                    if ts is not None and (y_raw is None or str(y_raw).strip() == ""):
                        target_source_counts["missing"] += 1
                    rows_skipped += 1
                    continue
                target_source_counts[target_source] += 1
                x = build_feature_vector(row, float(clip_abs))
                if x is None:
                    rows_skipped += 1
                    continue
                split_name = split_name_for_ts(int(ts), win)
                if split_name is None:
                    continue
                rows_used += 1
                if split_name == "train":
                    train_x.append(x)
                    train_y.append(int(y))
                    train_edge.append(float(edge))
                elif split_name == "valid":
                    valid_x.append(x)
                    valid_y.append(int(y))
                    valid_edge.append(float(edge))
                else:
                    test_x.append(x)
                    test_y.append(int(y))
                    test_edge.append(float(edge))

    packed = {
        "train_x": np.asarray(train_x, dtype=np.float32, order="C"),
        "train_y": np.asarray(train_y, dtype=np.int32),
        "train_edge": np.asarray(train_edge, dtype=np.float32),
        "valid_x": np.asarray(valid_x, dtype=np.float32, order="C"),
        "valid_y": np.asarray(valid_y, dtype=np.int32),
        "valid_edge": np.asarray(valid_edge, dtype=np.float32),
        "test_x": np.asarray(test_x, dtype=np.float32, order="C"),
        "test_y": np.asarray(test_y, dtype=np.int32),
        "test_edge": np.asarray(test_edge, dtype=np.float32),
        "rows_total": int(rows_total),
        "rows_used": int(rows_used),
        "rows_skipped": int(rows_skipped),
        "target_source_counts": target_source_counts,
    }
    # Release list-backed buffers early before returning numpy arrays.
    train_x.clear()
    train_y.clear()
    train_edge.clear()
    valid_x.clear()
    valid_y.clear()
    valid_edge.clear()
    test_x.clear()
    test_y.clear()
    test_edge.clear()
    gc.collect()
    return packed


def train_single_fold(
    *,
    fold_id: int,
    fold_data: Dict[str, Any],
    args: argparse.Namespace,
    model_dir: pathlib.Path,
) -> Tuple[Dict[str, Any], Dict[str, np.ndarray]]:
    x_train = fold_data["train_x"]
    y_train = fold_data["train_y"]
    x_valid = fold_data["valid_x"]
    y_valid = fold_data["valid_y"]
    x_test = fold_data["test_x"]
    y_test = fold_data["test_y"]

    if x_train.shape[0] <= 0 or x_valid.shape[0] <= 0:
        raise RuntimeError(f"fold {fold_id}: empty train/valid split")
    if len(np.unique(y_train)) < 2 or len(np.unique(y_valid)) < 2:
        raise RuntimeError(f"fold {fold_id}: target has single class in train/valid")

    num_threads = resolve_num_threads(args)
    train_set = lgb.Dataset(
        np.asarray(x_train, dtype=np.float32, order="C"),
        label=np.asarray(y_train, dtype=np.int32),
        free_raw_data=True,
    )
    valid_set = lgb.Dataset(
        np.asarray(x_valid, dtype=np.float32, order="C"),
        label=np.asarray(y_valid, dtype=np.int32),
        reference=train_set,
        free_raw_data=True,
    )
    params = {
        "objective": "binary",
        "metric": "auc",
        "learning_rate": float(args.learning_rate),
        "num_leaves": int(args.num_leaves),
        "max_depth": int(args.max_depth),
        "min_data_in_leaf": int(args.min_data_in_leaf),
        "feature_fraction": float(args.feature_fraction),
        "bagging_fraction": float(args.bagging_fraction),
        "bagging_freq": int(args.bagging_freq),
        "lambda_l1": float(args.lambda_l1),
        "lambda_l2": float(args.lambda_l2),
        "max_bin": int(args.max_bin),
        "min_data_in_bin": int(args.min_data_in_bin),
        "num_threads": int(num_threads),
        "seed": int(args.random_state) + int(fold_id),
        "verbosity": -1,
    }

    fit_start = time.perf_counter()
    booster = lgb.train(
        params=params,
        train_set=train_set,
        num_boost_round=int(args.n_estimators),
        valid_sets=[valid_set],
        valid_names=["valid"],
        callbacks=[lgb.early_stopping(stopping_rounds=int(args.early_stopping_rounds), verbose=False)],
    )
    fit_sec = float(time.perf_counter() - fit_start)
    model = BoosterBinaryWrapper(booster)

    p_train_raw = get_positive_prob(model, x_train)
    p_valid_raw = get_positive_prob(model, x_valid)
    p_test_raw = get_positive_prob(model, x_test) if x_test.shape[0] > 0 else np.empty((0,), dtype=np.float64)

    if str(args.calibration_mode) == "platt":
        calib = fit_platt(p_valid_raw, y_valid, max_iter=200, eps=float(args.prob_eps))
    else:
        calib = {"type": "identity", "a": 1.0, "b": 0.0, "status": "disabled"}

    p_train = apply_platt(p_train_raw, float(calib.get("a", 1.0)), float(calib.get("b", 0.0)), float(args.prob_eps))
    p_valid = apply_platt(p_valid_raw, float(calib.get("a", 1.0)), float(calib.get("b", 0.0)), float(args.prob_eps))
    p_test = apply_platt(p_test_raw, float(calib.get("a", 1.0)), float(calib.get("b", 0.0)), float(args.prob_eps))

    best_iter = int(getattr(booster, "best_iteration", 0) or 0)
    gain = booster.feature_importance(importance_type="gain")
    split = booster.feature_importance(importance_type="split")

    fold_model_dir = model_dir / "GLOBAL"
    fold_model_dir.mkdir(parents=True, exist_ok=True)
    h5_model_path = fold_model_dir / f"fold_{int(fold_id)}_h5_lightgbm.joblib"
    h5_model_txt_path = fold_model_dir / f"fold_{int(fold_id)}_h5_lightgbm.txt"
    booster.save_model(
        str(h5_model_txt_path),
        num_iteration=(int(best_iter) if int(best_iter) > 0 else -1),
    )
    joblib.dump(
        {
            "target": "h5",
            "target_column": str(args.h5_target_column),
            "model_type": "lightgbm_classifier",
            "fold_id": int(fold_id),
            "feature_columns": FEATURE_COLUMNS,
            "transform_version": "v1_clip_transform",
            "calibration": calib,
            "best_iteration": int(best_iter),
            "lightgbm_txt_model_path": str(h5_model_txt_path),
            "model": model,
        },
        h5_model_path,
    )
    del train_set
    del valid_set
    gc.collect()

    fold_summary = {
        "fold_id": int(fold_id),
        "train_count": int(x_train.shape[0]),
        "valid_count": int(x_valid.shape[0]),
        "test_count": int(x_test.shape[0]),
        "fit_seconds": float(fit_sec),
        "best_iteration": int(best_iter),
        "calibration": {"h5": calib},
        "metrics": {
            "h5": {
                "train": classification_metrics(y_train, p_train, float(args.prob_eps)),
                "valid": classification_metrics(y_valid, p_valid, float(args.prob_eps)),
                "test": classification_metrics(y_test, p_test, float(args.prob_eps)),
            }
        },
        "model_artifacts": {
            "h5_model": str(h5_model_path),
            "h5_model_txt": str(h5_model_txt_path),
        },
        "data_loading": {
            "rows_total": int(fold_data["rows_total"]),
            "rows_used": int(fold_data["rows_used"]),
            "rows_skipped": int(fold_data["rows_skipped"]),
            "target_source_counts": fold_data.get("target_source_counts", {}),
        },
    }
    return fold_summary, {"gain": gain, "split": split}


def evaluate_existing_fold(
    *,
    fold_id: int,
    fold_data: Dict[str, Any],
    args: argparse.Namespace,
    model_path: pathlib.Path,
) -> Tuple[Dict[str, Any], Dict[str, np.ndarray]]:
    x_train = fold_data["train_x"]
    y_train = fold_data["train_y"]
    x_valid = fold_data["valid_x"]
    y_valid = fold_data["valid_y"]
    x_test = fold_data["test_x"]
    y_test = fold_data["test_y"]

    package = joblib.load(model_path)
    if isinstance(package, dict):
        clf = package.get("model")
        loaded_calib = package.get(
            "calibration",
            {"type": "identity", "a": 1.0, "b": 0.0, "status": "loaded_default"},
        )
        best_iter = int(package.get("best_iteration", 0) or 0)
    else:
        clf = package
        loaded_calib = {"type": "identity", "a": 1.0, "b": 0.0, "status": "loaded_default"}
        best_iter = 0

    if clf is None:
        raise RuntimeError(f"fold {fold_id}: existing model package is missing 'model': {model_path}")

    p_train_raw = get_positive_prob(clf, x_train)
    p_valid_raw = get_positive_prob(clf, x_valid)
    p_test_raw = get_positive_prob(clf, x_test) if x_test.shape[0] > 0 else np.empty((0,), dtype=np.float64)
    if str(args.calibration_mode) == "platt":
        calib = fit_platt(p_valid_raw, y_valid, max_iter=200, eps=float(args.prob_eps))
        if str(calib.get("status", "")).strip():
            calib["status"] = f"resume_refit_{calib['status']}"
    else:
        calib = loaded_calib
    p_train = apply_platt(p_train_raw, float(calib.get("a", 1.0)), float(calib.get("b", 0.0)), float(args.prob_eps))
    p_valid = apply_platt(p_valid_raw, float(calib.get("a", 1.0)), float(calib.get("b", 0.0)), float(args.prob_eps))
    p_test = apply_platt(p_test_raw, float(calib.get("a", 1.0)), float(calib.get("b", 0.0)), float(args.prob_eps))

    booster, best_iter = get_booster_and_best_iteration(clf, best_iter)
    txt_model_path_raw = (
        str(package.get("lightgbm_txt_model_path", "")).strip()
        if isinstance(package, dict)
        else ""
    )
    txt_model_path = pathlib.Path(txt_model_path_raw) if txt_model_path_raw else model_path.with_name(model_path.stem + ".txt")
    txt_model_path.parent.mkdir(parents=True, exist_ok=True)
    booster.save_model(
        str(txt_model_path),
        num_iteration=(int(best_iter) if int(best_iter) > 0 else -1),
    )
    if isinstance(package, dict):
        package["model"] = clf
        package["calibration"] = calib
        package["best_iteration"] = int(best_iter)
        package["lightgbm_txt_model_path"] = str(txt_model_path)
        joblib.dump(package, model_path)
    elif str(args.calibration_mode) == "platt":
        joblib.dump(
            {
                "target": "h5",
                "target_column": str(args.h5_target_column),
                "model_type": "lightgbm_classifier",
                "fold_id": int(fold_id),
                "feature_columns": FEATURE_COLUMNS,
                "transform_version": "v1_clip_transform",
                "calibration": calib,
                "best_iteration": int(best_iter),
                "lightgbm_txt_model_path": str(txt_model_path),
                "model": clf,
            },
            model_path,
        )
    gain = booster.feature_importance(importance_type="gain")
    split = booster.feature_importance(importance_type="split")

    fold_summary = {
        "fold_id": int(fold_id),
        "train_count": int(x_train.shape[0]),
        "valid_count": int(x_valid.shape[0]),
        "test_count": int(x_test.shape[0]),
        "fit_seconds": 0.0,
        "best_iteration": int(best_iter),
        "resume_loaded": True,
        "calibration": {"h5": calib},
        "metrics": {
            "h5": {
                "train": classification_metrics(y_train, p_train, float(args.prob_eps)),
                "valid": classification_metrics(y_valid, p_valid, float(args.prob_eps)),
                "test": classification_metrics(y_test, p_test, float(args.prob_eps)),
            }
        },
        "model_artifacts": {
            "h5_model": str(model_path),
            "h5_model_txt": str(txt_model_path),
        },
        "data_loading": {
            "rows_total": int(fold_data["rows_total"]),
            "rows_used": int(fold_data["rows_used"]),
            "rows_skipped": int(fold_data["rows_skipped"]),
            "target_source_counts": fold_data.get("target_source_counts", {}),
        },
    }
    return fold_summary, {"gain": gain, "split": split}

def process_memory_mb() -> Optional[float]:
    if psutil is None:
        return None
    try:
        proc = psutil.Process()
        return float(proc.memory_info().rss) / (1024.0 * 1024.0)
    except Exception:
        return None


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    split_manifest_path = resolve_repo_path(args.split_manifest_json)
    output_json_path = resolve_repo_path(args.output_json)
    fold_auc_json_path = resolve_repo_path(args.fold_auc_json)
    feature_importance_json_path = resolve_repo_path(args.feature_importance_json)
    model_dir = resolve_repo_path(args.model_dir)

    split_manifest = load_json_or_none(split_manifest_path)
    if not isinstance(split_manifest, dict):
        raise RuntimeError(f"invalid split manifest: {split_manifest_path}")

    datasets = split_manifest.get("datasets", [])
    if not isinstance(datasets, list) or not datasets:
        raise RuntimeError("split manifest has no datasets")

    available_fold_ids: List[int] = sorted(
        {
            int(f.get("fold_id", 0) or 0)
            for ds in datasets
            if isinstance(ds, dict)
            for f in (ds.get("folds", []) or [])
            if isinstance(f, dict)
        }
    )
    available_fold_ids = [x for x in available_fold_ids if x > 0]
    fold_ids = parse_requested_fold_ids(str(args.fold_ids), available_fold_ids)
    if not fold_ids:
        raise RuntimeError("no fold ids found")

    started_at = utc_now_iso()
    started_perf = time.perf_counter()
    mem_before = process_memory_mb()

    fold_summaries: List[Dict[str, Any]] = []
    gain_sums = np.zeros((len(FEATURE_COLUMNS),), dtype=np.float64)
    split_sums = np.zeros((len(FEATURE_COLUMNS),), dtype=np.float64)

    for fid in fold_ids:
        print(f"[StageG3-LGBM] fold={fid} data_loading...", flush=True)
        fold_data = collect_fold_data(
            datasets=datasets,
            fold_id=int(fid),
            clip_abs=float(args.clip_abs),
            h5_target_column=str(args.h5_target_column),
            edge_column=str(args.edge_column),
            drop_neutral_target=bool(args.drop_neutral_target),
        )
        print(
            f"[StageG3-LGBM] fold={fid} train={fold_data['train_x'].shape[0]} valid={fold_data['valid_x'].shape[0]} test={fold_data['test_x'].shape[0]}",
            flush=True,
        )

        model_path = model_dir / "GLOBAL" / f"fold_{int(fid)}_h5_lightgbm.joblib"
        if bool(args.resume_existing_folds) and model_path.exists():
            print(f"[StageG3-LGBM] fold={fid} resume-existing model={model_path}", flush=True)
            fold_summary, importances = evaluate_existing_fold(
                fold_id=int(fid),
                fold_data=fold_data,
                args=args,
                model_path=model_path,
            )
        else:
            fold_summary, importances = train_single_fold(
                fold_id=int(fid),
                fold_data=fold_data,
                args=args,
                model_dir=model_dir,
            )
        fold_summaries.append(fold_summary)
        gain_sums += np.asarray(importances["gain"], dtype=np.float64)
        split_sums += np.asarray(importances["split"], dtype=np.float64)
        fold_data.clear()
        del fold_data
        gc.collect()

    valid_aucs = [float(((f.get("metrics") or {}).get("h5") or {}).get("valid", {}).get("auc", math.nan)) for f in fold_summaries]
    train_aucs = [float(((f.get("metrics") or {}).get("h5") or {}).get("train", {}).get("auc", math.nan)) for f in fold_summaries]
    test_aucs = [float(((f.get("metrics") or {}).get("h5") or {}).get("test", {}).get("auc", math.nan)) for f in fold_summaries]

    finished_perf = time.perf_counter()
    mem_after = process_memory_mb()

    summary = {
        "version": "probabilistic_pattern_model_global_v3_lightgbm",
        "scope": "global_cross_market",
        "status": "pass",
        "started_at_utc": started_at,
        "finished_at_utc": utc_now_iso(),
        "elapsed_seconds": float(finished_perf - started_perf),
        "split_manifest_json": str(split_manifest_path),
        "model_dir": str(model_dir),
        "feature_columns": FEATURE_COLUMNS,
        "target_columns": {
            "h5_requested": str(args.h5_target_column),
            "h5_primary": TARGET_TP1_PRIMARY,
            "h5_deprecated_alias": TARGET_TP1_DEPRECATED_ALIAS,
            "edge": str(args.edge_column),
            "drop_neutral_target": bool(args.drop_neutral_target),
        },
        "pipeline_version": infer_pipeline_version(split_manifest),
        "lightgbm_config": {
            "objective": "binary",
            "metric": "auc",
            "learning_rate": float(args.learning_rate),
            "num_leaves": int(args.num_leaves),
            "max_depth": int(args.max_depth),
            "min_data_in_leaf": int(args.min_data_in_leaf),
            "max_bin": int(args.max_bin),
            "min_data_in_bin": int(args.min_data_in_bin),
            "feature_fraction": float(args.feature_fraction),
            "bagging_fraction": float(args.bagging_fraction),
            "bagging_freq": int(args.bagging_freq),
            "lambda_l1": float(args.lambda_l1),
            "lambda_l2": float(args.lambda_l2),
            "n_estimators": int(args.n_estimators),
            "early_stopping_rounds": int(args.early_stopping_rounds),
            "random_state": int(args.random_state),
            "num_threads": int(resolve_num_threads(args)),
            "n_jobs": int(args.n_jobs),
        },
        "calibration_mode": str(args.calibration_mode),
        "memory_mb": {"before": mem_before, "after": mem_after},
        "resume_existing_folds": bool(args.resume_existing_folds),
        "available_fold_ids": available_fold_ids,
        "requested_fold_ids": fold_ids,
        "fold_ids": fold_ids,
        "global_model": {"fold_count": len(fold_summaries), "folds": fold_summaries},
        "summary_metrics": {
            "h5_train_auc_mean": float(np.nanmean(np.asarray(train_aucs, dtype=np.float64))),
            "h5_valid_auc_mean": float(np.nanmean(np.asarray(valid_aucs, dtype=np.float64))),
            "h5_test_auc_mean": float(np.nanmean(np.asarray(test_aucs, dtype=np.float64))),
        },
    }
    dump_json(output_json_path, summary)

    fold_auc_payload = {
        "generated_at_utc": utc_now_iso(),
        "status": "pass",
        "train_summary_json": str(output_json_path),
        "folds": [
            {
                "fold_id": int(f["fold_id"]),
                "best_iteration": int(f.get("best_iteration", 0) or 0),
                "train_auc": float(((f.get("metrics") or {}).get("h5") or {}).get("train", {}).get("auc", math.nan)),
                "valid_auc": float(((f.get("metrics") or {}).get("h5") or {}).get("valid", {}).get("auc", math.nan)),
                "test_auc": float(((f.get("metrics") or {}).get("h5") or {}).get("test", {}).get("auc", math.nan)),
            }
            for f in fold_summaries
        ],
        "summary": {
            "train_auc_mean": float(np.nanmean(np.asarray(train_aucs, dtype=np.float64))),
            "valid_auc_mean": float(np.nanmean(np.asarray(valid_aucs, dtype=np.float64))),
            "test_auc_mean": float(np.nanmean(np.asarray(test_aucs, dtype=np.float64))),
            "success_valid_auc_ge_0_55": bool(float(np.nanmean(np.asarray(valid_aucs, dtype=np.float64))) >= 0.55),
        },
    }
    dump_json(fold_auc_json_path, fold_auc_payload)

    rows = []
    for idx, feat in enumerate(FEATURE_COLUMNS):
        rows.append(
            {
                "feature": feat,
                "gain": float(gain_sums[idx]),
                "split": float(split_sums[idx]),
            }
        )
    rows.sort(key=lambda x: x["gain"], reverse=True)
    fi_payload = {
        "generated_at_utc": utc_now_iso(),
        "status": "pass",
        "train_summary_json": str(output_json_path),
        "top10_gain": rows[:10],
        "top50_gain": rows[:50],
        "all_features": rows,
    }
    dump_json(feature_importance_json_path, fi_payload)

    print("[StageG3-LGBM] completed", flush=True)
    print(f"output_json={output_json_path}", flush=True)
    print(f"fold_auc_json={fold_auc_json_path}", flush=True)
    print(f"feature_importance_json={feature_importance_json_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
