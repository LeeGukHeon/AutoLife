
#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import math
import pathlib
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import joblib
import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score

from _script_common import dump_json, load_json_or_none, resolve_repo_path
from train_probabilistic_pattern_model import FEATURE_COLUMNS, apply_platt


@dataclass
class LoadedDataset:
    dataset_name: str
    csv_paths: List[pathlib.Path]
    market_list: List[str]
    timestamps: np.ndarray
    markets: np.ndarray
    label_up_h1: np.ndarray
    label_up_h5: np.ndarray
    label_edge_bps_h5: np.ndarray
    features_raw: Dict[str, np.ndarray]
    transformed_matrix: np.ndarray
    base_regime: np.ndarray


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Stage G1 training dataset structural audit")
    p.add_argument("--train-feature-dir", default=r".\data\model_input\probabilistic_features_v2_draft_gate4_20260224")
    p.add_argument("--split-manifest-json", default=r".\data\model_input\probabilistic_features_v2_draft_gate4_20260224\probabilistic_split_manifest_v2_draft.json")
    p.add_argument("--train-summary-json", default=r".\build\Release\logs\probabilistic_model_train_summary_global_v2_gate4_20260224.json")
    p.add_argument("--fresh14-root", default=r".\data\backtest_fresh_14d")
    p.add_argument("--output-dir", default=r".\build\Release\logs")
    p.add_argument("--fresh14-temp-dir", default=r".\build\Release\tmp\stageG1_fresh14_features")
    p.add_argument("--include-drift-precheck", action="store_true", default=True)
    p.add_argument("--skip-drift-precheck", dest="include_drift_precheck", action="store_false")
    return p.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def safe_float(raw: Any) -> Optional[float]:
    try:
        v = float(raw)
    except Exception:
        return None
    return v if math.isfinite(v) else None


def safe_int01(raw: Any) -> Optional[int]:
    try:
        v = int(float(raw))
    except Exception:
        return None
    return v if v in (0, 1) else None


def infer_base_regime(sign60: float, sign240: float) -> str:
    s60 = 1 if sign60 > 0 else (-1 if sign60 < 0 else 0)
    s240 = 1 if sign240 > 0 else (-1 if sign240 < 0 else 0)
    if s60 > 0 and s240 > 0:
        return "TRENDING_UP"
    if s60 < 0 and s240 < 0:
        return "TRENDING_DOWN"
    return "RANGING"


def transform_feature_column(name: str, values: np.ndarray, clip_abs: float = 8.0) -> np.ndarray:
    out = np.array(values, dtype=np.float64, copy=True)
    if name == "rsi_14" or name.endswith("_rsi_14"):
        out = (out - 50.0) / 50.0
    elif name.endswith("_age_min"):
        out = np.clip(out, 0.0, 240.0) / 240.0
    elif name in ("vol_ratio_20", "notional_ratio_20"):
        out = np.log(np.maximum(out, 1e-9))
    elif name.endswith("_sign"):
        pass
    elif name.startswith("ret_") or "_ret_" in name or "atr_pct" in name or "bb_width" in name or "gap_" in name or "_gap_" in name:
        out = out * 100.0
    return np.clip(out, -clip_abs, clip_abs)


def load_feature_dataset(feature_dir: pathlib.Path, dataset_name: str) -> LoadedDataset:
    csv_paths = sorted([p for p in feature_dir.glob("prob_features_*.csv") if p.is_file()])
    if not csv_paths:
        raise RuntimeError(f"no feature csv found: {feature_dir}")

    feats: Dict[str, List[float]] = {k: [] for k in FEATURE_COLUMNS}
    ts_list: List[int] = []
    markets: List[str] = []
    y1: List[int] = []
    y5: List[int] = []
    edge: List[float] = []
    regimes: List[str] = []

    required = set(FEATURE_COLUMNS) | {"timestamp", "market", "label_up_h1", "label_up_h5", "label_edge_bps_h5"}

    for csv_path in csv_paths:
        with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
            rd = csv.DictReader(fh)
            head = set(rd.fieldnames or [])
            miss = sorted(required - head)
            if miss:
                raise RuntimeError(f"missing required columns in {csv_path.name}: {miss}")
            for row in rd:
                ts_v = safe_float(row.get("timestamp"))
                m = str(row.get("market", "")).strip()
                h1 = safe_int01(row.get("label_up_h1"))
                h5v = safe_int01(row.get("label_up_h5"))
                ev = safe_float(row.get("label_edge_bps_h5"))
                if ts_v is None or not m or h1 is None or h5v is None or ev is None:
                    continue
                vals: Dict[str, float] = {}
                ok = True
                for c in FEATURE_COLUMNS:
                    fv = safe_float(row.get(c))
                    if fv is None:
                        ok = False
                        break
                    vals[c] = float(fv)
                if not ok:
                    continue
                ts_list.append(int(ts_v))
                markets.append(m)
                y1.append(h1)
                y5.append(h5v)
                edge.append(ev)
                for c in FEATURE_COLUMNS:
                    feats[c].append(vals[c])
                regimes.append(infer_base_regime(vals["regime_trend_60_sign"], vals["regime_trend_240_sign"]))

    if not ts_list:
        raise RuntimeError(f"dataset load failed: {feature_dir}")

    raw = {k: np.asarray(v, dtype=np.float64) for k, v in feats.items()}
    transformed = np.column_stack([transform_feature_column(k, raw[k]) for k in FEATURE_COLUMNS]).astype(np.float64, copy=False)
    return LoadedDataset(
        dataset_name=dataset_name,
        csv_paths=csv_paths,
        market_list=sorted(set(markets)),
        timestamps=np.asarray(ts_list, dtype=np.int64),
        markets=np.asarray(markets, dtype=object),
        label_up_h1=np.asarray(y1, dtype=np.int8),
        label_up_h5=np.asarray(y5, dtype=np.int8),
        label_edge_bps_h5=np.asarray(edge, dtype=np.float64),
        features_raw=raw,
        transformed_matrix=transformed,
        base_regime=np.asarray(regimes, dtype=object),
    )

def np_stats(v: np.ndarray) -> Dict[str, Any]:
    if v.size == 0:
        return {"count": 0, "mean": math.nan, "std": math.nan, "min": math.nan, "max": math.nan, "q25": math.nan, "q50": math.nan, "q75": math.nan, "iqr": math.nan}
    q25, q50, q75 = np.quantile(v, [0.25, 0.5, 0.75])
    return {
        "count": int(v.size), "mean": float(np.mean(v)), "std": float(np.std(v)), "min": float(np.min(v)), "max": float(np.max(v)),
        "q25": float(q25), "q50": float(q50), "q75": float(q75), "iqr": float(q75 - q25),
    }


def distribution_summary(v: np.ndarray) -> Dict[str, Any]:
    if v.size == 0:
        return {"count": 0, "mean": math.nan, "std": math.nan, "p10": math.nan, "p50": math.nan, "p90": math.nan}
    p10, p50, p90 = np.quantile(v, [0.1, 0.5, 0.9])
    return {"count": int(v.size), "mean": float(np.mean(v)), "std": float(np.std(v)), "p10": float(p10), "p50": float(p50), "p90": float(p90)}


def safe_positive_ratio(v: np.ndarray) -> float:
    return float(np.mean((v > 0).astype(np.float64))) if v.size else math.nan


def safe_auc(y: np.ndarray, p: np.ndarray) -> float:
    if y.size == 0 or p.size == 0:
        return math.nan
    if int(np.min(y)) == int(np.max(y)):
        return math.nan
    try:
        return float(roc_auc_score(y.astype(np.int64), p.astype(np.float64)))
    except Exception:
        return math.nan


def calibration_slope(y: np.ndarray, p: np.ndarray) -> Dict[str, Any]:
    if y.size < 100:
        return {"status": "insufficient_samples", "slope": math.nan, "intercept": math.nan, "sample_count": int(y.size)}
    if int(np.min(y)) == int(np.max(y)):
        return {"status": "single_class", "slope": math.nan, "intercept": math.nan, "sample_count": int(y.size)}
    p2 = np.clip(p.astype(np.float64), 1e-6, 1 - 1e-6)
    x = np.log(p2 / (1 - p2)).reshape(-1, 1)
    try:
        lr = LogisticRegression(solver="lbfgs", C=1_000_000.0, fit_intercept=True, max_iter=500)
        lr.fit(x, y.astype(np.int64))
        return {"status": "ok", "slope": float(lr.coef_[0][0]), "intercept": float(lr.intercept_[0]), "sample_count": int(y.size)}
    except Exception as e:
        return {"status": "fit_failed", "slope": math.nan, "intercept": math.nan, "sample_count": int(y.size), "error": str(e)}


def auc_band(v: float) -> str:
    if not math.isfinite(v):
        return "unknown"
    if v < 0.55:
        return "near_random"
    if v < 0.60:
        return "weak_signal"
    return "meaningful_signal"


def load_fold_models(train_summary: Dict[str, Any]) -> Dict[int, Dict[str, Any]]:
    out: Dict[int, Dict[str, Any]] = {}
    folds = (((train_summary.get("global_model") or {}).get("folds")) or [])
    for fold in folds:
        if not isinstance(fold, dict):
            continue
        fid = int(fold.get("fold_id", 0) or 0)
        if fid <= 0:
            continue
        h5 = pathlib.Path(str((fold.get("model_artifacts") or {}).get("h5_model", "")).strip())
        if not h5.exists():
            continue
        payload = joblib.load(h5)
        mdl = payload.get("model")
        if mdl is None:
            continue
        cal = ((fold.get("calibration") or {}).get("h5") or {})
        out[fid] = {"model": mdl, "calib_a": float(cal.get("a", 1.0) or 1.0), "calib_b": float(cal.get("b", 0.0) or 0.0)}
    if not out:
        raise RuntimeError("no fold model loaded from train summary")
    return out


def predict_h5_ensemble(ds: LoadedDataset, fold_models: Dict[int, Dict[str, Any]]) -> np.ndarray:
    probs = []
    x = ds.transformed_matrix
    for fid in sorted(fold_models.keys()):
        pack = fold_models[fid]
        raw = pack["model"].predict_proba(x)[:, 1].astype(np.float64)
        cal = apply_platt(raw, pack["calib_a"], pack["calib_b"], 1e-6)
        probs.append(np.asarray(cal, dtype=np.float64))
    return np.mean(np.vstack(probs), axis=0)


def build_split_windows(split_manifest: Dict[str, Any]) -> Dict[str, Dict[int, Dict[str, int]]]:
    out: Dict[str, Dict[int, Dict[str, int]]] = {}
    for ds in split_manifest.get("datasets", []) or []:
        if not isinstance(ds, dict):
            continue
        market = str(ds.get("market", "")).strip()
        if not market:
            continue
        fmap: Dict[int, Dict[str, int]] = {}
        for fold in ds.get("folds", []) or []:
            if not isinstance(fold, dict):
                continue
            fid = int(fold.get("fold_id", 0) or 0)
            if fid <= 0:
                continue
            def ts(key: str) -> int:
                iso = str(fold.get(key, "")).strip()
                if not iso:
                    return 0
                return int(datetime.fromisoformat(iso.replace("Z", "+00:00")).astimezone(timezone.utc).timestamp() * 1000)
            fmap[fid] = {"train_start": ts("train_start_utc"), "train_end": ts("train_end_utc"), "valid_start": ts("valid_start_utc"), "valid_end": ts("valid_end_utc")}
        if fmap:
            out[market] = fmap
    return out


def run_fold_auc_audit(ds: LoadedDataset, split_manifest: Dict[str, Any], fold_models: Dict[int, Dict[str, Any]]) -> Dict[str, Any]:
    windows = build_split_windows(split_manifest)
    rows = []
    for fid in sorted(fold_models.keys()):
        tr_parts: List[np.ndarray] = []
        va_parts: List[np.ndarray] = []
        for market in ds.market_list:
            win = windows.get(market, {}).get(fid)
            if not win:
                continue
            mm = ds.markets == market
            ts = ds.timestamps
            tr_parts.append(np.where(mm & (ts >= win["train_start"]) & (ts <= win["train_end"]))[0])
            va_parts.append(np.where(mm & (ts >= win["valid_start"]) & (ts <= win["valid_end"]))[0])
        tri = np.concatenate(tr_parts) if tr_parts else np.empty((0,), dtype=np.int64)
        vai = np.concatenate(va_parts) if va_parts else np.empty((0,), dtype=np.int64)
        xtr = ds.transformed_matrix[tri] if tri.size else np.empty((0, len(FEATURE_COLUMNS)))
        xva = ds.transformed_matrix[vai] if vai.size else np.empty((0, len(FEATURE_COLUMNS)))
        ytr = ds.label_up_h5[tri] if tri.size else np.empty((0,), dtype=np.int8)
        yva = ds.label_up_h5[vai] if vai.size else np.empty((0,), dtype=np.int8)

        pack = fold_models[fid]
        ptr_raw = pack["model"].predict_proba(xtr)[:, 1].astype(np.float64) if xtr.shape[0] > 0 else np.empty((0,))
        pva_raw = pack["model"].predict_proba(xva)[:, 1].astype(np.float64) if xva.shape[0] > 0 else np.empty((0,))
        ptr = apply_platt(ptr_raw, pack["calib_a"], pack["calib_b"], 1e-6) if ptr_raw.size else ptr_raw
        pva = apply_platt(pva_raw, pack["calib_a"], pack["calib_b"], 1e-6) if pva_raw.size else pva_raw

        auc_tr = safe_auc(ytr, ptr)
        auc_va = safe_auc(yva, pva)
        rows.append({
            "fold_id": int(fid),
            "h5_train_count": int(ytr.size),
            "h5_valid_count": int(yva.size),
            "h5_train_auc": auc_tr,
            "h5_valid_auc": auc_va,
            "h5_valid_auc_band": auc_band(auc_va),
            "h5_validation_calibration_slope": calibration_slope(yva, pva),
            "h5_platt_parameters": {"a": float(pack["calib_a"]), "b": float(pack["calib_b"])},
        })
    m = float(np.nanmean([r["h5_valid_auc"] for r in rows])) if rows else math.nan
    return {
        "generated_at_utc": utc_now_iso(),
        "fold_count": len(rows),
        "folds": rows,
        "summary": {
            "h5_valid_auc_mean": m,
            "h5_valid_auc_band": auc_band(m),
            "note": "AUC thresholds: <0.55 near_random, 0.55~0.6 weak_signal, >0.6 meaningful_signal",
        },
    }

def summarize_label_balance(ds: LoadedDataset) -> Dict[str, Any]:
    h1 = ds.label_up_h1.astype(np.float64)
    h5 = ds.label_up_h5.astype(np.float64)
    edge = ds.label_edge_bps_h5.astype(np.float64)

    by_market = []
    for market in ds.market_list:
        m = ds.markets == market
        by_market.append({
            "market": market,
            "sample_count": int(np.sum(m)),
            "label_up_h1_ratio": float(np.mean(h1[m])) if np.any(m) else math.nan,
            "label_up_h5_ratio": float(np.mean(h5[m])) if np.any(m) else math.nan,
            "label_edge_bps_h5_mean": float(np.mean(edge[m])) if np.any(m) else math.nan,
            "label_edge_bps_h5_std": float(np.std(edge[m])) if np.any(m) else math.nan,
            "edge_bps_h5_positive_ratio": safe_positive_ratio(edge[m]) if np.any(m) else math.nan,
        })

    by_regime = []
    for regime in ["RANGING", "TRENDING_UP", "TRENDING_DOWN"]:
        m = ds.base_regime == regime
        by_regime.append({
            "regime": regime,
            "sample_count": int(np.sum(m)),
            "label_up_h1_ratio": float(np.mean(h1[m])) if np.any(m) else math.nan,
            "label_up_h5_ratio": float(np.mean(h5[m])) if np.any(m) else math.nan,
            "label_edge_bps_h5_mean": float(np.mean(edge[m])) if np.any(m) else math.nan,
            "label_edge_bps_h5_std": float(np.std(edge[m])) if np.any(m) else math.nan,
            "edge_bps_h5_positive_ratio": safe_positive_ratio(edge[m]) if np.any(m) else math.nan,
        })

    return {
        "generated_at_utc": utc_now_iso(),
        "dataset_name": ds.dataset_name,
        "sample_count": int(h1.size),
        "overall": {
            "label_up_h1_ratio": float(np.mean(h1)),
            "label_up_h5_ratio": float(np.mean(h5)),
            "label_edge_bps_h5_mean": float(np.mean(edge)),
            "label_edge_bps_h5_std": float(np.std(edge)),
            "edge_bps_h5_positive_ratio": safe_positive_ratio(edge),
            "edge_bps_h5_distribution": distribution_summary(edge),
        },
        "by_market": by_market,
        "by_regime_reconstructed": by_regime,
    }


def summarize_regime_composition(ds: LoadedDataset) -> Dict[str, Any]:
    total = float(ds.base_regime.size)
    overall = []
    for regime in ["RANGING", "TRENDING_UP", "TRENDING_DOWN"]:
        c = int(np.sum(ds.base_regime == regime))
        overall.append({"regime": regime, "count": c, "ratio": float(c) / total if total > 0 else math.nan})

    by_market = []
    for market in ds.market_list:
        mm = ds.markets == market
        mt = int(np.sum(mm))
        comp = []
        for regime in ["RANGING", "TRENDING_UP", "TRENDING_DOWN"]:
            c = int(np.sum(mm & (ds.base_regime == regime)))
            comp.append({"regime": regime, "count": c, "ratio": float(c) / float(mt) if mt > 0 else math.nan})
        by_market.append({"market": market, "sample_count": mt, "composition": comp})

    atr = ds.features_raw["regime_vol_60_atr_pct"]
    high_thr = 0.008
    high_count = int(np.sum(atr >= high_thr))
    return {
        "generated_at_utc": utc_now_iso(),
        "dataset_name": ds.dataset_name,
        "sample_count": int(ds.base_regime.size),
        "reconstructed_regime_composition": overall,
        "by_market": by_market,
        "high_vol_observation": {
            "feature": "regime_vol_60_atr_pct",
            "high_vol_threshold": high_thr,
            "high_vol_count": high_count,
            "high_vol_ratio": float(high_count) / float(ds.base_regime.size),
        },
    }


def top_abs_correlations(features_raw: Dict[str, np.ndarray], limit: int = 10) -> List[Dict[str, Any]]:
    cols = list(FEATURE_COLUMNS)
    mat = np.column_stack([features_raw[c] for c in cols]).astype(np.float64, copy=False)
    corr = np.corrcoef(mat, rowvar=False)
    pairs = []
    for i in range(len(cols)):
        for j in range(i + 1, len(cols)):
            c = corr[i, j]
            if math.isfinite(float(c)):
                pairs.append((abs(float(c)), i, j))
    pairs.sort(key=lambda x: x[0], reverse=True)
    out = []
    for _, i, j in pairs[: max(1, int(limit))]:
        out.append({"feature_a": cols[i], "feature_b": cols[j], "corr": float(corr[i, j]), "abs_corr": float(abs(corr[i, j]))})
    return out


def summarize_feature_variance(ds: LoadedDataset) -> Dict[str, Any]:
    per_feature: Dict[str, Dict[str, Any]] = {}
    clip_info: Dict[str, Dict[str, Any]] = {}
    clip_abs = 8.0
    for idx, name in enumerate(FEATURE_COLUMNS):
        raw = ds.features_raw[name]
        t = ds.transformed_matrix[:, idx]
        clipped = np.abs(t) >= (clip_abs - 1e-12)
        per_feature[name] = {**np_stats(raw), "near_zero_ratio_abs_le_1e_6": float(np.mean((np.abs(raw) <= 1e-6).astype(np.float64)))}
        clip_info[name] = {
            "clip_abs": clip_abs,
            "clipped_count": int(np.sum(clipped)),
            "clipped_ratio": float(np.mean(clipped.astype(np.float64))),
            "post_transform_mean": float(np.mean(t)),
            "post_transform_std": float(np.std(t)),
        }

    focus = ["rsi_14", "ema_gap_12_26", "ctx_15m_ema_gap_20", "ctx_60m_ema_gap_20", "ret_1m", "ret_5m", "regime_trend_60_sign"]
    return {
        "generated_at_utc": utc_now_iso(),
        "dataset_name": ds.dataset_name,
        "sample_count": int(ds.timestamps.size),
        "per_feature_stats": per_feature,
        "focus_feature_stats": {k: per_feature[k] for k in focus if k in per_feature},
        "top_abs_corr_pairs": top_abs_correlations(ds.features_raw, limit=10),
        "post_transform_clipping": clip_info,
    }


def ks_distance(a: np.ndarray, b: np.ndarray) -> float:
    if a.size == 0 or b.size == 0:
        return math.nan
    a2 = np.sort(a.astype(np.float64))
    b2 = np.sort(b.astype(np.float64))
    grid = np.sort(np.concatenate([a2, b2]))
    cdfa = np.searchsorted(a2, grid, side="right") / float(a2.size)
    cdfb = np.searchsorted(b2, grid, side="right") / float(b2.size)
    return float(np.max(np.abs(cdfa - cdfb)))


def copy_fresh14_input_files(fresh14_root: pathlib.Path, temp_input: pathlib.Path, markets: List[str]) -> None:
    temp_input.mkdir(parents=True, exist_ok=True)
    for market in markets:
        safe = market.replace("-", "_")
        for tf in (1, 5, 15, 60, 240):
            src = fresh14_root / f"upbit_{safe}_{tf}m_fresh14.csv"
            if not src.exists():
                raise RuntimeError(f"missing Fresh14 source file: {src}")
            shutil.copy2(src, temp_input / f"upbit_{safe}_{tf}m_full.csv")


def prepare_fresh14_feature_dataset(repo_root: pathlib.Path, fresh14_root: pathlib.Path, temp_root: pathlib.Path, markets: List[str]) -> pathlib.Path:
    temp_input = temp_root / "input_full_alias"
    temp_output = temp_root / "model_input"
    summary_json = temp_root / "fresh14_feature_build_summary.json"
    manifest_json = temp_output / "feature_dataset_manifest.json"
    if temp_root.exists():
        shutil.rmtree(temp_root)
    temp_root.mkdir(parents=True, exist_ok=True)
    copy_fresh14_input_files(fresh14_root, temp_input, markets)

    cmd = [
        sys.executable,
        str((repo_root / "scripts" / "build_probabilistic_feature_dataset.py").resolve()),
        "--input-dir", str(temp_input.resolve()),
        "--output-dir", str(temp_output.resolve()),
        "--summary-json", str(summary_json.resolve()),
        "--manifest-json", str(manifest_json.resolve()),
        "--markets", ",".join(markets),
        "--pipeline-version", "v2",
    ]
    proc = subprocess.run(cmd, cwd=str(repo_root), text=True, capture_output=True, encoding="utf-8", errors="ignore")
    if proc.returncode != 0:
        raise RuntimeError(f"Fresh14 feature build failed\ncmd={' '.join(cmd)}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    return temp_output


def build_drift_precheck(train_ds: LoadedDataset, fresh_ds: LoadedDataset, p_train: np.ndarray, p_fresh: np.ndarray) -> Dict[str, Any]:
    rows = []
    for feat in FEATURE_COLUMNS:
        tr = train_ds.features_raw[feat]
        fr = fresh_ds.features_raw.get(feat, np.empty((0,), dtype=np.float64))
        if fr.size == 0:
            continue
        mtr = float(np.mean(tr))
        mfr = float(np.mean(fr))
        s = float(np.std(tr))
        rows.append({"feature": feat, "train_mean": mtr, "fresh14_mean": mfr, "delta_fresh_minus_train": mfr - mtr, "delta_in_train_std_units": (mfr - mtr) / (s + 1e-12)})
    rows.sort(key=lambda x: abs(float(x["delta_in_train_std_units"])), reverse=True)

    ret_train = train_ds.features_raw["ret_1m"]
    ret_fresh = fresh_ds.features_raw["ret_1m"]
    vol_train = train_ds.features_raw["atr_pct_14"]
    vol_fresh = fresh_ds.features_raw["atr_pct_14"]

    return {
        "status": "computed",
        "p_h5_mean": {
            "train_mean": float(np.mean(p_train)) if p_train.size else math.nan,
            "fresh14_mean": float(np.mean(p_fresh)) if p_fresh.size else math.nan,
            "delta_fresh_minus_train": (float(np.mean(p_fresh)) - float(np.mean(p_train))) if p_train.size and p_fresh.size else math.nan,
        },
        "feature_mean_diff_top10_abs_std": rows[:10],
        "volatility_mean_diff": {"feature": "atr_pct_14", "train_mean": float(np.mean(vol_train)), "fresh14_mean": float(np.mean(vol_fresh)), "delta_fresh_minus_train": float(np.mean(vol_fresh) - np.mean(vol_train))},
        "ret_1m_distribution_diff": {"train": distribution_summary(ret_train), "fresh14": distribution_summary(ret_fresh), "ks_distance": ks_distance(ret_train, ret_fresh)},
        "notes": ["p_h5 mean uses fold-model ensemble calibrated probabilities", "Fresh14 comparison uses temporary feature dataset generated from data/backtest_fresh_14d"],
    }

def build_training_distribution_report(train_ds: LoadedDataset, label_report: Dict[str, Any], regime_report: Dict[str, Any], drift_precheck: Dict[str, Any], train_feature_dir: pathlib.Path, split_manifest_json: pathlib.Path) -> Dict[str, Any]:
    return {
        "generated_at_utc": utc_now_iso(),
        "hard_lock": {
            "training_feature_dir": str(train_feature_dir),
            "split_manifest_json": str(split_manifest_json),
            "model_retrain": "forbidden",
            "strategy_or_gate_changes": "forbidden",
        },
        "dataset_overview": {
            "dataset_name": train_ds.dataset_name,
            "sample_count": int(train_ds.timestamps.size),
            "markets": train_ds.market_list,
            "csv_paths": [str(p) for p in train_ds.csv_paths],
        },
        "label_overview": {
            "label_up_h1_ratio": label_report["overall"]["label_up_h1_ratio"],
            "label_up_h5_ratio": label_report["overall"]["label_up_h5_ratio"],
            "label_edge_bps_h5_mean": label_report["overall"]["label_edge_bps_h5_mean"],
            "label_edge_bps_h5_std": label_report["overall"]["label_edge_bps_h5_std"],
            "edge_bps_h5_positive_ratio": label_report["overall"]["edge_bps_h5_positive_ratio"],
        },
        "reconstructed_regime_overview": regime_report["reconstructed_regime_composition"],
        "drift_precheck_fresh14": drift_precheck,
    }


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    repo_root = pathlib.Path.cwd().resolve()
    train_feature_dir = resolve_repo_path(args.train_feature_dir)
    split_manifest_json = resolve_repo_path(args.split_manifest_json)
    train_summary_json = resolve_repo_path(args.train_summary_json)
    fresh14_root = resolve_repo_path(args.fresh14_root)
    output_dir = resolve_repo_path(args.output_dir)
    temp_root = resolve_repo_path(args.fresh14_temp_dir)

    train_ds = load_feature_dataset(train_feature_dir, dataset_name="train_v2_gate4_20260224")
    split_manifest = load_json_or_none(split_manifest_json)
    train_summary = load_json_or_none(train_summary_json)
    if not isinstance(split_manifest, dict):
        raise RuntimeError(f"invalid split manifest json: {split_manifest_json}")
    if not isinstance(train_summary, dict):
        raise RuntimeError(f"invalid train summary json: {train_summary_json}")

    fold_models = load_fold_models(train_summary)
    p_train = predict_h5_ensemble(train_ds, fold_models)

    label_report = summarize_label_balance(train_ds)
    regime_report = summarize_regime_composition(train_ds)
    feature_report = summarize_feature_variance(train_ds)
    fold_auc_report = run_fold_auc_audit(train_ds, split_manifest, fold_models)

    drift_precheck: Dict[str, Any] = {"status": "skipped"}
    if bool(args.include_drift_precheck):
        fresh_feature_dir = prepare_fresh14_feature_dataset(repo_root, fresh14_root, temp_root, train_ds.market_list)
        fresh_ds = load_feature_dataset(fresh_feature_dir, dataset_name="fresh14_temp_feature_projection")
        p_fresh = predict_h5_ensemble(fresh_ds, fold_models)
        drift_precheck = build_drift_precheck(train_ds, fresh_ds, p_train, p_fresh)

    training_distribution_report = build_training_distribution_report(
        train_ds=train_ds,
        label_report=label_report,
        regime_report=regime_report,
        drift_precheck=drift_precheck,
        train_feature_dir=train_feature_dir,
        split_manifest_json=split_manifest_json,
    )

    output_dir.mkdir(parents=True, exist_ok=True)
    outputs = {
        "stageG1_training_distribution_report.json": training_distribution_report,
        "stageG1_feature_variance_report.json": feature_report,
        "stageG1_label_balance_report.json": label_report,
        "stageG1_regime_composition_report.json": regime_report,
        "stageG1_fold_auc_report.json": fold_auc_report,
    }
    for name, payload in outputs.items():
        dump_json(output_dir / name, payload)

    print("[StageG1] completed")
    for name in outputs.keys():
        print(str((output_dir / name).resolve()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
