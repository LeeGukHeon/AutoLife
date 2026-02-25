#!/usr/bin/env python3
import argparse
import csv
import math
import pathlib
from datetime import datetime, timezone
from typing import Any, Dict, List

import joblib
import numpy as np

from _script_common import dump_json, load_json_or_none, resolve_repo_path
from train_probabilistic_pattern_model import (
    apply_platt,
    build_feature_vector,
    clip_prob,
    parse_iso_to_ts_ms,
    safe_float,
)


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate inference parity between sklearn predict_proba and manual logistic scoring."
    )
    parser.add_argument(
        "--train-summary-json",
        default=r".\build\Release\logs\probabilistic_model_train_summary_global_latest.json",
    )
    parser.add_argument(
        "--split-manifest-json",
        default=r".\data\model_input\probabilistic_features_v2_draft_latest\probabilistic_split_manifest_v2_draft.json",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\probabilistic_inference_parity_latest.json",
    )
    parser.add_argument("--samples-per-fold", type=int, default=2000)
    parser.add_argument("--clip-abs", type=float, default=8.0)
    parser.add_argument("--prob-eps", type=float, default=1e-6)
    parser.add_argument("--abs-tol", type=float, default=1e-9)
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def sigmoid(z: np.ndarray) -> np.ndarray:
    zz = np.clip(z, -40.0, 40.0)
    return 1.0 / (1.0 + np.exp(-zz))


def build_test_windows(split_manifest: Dict[str, Any]) -> Dict[str, Dict[int, Dict[str, int]]]:
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


def compare_fold_parity(
    *,
    model_payload: Dict[str, Any],
    calib_a: float,
    calib_b: float,
    x: np.ndarray,
    prob_eps: float,
) -> Dict[str, float]:
    model = model_payload["model"]
    raw_a = clip_prob(model.predict_proba(x)[:, 1], prob_eps)
    coef = np.asarray(model.coef_, dtype=np.float64).reshape(-1)
    intercept = float(np.asarray(model.intercept_, dtype=np.float64).reshape(-1)[0])
    raw_b = clip_prob(sigmoid((x @ coef) + intercept), prob_eps)

    cal_a = apply_platt(raw_a, calib_a, calib_b, prob_eps)
    cal_b = apply_platt(raw_b, calib_a, calib_b, prob_eps)

    diff_raw = np.abs(raw_a - raw_b)
    diff_cal = np.abs(cal_a - cal_b)
    return {
        "n": int(x.shape[0]),
        "raw_max_abs_diff": float(np.max(diff_raw)) if diff_raw.size > 0 else 0.0,
        "raw_mean_abs_diff": float(np.mean(diff_raw)) if diff_raw.size > 0 else 0.0,
        "cal_max_abs_diff": float(np.max(diff_cal)) if diff_cal.size > 0 else 0.0,
        "cal_mean_abs_diff": float(np.mean(diff_cal)) if diff_cal.size > 0 else 0.0,
    }


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

    window_map = build_test_windows(split_manifest)
    samples_per_fold = max(100, int(args.samples_per_fold))
    prob_eps = float(args.prob_eps)
    abs_tol = float(args.abs_tol)

    print("[InferenceParity] start", flush=True)
    print(f"train_summary={train_summary_path}", flush=True)
    print(f"split_manifest={split_manifest_path}", flush=True)
    print(f"samples_per_fold={samples_per_fold}", flush=True)

    results = []
    worst_raw = 0.0
    worst_cal = 0.0

    for ds in train_summary.get("datasets", []) or []:
        if not isinstance(ds, dict):
            continue
        market = str(ds.get("market", "")).strip()
        csv_path = pathlib.Path(str(ds.get("csv_path", "")).strip())
        if not market or not csv_path.exists():
            continue
        if market not in window_map:
            continue

        fold_contexts = []
        for fold in ds.get("folds", []) or []:
            if not isinstance(fold, dict):
                continue
            fid = int(fold.get("fold_id", 0) or 0)
            if fid <= 0:
                continue
            win = window_map.get(market, {}).get(fid)
            if not win:
                continue
            h5_path = str(fold.get("model_artifacts", {}).get("h5_model", "")).strip()
            if not h5_path:
                continue
            model_payload = joblib.load(h5_path)
            calib = fold.get("calibration", {}).get("h5", {}) or {}
            fold_contexts.append(
                {
                    "fold_id": fid,
                    "test_start": int(win["test_start"]),
                    "test_end": int(win["test_end"]),
                    "model_payload": model_payload,
                    "calib_a": float(calib.get("a", 1.0) or 1.0),
                    "calib_b": float(calib.get("b", 0.0) or 0.0),
                    "x": [],
                }
            )
        if not fold_contexts:
            continue

        print(f"[InferenceParity] market={market} folds={len(fold_contexts)}", flush=True)
        with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                ts = safe_float(row.get("timestamp"))
                if ts is None:
                    continue
                x = build_feature_vector(row, float(args.clip_abs))
                if x is None:
                    continue
                ts_i = int(ts)
                done = True
                for fc in fold_contexts:
                    if len(fc["x"]) < samples_per_fold and fc["test_start"] <= ts_i <= fc["test_end"]:
                        fc["x"].append(x)
                    if len(fc["x"]) < samples_per_fold:
                        done = False
                if done:
                    break

        for fc in fold_contexts:
            x = np.asarray(fc["x"], dtype=np.float64)
            if x.shape[0] == 0:
                continue
            comp = compare_fold_parity(
                model_payload=fc["model_payload"],
                calib_a=float(fc["calib_a"]),
                calib_b=float(fc["calib_b"]),
                x=x,
                prob_eps=prob_eps,
            )
            worst_raw = max(worst_raw, float(comp["raw_max_abs_diff"]))
            worst_cal = max(worst_cal, float(comp["cal_max_abs_diff"]))
            results.append(
                {
                    "market": market,
                    "fold_id": int(fc["fold_id"]),
                    **comp,
                }
            )

    status = "pass" if (worst_raw <= abs_tol and worst_cal <= abs_tol and len(results) > 0) else "fail"
    summary = {
        "generated_at_utc": utc_now_iso(),
        "status": status,
        "train_summary_json": str(train_summary_path),
        "split_manifest_json": str(split_manifest_path),
        "samples_per_fold": int(samples_per_fold),
        "abs_tolerance": abs_tol,
        "worst_raw_max_abs_diff": float(worst_raw),
        "worst_cal_max_abs_diff": float(worst_cal),
        "comparison_count": int(len(results)),
        "results": results,
    }
    dump_json(output_path, summary)

    print("[InferenceParity] completed", flush=True)
    print(f"status={status}", flush=True)
    print(f"comparisons={len(results)} worst_raw={worst_raw:.3e} worst_cal={worst_cal:.3e}", flush=True)
    print(f"output={output_path}", flush=True)
    return 0 if status == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
