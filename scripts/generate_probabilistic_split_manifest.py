#!/usr/bin/env python3
import argparse
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

    dataset_entries: List[Dict[str, object]] = []
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

        folds = build_fold_windows(
            start_ts=from_ts,
            end_ts=to_ts,
            train_ratio=float(args.train_ratio),
            valid_ratio=float(args.valid_ratio),
            test_ratio=float(args.test_ratio),
            folds=int(args.walk_forward_folds),
        )
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

    out = {
        "generated_at_utc": datetime.now(tz=timezone.utc).isoformat(),
        "source_manifest_json": str(input_manifest),
        "source_manifest_kind": selected_kind,
        "source_manifest_version": str(payload.get("version", "")),
        "split_policy": {
            "mode": "walk_forward_time_ordered",
            "train_ratio": float(args.train_ratio),
            "valid_ratio": float(args.valid_ratio),
            "test_ratio": float(args.test_ratio),
            "walk_forward_folds": int(max(1, int(args.walk_forward_folds))),
            "random_split_allowed": False,
        },
        "dataset_count": len(dataset_entries),
        "datasets": dataset_entries,
    }
    dump_json(output_manifest, out)

    print("[GenerateProbabilisticSplitManifest] Completed")
    print(f"source={input_manifest}")
    print(f"source_kind={selected_kind}")
    print(f"output={output_manifest}")
    print(f"dataset_count={len(dataset_entries)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
