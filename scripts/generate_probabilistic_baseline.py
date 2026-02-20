#!/usr/bin/env python3
import argparse
import csv
import math
import pathlib
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate baseline metrics on probabilistic feature split manifest."
    )
    parser.add_argument(
        "--split-manifest-json",
        default=r".\data\model_input\probabilistic_features_v1_full_20260220_181345\probabilistic_split_manifest_v1.json",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\probabilistic_baseline_summary.json",
    )
    parser.add_argument("--clip-eps", type=float, default=1e-9)
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_iso_to_ts_ms(value: str) -> int:
    token = str(value or "").strip()
    if not token:
        return 0
    dt = datetime.fromisoformat(token.replace("Z", "+00:00")).astimezone(timezone.utc)
    return int(dt.timestamp() * 1000)


def classify_split(ts_ms: int, fold: Dict[str, int]) -> Optional[str]:
    if fold["train_start"] <= ts_ms <= fold["train_end"]:
        return "train"
    if fold["valid_start"] <= ts_ms <= fold["valid_end"]:
        return "valid"
    if fold["test_start"] <= ts_ms <= fold["test_end"]:
        return "test"
    return None


def clipped_prob(p: float, eps: float) -> float:
    return min(1.0 - eps, max(eps, float(p)))


def logloss_from_counts(n: int, pos: int, p: float, eps: float) -> float:
    if n <= 0:
        return math.nan
    q = clipped_prob(p, eps)
    neg = n - pos
    return -((float(pos) * math.log(q)) + (float(neg) * math.log(1.0 - q))) / float(n)


def brier_from_counts(n: int, pos: int, p: float) -> float:
    if n <= 0:
        return math.nan
    neg = n - pos
    return ((float(neg) * (p ** 2)) + (float(pos) * ((1.0 - p) ** 2))) / float(n)


def accuracy_from_counts(n: int, pos: int, p: float) -> float:
    if n <= 0:
        return math.nan
    pred = 1 if float(p) >= 0.5 else 0
    correct = pos if pred == 1 else (n - pos)
    return float(correct) / float(n)


def init_split_bucket() -> Dict[str, float]:
    return {
        "n": 0,
        "pos_h1": 0,
        "pos_h5": 0,
        "edge_sum_h5": 0.0,
        "edge_pos_count_h5": 0,
    }


def parse_int01(value: Any) -> Optional[int]:
    try:
        v = int(float(value))
    except Exception:
        return None
    if v not in (0, 1):
        return None
    return v


def parse_float(value: Any) -> Optional[float]:
    try:
        v = float(value)
    except Exception:
        return None
    if not math.isfinite(v):
        return None
    return v


def load_dataset_metrics(
    *,
    dataset: Dict[str, Any],
    clip_eps: float,
) -> Dict[str, Any]:
    market = str(dataset.get("market", "")).strip()
    csv_path = pathlib.Path(str(dataset.get("output_path", "")).strip())
    if not csv_path.exists():
        return {
            "market": market,
            "status": "missing_csv",
            "csv_path": str(csv_path),
            "pass": False,
            "errors": [f"missing csv: {csv_path}"],
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

    folds: List[Dict[str, Any]] = []
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
        if fold_id <= 0:
            continue
        if min(win.values()) <= 0:
            continue
        folds.append(
            {
                "fold_id": fold_id,
                "win": win,
                "train": init_split_bucket(),
                "valid": init_split_bucket(),
                "test": init_split_bucket(),
            }
        )

    if not folds:
        return {
            "market": market,
            "status": "invalid_fold_windows",
            "csv_path": str(csv_path),
            "pass": False,
            "errors": ["no valid fold windows"],
        }

    skipped_rows = 0
    read_rows = 0
    with csv_path.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            read_rows += 1
            try:
                ts = int(float(row.get("timestamp", "")))
            except Exception:
                skipped_rows += 1
                continue

            y1 = parse_int01(row.get("label_up_h1"))
            y5 = parse_int01(row.get("label_up_h5"))
            edge5 = parse_float(row.get("label_edge_bps_h5"))
            if y1 is None or y5 is None or edge5 is None:
                skipped_rows += 1
                continue

            for fold in folds:
                split_name = classify_split(ts, fold["win"])
                if split_name is None:
                    continue
                bucket = fold[split_name]
                bucket["n"] += 1
                bucket["pos_h1"] += y1
                bucket["pos_h5"] += y5
                bucket["edge_sum_h5"] += edge5
                if edge5 > 0.0:
                    bucket["edge_pos_count_h5"] += 1

    fold_metrics = []
    for fold in folds:
        train = fold["train"]
        valid = fold["valid"]
        test = fold["test"]

        n_train = int(train["n"])
        n_valid = int(valid["n"])
        n_test = int(test["n"])

        p_h1_train = (float(train["pos_h1"]) / float(n_train)) if n_train > 0 else math.nan
        p_h5_train = (float(train["pos_h5"]) / float(n_train)) if n_train > 0 else math.nan

        fold_metrics.append(
            {
                "fold_id": int(fold["fold_id"]),
                "counts": {
                    "train": n_train,
                    "valid": n_valid,
                    "test": n_test,
                },
                "train_priors": {
                    "p_h1": p_h1_train,
                    "p_h5": p_h5_train,
                },
                "valid_metrics": {
                    "h1_logloss": logloss_from_counts(n_valid, int(valid["pos_h1"]), p_h1_train, clip_eps),
                    "h1_brier": brier_from_counts(n_valid, int(valid["pos_h1"]), p_h1_train),
                    "h1_accuracy": accuracy_from_counts(n_valid, int(valid["pos_h1"]), p_h1_train),
                    "h5_logloss": logloss_from_counts(n_valid, int(valid["pos_h5"]), p_h5_train, clip_eps),
                    "h5_brier": brier_from_counts(n_valid, int(valid["pos_h5"]), p_h5_train),
                    "h5_accuracy": accuracy_from_counts(n_valid, int(valid["pos_h5"]), p_h5_train),
                    "h5_observed_positive_rate": (float(valid["pos_h5"]) / float(n_valid)) if n_valid > 0 else math.nan,
                    "h5_mean_edge_bps": (float(valid["edge_sum_h5"]) / float(n_valid)) if n_valid > 0 else math.nan,
                    "h5_positive_edge_rate": (float(valid["edge_pos_count_h5"]) / float(n_valid)) if n_valid > 0 else math.nan,
                },
                "test_metrics": {
                    "h1_logloss": logloss_from_counts(n_test, int(test["pos_h1"]), p_h1_train, clip_eps),
                    "h1_brier": brier_from_counts(n_test, int(test["pos_h1"]), p_h1_train),
                    "h1_accuracy": accuracy_from_counts(n_test, int(test["pos_h1"]), p_h1_train),
                    "h5_logloss": logloss_from_counts(n_test, int(test["pos_h5"]), p_h5_train, clip_eps),
                    "h5_brier": brier_from_counts(n_test, int(test["pos_h5"]), p_h5_train),
                    "h5_accuracy": accuracy_from_counts(n_test, int(test["pos_h5"]), p_h5_train),
                    "h5_observed_positive_rate": (float(test["pos_h5"]) / float(n_test)) if n_test > 0 else math.nan,
                    "h5_mean_edge_bps": (float(test["edge_sum_h5"]) / float(n_test)) if n_test > 0 else math.nan,
                    "h5_positive_edge_rate": (float(test["edge_pos_count_h5"]) / float(n_test)) if n_test > 0 else math.nan,
                },
            }
        )

    return {
        "market": market,
        "status": "ok",
        "csv_path": str(csv_path),
        "pass": True,
        "rows_read": int(read_rows),
        "rows_skipped": int(skipped_rows),
        "folds": fold_metrics,
    }


def weighted_mean(items: List[Dict[str, Any]], metric_path: List[str], weight_path: List[str]) -> float:
    num = 0.0
    den = 0.0
    for obj in items:
        cur = obj
        ok = True
        for k in metric_path:
            if not isinstance(cur, dict) or k not in cur:
                ok = False
                break
            cur = cur[k]
        if not ok:
            continue
        metric = float(cur)
        if not math.isfinite(metric):
            continue

        cur_w = obj
        ok_w = True
        for k in weight_path:
            if not isinstance(cur_w, dict) or k not in cur_w:
                ok_w = False
                break
            cur_w = cur_w[k]
        if not ok_w:
            continue
        w = float(cur_w)
        if not math.isfinite(w) or w <= 0.0:
            continue
        num += (metric * w)
        den += w
    return (num / den) if den > 0.0 else math.nan


def main(argv=None) -> int:
    args = parse_args(argv)
    split_manifest_path = resolve_repo_path(args.split_manifest_json)
    output_json_path = resolve_repo_path(args.output_json)

    payload = load_json_or_none(split_manifest_path)
    if not isinstance(payload, dict):
        raise RuntimeError(f"invalid split manifest: {split_manifest_path}")

    datasets = payload.get("datasets", [])
    if not isinstance(datasets, list):
        datasets = []

    print("[GenerateProbabilisticBaseline] start", flush=True)
    print(f"split_manifest={split_manifest_path}", flush=True)
    print(f"dataset_count={len(datasets)}", flush=True)

    results = []
    failed = []
    fold_pool = []
    for idx, dataset in enumerate(datasets, start=1):
        if not isinstance(dataset, dict):
            continue
        market = str(dataset.get("market", "")).strip()
        print(f"[{idx}] baseline {market}", flush=True)
        result = load_dataset_metrics(dataset=dataset, clip_eps=float(args.clip_eps))
        results.append(result)
        if not bool(result.get("pass", False)):
            failed.append({"market": market, "errors": result.get("errors", [])})
            print(f"[{idx}] fail {market}: {result.get('errors', [])}", flush=True)
            continue
        print(
            f"[{idx}] pass {market} rows_read={result.get('rows_read', 0)} skipped={result.get('rows_skipped', 0)}",
            flush=True,
        )
        for f in result.get("folds", []) or []:
            if isinstance(f, dict):
                fold_pool.append(f)

    summary = {
        "generated_at_utc": utc_now_iso(),
        "status": "pass" if len(failed) == 0 else "fail",
        "split_manifest_json": str(split_manifest_path),
        "dataset_count": len(results),
        "dataset_passed": sum(1 for x in results if bool(x.get("pass", False))),
        "dataset_failed": len(failed),
        "failed": failed,
        "weighted_overall": {
            "test_h1_logloss": weighted_mean(
                fold_pool, ["test_metrics", "h1_logloss"], ["counts", "test"]
            ),
            "test_h1_brier": weighted_mean(
                fold_pool, ["test_metrics", "h1_brier"], ["counts", "test"]
            ),
            "test_h1_accuracy": weighted_mean(
                fold_pool, ["test_metrics", "h1_accuracy"], ["counts", "test"]
            ),
            "test_h5_logloss": weighted_mean(
                fold_pool, ["test_metrics", "h5_logloss"], ["counts", "test"]
            ),
            "test_h5_brier": weighted_mean(
                fold_pool, ["test_metrics", "h5_brier"], ["counts", "test"]
            ),
            "test_h5_accuracy": weighted_mean(
                fold_pool, ["test_metrics", "h5_accuracy"], ["counts", "test"]
            ),
            "test_h5_observed_positive_rate": weighted_mean(
                fold_pool, ["test_metrics", "h5_observed_positive_rate"], ["counts", "test"]
            ),
            "test_h5_mean_edge_bps": weighted_mean(
                fold_pool, ["test_metrics", "h5_mean_edge_bps"], ["counts", "test"]
            ),
            "test_h5_positive_edge_rate": weighted_mean(
                fold_pool, ["test_metrics", "h5_positive_edge_rate"], ["counts", "test"]
            ),
        },
        "datasets": results,
    }
    dump_json(output_json_path, summary)

    print("[GenerateProbabilisticBaseline] completed", flush=True)
    print(f"status={summary['status']}", flush=True)
    print(f"output={output_json_path}", flush=True)
    return 0 if summary["status"] == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())

