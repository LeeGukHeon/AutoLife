#!/usr/bin/env python3
import argparse
import math
from datetime import datetime, timezone
from typing import Dict, List, Tuple

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate leakage-safe walk-forward split manifest from probabilistic data manifest."
    )
    parser.add_argument(
        "--input-manifest-json",
        default=r".\data\backtest_probabilistic\probabilistic_bundle_manifest.json",
    )
    parser.add_argument(
        "--output-split-manifest-json",
        default=r".\data\backtest_probabilistic\probabilistic_split_manifest_v1.json",
    )
    parser.add_argument(
        "--dataset-kind",
        choices=("auto", "bundle", "feature"),
        default="auto",
        help="Input manifest kind. 'auto' infers from payload fields.",
    )
    parser.add_argument("--train-ratio", type=float, default=0.70)
    parser.add_argument("--valid-ratio", type=float, default=0.15)
    parser.add_argument("--test-ratio", type=float, default=0.15)
    parser.add_argument("--walk-forward-folds", type=int, default=3)
    parser.add_argument(
        "--enable-purged-walk-forward",
        action="store_true",
        help="Enable EXT-51 purged walk-forward + embargo windows.",
    )
    parser.add_argument("--h1-bars", type=int, default=1)
    parser.add_argument("--h5-bars", type=int, default=5)
    parser.add_argument(
        "--purge-bars",
        type=int,
        default=-1,
        help="Purged bars between adjacent splits. Default is max(h1_bars, h5_bars) when enabled.",
    )
    parser.add_argument(
        "--embargo-bars",
        type=int,
        default=-1,
        help="Embargo bars between adjacent splits. Default is ceil(purge_bars*0.1) when enabled.",
    )
    parser.add_argument(
        "--split-plan-json",
        default="",
        help="Optional split plan output path. When purge mode is enabled and unset, writes sibling split_plan.json.",
    )
    return parser.parse_args(argv)


def parse_iso_to_ts_ms(value: str) -> int:
    token = str(value or "").strip()
    if not token:
        return 0
    dt = datetime.fromisoformat(token.replace("Z", "+00:00")).astimezone(timezone.utc)
    return int(dt.timestamp() * 1000)


def parse_anchor_tf_to_unit_min(value: object) -> int:
    token = str(value or "").strip().lower()
    if token.endswith("m"):
        token = token[:-1]
    try:
        out = int(token)
    except Exception:
        return 0
    return max(0, out)


def ts_ms_to_iso(value: int) -> str:
    if int(value) <= 0:
        return ""
    return datetime.fromtimestamp(float(value) / 1000.0, tz=timezone.utc).isoformat()


def ratio_triplet(train_ratio: float, valid_ratio: float, test_ratio: float) -> Tuple[float, float, float]:
    a = max(0.0, float(train_ratio))
    b = max(0.0, float(valid_ratio))
    c = max(0.0, float(test_ratio))
    total = a + b + c
    if total <= 1e-12:
        raise RuntimeError("Invalid split ratios: all zero.")
    return a / total, b / total, c / total


def normalize_purge_embargo(
    *,
    enabled: bool,
    h1_bars: int,
    h5_bars: int,
    purge_bars: int,
    embargo_bars: int,
) -> Tuple[int, int]:
    if not bool(enabled):
        return 0, 0
    auto_purge = max(1, max(int(h1_bars), int(h5_bars)))
    resolved_purge = int(auto_purge if int(purge_bars) < 0 else max(0, int(purge_bars)))
    auto_embargo = int(math.ceil(float(resolved_purge) * 0.1))
    resolved_embargo = int(auto_embargo if int(embargo_bars) < 0 else max(0, int(embargo_bars)))
    return int(resolved_purge), int(resolved_embargo)


def estimate_rows_in_window(start_ts: int, end_ts: int, unit_ms: int) -> int:
    if int(unit_ms) <= 0:
        return 0
    if int(end_ts) < int(start_ts):
        return 0
    return int(((int(end_ts) - int(start_ts)) // int(unit_ms)) + 1)


def has_overlap(left_end_ts: int, right_start_ts: int, horizon_ms: int) -> bool:
    if int(left_end_ts) <= 0 or int(right_start_ts) <= 0 or int(horizon_ms) <= 0:
        return False
    return int(left_end_ts) + int(horizon_ms) >= int(right_start_ts)


def infer_manifest_kind(payload: Dict[str, object]) -> str:
    if "anchor_tf" in payload and "output_dir" in payload:
        return "feature"
    return "bundle"


def build_fold_windows(
    start_ts: int,
    end_ts: int,
    train_ratio: float,
    valid_ratio: float,
    test_ratio: float,
    folds: int,
) -> List[Dict[str, object]]:
    span = int(end_ts) - int(start_ts)
    if span <= 0:
        return []
    tr, vr, sr = ratio_triplet(train_ratio, valid_ratio, test_ratio)
    fold_list: List[Dict[str, object]] = []
    folds_count = max(1, int(folds))

    # Anchored walk-forward:
    # fold k expands train end point, then keeps valid/test contiguous after train.
    # This remains strictly time-ordered and leakage-safe.
    for idx in range(folds_count):
        progress = float(idx + 1) / float(folds_count)
        train_end_ratio = min(0.92, tr + (progress * (1.0 - tr - vr - sr + sr)))
        # Keep valid/test proportions relative to remaining tail.
        tail_ratio = max(0.05, 1.0 - train_end_ratio)
        valid_tail_ratio = vr / max(1e-12, (vr + sr))
        valid_ratio_abs = tail_ratio * valid_tail_ratio
        test_ratio_abs = tail_ratio - valid_ratio_abs

        train_end_ts = int(start_ts + (span * train_end_ratio))
        valid_end_ts = int(train_end_ts + (span * valid_ratio_abs))
        test_end_ts = int(valid_end_ts + (span * test_ratio_abs))
        test_end_ts = min(test_end_ts, end_ts)

        fold_list.append(
            {
                "fold_id": idx + 1,
                "train_start_utc": ts_ms_to_iso(start_ts),
                "train_end_utc": ts_ms_to_iso(train_end_ts),
                "valid_start_utc": ts_ms_to_iso(train_end_ts + 1),
                "valid_end_utc": ts_ms_to_iso(valid_end_ts),
                "test_start_utc": ts_ms_to_iso(valid_end_ts + 1),
                "test_end_utc": ts_ms_to_iso(test_end_ts),
            }
        )
    return fold_list


def build_fold_windows_purged(
    *,
    start_ts: int,
    end_ts: int,
    train_ratio: float,
    valid_ratio: float,
    test_ratio: float,
    folds: int,
    unit_min: int,
    h1_bars: int,
    h5_bars: int,
    purge_bars: int,
    embargo_bars: int,
) -> Tuple[List[Dict[str, object]], int]:
    span = int(end_ts) - int(start_ts)
    if span <= 0:
        return [], 0
    unit_ms = int(max(1, int(unit_min)) * 60 * 1000)
    horizon_bars = max(1, max(int(h1_bars), int(h5_bars)))
    horizon_ms = int(horizon_bars * unit_ms)
    purge_ms = int(max(0, int(purge_bars)) * unit_ms)
    embargo_ms = int(max(0, int(embargo_bars)) * unit_ms)

    tr, vr, sr = ratio_triplet(train_ratio, valid_ratio, test_ratio)
    fold_list: List[Dict[str, object]] = []
    total_removed_samples_est = 0
    folds_count = max(1, int(folds))

    for idx in range(folds_count):
        progress = float(idx + 1) / float(folds_count)
        train_end_ratio = min(0.92, tr + (progress * (1.0 - tr - vr - sr + sr)))
        tail_ratio = max(0.05, 1.0 - train_end_ratio)
        valid_tail_ratio = vr / max(1e-12, (vr + sr))
        valid_ratio_abs = tail_ratio * valid_tail_ratio
        test_ratio_abs = tail_ratio - valid_ratio_abs

        raw_train_start_ts = int(start_ts)
        raw_train_end_ts = int(start_ts + (span * train_end_ratio))
        raw_valid_start_ts = int(raw_train_end_ts + 1)
        raw_valid_end_ts = int(raw_train_end_ts + (span * valid_ratio_abs))
        raw_test_start_ts = int(raw_valid_end_ts + 1)
        raw_test_end_ts = int(min(int(raw_valid_end_ts + (span * test_ratio_abs)), int(end_ts)))

        train_start_ts = int(raw_train_start_ts)
        train_end_ts = int(raw_train_end_ts - purge_ms)
        valid_start_ts = int(raw_valid_start_ts + embargo_ms)
        valid_end_ts = int(raw_valid_end_ts - purge_ms)
        test_start_ts = int(raw_test_start_ts + embargo_ms)
        test_end_ts = int(raw_test_end_ts)

        train_end_ts = max(train_start_ts, train_end_ts)
        valid_start_ts = max(1, valid_start_ts)
        valid_end_ts = max(1, valid_end_ts)
        test_start_ts = max(1, test_start_ts)
        test_end_ts = max(1, test_end_ts)

        raw_train_rows = estimate_rows_in_window(raw_train_start_ts, raw_train_end_ts, unit_ms)
        raw_valid_rows = estimate_rows_in_window(raw_valid_start_ts, raw_valid_end_ts, unit_ms)
        raw_test_rows = estimate_rows_in_window(raw_test_start_ts, raw_test_end_ts, unit_ms)
        adj_train_rows = estimate_rows_in_window(train_start_ts, train_end_ts, unit_ms)
        adj_valid_rows = estimate_rows_in_window(valid_start_ts, valid_end_ts, unit_ms)
        adj_test_rows = estimate_rows_in_window(test_start_ts, test_end_ts, unit_ms)
        removed_train = max(0, int(raw_train_rows - adj_train_rows))
        removed_valid = max(0, int(raw_valid_rows - adj_valid_rows))
        removed_test = max(0, int(raw_test_rows - adj_test_rows))
        removed_total = int(removed_train + removed_valid + removed_test)
        total_removed_samples_est += removed_total

        overlap_train_valid = has_overlap(train_end_ts, valid_start_ts, horizon_ms)
        overlap_valid_test = has_overlap(valid_end_ts, test_start_ts, horizon_ms)
        if overlap_train_valid or overlap_valid_test:
            raise RuntimeError(
                "purged split overlap detected after adjustment: "
                f"fold={idx + 1} overlap_train_valid={overlap_train_valid} overlap_valid_test={overlap_valid_test}"
            )

        fold_list.append(
            {
                "fold_id": idx + 1,
                "train_start_utc": ts_ms_to_iso(train_start_ts),
                "train_end_utc": ts_ms_to_iso(train_end_ts),
                "valid_start_utc": ts_ms_to_iso(valid_start_ts),
                "valid_end_utc": ts_ms_to_iso(valid_end_ts),
                "test_start_utc": ts_ms_to_iso(test_start_ts),
                "test_end_utc": ts_ms_to_iso(test_end_ts),
                "purge_embargo": {
                    "enabled": True,
                    "h1_bars": int(h1_bars),
                    "h5_bars": int(h5_bars),
                    "horizon_bars": int(horizon_bars),
                    "purge_bars": int(purge_bars),
                    "embargo_bars": int(embargo_bars),
                    "removed_sample_estimate": {
                        "train": int(removed_train),
                        "valid": int(removed_valid),
                        "test": int(removed_test),
                        "total": int(removed_total),
                    },
                    "raw_windows": {
                        "train_start_utc": ts_ms_to_iso(raw_train_start_ts),
                        "train_end_utc": ts_ms_to_iso(raw_train_end_ts),
                        "valid_start_utc": ts_ms_to_iso(raw_valid_start_ts),
                        "valid_end_utc": ts_ms_to_iso(raw_valid_end_ts),
                        "test_start_utc": ts_ms_to_iso(raw_test_start_ts),
                        "test_end_utc": ts_ms_to_iso(raw_test_end_ts),
                    },
                    "overlap_check": {
                        "train_valid_overlap": False,
                        "valid_test_overlap": False,
                    },
                },
            }
        )
    return fold_list, int(total_removed_samples_est)


def main(argv=None) -> int:
    args = parse_args(argv)
    input_manifest = resolve_repo_path(args.input_manifest_json)
    output_manifest = resolve_repo_path(args.output_split_manifest_json)

    payload = load_json_or_none(input_manifest)
    if not isinstance(payload, dict):
        raise RuntimeError(f"Input manifest missing/invalid: {input_manifest}")

    jobs = payload.get("jobs", [])
    if not isinstance(jobs, list):
        jobs = []

    selected_kind = str(args.dataset_kind).strip().lower()
    if selected_kind == "auto":
        selected_kind = infer_manifest_kind(payload)
    if selected_kind not in ("bundle", "feature"):
        raise RuntimeError(f"Unsupported dataset kind: {selected_kind}")

    purge_bars, embargo_bars = normalize_purge_embargo(
        enabled=bool(args.enable_purged_walk_forward),
        h1_bars=int(args.h1_bars),
        h5_bars=int(args.h5_bars),
        purge_bars=int(args.purge_bars),
        embargo_bars=int(args.embargo_bars),
    )

    dataset_entries: List[Dict[str, object]] = []
    split_plan_datasets: List[Dict[str, object]] = []
    total_removed_samples_estimate = 0
    anchor_unit_min = parse_anchor_tf_to_unit_min(payload.get("anchor_tf"))
    for job in jobs:
        if not isinstance(job, dict):
            continue
        status = str(job.get("status", "")).strip().lower()
        market = str(job.get("market", "")).strip()
        output_path = str(job.get("output_path", "")).strip()

        if selected_kind == "bundle":
            if status not in ("fetched", "skipped_existing"):
                continue
            unit_min = int(job.get("unit_min", 0) or 0)
            rows = int(job.get("rows", 0) or 0)
        else:
            if status not in ("built", "skipped_existing"):
                continue
            unit_min = int(anchor_unit_min or 0)
            rows = int(job.get("feature_rows_written", 0) or 0)

        from_ts = parse_iso_to_ts_ms(str(job.get("from_utc", "")))
        to_ts = parse_iso_to_ts_ms(str(job.get("to_utc", "")))
        if not market or unit_min <= 0 or rows <= 0 or to_ts <= from_ts:
            continue

        if bool(args.enable_purged_walk_forward):
            folds, removed_est = build_fold_windows_purged(
                start_ts=from_ts,
                end_ts=to_ts,
                train_ratio=float(args.train_ratio),
                valid_ratio=float(args.valid_ratio),
                test_ratio=float(args.test_ratio),
                folds=int(args.walk_forward_folds),
                unit_min=int(unit_min),
                h1_bars=int(args.h1_bars),
                h5_bars=int(args.h5_bars),
                purge_bars=int(purge_bars),
                embargo_bars=int(embargo_bars),
            )
            total_removed_samples_estimate += int(removed_est)
        else:
            folds = build_fold_windows(
                start_ts=from_ts,
                end_ts=to_ts,
                train_ratio=float(args.train_ratio),
                valid_ratio=float(args.valid_ratio),
                test_ratio=float(args.test_ratio),
                folds=int(args.walk_forward_folds),
            )
            removed_est = 0
        dataset_entries.append(
            {
                "market": market,
                "dataset_kind": selected_kind,
                "unit_min": unit_min,
                "rows": rows,
                "from_utc": str(job.get("from_utc", "")),
                "to_utc": str(job.get("to_utc", "")),
                "output_path": output_path,
                "folds": folds,
            }
        )
        if bool(args.enable_purged_walk_forward):
            split_plan_datasets.append(
                {
                    "market": market,
                    "unit_min": int(unit_min),
                    "rows": int(rows),
                    "from_utc": str(job.get("from_utc", "")),
                    "to_utc": str(job.get("to_utc", "")),
                    "removed_sample_estimate_total": int(removed_est),
                    "folds": folds,
                }
            )

    out = {
        "generated_at_utc": datetime.now(tz=timezone.utc).isoformat(),
        "source_manifest_json": str(input_manifest),
        "source_manifest_kind": selected_kind,
        "source_manifest_version": str(payload.get("version", "")),
        "split_policy": {
            "mode": (
                "walk_forward_time_ordered_purged_embargo"
                if bool(args.enable_purged_walk_forward)
                else "walk_forward_time_ordered"
            ),
            "train_ratio": float(args.train_ratio),
            "valid_ratio": float(args.valid_ratio),
            "test_ratio": float(args.test_ratio),
            "walk_forward_folds": int(max(1, int(args.walk_forward_folds))),
            "random_split_allowed": False,
        },
        "dataset_count": len(dataset_entries),
        "datasets": dataset_entries,
    }
    if bool(args.enable_purged_walk_forward):
        out["split_policy"]["purge_embargo"] = {
            "enabled": True,
            "h1_bars": int(args.h1_bars),
            "h5_bars": int(args.h5_bars),
            "purge_bars": int(purge_bars),
            "embargo_bars": int(embargo_bars),
            "total_removed_samples_estimate": int(total_removed_samples_estimate),
        }
    dump_json(output_manifest, out)

    if bool(args.enable_purged_walk_forward):
        if str(args.split_plan_json).strip():
            split_plan_path = resolve_repo_path(str(args.split_plan_json).strip())
        else:
            split_plan_path = output_manifest.with_name("split_plan.json")
        split_plan_payload = {
            "generated_at_utc": datetime.now(tz=timezone.utc).isoformat(),
            "source_manifest_json": str(input_manifest),
            "split_manifest_json": str(output_manifest),
            "policy": {
                "mode": "purged_walk_forward_embargo",
                "h1_bars": int(args.h1_bars),
                "h5_bars": int(args.h5_bars),
                "purge_bars": int(purge_bars),
                "embargo_bars": int(embargo_bars),
                "walk_forward_folds": int(max(1, int(args.walk_forward_folds))),
            },
            "dataset_count": int(len(split_plan_datasets)),
            "total_removed_samples_estimate": int(total_removed_samples_estimate),
            "datasets": split_plan_datasets,
        }
        dump_json(split_plan_path, split_plan_payload)
        print(f"split_plan={split_plan_path}")

    print("[GenerateProbabilisticSplitManifest] Completed")
    print(f"source={input_manifest}")
    print(f"source_kind={selected_kind}")
    print(f"output={output_manifest}")
    print(f"dataset_count={len(dataset_entries)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
