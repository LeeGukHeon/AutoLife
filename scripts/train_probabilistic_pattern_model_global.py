#!/usr/bin/env python3
import argparse
import csv
import pathlib
from typing import Any, Dict, List

from _script_common import dump_json, load_json_or_none, resolve_repo_path
from train_probabilistic_pattern_model import (
    FEATURE_COLUMNS,
    compare_with_baseline,
    evaluate_fold_state,
    flush_infer_buffer,
    flush_train_buffer,
    init_fold_state,
    parse_iso_to_ts_ms,
    safe_float,
    safe_int01,
    save_fold_model_artifacts,
    split_name_for_ts,
    weighted_from_folds,
    utc_now_iso,
    build_feature_vector,
)


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Train a global cross-market probabilistic model "
            "(all markets merged, time-ordered fold windows preserved per market)."
        )
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
        default=r".\build\Release\logs\probabilistic_model_train_summary_global_full_20260220.json",
    )
    parser.add_argument("--model-dir", default=r".\build\Release\models\probabilistic_pattern_global_v1")
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
        help="Target column for h5 head.",
    )
    parser.add_argument(
        "--edge-column",
        default="label_edge_bps_h5",
        help="Edge/return column name recorded in artifact metadata.",
    )
    parser.add_argument(
        "--drop-neutral-target",
        action="store_true",
        default=True,
        help="Compatibility flag with per-market trainer artifact schema.",
    )
    parser.add_argument(
        "--keep-neutral-target",
        dest="drop_neutral_target",
        action="store_false",
        help="Compatibility flag with per-market trainer artifact schema.",
    )
    parser.add_argument(
        "--enable-edge-regressor",
        action="store_true",
        default=True,
        help="Enable h5 edge regressor head (shared with per-market trainer state layout).",
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
    parser.add_argument(
        "--ensemble-k",
        type=int,
        default=1,
        help="EXT-53 optional: number of independent global members to train (default 1 = baseline).",
    )
    parser.add_argument(
        "--ensemble-seed-step",
        type=int,
        default=1000,
        help="Seed offset step between ensemble members.",
    )
    return parser.parse_args(argv)


def parse_dataset_fold_windows(dataset: Dict[str, Any]) -> Dict[int, Dict[str, int]]:
    fold_windows: Dict[int, Dict[str, int]] = {}
    for fold in dataset.get("folds", []) or []:
        if not isinstance(fold, dict):
            continue
        fold_id = int(fold.get("fold_id", 0) or 0)
        if fold_id <= 0:
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
        fold_windows[fold_id] = win
    return fold_windows


def build_global_state(fold_id: int, args: argparse.Namespace) -> Dict[str, Any]:
    # Reuse the exact state layout used by the per-market trainer.
    dummy = {
        "fold_id": int(fold_id),
        "win": {
            "train_start": 1,
            "train_end": 1,
            "valid_start": 1,
            "valid_end": 1,
            "test_start": 1,
            "test_end": 1,
        },
    }
    state = init_fold_state(dummy, args)
    state["fold_id"] = int(fold_id)
    return state


def stream_dataset_into_global_states(
    *,
    dataset_market: str,
    csv_path: pathlib.Path,
    dataset_fold_windows: Dict[int, Dict[str, int]],
    global_states: Dict[int, Dict[str, Any]],
    args: argparse.Namespace,
) -> Dict[str, Any]:
    rows_total = 0
    rows_used = 0
    rows_skipped = 0

    print(
        f"[TrainProbModelGlobal] stream market={dataset_market} file={csv_path.name}",
        flush=True,
    )
    with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows_total += 1
            ts = safe_float(row.get("timestamp"))
            y1 = safe_int01(row.get("label_up_h1"))
            y5 = safe_int01(row.get("label_up_h5"))
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
            for fold_id, win in dataset_fold_windows.items():
                state = global_states.get(int(fold_id))
                if not isinstance(state, dict):
                    continue
                split = split_name_for_ts(ts_i, win)
                if split is None:
                    continue
                if split == "train":
                    state["train_x"].append(x)
                    state["train_y_h1"].append(int(y1))
                    state["train_y_h5"].append(int(y5))
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

    return {
        "market": str(dataset_market),
        "csv_path": str(csv_path),
        "rows_total": int(rows_total),
        "rows_used": int(rows_used),
        "rows_skipped": int(rows_skipped),
        "fold_ids": sorted([int(x) for x in dataset_fold_windows.keys()]),
        "status": "ok",
    }


def build_weighted_summary(global_folds: List[Dict[str, Any]]) -> Dict[str, Any]:
    weighted_summary = {
        "h1_test_raw_logloss": weighted_from_folds(global_folds, "h1", "test_raw", "logloss"),
        "h1_test_calibrated_logloss": weighted_from_folds(global_folds, "h1", "test_calibrated", "logloss"),
        "h1_test_raw_brier": weighted_from_folds(global_folds, "h1", "test_raw", "brier"),
        "h1_test_calibrated_brier": weighted_from_folds(global_folds, "h1", "test_calibrated", "brier"),
        "h1_test_raw_accuracy": weighted_from_folds(global_folds, "h1", "test_raw", "accuracy"),
        "h1_test_calibrated_accuracy": weighted_from_folds(global_folds, "h1", "test_calibrated", "accuracy"),
        "h5_test_raw_logloss": weighted_from_folds(global_folds, "h5", "test_raw", "logloss"),
        "h5_test_calibrated_logloss": weighted_from_folds(global_folds, "h5", "test_calibrated", "logloss"),
        "h5_test_raw_brier": weighted_from_folds(global_folds, "h5", "test_raw", "brier"),
        "h5_test_calibrated_brier": weighted_from_folds(global_folds, "h5", "test_calibrated", "brier"),
        "h5_test_raw_accuracy": weighted_from_folds(global_folds, "h5", "test_raw", "accuracy"),
        "h5_test_calibrated_accuracy": weighted_from_folds(global_folds, "h5", "test_calibrated", "accuracy"),
    }

    total_h5_test = 0
    total_h5_selected = 0
    sum_h5_selected_edge = 0.0
    sum_h5_selected_pos = 0
    for fold in global_folds:
        h5_trade = fold.get("metrics", {}).get("h5", {}).get("test_trade_metrics", {})
        n_total = int(h5_trade.get("n_total", 0) or 0)
        n_sel = int(h5_trade.get("n_selected", 0) or 0)
        total_h5_test += n_total
        total_h5_selected += n_sel
        mean_edge = safe_float(h5_trade.get("mean_edge_bps"))
        pos_edge = safe_float(h5_trade.get("positive_edge_rate"))
        if mean_edge is not None and n_sel > 0:
            sum_h5_selected_edge += (float(mean_edge) * n_sel)
        if pos_edge is not None and n_sel > 0:
            sum_h5_selected_pos += int(round(float(pos_edge) * n_sel))

    weighted_summary["h5_test_selection_coverage"] = (
        float(total_h5_selected) / float(total_h5_test) if total_h5_test > 0 else 0.0
    )
    weighted_summary["h5_test_selected_mean_edge_bps"] = (
        float(sum_h5_selected_edge) / float(total_h5_selected) if total_h5_selected > 0 else float("nan")
    )
    weighted_summary["h5_test_selected_positive_edge_rate"] = (
        float(sum_h5_selected_pos) / float(total_h5_selected) if total_h5_selected > 0 else float("nan")
    )
    weighted_summary["h5_test_selected_count"] = int(total_h5_selected)
    weighted_summary["h5_test_total_count"] = int(total_h5_test)
    return weighted_summary


def run_single_global_member(
    *,
    prepared: List[Dict[str, Any]],
    fold_ids: List[int],
    fold_dataset_counts: Dict[int, int],
    args: argparse.Namespace,
    model_dir: pathlib.Path,
    member_index: int,
    member_random_state: int,
) -> Dict[str, Any]:
    member_args = argparse.Namespace(**vars(args))
    member_args.random_state = int(member_random_state)
    if int(getattr(args, "ensemble_k", 1)) > 1:
        member_model_dir = model_dir / f"ensemble_member_{int(member_index):02d}"
    else:
        member_model_dir = model_dir
    member_model_dir.mkdir(parents=True, exist_ok=True)

    global_states: Dict[int, Dict[str, Any]] = {
        int(fid): build_global_state(int(fid), member_args) for fid in fold_ids
    }

    dataset_results: List[Dict[str, Any]] = []
    failed_datasets: List[Dict[str, Any]] = []
    for ds in prepared:
        market = str(ds["market"])
        csv_path = pathlib.Path(ds["csv_path"])
        try:
            result = stream_dataset_into_global_states(
                dataset_market=market,
                csv_path=csv_path,
                dataset_fold_windows=ds["fold_windows"],
                global_states=global_states,
                args=member_args,
            )
            dataset_results.append(result)
        except Exception as exc:
            failed = {
                "market": market,
                "csv_path": str(csv_path),
                "status": "failed",
                "error": str(exc),
            }
            dataset_results.append(failed)
            failed_datasets.append(failed)

    for state in global_states.values():
        flush_train_buffer(state)
        flush_infer_buffer(state, "valid", float(member_args.prob_eps))
        flush_infer_buffer(state, "test", float(member_args.prob_eps))

    global_fold_rows: List[Dict[str, Any]] = []
    failed_folds: List[Dict[str, Any]] = []
    for fold_id in fold_ids:
        state = global_states[int(fold_id)]
        fold_eval = evaluate_fold_state(state=state, args=member_args)
        model_paths = save_fold_model_artifacts(
            model_dir=member_model_dir,
            market="GLOBAL",
            fold_eval=fold_eval,
            args=member_args,
        )
        fold_eval.pop("model_state", None)
        fold_eval["model_artifacts"] = model_paths
        fold_eval["dataset_count_for_fold"] = int(fold_dataset_counts.get(int(fold_id), 0))
        global_fold_rows.append(fold_eval)
        if fold_eval["train_count"] <= 0 or fold_eval["test_count"] <= 0:
            failed_folds.append(
                {
                    "fold_id": int(fold_id),
                    "reason": "insufficient_samples",
                    "train_count": int(fold_eval["train_count"]),
                    "test_count": int(fold_eval["test_count"]),
                }
            )

    weighted_summary = build_weighted_summary(global_fold_rows)
    status = "pass" if not failed_datasets and not failed_folds else "partial_fail"
    return {
        "member_index": int(member_index),
        "random_state": int(member_random_state),
        "status": str(status),
        "model_dir": str(member_model_dir),
        "datasets": dataset_results,
        "failed_datasets": failed_datasets,
        "global_folds": global_fold_rows,
        "failed_folds": failed_folds,
        "weighted_summary": weighted_summary,
    }


def main(argv=None) -> int:
    args = parse_args(argv)
    split_manifest_path = resolve_repo_path(args.split_manifest_json)
    baseline_json_path = resolve_repo_path(args.baseline_json)
    output_json_path = resolve_repo_path(args.output_json)
    model_dir = resolve_repo_path(args.model_dir)
    model_dir.mkdir(parents=True, exist_ok=True)

    split_manifest = load_json_or_none(split_manifest_path)
    if not isinstance(split_manifest, dict):
        raise RuntimeError(f"invalid split manifest: {split_manifest_path}")
    split_policy = split_manifest.get("split_policy", {})
    if not isinstance(split_policy, dict):
        split_policy = {}
    split_cost_model = split_manifest.get("cost_model", {})
    if not isinstance(split_cost_model, dict):
        split_cost_model = {}
    raw_datasets = split_manifest.get("datasets", [])
    if not isinstance(raw_datasets, list) or not raw_datasets:
        raise RuntimeError(f"empty datasets in split manifest: {split_manifest_path}")

    prepared: List[Dict[str, Any]] = []
    for ds in raw_datasets:
        if not isinstance(ds, dict):
            continue
        market = str(ds.get("market", "")).strip()
        output_path = str(ds.get("output_path", "")).strip()
        if not market or not output_path:
            continue
        csv_path = pathlib.Path(output_path)
        if not csv_path.exists():
            continue
        fold_windows = parse_dataset_fold_windows(ds)
        if not fold_windows:
            continue
        prepared.append(
            {
                "market": market,
                "csv_path": csv_path,
                "fold_windows": fold_windows,
            }
        )

    if int(args.max_datasets) > 0:
        prepared = prepared[: int(args.max_datasets)]
    if not prepared:
        raise RuntimeError("no valid datasets for global training")

    fold_ids = sorted({int(fid) for ds in prepared for fid in ds["fold_windows"].keys()})
    if not fold_ids:
        raise RuntimeError("no fold ids discovered for global training")

    fold_dataset_counts: Dict[int, int] = {int(fid): 0 for fid in fold_ids}
    for ds in prepared:
        for fid in ds["fold_windows"].keys():
            fold_dataset_counts[int(fid)] = fold_dataset_counts.get(int(fid), 0) + 1

    print("[TrainProbModelGlobal] start", flush=True)
    print(f"split_manifest={split_manifest_path}", flush=True)
    print(f"datasets={len(prepared)}", flush=True)
    print(f"fold_ids={fold_ids}", flush=True)
    print(f"model_dir={model_dir}", flush=True)

    started_at = utc_now_iso()
    ensemble_k = max(1, int(args.ensemble_k))
    ensemble_seed_step = max(1, int(args.ensemble_seed_step))
    member_runs: List[Dict[str, Any]] = []
    for member_index in range(ensemble_k):
        member_random_state = int(args.random_state) + (int(member_index) * int(ensemble_seed_step))
        print(
            f"[TrainProbModelGlobal] ensemble member={member_index + 1}/{ensemble_k} seed={member_random_state}",
            flush=True,
        )
        member_result = run_single_global_member(
            prepared=prepared,
            fold_ids=fold_ids,
            fold_dataset_counts=fold_dataset_counts,
            args=args,
            model_dir=model_dir,
            member_index=member_index,
            member_random_state=member_random_state,
        )
        member_runs.append(member_result)

    primary_member = member_runs[0]
    dataset_results = list(primary_member.get("datasets", []))
    failed_datasets = list(primary_member.get("failed_datasets", []))
    global_fold_rows = list(primary_member.get("global_folds", []))
    failed_folds = list(primary_member.get("failed_folds", []))
    weighted_summary = dict(primary_member.get("weighted_summary", {}))
    baseline_compare = compare_with_baseline(
        baseline_json=baseline_json_path,
        weighted_summary=weighted_summary,
    )

    ended_at = utc_now_iso()
    status = "pass"
    if failed_datasets or failed_folds:
        status = "partial_fail"
    if any(str(m.get("status", "partial_fail")) != "pass" for m in member_runs):
        status = "partial_fail"

    out = {
        "version": "probabilistic_pattern_model_global_v1",
        "scope": "global_cross_market",
        "started_at_utc": started_at,
        "finished_at_utc": ended_at,
        "status": status,
        "split_manifest_json": str(split_manifest_path),
        "baseline_json": str(baseline_json_path),
        "model_dir": str(model_dir),
        "feature_columns": FEATURE_COLUMNS,
        "sgd_config": {
            "alpha": float(args.alpha),
            "l1_ratio": float(args.l1_ratio),
            "batch_size": int(args.batch_size),
            "infer_batch_size": int(args.infer_batch_size),
            "random_state": int(args.random_state),
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
        "dataset_count": int(len(prepared)),
        "dataset_processed": int(len(dataset_results)),
        "dataset_failed": int(len(failed_datasets)),
        "failed_datasets": failed_datasets,
        "fold_ids": fold_ids,
        "global_model": {
            "status": "pass" if not failed_folds else "partial_fail",
            "fold_count": int(len(global_fold_rows)),
            "failed_folds": failed_folds,
            "folds": global_fold_rows,
        },
        "weighted_summary": weighted_summary,
        "baseline_comparison": baseline_compare,
        "datasets": dataset_results,
    }
    purge_embargo_cfg = split_policy.get("purge_embargo", {})
    if isinstance(purge_embargo_cfg, dict) and bool(purge_embargo_cfg.get("enabled", False)):
        out["purge_embargo"] = purge_embargo_cfg
    if bool(split_cost_model.get("enabled", False)):
        out["cost_model"] = split_cost_model
    if ensemble_k > 1:
        out["ensemble"] = {
            "enabled": True,
            "ensemble_k": int(ensemble_k),
            "seed_step": int(ensemble_seed_step),
            "members": [
                {
                    "member_index": int(m.get("member_index", 0) or 0),
                    "random_state": int(m.get("random_state", 0) or 0),
                    "status": str(m.get("status", "partial_fail")),
                    "model_dir": str(m.get("model_dir", "")),
                    "weighted_summary": m.get("weighted_summary", {}),
                    "global_model": {
                        "status": "pass" if not m.get("failed_folds", []) else "partial_fail",
                        "fold_count": int(len(m.get("global_folds", []) or [])),
                        "failed_folds": m.get("failed_folds", []),
                        "folds": m.get("global_folds", []),
                    },
                    "dataset_processed": int(len(m.get("datasets", []) or [])),
                    "dataset_failed": int(len(m.get("failed_datasets", []) or [])),
                }
                for m in member_runs
            ],
        }
    dump_json(output_json_path, out)

    print("[TrainProbModelGlobal] completed", flush=True)
    print(f"status={status}", flush=True)
    print(f"datasets={len(prepared)} failed={len(failed_datasets)}", flush=True)
    print(f"folds={len(global_fold_rows)} fold_failed={len(failed_folds)}", flush=True)
    print(f"output={output_json_path}", flush=True)
    return 0 if status == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
