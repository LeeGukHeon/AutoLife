#!/usr/bin/env python3
import argparse
import csv
import math
import pathlib
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import joblib
import numpy as np

from _script_common import dump_json, load_json_or_none, resolve_repo_path
from train_probabilistic_pattern_model import (
    apply_platt,
    build_feature_vector,
    clip_prob,
    parse_iso_to_ts_ms,
    safe_float,
    safe_int01,
)


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate walk-forward decomposition report (market/regime/volatility) from trained probabilistic model."
    )
    parser.add_argument(
        "--train-summary-json",
        default=r".\build\Release\logs\probabilistic_model_train_summary_full_20260220.json",
    )
    parser.add_argument(
        "--split-manifest-json",
        default=r".\data\model_input\probabilistic_features_v1_full_20260220_181345\probabilistic_split_manifest_v1.json",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\probabilistic_walkforward_report_full_20260220.json",
    )
    parser.add_argument("--infer-batch-size", type=int, default=8192)
    parser.add_argument("--prob-eps", type=float, default=1e-6)
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def trend_bucket(sign60: int, sign240: int) -> str:
    if sign60 == 1 and sign240 == 1:
        return "trend_up_sync"
    if sign60 == -1 and sign240 == -1:
        return "trend_down_sync"
    if sign60 == 0 or sign240 == 0:
        return "trend_flat_mixed"
    return "trend_mixed"


def vol_bucket(atr60: float) -> str:
    if not math.isfinite(atr60):
        return "vol_unknown"
    if atr60 < 0.004:
        return "vol_low"
    if atr60 < 0.008:
        return "vol_mid"
    return "vol_high"


def init_agg() -> Dict[str, Any]:
    return {
        "n": 0,
        "sum_logloss": 0.0,
        "sum_brier": 0.0,
        "sum_acc": 0.0,
        "sum_prob": 0.0,
        "sum_label": 0.0,
        "selected_n": 0,
        "selected_edge_sum": 0.0,
        "selected_pos_edge_n": 0,
    }


def update_agg(agg: Dict[str, Any], y: int, p: float, edge: float, selected: bool, eps: float) -> None:
    p2 = float(min(1.0 - eps, max(eps, p)))
    yy = int(y)
    agg["n"] += 1
    agg["sum_logloss"] += -((yy * math.log(p2)) + ((1 - yy) * math.log(1.0 - p2)))
    agg["sum_brier"] += (p2 - yy) * (p2 - yy)
    agg["sum_acc"] += 1.0 if ((p2 >= 0.5) == (yy == 1)) else 0.0
    agg["sum_prob"] += p2
    agg["sum_label"] += yy
    if selected:
        agg["selected_n"] += 1
        agg["selected_edge_sum"] += float(edge)
        if edge > 0.0:
            agg["selected_pos_edge_n"] += 1


def finalize_agg(name: str, agg: Dict[str, Any]) -> Dict[str, Any]:
    n = int(agg["n"])
    sel = int(agg["selected_n"])
    return {
        "bucket": name,
        "n": n,
        "logloss": (float(agg["sum_logloss"]) / float(n)) if n > 0 else math.nan,
        "brier": (float(agg["sum_brier"]) / float(n)) if n > 0 else math.nan,
        "accuracy": (float(agg["sum_acc"]) / float(n)) if n > 0 else math.nan,
        "mean_prob": (float(agg["sum_prob"]) / float(n)) if n > 0 else math.nan,
        "label_positive_rate": (float(agg["sum_label"]) / float(n)) if n > 0 else math.nan,
        "selected_n": sel,
        "selected_coverage": (float(sel) / float(n)) if n > 0 else 0.0,
        "selected_mean_edge_bps": (float(agg["selected_edge_sum"]) / float(sel)) if sel > 0 else math.nan,
        "selected_positive_edge_rate": (float(agg["selected_pos_edge_n"]) / float(sel)) if sel > 0 else math.nan,
    }


def build_split_window_map(split_manifest: Dict[str, Any]) -> Dict[str, Dict[int, Dict[str, int]]]:
    out: Dict[str, Dict[int, Dict[str, int]]] = {}
    for ds in split_manifest.get("datasets", []) or []:
        if not isinstance(ds, dict):
            continue
        market = str(ds.get("market", "")).strip()
        if not market:
            continue
        fold_map: Dict[int, Dict[str, int]] = {}
        for fold in ds.get("folds", []) or []:
            if not isinstance(fold, dict):
                continue
            fid = int(fold.get("fold_id", 0) or 0)
            if fid <= 0:
                continue
            fold_map[fid] = {
                "test_start": parse_iso_to_ts_ms(str(fold.get("test_start_utc", ""))),
                "test_end": parse_iso_to_ts_ms(str(fold.get("test_end_utc", ""))),
            }
        if fold_map:
            out[market] = fold_map
    return out


def flush_fold_batch(
    *,
    fold_ctx: Dict[str, Any],
    market_agg: Dict[str, Any],
    regime_agg: Dict[str, Any],
    vol_agg: Dict[str, Any],
    prob_eps: float,
) -> None:
    if not fold_ctx["x"]:
        return
    x = np.asarray(fold_ctx["x"], dtype=np.float64)
    y = np.asarray(fold_ctx["y"], dtype=np.int64)
    edge = np.asarray(fold_ctx["edge"], dtype=np.float64)
    reg = fold_ctx["regime"]
    vol = fold_ctx["vol"]

    raw = fold_ctx["model"].predict_proba(x)[:, 1]
    raw = clip_prob(raw, prob_eps)
    p = apply_platt(raw, fold_ctx["calib_a"], fold_ctx["calib_b"], prob_eps)
    thr = float(fold_ctx["threshold"])

    for i in range(len(y)):
        yy = int(y[i])
        pp = float(p[i])
        ee = float(edge[i])
        selected = bool(pp >= thr)
        update_agg(market_agg, yy, pp, ee, selected, prob_eps)
        rkey = str(reg[i])
        vkey = str(vol[i])
        if rkey not in regime_agg:
            regime_agg[rkey] = init_agg()
        if vkey not in vol_agg:
            vol_agg[vkey] = init_agg()
        update_agg(regime_agg[rkey], yy, pp, ee, selected, prob_eps)
        update_agg(vol_agg[vkey], yy, pp, ee, selected, prob_eps)

    fold_ctx["x"].clear()
    fold_ctx["y"].clear()
    fold_ctx["edge"].clear()
    fold_ctx["regime"].clear()
    fold_ctx["vol"].clear()


def main(argv=None) -> int:
    args = parse_args(argv)
    train_summary_path = resolve_repo_path(args.train_summary_json)
    split_manifest_path = resolve_repo_path(args.split_manifest_json)
    output_path = resolve_repo_path(args.output_json)

    train_summary = load_json_or_none(train_summary_path)
    split_manifest = load_json_or_none(split_manifest_path)
    if not isinstance(train_summary, dict):
        raise RuntimeError(f"invalid train summary: {train_summary_path}")
    if not isinstance(split_manifest, dict):
        raise RuntimeError(f"invalid split manifest: {split_manifest_path}")

    split_map = build_split_window_map(split_manifest)
    market_rows = []
    overall = init_agg()

    print("[WalkforwardReport] start", flush=True)
    print(f"train_summary={train_summary_path}", flush=True)
    print(f"split_manifest={split_manifest_path}", flush=True)

    for ds in train_summary.get("datasets", []) or []:
        if not isinstance(ds, dict):
            continue
        market = str(ds.get("market", "")).strip()
        csv_path = pathlib.Path(str(ds.get("csv_path", "")).strip())
        if not market or not csv_path.exists():
            continue
        if market not in split_map:
            continue

        fold_contexts: List[Dict[str, Any]] = []
        for fold in ds.get("folds", []) or []:
            if not isinstance(fold, dict):
                continue
            fid = int(fold.get("fold_id", 0) or 0)
            if fid <= 0:
                continue
            sw = split_map.get(market, {}).get(fid)
            if not sw:
                continue
            h5_art = str(fold.get("model_artifacts", {}).get("h5_model", "")).strip()
            if not h5_art:
                continue
            h5_payload = joblib.load(h5_art)
            calib = fold.get("calibration", {}).get("h5", {}) or {}
            thr = float(fold.get("metrics", {}).get("h5", {}).get("threshold_selection", {}).get("threshold", 0.6) or 0.6)
            fold_contexts.append(
                {
                    "fold_id": fid,
                    "test_start": int(sw["test_start"]),
                    "test_end": int(sw["test_end"]),
                    "model": h5_payload["model"],
                    "calib_a": float(calib.get("a", 1.0) or 1.0),
                    "calib_b": float(calib.get("b", 0.0) or 0.0),
                    "threshold": thr,
                    "x": [],
                    "y": [],
                    "edge": [],
                    "regime": [],
                    "vol": [],
                }
            )
        if not fold_contexts:
            continue

        market_agg = init_agg()
        regime_agg: Dict[str, Dict[str, Any]] = {}
        vol_agg: Dict[str, Dict[str, Any]] = {}

        print(f"[WalkforwardReport] market={market} folds={len(fold_contexts)}", flush=True)
        with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                ts = safe_float(row.get("timestamp"))
                y = safe_int01(row.get("label_up_h5"))
                edge = safe_float(row.get("label_edge_bps_h5"))
                s60 = safe_float(row.get("regime_trend_60_sign"))
                s240 = safe_float(row.get("regime_trend_240_sign"))
                atr60 = safe_float(row.get("regime_vol_60_atr_pct"))
                if ts is None or y is None or edge is None or s60 is None or s240 is None or atr60 is None:
                    continue
                x = build_feature_vector(row, 8.0)
                if x is None:
                    continue
                regime_key = trend_bucket(int(s60), int(s240))
                vol_key = vol_bucket(float(atr60))
                ts_i = int(ts)
                for fc in fold_contexts:
                    if fc["test_start"] <= ts_i <= fc["test_end"]:
                        fc["x"].append(x)
                        fc["y"].append(int(y))
                        fc["edge"].append(float(edge))
                        fc["regime"].append(regime_key)
                        fc["vol"].append(vol_key)
                        if len(fc["x"]) >= int(args.infer_batch_size):
                            flush_fold_batch(
                                fold_ctx=fc,
                                market_agg=market_agg,
                                regime_agg=regime_agg,
                                vol_agg=vol_agg,
                                prob_eps=float(args.prob_eps),
                            )
        for fc in fold_contexts:
            flush_fold_batch(
                fold_ctx=fc,
                market_agg=market_agg,
                regime_agg=regime_agg,
                vol_agg=vol_agg,
                prob_eps=float(args.prob_eps),
            )

        market_final = finalize_agg(market, market_agg)
        market_final["regime_buckets"] = [finalize_agg(k, v) for k, v in sorted(regime_agg.items())]
        market_final["volatility_buckets"] = [finalize_agg(k, v) for k, v in sorted(vol_agg.items())]
        market_rows.append(market_final)

        # accumulate to overall
        for bucket in [market_agg]:
            overall["n"] += bucket["n"]
            overall["sum_logloss"] += bucket["sum_logloss"]
            overall["sum_brier"] += bucket["sum_brier"]
            overall["sum_acc"] += bucket["sum_acc"]
            overall["sum_prob"] += bucket["sum_prob"]
            overall["sum_label"] += bucket["sum_label"]
            overall["selected_n"] += bucket["selected_n"]
            overall["selected_edge_sum"] += bucket["selected_edge_sum"]
            overall["selected_pos_edge_n"] += bucket["selected_pos_edge_n"]

    out = {
        "generated_at_utc": utc_now_iso(),
        "status": "pass",
        "train_summary_json": str(train_summary_path),
        "split_manifest_json": str(split_manifest_path),
        "overall": finalize_agg("overall", overall),
        "markets": market_rows,
    }
    dump_json(output_path, out)

    print("[WalkforwardReport] completed", flush=True)
    print(f"markets={len(market_rows)}", flush=True)
    print(f"output={output_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

