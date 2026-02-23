#!/usr/bin/env python3
import argparse
import csv
import math
import pathlib
from collections import deque
from datetime import datetime, timezone
from typing import Any, Dict, List, Tuple

from _script_common import dump_json, load_json_or_none, resolve_repo_path
from probabilistic_cost_model import resolve_label_cost_bps


BASELINE_COLUMN_ORDER = [
    "timestamp",
    "timestamp_utc",
    "market",
    "close",
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
    "label_up_h1",
    "label_up_h5",
    "label_edge_bps_h5",
]

BASELINE_NUMERIC_COLUMNS = [
    "close",
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
    "regime_vol_60_atr_pct",
    "label_edge_bps_h5",
]

BASELINE_LABEL_COLUMNS = ["label_up_h1", "label_up_h5"]
PIPELINE_TO_DATASET_VERSION = {"v1": "prob_features_v1", "v2": "prob_features_v2_draft"}
PIPELINE_TO_CONTRACT_VERSION = {"v1": "v1", "v2": "v2_draft"}


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate probabilistic feature dataset quality (schema, missing, order, leakage-safe labels)."
    )
    parser.add_argument(
        "--dataset-manifest-json",
        default=r".\data\model_input\probabilistic_features_v1\feature_dataset_manifest.json",
    )
    parser.add_argument(
        "--contract-json",
        default=r".\config\model\probabilistic_feature_contract_v1.json",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\probabilistic_feature_validation_summary.json",
    )
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("auto", "v1", "v2"),
        default="auto",
        help="Gate profile selector. auto infers from manifest.",
    )
    parser.add_argument("--edge-tolerance", type=float, default=1e-6)
    parser.add_argument("--strict", action="store_true")
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def round6(value: float) -> float:
    if not math.isfinite(value):
        return math.nan
    return round(float(value), 6)


def normalize_nonempty_str_list(raw: Any) -> List[str]:
    if not isinstance(raw, list):
        return []
    out: List[str] = []
    seen = set()
    for item in raw:
        token = str(item).strip()
        if not token or token in seen:
            continue
        seen.add(token)
        out.append(token)
    return out


def infer_pipeline_from_manifest(manifest: Dict[str, Any]) -> str:
    explicit = str(manifest.get("pipeline_version", "")).strip().lower()
    if explicit in ("v1", "v2"):
        return explicit
    version = str(manifest.get("version", "")).strip().lower()
    if "v2" in version:
        return "v2"
    return "v1"


def infer_pipeline_from_contract(contract: Dict[str, Any]) -> str:
    version = str(contract.get("version", "")).strip().lower()
    if version == "v2_draft":
        return "v2"
    if version == "v1":
        return "v1"
    if "v2" in version:
        return "v2"
    return "v1"


def read_float(row: Dict[str, str], key: str) -> Tuple[bool, float]:
    raw = str(row.get(key, "")).strip()
    if not raw:
        return False, math.nan
    try:
        v = float(raw)
    except Exception:
        return False, math.nan
    if not math.isfinite(v):
        return False, math.nan
    return True, v


def read_int_label(row: Dict[str, str], key: str) -> Tuple[bool, int]:
    raw = str(row.get(key, "")).strip()
    if raw == "":
        return False, -1
    try:
        v = int(float(raw))
    except Exception:
        return False, -1
    if v not in (0, 1):
        return False, v
    return True, v


def validate_single_file(
    *,
    path_value: pathlib.Path,
    expected_market: str,
    roundtrip_cost_bps: float,
    cost_model_config: Dict[str, object],
    forbidden_fields: List[str],
    edge_tolerance: float,
    expected_header: List[str],
    numeric_columns: List[str],
    label_columns: List[str],
) -> Dict[str, Any]:
    if not path_value.exists():
        return {
            "file": str(path_value),
            "market": expected_market,
            "status": "missing_file",
            "pass": False,
            "errors": [f"missing file: {path_value}"],
        }

    expected_header_set = set(expected_header)
    enable_h1_check = ("close" in expected_header_set) and ("label_up_h1" in expected_header_set)
    enable_h5_check = ("close" in expected_header_set) and ("label_up_h5" in expected_header_set)
    enable_edge_check = all(
        key in expected_header_set
        for key in ("close", "label_edge_bps_h5", "atr_pct_14", "bb_width_20", "vol_ratio_20", "notional_ratio_20")
    )

    header: List[str] = []
    row_count = 0
    ts_order_violations = 0
    market_mismatch_count = 0
    invalid_numeric_counts = {k: 0 for k in numeric_columns}
    invalid_label_counts = {k: 0 for k in label_columns}
    label_positive_counts = {k: 0 for k in label_columns}
    forbidden_present = []
    malformed_timestamp_count = 0
    label_h1_mismatch = 0
    label_h5_mismatch = 0
    edge_h5_mismatch = 0
    edge_min = math.inf
    edge_max = -math.inf
    edge_sum = 0.0
    last_ts = -1

    window: deque = deque()
    max_h = 5

    with path_value.open("r", encoding="utf-8", errors="ignore", newline="") as fh:
        reader = csv.DictReader(fh)
        header = list(reader.fieldnames or [])
        header_set = set(header)
        for f in forbidden_fields:
            if f in header_set:
                forbidden_present.append(f)

        for row in reader:
            row_count += 1
            market_value = str(row.get("market", "")).strip()
            if market_value != expected_market:
                market_mismatch_count += 1

            ts_raw = str(row.get("timestamp", "")).strip()
            try:
                ts = int(float(ts_raw))
            except Exception:
                malformed_timestamp_count += 1
                ts = -1
            if ts <= last_ts:
                ts_order_violations += 1
            if ts > 0:
                last_ts = ts

            for key in numeric_columns:
                ok, _ = read_float(row, key)
                if not ok:
                    invalid_numeric_counts[key] += 1

            for key in label_columns:
                ok, v = read_int_label(row, key)
                if not ok:
                    invalid_label_counts[key] += 1
                elif v == 1:
                    label_positive_counts[key] += 1

            ok_edge, edge = read_float(row, "label_edge_bps_h5")
            if ok_edge:
                edge_min = min(edge_min, edge)
                edge_max = max(edge_max, edge)
                edge_sum += edge

            ok_close, close = read_float(row, "close")
            ok_h1, label_h1 = read_int_label(row, "label_up_h1")
            ok_h5, label_h5 = read_int_label(row, "label_up_h5")
            window.append(
                {
                    "ok_close": ok_close,
                    "close": close,
                    "ok_h1": ok_h1,
                    "label_h1": label_h1,
                    "ok_h5": ok_h5,
                    "label_h5": label_h5,
                    "ok_edge": ok_edge,
                    "edge": edge if ok_edge else math.nan,
                    "atr_pct_14": read_float(row, "atr_pct_14")[1],
                    "bb_width_20": read_float(row, "bb_width_20")[1],
                    "vol_ratio_20": read_float(row, "vol_ratio_20")[1],
                    "notional_ratio_20": read_float(row, "notional_ratio_20")[1],
                }
            )

            if len(window) >= (max_h + 1):
                cur = window[0]
                nxt1 = window[1]
                nxt5 = window[5]
                if enable_h1_check and cur["ok_close"] and nxt1["ok_close"] and cur["ok_h1"]:
                    expected_h1 = 1 if float(nxt1["close"]) > float(cur["close"]) else 0
                    if int(cur["label_h1"]) != int(expected_h1):
                        label_h1_mismatch += 1
                if enable_h5_check and cur["ok_close"] and nxt5["ok_close"] and cur["ok_h5"]:
                    expected_h5 = 1 if float(nxt5["close"]) > float(cur["close"]) else 0
                    if int(cur["label_h5"]) != int(expected_h5):
                        label_h5_mismatch += 1
                if enable_edge_check and cur["ok_close"] and nxt5["ok_close"] and cur["ok_edge"]:
                    c0 = float(cur["close"])
                    c5 = float(nxt5["close"])
                    row_cost_bps = resolve_label_cost_bps(
                        roundtrip_cost_bps=float(roundtrip_cost_bps),
                        cost_model_config=cost_model_config,
                        atr_pct_14=float(cur["atr_pct_14"]),
                        bb_width_20=float(cur["bb_width_20"]),
                        vol_ratio_20=float(cur["vol_ratio_20"]),
                        notional_ratio_20=float(cur["notional_ratio_20"]),
                    )
                    expected_edge = round6((((c5 / c0) - 1.0) * 10000.0) - float(row_cost_bps))
                    if not math.isfinite(expected_edge) or abs(float(cur["edge"]) - expected_edge) > float(edge_tolerance):
                        edge_h5_mismatch += 1
                window.popleft()

    unverifiable_tail_rows = len(window)
    missing_columns = [c for c in expected_header if c not in header]
    extra_columns = [c for c in header if c not in expected_header]
    header_mismatch = list(header) != list(expected_header)

    numeric_invalid_total = int(sum(invalid_numeric_counts.values()))
    label_invalid_total = int(sum(invalid_label_counts.values()))
    edge_mean = (edge_sum / float(row_count)) if row_count > 0 else math.nan
    h1_pos_rate = (
        float(label_positive_counts.get("label_up_h1", 0)) / float(row_count)
        if row_count > 0 and "label_up_h1" in label_columns
        else math.nan
    )
    h5_pos_rate = (
        float(label_positive_counts.get("label_up_h5", 0)) / float(row_count)
        if row_count > 0 and "label_up_h5" in label_columns
        else math.nan
    )

    checks = {
        "schema_missing_columns_zero": len(missing_columns) == 0,
        "schema_header_exact_match": not header_mismatch,
        "forbidden_fields_absent": len(forbidden_present) == 0,
        "timestamp_order_violations_zero": ts_order_violations == 0,
        "timestamp_parse_errors_zero": malformed_timestamp_count == 0,
        "market_mismatch_zero": market_mismatch_count == 0,
        "invalid_numeric_zero": numeric_invalid_total == 0,
        "invalid_label_zero": label_invalid_total == 0,
        "label_h1_leakage_mismatch_zero": (not enable_h1_check) or (label_h1_mismatch == 0),
        "label_h5_leakage_mismatch_zero": (not enable_h5_check) or (label_h5_mismatch == 0),
        "edge_h5_leakage_mismatch_zero": (not enable_edge_check) or (edge_h5_mismatch == 0),
    }
    passed = all(bool(v) for v in checks.values())

    errors = []
    if len(missing_columns) > 0:
        errors.append(f"missing_columns={len(missing_columns)}")
    if header_mismatch:
        errors.append("header_mismatch")
    if len(forbidden_present) > 0:
        errors.append(f"forbidden_present={','.join(forbidden_present)}")
    if ts_order_violations > 0:
        errors.append(f"ts_order_violations={ts_order_violations}")
    if malformed_timestamp_count > 0:
        errors.append(f"ts_parse_errors={malformed_timestamp_count}")
    if market_mismatch_count > 0:
        errors.append(f"market_mismatch={market_mismatch_count}")
    if numeric_invalid_total > 0:
        errors.append(f"invalid_numeric_total={numeric_invalid_total}")
    if label_invalid_total > 0:
        errors.append(f"invalid_label_total={label_invalid_total}")
    if enable_h1_check and label_h1_mismatch > 0:
        errors.append(f"label_h1_mismatch={label_h1_mismatch}")
    if enable_h5_check and label_h5_mismatch > 0:
        errors.append(f"label_h5_mismatch={label_h5_mismatch}")
    if enable_edge_check and edge_h5_mismatch > 0:
        errors.append(f"edge_h5_mismatch={edge_h5_mismatch}")

    return {
        "file": str(path_value),
        "market": expected_market,
        "status": "checked",
        "pass": bool(passed),
        "row_count": int(row_count),
        "schema": {
            "header_columns": int(len(header)),
            "missing_columns": missing_columns,
            "extra_columns": extra_columns,
            "forbidden_present": forbidden_present,
            "header_mismatch": bool(header_mismatch),
        },
        "quality": {
            "timestamp_order_violations": int(ts_order_violations),
            "malformed_timestamp_count": int(malformed_timestamp_count),
            "market_mismatch_count": int(market_mismatch_count),
            "invalid_numeric_counts": invalid_numeric_counts,
            "invalid_label_counts": invalid_label_counts,
            "label_up_h1_positive_rate": h1_pos_rate,
            "label_up_h5_positive_rate": h5_pos_rate,
            "label_edge_bps_h5_min": edge_min if math.isfinite(edge_min) else math.nan,
            "label_edge_bps_h5_max": edge_max if math.isfinite(edge_max) else math.nan,
            "label_edge_bps_h5_mean": edge_mean,
        },
        "leakage_consistency": {
            "checks_enabled": {
                "label_h1": bool(enable_h1_check),
                "label_h5": bool(enable_h5_check),
                "edge_h5": bool(enable_edge_check),
            },
            "checked_rows_for_horizon": int(max(0, row_count - max_h)),
            "unverifiable_tail_rows": int(unverifiable_tail_rows),
            "label_h1_mismatch_count": int(label_h1_mismatch),
            "label_h5_mismatch_count": int(label_h5_mismatch),
            "edge_h5_mismatch_count": int(edge_h5_mismatch),
        },
        "checks": checks,
        "errors": errors,
    }


def normalize_dataset_manifest_path(path_value: pathlib.Path) -> pathlib.Path:
    if path_value.is_dir():
        return path_value / "feature_dataset_manifest.json"
    return path_value


def main(argv=None) -> int:
    args = parse_args(argv)
    dataset_manifest_path = normalize_dataset_manifest_path(resolve_repo_path(args.dataset_manifest_json))
    contract_path = resolve_repo_path(args.contract_json)
    output_path = resolve_repo_path(args.output_json)

    manifest = load_json_or_none(dataset_manifest_path)
    if not isinstance(manifest, dict):
        raise RuntimeError(f"invalid dataset manifest: {dataset_manifest_path}")

    contract = load_json_or_none(contract_path)
    if not isinstance(contract, dict):
        raise RuntimeError(f"invalid feature contract: {contract_path}")

    requested_pipeline = str(args.pipeline_version).strip().lower()
    manifest_pipeline = infer_pipeline_from_manifest(manifest)
    contract_pipeline = infer_pipeline_from_contract(contract)
    resolved_pipeline = requested_pipeline if requested_pipeline in ("v1", "v2") else manifest_pipeline
    gate_profile = "v2_strict" if resolved_pipeline == "v2" else "v1"

    expected_dataset_version = PIPELINE_TO_DATASET_VERSION[resolved_pipeline]
    expected_contract_version = PIPELINE_TO_CONTRACT_VERSION[resolved_pipeline]

    manifest_version = str(manifest.get("version", "")).strip().lower()
    manifest_feature_contract_version = str(manifest.get("feature_contract_version", "")).strip().lower()
    contract_version = str(contract.get("version", "")).strip().lower()
    row_schema = contract.get("row_schema", {})
    if not isinstance(row_schema, dict):
        row_schema = {}
    contract_header = normalize_nonempty_str_list(row_schema.get("column_order", []))
    expected_header = list(contract_header) if contract_header else list(BASELINE_COLUMN_ORDER)

    contract_column_count_raw = row_schema.get("column_count", 0)
    try:
        contract_column_count = int(float(contract_column_count_raw))
    except Exception:
        contract_column_count = 0

    preflight_checks = {
        "requested_pipeline_matches_manifest": (
            requested_pipeline == "auto" or requested_pipeline == manifest_pipeline
        ),
        "manifest_version_matches_pipeline": str(manifest_version) == str(expected_dataset_version),
        "contract_pipeline_matches_resolved_pipeline": str(contract_pipeline) == str(resolved_pipeline),
        "contract_version_matches_pipeline": str(contract_version) == str(expected_contract_version),
        "contract_row_schema_non_empty": len(contract_header) > 0,
        "contract_column_count_consistent": (
            (contract_column_count <= 0) or (contract_column_count == len(contract_header))
        ),
        "manifest_contract_tag_matches_pipeline": (
            manifest_feature_contract_version in ("", "v1")
            if resolved_pipeline == "v1"
            else (manifest_feature_contract_version == "v2_draft")
        ),
    }
    preflight_errors = [key for key, value in preflight_checks.items() if not bool(value)]

    forbidden_fields = [
        str(x).strip()
        for x in (contract.get("leakage_rules", {}).get("forbidden_fields", []) or [])
        if str(x).strip()
    ]

    jobs = manifest.get("jobs", [])
    if not isinstance(jobs, list):
        jobs = []

    roundtrip_cost_bps = float(manifest.get("roundtrip_cost_bps", 12.0) or 12.0)
    cost_model_config = manifest.get("cost_model", {})
    if not isinstance(cost_model_config, dict):
        cost_model_config = {"enabled": False}

    numeric_columns = [c for c in BASELINE_NUMERIC_COLUMNS if c in set(expected_header)]
    label_columns = [c for c in BASELINE_LABEL_COLUMNS if c in set(expected_header)]

    file_results: List[Dict[str, Any]] = []
    failed_files = []
    total_rows = 0

    print("[ValidateProbabilisticFeatureDataset] start", flush=True)
    print(f"manifest={dataset_manifest_path}", flush=True)
    print(f"contract={contract_path}", flush=True)
    print(f"pipeline_version={resolved_pipeline}", flush=True)
    print(f"gate_profile={gate_profile}", flush=True)
    print(f"jobs={len(jobs)}", flush=True)

    for idx, job in enumerate(jobs, start=1):
        if not isinstance(job, dict):
            continue
        if str(job.get("status", "")).strip().lower() != "built":
            continue
        market = str(job.get("market", "")).strip()
        output_file = str(job.get("output_path", "")).strip()
        if not market or not output_file:
            continue
        file_path = pathlib.Path(output_file)
        print(f"[{idx}] checking {market} -> {file_path.name}", flush=True)

        result = validate_single_file(
            path_value=file_path,
            expected_market=market,
            roundtrip_cost_bps=roundtrip_cost_bps,
            cost_model_config=cost_model_config,
            forbidden_fields=forbidden_fields,
            edge_tolerance=float(args.edge_tolerance),
            expected_header=expected_header,
            numeric_columns=numeric_columns,
            label_columns=label_columns,
        )
        file_results.append(result)
        total_rows += int(result.get("row_count", 0) or 0)
        if not bool(result.get("pass", False)):
            failed_files.append({"market": market, "file": str(file_path), "errors": result.get("errors", [])})
            print(f"[{idx}] fail {market} errors={result.get('errors', [])}", flush=True)
        else:
            print(f"[{idx}] pass {market} rows={result.get('row_count', 0)}", flush=True)

    checks_total = len(file_results)
    checks_passed = sum(1 for x in file_results if bool(x.get("pass", False)))
    overall_pass = bool(
        len(preflight_errors) == 0 and checks_total > 0 and checks_total == checks_passed
    )
    summary = {
        "generated_at_utc": utc_now_iso(),
        "status": "pass" if overall_pass else "fail",
        "gate_profile": gate_profile,
        "pipeline_version": resolved_pipeline,
        "manifest_pipeline_version": manifest_pipeline,
        "contract_pipeline_version": contract_pipeline,
        "requested_pipeline_version": requested_pipeline,
        "dataset_manifest_json": str(dataset_manifest_path),
        "feature_contract_json": str(contract_path),
        "roundtrip_cost_bps": roundtrip_cost_bps,
        "cost_model": cost_model_config,
        "expected_dataset_version": expected_dataset_version,
        "observed_dataset_version": manifest_version,
        "expected_contract_version": expected_contract_version,
        "observed_contract_version": contract_version,
        "observed_manifest_feature_contract_version": manifest_feature_contract_version,
        "expected_header_columns": expected_header,
        "expected_header_count": len(expected_header),
        "preflight_checks": preflight_checks,
        "preflight_errors": preflight_errors,
        "file_count_checked": int(checks_total),
        "file_count_passed": int(checks_passed),
        "file_count_failed": int(max(0, checks_total - checks_passed)),
        "total_rows_checked": int(total_rows),
        "edge_tolerance": float(args.edge_tolerance),
        "files": file_results,
        "failed_files": failed_files,
    }
    dump_json(output_path, summary)

    print("[ValidateProbabilisticFeatureDataset] completed", flush=True)
    print(f"status={summary['status']}", flush=True)
    if preflight_errors:
        print(f"preflight_errors={','.join(preflight_errors)}", flush=True)
    print(f"checked={checks_total} passed={checks_passed} failed={summary['file_count_failed']}", flush=True)
    print(f"total_rows_checked={total_rows}", flush=True)
    print(f"output={output_path}", flush=True)

    if bool(args.strict) and summary["status"] != "pass":
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
