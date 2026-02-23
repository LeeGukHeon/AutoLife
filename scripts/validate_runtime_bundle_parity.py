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
        description="Validate parity between runtime JSON bundle inference and original joblib inference."
    )
    parser.add_argument(
        "--runtime-bundle-json",
        default=r".\config\model\probabilistic_runtime_bundle_v1.json",
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
        default=r".\build\Release\logs\probabilistic_runtime_bundle_parity_full_20260220.json",
    )
    parser.add_argument("--samples-per-market", type=int, default=3000)
    parser.add_argument("--clip-abs", type=float, default=8.0)
    parser.add_argument("--prob-eps", type=float, default=1e-6)
    parser.add_argument("--abs-tol", type=float, default=1e-9)
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def sigmoid(z: np.ndarray) -> np.ndarray:
    zz = np.clip(z, -40.0, 40.0)
    return 1.0 / (1.0 + np.exp(-zz))


def extract_edge_regressor(node: Any) -> Any:
    if not isinstance(node, dict):
        return None
    linear = node.get("linear")
    if not isinstance(linear, dict):
        return None
    coef_raw = linear.get("coef")
    if not isinstance(coef_raw, list) or not coef_raw:
        return None
    try:
        coef = np.asarray(coef_raw, dtype=np.float64)
        intercept = float(linear.get("intercept", 0.0) or 0.0)
        clip_abs = float(node.get("clip_abs_bps", 250.0) or 250.0)
    except Exception:
        return None
    if not np.isfinite(intercept):
        return None
    if not np.isfinite(clip_abs):
        clip_abs = 250.0
    clip_abs = float(np.clip(clip_abs, 10.0, 5000.0))
    return {
        "coef": coef,
        "intercept": intercept,
        "clip_abs_bps": clip_abs,
    }


def get_test_window(split_manifest: Dict[str, Any], market: str, fold_id: int) -> Dict[str, int]:
    for ds in split_manifest.get("datasets", []) or []:
        if not isinstance(ds, dict):
            continue
        if str(ds.get("market", "")).strip() != market:
            continue
        for fold in ds.get("folds", []) or []:
            if not isinstance(fold, dict):
                continue
            fid = int(fold.get("fold_id", 0) or 0)
            if fid == int(fold_id):
                return {
                    "test_start": parse_iso_to_ts_ms(str(fold.get("test_start_utc", ""))),
                    "test_end": parse_iso_to_ts_ms(str(fold.get("test_end_utc", ""))),
                }
    return {"test_start": 0, "test_end": 0}


def main(argv=None) -> int:
    args = parse_args(argv)
    bundle_path = resolve_repo_path(args.runtime_bundle_json)
    summary_path = resolve_repo_path(args.train_summary_json)
    split_path = resolve_repo_path(args.split_manifest_json)
    output_path = resolve_repo_path(args.output_json)

    bundle = load_json_or_none(bundle_path)
    summary = load_json_or_none(summary_path)
    split_manifest = load_json_or_none(split_path)
    if not isinstance(bundle, dict):
        raise RuntimeError(f"invalid runtime bundle: {bundle_path}")
    if not isinstance(summary, dict):
        raise RuntimeError(f"invalid train summary: {summary_path}")
    if not isinstance(split_manifest, dict):
        raise RuntimeError(f"invalid split manifest: {split_path}")

    by_market_summary = {
        str(ds.get("market", "")).strip(): ds
        for ds in (summary.get("datasets", []) or [])
        if isinstance(ds, dict)
    }

    results = []
    worst = 0.0
    bundle_markets = [x for x in (bundle.get("markets", []) or []) if isinstance(x, dict)]
    mode = "market_entries"

    for item in bundle_markets:
        if not isinstance(item, dict):
            continue
        market = str(item.get("market", "")).strip()
        fold_id = int(item.get("selected_fold_id", 0) or 0)
        if not market or fold_id <= 0:
            continue
        ds = by_market_summary.get(market)
        if not isinstance(ds, dict):
            continue
        csv_path = pathlib.Path(str(ds.get("csv_path", "")).strip())
        if not csv_path.exists():
            continue
        fold_match = None
        for f in ds.get("folds", []) or []:
            if isinstance(f, dict) and int(f.get("fold_id", 0) or 0) == fold_id:
                fold_match = f
                break
        if not isinstance(fold_match, dict):
            continue

        h5_model_path = str(fold_match.get("model_artifacts", {}).get("h5_model", "")).strip()
        if not h5_model_path:
            continue
        model_payload = joblib.load(h5_model_path)
        model = model_payload["model"]
        edge_reg_job = extract_edge_regressor(model_payload.get("edge_regressor"))

        win = get_test_window(split_manifest, market, fold_id)
        if int(win["test_start"]) <= 0 or int(win["test_end"]) <= 0:
            continue

        x_rows: List[List[float]] = []
        with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
            reader = csv.DictReader(fh)
            for row in reader:
                ts = safe_float(row.get("timestamp"))
                if ts is None:
                    continue
                ts_i = int(ts)
                if not (int(win["test_start"]) <= ts_i <= int(win["test_end"])):
                    continue
                x = build_feature_vector(row, float(args.clip_abs))
                if x is None:
                    continue
                x_rows.append(x)
                if len(x_rows) >= int(args.samples_per_market):
                    break
        if not x_rows:
            continue

        x_np = np.asarray(x_rows, dtype=np.float64)
        prob_job = clip_prob(model.predict_proba(x_np)[:, 1], float(args.prob_eps))
        cal = fold_match.get("calibration", {}).get("h5", {}) or {}
        prob_job_cal = apply_platt(
            prob_job,
            float(cal.get("a", 1.0) or 1.0),
            float(cal.get("b", 0.0) or 0.0),
            float(args.prob_eps),
        )

        lin = item.get("h5_model", {}).get("linear", {}) or {}
        coef = np.asarray(lin.get("coef", []), dtype=np.float64)
        intercept = float(lin.get("intercept", 0.0) or 0.0)
        if coef.size != x_np.shape[1]:
            raise RuntimeError(f"coef size mismatch for {market}: {coef.size} vs {x_np.shape[1]}")
        prob_bundle = clip_prob(sigmoid((x_np @ coef) + intercept), float(args.prob_eps))
        cal2 = item.get("h5_model", {}).get("calibration", {}) or {}
        prob_bundle_cal = apply_platt(
            prob_bundle,
            float(cal2.get("a", 1.0) or 1.0),
            float(cal2.get("b", 0.0) or 0.0),
            float(args.prob_eps),
        )

        diff = np.abs(prob_job_cal - prob_bundle_cal)
        max_diff = float(np.max(diff))
        mean_diff = float(np.mean(diff))
        worst = max(worst, max_diff)
        edge_max_diff = math.nan
        edge_mean_diff = math.nan
        edge_parity_mode = "not_available"
        edge_reg_bundle = extract_edge_regressor(item.get("h5_model", {}).get("edge_regressor"))
        if edge_reg_job is not None and edge_reg_bundle is not None:
            if edge_reg_job["coef"].size != x_np.shape[1]:
                raise RuntimeError(
                    f"job edge coef size mismatch for {market}: {edge_reg_job['coef'].size} vs {x_np.shape[1]}"
                )
            if edge_reg_bundle["coef"].size != x_np.shape[1]:
                raise RuntimeError(
                    f"bundle edge coef size mismatch for {market}: {edge_reg_bundle['coef'].size} vs {x_np.shape[1]}"
                )
            edge_job = np.clip(
                (x_np @ edge_reg_job["coef"]) + float(edge_reg_job["intercept"]),
                -float(edge_reg_job["clip_abs_bps"]),
                float(edge_reg_job["clip_abs_bps"]),
            )
            edge_bundle = np.clip(
                (x_np @ edge_reg_bundle["coef"]) + float(edge_reg_bundle["intercept"]),
                -float(edge_reg_bundle["clip_abs_bps"]),
                float(edge_reg_bundle["clip_abs_bps"]),
            )
            edge_diff = np.abs(edge_job - edge_bundle)
            edge_max_diff = float(np.max(edge_diff))
            edge_mean_diff = float(np.mean(edge_diff))
            worst = max(worst, edge_max_diff)
            edge_parity_mode = "checked"
        results.append(
            {
                "market": market,
                "fold_id": fold_id,
                "n": int(x_np.shape[0]),
                "max_abs_diff": max_diff,
                "mean_abs_diff": mean_diff,
                "edge_parity_mode": edge_parity_mode,
                "edge_max_abs_diff_bps": edge_max_diff,
                "edge_mean_abs_diff_bps": edge_mean_diff,
            }
        )

    if not results:
        default_model = bundle.get("default_model", {})
        global_folds = summary.get("global_model", {}).get("folds", [])
        if isinstance(default_model, dict) and isinstance(global_folds, list) and global_folds:
            mode = "global_default"
            fold_id = int(default_model.get("selected_fold_id", 0) or 0)
            fold_match = None
            for f in global_folds:
                if isinstance(f, dict) and int(f.get("fold_id", 0) or 0) == fold_id:
                    fold_match = f
                    break
            if not isinstance(fold_match, dict):
                fold_match = global_folds[-1] if isinstance(global_folds[-1], dict) else None
                fold_id = int(fold_match.get("fold_id", 0) or 0) if isinstance(fold_match, dict) else 0

            if isinstance(fold_match, dict) and fold_id > 0:
                h5_model_path = str(fold_match.get("model_artifacts", {}).get("h5_model", "")).strip()
                if str(h5_model_path):
                    model_payload = joblib.load(h5_model_path)
                    model = model_payload["model"]
                    edge_reg_job = extract_edge_regressor(model_payload.get("edge_regressor"))
                    lin = default_model.get("h5_model", {}).get("linear", {}) or {}
                    coef = np.asarray(lin.get("coef", []), dtype=np.float64)
                    intercept = float(lin.get("intercept", 0.0) or 0.0)
                    cal2 = default_model.get("h5_model", {}).get("calibration", {}) or {}
                    edge_reg_bundle = extract_edge_regressor(default_model.get("h5_model", {}).get("edge_regressor"))
                    cal_fold = fold_match.get("calibration", {}).get("h5", {}) or {}

                    for market, ds in sorted(by_market_summary.items()):
                        csv_path = pathlib.Path(str(ds.get("csv_path", "")).strip())
                        if not csv_path.exists():
                            continue
                        win = get_test_window(split_manifest, market, fold_id)
                        if int(win["test_start"]) <= 0 or int(win["test_end"]) <= 0:
                            continue

                        x_rows: List[List[float]] = []
                        with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
                            reader = csv.DictReader(fh)
                            for row in reader:
                                ts = safe_float(row.get("timestamp"))
                                if ts is None:
                                    continue
                                ts_i = int(ts)
                                if not (int(win["test_start"]) <= ts_i <= int(win["test_end"])):
                                    continue
                                x = build_feature_vector(row, float(args.clip_abs))
                                if x is None:
                                    continue
                                x_rows.append(x)
                                if len(x_rows) >= int(args.samples_per_market):
                                    break
                        if not x_rows:
                            continue

                        x_np = np.asarray(x_rows, dtype=np.float64)
                        prob_job = clip_prob(model.predict_proba(x_np)[:, 1], float(args.prob_eps))
                        prob_job_cal = apply_platt(
                            prob_job,
                            float(cal_fold.get("a", 1.0) or 1.0),
                            float(cal_fold.get("b", 0.0) or 0.0),
                            float(args.prob_eps),
                        )

                        if coef.size != x_np.shape[1]:
                            raise RuntimeError(
                                f"coef size mismatch for global default {market}: {coef.size} vs {x_np.shape[1]}"
                            )
                        prob_bundle = clip_prob(sigmoid((x_np @ coef) + intercept), float(args.prob_eps))
                        prob_bundle_cal = apply_platt(
                            prob_bundle,
                            float(cal2.get("a", 1.0) or 1.0),
                            float(cal2.get("b", 0.0) or 0.0),
                            float(args.prob_eps),
                        )

                        diff = np.abs(prob_job_cal - prob_bundle_cal)
                        max_diff = float(np.max(diff))
                        mean_diff = float(np.mean(diff))
                        worst = max(worst, max_diff)
                        edge_max_diff = math.nan
                        edge_mean_diff = math.nan
                        edge_parity_mode = "not_available"
                        if edge_reg_job is not None and edge_reg_bundle is not None:
                            if edge_reg_job["coef"].size != x_np.shape[1]:
                                raise RuntimeError(
                                    f"job edge coef size mismatch for {market}: {edge_reg_job['coef'].size} vs {x_np.shape[1]}"
                                )
                            if edge_reg_bundle["coef"].size != x_np.shape[1]:
                                raise RuntimeError(
                                    f"bundle edge coef size mismatch for {market}: {edge_reg_bundle['coef'].size} vs {x_np.shape[1]}"
                                )
                            edge_job = np.clip(
                                (x_np @ edge_reg_job["coef"]) + float(edge_reg_job["intercept"]),
                                -float(edge_reg_job["clip_abs_bps"]),
                                float(edge_reg_job["clip_abs_bps"]),
                            )
                            edge_bundle = np.clip(
                                (x_np @ edge_reg_bundle["coef"]) + float(edge_reg_bundle["intercept"]),
                                -float(edge_reg_bundle["clip_abs_bps"]),
                                float(edge_reg_bundle["clip_abs_bps"]),
                            )
                            edge_diff = np.abs(edge_job - edge_bundle)
                            edge_max_diff = float(np.max(edge_diff))
                            edge_mean_diff = float(np.mean(edge_diff))
                            worst = max(worst, edge_max_diff)
                            edge_parity_mode = "checked"
                        results.append(
                            {
                                "market": market,
                                "fold_id": fold_id,
                                "n": int(x_np.shape[0]),
                                "max_abs_diff": max_diff,
                                "mean_abs_diff": mean_diff,
                                "edge_parity_mode": edge_parity_mode,
                                "edge_max_abs_diff_bps": edge_max_diff,
                                "edge_mean_abs_diff_bps": edge_mean_diff,
                            }
                        )

    status = "pass" if (len(results) > 0 and worst <= float(args.abs_tol)) else "fail"
    out = {
        "generated_at_utc": utc_now_iso(),
        "status": status,
        "mode": mode,
        "runtime_bundle_json": str(bundle_path),
        "train_summary_json": str(summary_path),
        "split_manifest_json": str(split_path),
        "comparison_count": len(results),
        "samples_per_market": int(args.samples_per_market),
        "abs_tolerance": float(args.abs_tol),
        "worst_max_abs_diff": float(worst),
        "results": results,
    }
    dump_json(output_path, out)

    print("[RuntimeBundleParity] completed", flush=True)
    print(f"status={status}", flush=True)
    print(f"comparison_count={len(results)} worst={worst:.3e}", flush=True)
    print(f"output={output_path}", flush=True)
    return 0 if status == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
