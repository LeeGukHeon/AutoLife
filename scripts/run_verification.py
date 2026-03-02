#!/usr/bin/env python3
import argparse
import bisect
from collections import Counter, deque
import copy
import csv
import hashlib
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import uuid
from typing import Any, Dict, List, Optional, Tuple

from _script_common import verification_lock


VOL_BUCKET_LOW = "LOW"
VOL_BUCKET_MID = "MID"
VOL_BUCKET_HIGH = "HIGH"
VOL_BUCKET_NONE = "NONE"
VOL_BUCKET_ORDER = [VOL_BUCKET_LOW, VOL_BUCKET_MID, VOL_BUCKET_HIGH, VOL_BUCKET_NONE]
VNEXT_POLICY_DECISION_ARTIFACT = "vnext_policy_decisions_backtest.jsonl"


def resolve_policy_decision_artifact_path(
    exe_dir: pathlib.Path,
    run_dir: Optional[pathlib.Path] = None,
) -> pathlib.Path:
    return (resolve_runtime_logs_dir(exe_dir, run_dir) / VNEXT_POLICY_DECISION_ARTIFACT).resolve()


def resolve_path(path_value: str, label: str, must_exist: bool = True) -> pathlib.Path:
    path_obj = pathlib.Path(path_value)
    if not path_obj.is_absolute():
        path_obj = (pathlib.Path.cwd() / path_obj).resolve()
    if must_exist and not path_obj.exists():
        raise FileNotFoundError(f"{label} not found: {path_obj}")
    return path_obj


def ensure_parent(path_value: pathlib.Path) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)


def is_upbit_primary_1m_dataset(dataset_path: pathlib.Path) -> bool:
    stem = dataset_path.stem.lower()
    return stem.startswith("upbit_") and "_1m_" in stem


def has_higher_tf_companions(data_dir: pathlib.Path, primary_1m_dataset: pathlib.Path) -> bool:
    if not is_upbit_primary_1m_dataset(primary_1m_dataset):
        return False
    prefix = primary_1m_dataset.stem.split("_1m_", 1)[0]
    for tf in ("5m", "15m", "60m", "240m"):
        if not any(data_dir.glob(f"{prefix}_{tf}_*.csv")):
            return False
    return True


def extract_upbit_market_from_dataset_name(dataset_name: str) -> str:
    stem = pathlib.Path(dataset_name).stem
    match = re.match(r"^upbit_([A-Za-z0-9]+_[A-Za-z0-9]+)_1m_", stem)
    if not match:
        return ""
    token = str(match.group(1)).strip().upper()
    if "_" not in token:
        return ""
    return token.replace("_", "-")


def normalize_timestamp_ms(ts_value: int) -> int:
    # Keep parity with BacktestRuntime::normalizeTimestampMs.
    if int(ts_value) > 0 and int(ts_value) < 1000000000000:
        return int(ts_value) * 1000
    return int(ts_value)


def parse_timestamp_cell(cell_value: str) -> Optional[int]:
    token = str(cell_value or "").strip().strip('"')
    if not token:
        return None
    try:
        if any(ch in token for ch in (".", "e", "E")):
            return int(float(token))
        return int(token)
    except Exception:
        return None


def canonical_split_name(split_name: str) -> str:
    token = str(split_name or "").strip().lower()
    alias = {
        "dev": "development",
        "development": "development",
        "val": "validation",
        "validation": "validation",
        "qua": "quarantine",
        "quarantine": "quarantine",
        "all": "all_purged",
        "all_purged": "all_purged",
    }
    return alias.get(token, token)


def resolve_manifest_split_time_range(
    split_manifest_payload: Dict[str, Any],
    split_name: str,
    execution_prewarm_hours: float = 168.0,
) -> Dict[str, Any]:
    token = canonical_split_name(split_name)
    time_bounds = (
        split_manifest_payload.get("time_bounds", {})
        if isinstance(split_manifest_payload, dict)
        else {}
    )
    if not isinstance(time_bounds, dict):
        time_bounds = {}

    def _iv(key: str) -> int:
        try:
            return int(time_bounds.get(key, 0))
        except Exception:
            return 0

    eval_start = 0
    eval_end = 0
    eval_mode = ""
    cumulative_start = 0
    cumulative_end = 0
    execution_start = 0
    execution_end = 0
    execution_mode = ""
    source_keys: List[str] = []
    prewarm_ms = max(0, int(round(float(max(0.0, execution_prewarm_hours)) * 3600.0 * 1000.0)))

    if token in ("development", "validation", "quarantine"):
        intersection_start = _iv("intersection_start_ts")
        core_start_key = f"{token}_core_start_ts"
        core_end_key = f"{token}_core_end_ts"
        core_start = _iv(core_start_key)
        core_end = _iv(core_end_key)
        if token == "development":
            cumulative_end = _iv("development_end_ts")
            cumulative_end_key = "development_end_ts"
        elif token == "validation":
            cumulative_end = _iv("validation_end_ts")
            cumulative_end_key = "validation_end_ts"
        else:
            cumulative_end = _iv("intersection_end_ts")
            cumulative_end_key = "intersection_end_ts"
        cumulative_start = intersection_start
        if core_start > 0 and core_end > 0 and core_end >= core_start:
            eval_start = core_start
            eval_end = core_end
            eval_mode = "core"
            source_keys = [core_start_key, core_end_key, "intersection_start_ts", cumulative_end_key]
            execution_start = max(cumulative_start, eval_start - prewarm_ms) if cumulative_start > 0 else max(0, eval_start - prewarm_ms)
            execution_end = eval_end
            execution_mode = "prewarm_plus_core"
        else:
            if cumulative_start > 0 and cumulative_end > 0 and cumulative_end >= cumulative_start:
                eval_start = cumulative_start
                eval_end = cumulative_end
                eval_mode = "prefix_cumulative"
                source_keys = ["intersection_start_ts", cumulative_end_key]
                execution_start = eval_start
                execution_end = eval_end
                execution_mode = "prefix_cumulative"
    elif token == "all_purged":
        start = _iv("intersection_start_ts")
        end = _iv("intersection_end_ts")
        if start > 0 and end > 0 and end >= start:
            eval_start = start
            eval_end = end
            eval_mode = "all_purged"
            cumulative_start = start
            cumulative_end = end
            execution_start = start
            execution_end = end
            execution_mode = "all_purged"
            source_keys = ["intersection_start_ts", "intersection_end_ts"]

    if eval_start > 0 and eval_end > 0 and eval_end >= eval_start and execution_start > 0 and execution_end > 0 and execution_end >= execution_start:
        return {
            "resolved": True,
            "split_name": token,
            "evaluation_start_ts": int(eval_start),
            "evaluation_end_ts": int(eval_end),
            "evaluation_mode": str(eval_mode),
            "execution_start_ts": int(execution_start),
            "execution_end_ts": int(execution_end),
            "execution_mode": str(execution_mode),
            "cumulative_start_ts": int(cumulative_start),
            "cumulative_end_ts": int(cumulative_end),
            "execution_prewarm_hours": float(max(0.0, execution_prewarm_hours)),
            "source_keys": source_keys,
        }

    return {
        "resolved": False,
        "split_name": token,
        "evaluation_start_ts": 0,
        "evaluation_end_ts": 0,
        "evaluation_mode": "unresolved",
        "execution_start_ts": 0,
        "execution_end_ts": 0,
        "execution_mode": "unresolved",
        "cumulative_start_ts": 0,
        "cumulative_end_ts": 0,
        "execution_prewarm_hours": float(max(0.0, execution_prewarm_hours)),
        "source_keys": source_keys,
        "reason": "manifest_time_bounds_missing_for_split",
    }


def collect_market_family_csvs(primary_1m_dataset: pathlib.Path) -> List[pathlib.Path]:
    dataset = primary_1m_dataset.resolve()
    if not is_upbit_primary_1m_dataset(dataset):
        return [dataset]
    parent = dataset.parent
    prefix = dataset.stem.split("_1m_", 1)[0].lower() + "_"
    family: List[pathlib.Path] = []
    for candidate in sorted(parent.glob("*.csv"), key=lambda p: p.name.lower()):
        if candidate.stem.lower().startswith(prefix):
            family.append(candidate.resolve())
    if not family:
        family = [dataset]
    return family


def filter_csv_by_time_range(
    source_csv: pathlib.Path,
    output_csv: pathlib.Path,
    start_ts_ms: int,
    end_ts_ms: int,
    evaluation_start_ts_ms: Optional[int] = None,
    evaluation_end_ts_ms: Optional[int] = None,
) -> Dict[str, Any]:
    rows_before = 0
    rows_after = 0
    rows_in_evaluation = 0
    skipped_non_numeric = 0
    header_written = False

    eval_enabled = (
        evaluation_start_ts_ms is not None
        and evaluation_end_ts_ms is not None
        and int(evaluation_end_ts_ms) >= int(evaluation_start_ts_ms)
    )

    ensure_parent(output_csv)
    with source_csv.open("r", encoding="utf-8", errors="ignore", newline="") as src_fp:
        with output_csv.open("w", encoding="utf-8", newline="\n") as out_fp:
            for raw_line in src_fp:
                line = str(raw_line).rstrip("\r\n")
                if not line.strip():
                    continue
                first_cell = line.split(",", 1)[0]
                ts_raw = parse_timestamp_cell(first_cell)
                if ts_raw is None:
                    skipped_non_numeric += 1
                    if not header_written:
                        out_fp.write(line + "\n")
                        header_written = True
                    continue
                rows_before += 1
                ts_ms = normalize_timestamp_ms(ts_raw)
                if int(start_ts_ms) <= int(ts_ms) <= int(end_ts_ms):
                    out_fp.write(line + "\n")
                    rows_after += 1
                    if eval_enabled and int(evaluation_start_ts_ms) <= int(ts_ms) <= int(evaluation_end_ts_ms):
                        rows_in_evaluation += 1

    return {
        "source_csv": str(source_csv),
        "filtered_csv": str(output_csv),
        "rows_before_filter": int(rows_before),
        "rows_after_filter": int(rows_after),
        "rows_in_evaluation_range": int(rows_in_evaluation),
        "rows_removed_by_filter": max(0, int(rows_before) - int(rows_after)),
        "rows_skipped_non_numeric": int(skipped_non_numeric),
        "header_written": bool(header_written),
    }


def apply_manifest_split_filter_to_dataset_paths(
    dataset_paths: List[pathlib.Path],
    split_manifest_payload: Dict[str, Any],
    split_name: str,
    logs_dir: pathlib.Path,
    execution_prewarm_hours: float = 168.0,
) -> Dict[str, Any]:
    range_spec = resolve_manifest_split_time_range(
        split_manifest_payload=split_manifest_payload,
        split_name=split_name,
        execution_prewarm_hours=float(max(0.0, execution_prewarm_hours)),
    )
    if not bool(range_spec.get("resolved", False)):
        reason = str(range_spec.get("reason", "split_range_unresolved")).strip() or "split_range_unresolved"
        raise RuntimeError(
            f"Split manifest is provided but split range could not be resolved: split={split_name} reason={reason}"
        )

    execution_start_ts = int(range_spec.get("execution_start_ts", 0))
    execution_end_ts = int(range_spec.get("execution_end_ts", 0))
    evaluation_start_ts = int(range_spec.get("evaluation_start_ts", 0))
    evaluation_end_ts = int(range_spec.get("evaluation_end_ts", 0))
    cumulative_start_ts = int(range_spec.get("cumulative_start_ts", 0))
    cumulative_end_ts = int(range_spec.get("cumulative_end_ts", 0))
    split_token = str(range_spec.get("split_name", "")).strip().lower() or "unknown"
    nonce = uuid.uuid4().hex[:10]
    temp_root = (
        logs_dir
        / "_split_filtered_inputs"
        / f"{split_token}_{execution_start_ts}_{execution_end_ts}_{nonce}"
    ).resolve()
    temp_root.mkdir(parents=True, exist_ok=True)

    filtered_primary_dataset_paths: List[pathlib.Path] = []
    per_primary: List[Dict[str, Any]] = []
    rows_before_total = 0
    rows_after_total = 0
    rows_in_evaluation_total = 0
    rows_before_primary_1m = 0
    rows_after_primary_1m = 0
    rows_in_evaluation_primary_1m = 0

    for primary in dataset_paths:
        family_files = collect_market_family_csvs(primary)
        file_rows: List[Dict[str, Any]] = []
        for source_csv in family_files:
            filtered_csv = (temp_root / source_csv.name).resolve()
            stats = filter_csv_by_time_range(
                source_csv=source_csv,
                output_csv=filtered_csv,
                start_ts_ms=execution_start_ts,
                end_ts_ms=execution_end_ts,
                evaluation_start_ts_ms=evaluation_start_ts,
                evaluation_end_ts_ms=evaluation_end_ts,
            )
            file_rows.append(stats)
            rows_before_total += int(stats.get("rows_before_filter", 0))
            rows_after_total += int(stats.get("rows_after_filter", 0))
            rows_in_evaluation_total += int(stats.get("rows_in_evaluation_range", 0))
            if "_1m_" in source_csv.stem.lower():
                rows_before_primary_1m += int(stats.get("rows_before_filter", 0))
                rows_after_primary_1m += int(stats.get("rows_after_filter", 0))
                rows_in_evaluation_primary_1m += int(stats.get("rows_in_evaluation_range", 0))

        filtered_primary = (temp_root / primary.name).resolve()
        if not filtered_primary.exists():
            raise FileNotFoundError(f"Filtered primary dataset missing: {filtered_primary}")
        filtered_primary_dataset_paths.append(filtered_primary)
        per_primary.append(
            {
                "primary_dataset": str(primary),
                "filtered_primary_dataset": str(filtered_primary),
                "family_file_count": int(len(family_files)),
                "files": file_rows,
            }
        )

    return {
        "split_applied": True,
        "split_name": split_token,
        "execution_range_used": [int(execution_start_ts), int(execution_end_ts)],
        "evaluation_range_used": [int(evaluation_start_ts), int(evaluation_end_ts)],
        "cumulative_range_used": [int(cumulative_start_ts), int(cumulative_end_ts)],
        "split_range_used": [int(evaluation_start_ts), int(evaluation_end_ts)],
        "split_range_mode": str(range_spec.get("evaluation_mode", "")),
        "execution_range_mode": str(range_spec.get("execution_mode", "")),
        "execution_prewarm_hours": float(range_spec.get("execution_prewarm_hours", 0.0)),
        "split_range_source_keys": list(range_spec.get("source_keys", [])),
        "rows_before_filter": int(rows_before_total),
        "rows_after_filter": int(rows_after_total),
        "rows_in_core": int(rows_in_evaluation_total),
        "rows_before_filter_primary_1m": int(rows_before_primary_1m),
        "rows_after_filter_primary_1m": int(rows_after_primary_1m),
        "rows_in_core_primary_1m": int(rows_in_evaluation_primary_1m),
        "trades_before_filter": None,
        "trades_after_filter": None,
        "filtered_input_root_dir": str(temp_root),
        "per_primary_dataset": per_primary,
        "filtered_dataset_paths": [str(x) for x in filtered_primary_dataset_paths],
    }


def extract_phase4_market_cluster_map(bundle_payload: Dict[str, Any]) -> Dict[str, str]:
    phase4 = bundle_payload.get("phase4", {})
    if not isinstance(phase4, dict):
        return {}
    correlation = phase4.get("correlation_control", {})
    if not isinstance(correlation, dict):
        return {}
    raw_map = correlation.get("market_cluster_map", {})
    if not isinstance(raw_map, dict):
        return {}
    out: Dict[str, str] = {}
    for key, value in raw_map.items():
        market = str(key).strip().upper()
        cluster = str(value).strip()
        if market and cluster:
            out[market] = cluster
    return out


def extract_risk_adjusted_score_policy(bundle_payload: Dict[str, Any]) -> Dict[str, Any]:
    phase3 = bundle_payload.get("phase3", {})
    if not isinstance(phase3, dict):
        return {}
    gate = phase3.get("operations_dynamic_gate", {})
    if not isinstance(gate, dict):
        return {}
    policy = gate.get("risk_adjusted_score_policy", {})
    if not isinstance(policy, dict):
        return {}
    out: Dict[str, Any] = {}
    for key, value in policy.items():
        token = str(key).strip()
        if not token:
            continue
        if isinstance(value, (int, float, bool, str)):
            out[token] = value
    return out


def extract_risk_adjusted_score_guard_policy(bundle_payload: Dict[str, Any]) -> Dict[str, Any]:
    phase3 = bundle_payload.get("phase3", {})
    if not isinstance(phase3, dict):
        return {}
    gate = phase3.get("operations_dynamic_gate", {})
    if not isinstance(gate, dict):
        return {}
    policy = gate.get("risk_adjusted_score_guard", {})
    if not isinstance(policy, dict):
        return {}
    # Keep nested policy payload intact (lookup/dist/uncertainty blocks are nested).
    return copy.deepcopy(policy)


def load_supported_markets_from_runtime_bundle(bundle_path: pathlib.Path) -> Dict[str, Any]:
    with bundle_path.open("r", encoding="utf-8") as fp:
        payload = json.load(fp)
    bundle_version = str(payload.get("version", "")).strip()
    expected_pipeline = "v2"
    if bundle_version != "probabilistic_runtime_bundle_v2_draft":
        raise RuntimeError(f"Unsupported runtime bundle version for verification: {bundle_version or 'missing'}")
    declared_pipeline = str(payload.get("pipeline_version", expected_pipeline)).strip().lower() or expected_pipeline
    if declared_pipeline != expected_pipeline:
        raise RuntimeError(
            "Runtime bundle pipeline mismatch: "
            f"version={bundle_version or 'unknown'} pipeline_version={declared_pipeline}"
        )
    feature_contract_version = str(payload.get("feature_contract_version", "")).strip().lower()
    runtime_bundle_contract_version = str(payload.get("runtime_bundle_contract_version", "")).strip().lower()
    if feature_contract_version != "v2_draft":
        raise RuntimeError(
            "Runtime bundle contract mismatch for verification: "
            f"feature_contract_version={feature_contract_version or 'missing'}"
        )
    if runtime_bundle_contract_version != "v2_draft":
        raise RuntimeError(
            "Runtime bundle contract mismatch for verification: "
            f"runtime_bundle_contract_version={runtime_bundle_contract_version or 'missing'}"
        )
    markets_field = payload.get("markets", [])
    supported_markets: set[str] = set()
    if isinstance(markets_field, list):
        for item in markets_field:
            if not isinstance(item, dict):
                continue
            market = str(item.get("market", "")).strip().upper()
            if market:
                supported_markets.add(market)
    default_model = payload.get("default_model", None)
    global_fallback_enabled = bool(payload.get("global_fallback_enabled", False))
    if isinstance(default_model, dict) and default_model:
        global_fallback_enabled = True
    edge_semantics = str(payload.get("edge_semantics", "net")).strip().lower()
    if edge_semantics not in ("net", "gross"):
        edge_semantics = "net"
    prob_model_backend = str(payload.get("prob_model_backend", "sgd")).strip().lower()
    if prob_model_backend not in ("sgd", "lgbm"):
        prob_model_backend = "sgd"
    lgbm_model_path = str(payload.get("lgbm_model_path", "")).strip()
    lgbm_model_sha256 = str(payload.get("lgbm_model_sha256", "")).strip().lower()
    lgbm_h5_calibration = payload.get("lgbm_h5_calibration", {})
    if not isinstance(lgbm_h5_calibration, dict):
        lgbm_h5_calibration = {}
    lgbm_h5_calibration_a = to_float(lgbm_h5_calibration.get("a", float("nan")))
    lgbm_h5_calibration_b = to_float(lgbm_h5_calibration.get("b", float("nan")))
    has_lgbm_platt = math.isfinite(lgbm_h5_calibration_a) and math.isfinite(lgbm_h5_calibration_b)
    lgbm_calibration_mode = "platt" if has_lgbm_platt else "off"
    lgbm_ev_affine = payload.get("lgbm_ev_affine", {})
    if not isinstance(lgbm_ev_affine, dict):
        lgbm_ev_affine = {}
    lgbm_ev_affine_enabled = bool(lgbm_ev_affine.get("enabled", False))
    lgbm_ev_affine_scale = to_float(lgbm_ev_affine.get("scale", 1.0))
    lgbm_ev_affine_shift = to_float(lgbm_ev_affine.get("shift", 0.0))
    if not math.isfinite(lgbm_ev_affine_scale):
        lgbm_ev_affine_scale = 1.0
    if not math.isfinite(lgbm_ev_affine_shift):
        lgbm_ev_affine_shift = 0.0
    root_cost_model = payload.get("cost_model", {})
    if not isinstance(root_cost_model, dict):
        root_cost_model = {}
    phase3 = payload.get("phase3", {})
    if not isinstance(phase3, dict):
        phase3 = {}
    phase3_cost_model = phase3.get("cost_model", {})
    if not isinstance(phase3_cost_model, dict):
        phase3_cost_model = {}
    return {
        "bundle_path": str(bundle_path),
        "bundle_version": bundle_version,
        "pipeline_version": expected_pipeline,
        "supported_markets": sorted(list(supported_markets)),
        "global_fallback_enabled": bool(global_fallback_enabled),
        "export_mode": str(payload.get("export_mode", "")).strip(),
        "edge_semantics": edge_semantics,
        "prob_model_backend": prob_model_backend,
        "lgbm_model_path": lgbm_model_path,
        "lgbm_model_sha256": lgbm_model_sha256,
        "lgbm_calibration_mode": lgbm_calibration_mode,
        "lgbm_h5_calibration": {
            "a": float(lgbm_h5_calibration_a) if has_lgbm_platt else None,
            "b": float(lgbm_h5_calibration_b) if has_lgbm_platt else None,
        },
        "lgbm_ev_affine": {
            "enabled": lgbm_ev_affine_enabled,
            "scale": float(lgbm_ev_affine_scale),
            "shift": float(lgbm_ev_affine_shift),
        },
        "root_cost_model_enabled_configured": bool(root_cost_model.get("enabled", False)),
        "phase3_cost_model_enabled_configured": bool(phase3_cost_model.get("enabled", False)),
        "phase4_market_cluster_map": extract_phase4_market_cluster_map(payload),
        "risk_adjusted_score_policy": extract_risk_adjusted_score_policy(payload),
        "risk_adjusted_score_guard_policy": extract_risk_adjusted_score_guard_policy(payload),
    }


def resolve_verification_pipeline_version(
    requested_pipeline_version: str,
    bundle_meta: Dict[str, Any],
) -> str:
    requested = str(requested_pipeline_version or "auto").strip().lower()
    bundle_pipeline = str(bundle_meta.get("pipeline_version", "")).strip().lower()
    if requested == "v2":
        if bundle_pipeline and bundle_pipeline != requested:
            raise RuntimeError(
                "Verification pipeline version mismatches runtime bundle: "
                f"requested={requested} bundle={bundle_pipeline}"
            )
        return requested
    if requested not in ("", "auto"):
        raise RuntimeError(f"Unsupported verification pipeline version: {requested}")
    if bundle_pipeline == "v2":
        return bundle_pipeline
    return "v2"


def resolve_probabilistic_bundle_path(exe_path: pathlib.Path, config_payload: Dict[str, Any]) -> pathlib.Path:
    trading_cfg = config_payload.get("trading", {})
    if not isinstance(trading_cfg, dict):
        return pathlib.Path("")
    if not bool(trading_cfg.get("enable_probabilistic_runtime_model", True)):
        return pathlib.Path("")
    if not bool(trading_cfg.get("probabilistic_runtime_primary_mode", True)):
        return pathlib.Path("")
    raw_path = str(trading_cfg.get("probabilistic_runtime_bundle_path", "")).strip()
    if not raw_path:
        return pathlib.Path("")
    bundle_path = pathlib.Path(raw_path)
    if not bundle_path.is_absolute():
        bundle_path = (exe_path.parent / bundle_path).resolve()
    return bundle_path


def build_verification_row(dataset_name: str, result: Dict[str, Any]) -> Dict[str, Any]:
    row = {
        "dataset": str(dataset_name).strip(),
        "total_profit_krw": round(float(result.get("total_profit", 0.0)), 4),
        "profit_factor": round(float(result.get("profit_factor", 0.0)), 4),
        "expectancy_krw": round(float(result.get("expectancy_krw", 0.0)), 4),
        "max_drawdown_pct": round(float(result.get("max_drawdown", 0.0)) * 100.0, 4),
        "total_trades": int(result.get("total_trades", 0)),
        "win_rate_pct": round(float(result.get("win_rate", 0.0)) * 100.0, 4),
        "avg_fee_krw": round(float(result.get("avg_fee_krw", 0.0)), 4),
    }
    row["profitable"] = bool(float(row["total_profit_krw"]) > 0.0)
    return row


def summarize_verification_rows(rows: List[Dict[str, Any]]) -> Dict[str, float]:
    profits = [float(x.get("total_profit_krw", 0.0)) for x in rows]
    profit_factors = [float(x.get("profit_factor", 0.0)) for x in rows]
    expectancies = [float(x.get("expectancy_krw", 0.0)) for x in rows]
    drawdowns = [float(x.get("max_drawdown_pct", 0.0)) for x in rows]
    win_rates = [float(x.get("win_rate_pct", 0.0)) for x in rows]
    avg_fees = [float(x.get("avg_fee_krw", 0.0)) for x in rows]
    trades = [max(0.0, float(x.get("total_trades", 0.0))) for x in rows]
    profitable_ratio = (sum(1 for x in rows if bool(x.get("profitable", False))) / float(len(rows))) if rows else 0.0
    return {
        "avg_profit_factor": round(safe_weighted_avg(profit_factors, trades), 4),
        "avg_expectancy_krw": round(safe_weighted_avg(expectancies, trades), 4),
        "avg_win_rate_pct": round(safe_weighted_avg(win_rates, trades), 4),
        "avg_total_trades": round(safe_avg(trades), 4),
        "peak_max_drawdown_pct": round(max(drawdowns) if drawdowns else 0.0, 4),
        "profitable_ratio": round(profitable_ratio, 4),
        "total_profit_sum_krw": round(sum(profits), 4),
        "avg_fee_krw": round(safe_weighted_avg(avg_fees, trades), 4),
    }


PHASE4_POLICY_BLOCKS = (
    "portfolio_allocator",
    "correlation_control",
    "risk_budget",
    "drawdown_governor",
    "execution_aware_sizing",
    "portfolio_diagnostics",
)


def with_phase4_flags(bundle_payload: Dict[str, Any], enabled: bool) -> Dict[str, Any]:
    payload = copy.deepcopy(bundle_payload) if isinstance(bundle_payload, dict) else {}
    phase4 = payload.get("phase4", {})
    if not isinstance(phase4, dict):
        phase4 = {}
    for block_name in PHASE4_POLICY_BLOCKS:
        block = phase4.get(block_name, {})
        if not isinstance(block, dict):
            block = {}
        block["enabled"] = bool(enabled)
        phase4[block_name] = block
    payload["phase4"] = phase4
    return payload


def aggregate_phase4_compare_diags(diags: List[Dict[str, Any]]) -> Dict[str, Any]:
    counter_keys = [
        "candidates_total",
        "selected_total",
        "rejected_by_budget",
        "rejected_by_cluster_cap",
        "rejected_by_correlation_penalty",
        "cluster_cap_skips_count",
        "cluster_cap_would_exceed_count",
        "cluster_exposure_update_count",
        "rejected_by_execution_cap",
        "rejected_by_drawdown_governor",
        "candidate_snapshot_total",
        "candidate_snapshot_sampled",
    ]
    counters: Dict[str, int] = {k: 0 for k in counter_keys}
    flags: Dict[str, bool] = {
        "phase4_portfolio_allocator_enabled": False,
        "phase4_correlation_control_enabled": False,
        "phase4_risk_budget_enabled": False,
        "phase4_drawdown_governor_enabled": False,
        "phase4_execution_aware_sizing_enabled": False,
        "phase4_portfolio_diagnostics_enabled": False,
    }
    for diag in diags:
        if not isinstance(diag, dict):
            continue
        raw_counters = diag.get("counters", {})
        if isinstance(raw_counters, dict):
            for key in counter_keys:
                counters[key] += max(0, to_int(raw_counters.get(key, 0)))
        raw_flags = diag.get("flags", {})
        if isinstance(raw_flags, dict):
            for key in list(flags.keys()):
                flags[key] = bool(flags[key] or bool(raw_flags.get(key, False)))

    selection_rate = round(
        float(counters.get("selected_total", 0)) / float(max(1, counters.get("candidates_total", 0))),
        6,
    )
    top3 = sorted(
        [
            {"reason": key, "count": int(value)}
            for key, value in counters.items()
            if key.startswith("rejected_") and int(value) > 0
        ],
        key=lambda item: (-int(item["count"]), str(item["reason"])),
    )[:3]
    return {
        "flags": flags,
        "counters": {**counters, "selection_rate": float(selection_rate)},
        "selection_breakdown": {
            "candidates_total": int(counters.get("candidates_total", 0)),
            "selected_total": int(counters.get("selected_total", 0)),
            "selection_rate": float(selection_rate),
            "rejected_by_budget": int(counters.get("rejected_by_budget", 0)),
            "rejected_by_cluster_cap": int(counters.get("rejected_by_cluster_cap", 0)),
            "rejected_by_correlation_penalty": int(counters.get("rejected_by_correlation_penalty", 0)),
            "cluster_cap_skips_count": int(counters.get("cluster_cap_skips_count", 0)),
            "cluster_cap_would_exceed_count": int(counters.get("cluster_cap_would_exceed_count", 0)),
            "cluster_exposure_update_count": int(counters.get("cluster_exposure_update_count", 0)),
            "rejected_by_execution_cap": int(counters.get("rejected_by_execution_cap", 0)),
            "rejected_by_drawdown_governor": int(counters.get("rejected_by_drawdown_governor", 0)),
        },
        "top3_bottlenecks": top3,
    }


def build_phase4_off_on_comparison(
    exe_path: pathlib.Path,
    dataset_paths: List[pathlib.Path],
    require_higher_tf_companions: bool,
    disable_adaptive_state_io: bool,
    config_path: pathlib.Path,
    config_payload: Dict[str, Any],
    bundle_path: pathlib.Path,
) -> Dict[str, Any]:
    output: Dict[str, Any] = {
        "available": False,
        "reason": "",
        "dataset_count": len(dataset_paths),
        "datasets": [p.name for p in dataset_paths],
    }
    bundle_text = str(bundle_path).strip()
    if not bundle_text or bundle_text in (".", ".."):
        output["reason"] = "runtime_bundle_path_missing"
        return output
    if not bundle_path.exists() or not bundle_path.is_file():
        output["reason"] = "runtime_bundle_path_missing_or_invalid"
        return output
    if not config_path.exists():
        output["reason"] = "runtime_config_missing"
        return output

    base_bundle_payload: Dict[str, Any] = {}
    try:
        with bundle_path.open("r", encoding="utf-8") as fp:
            loaded_bundle = json.load(fp)
            if isinstance(loaded_bundle, dict):
                base_bundle_payload = loaded_bundle
    except Exception:
        output["reason"] = "runtime_bundle_load_failed"
        return output
    if not base_bundle_payload:
        output["reason"] = "runtime_bundle_empty_or_invalid"
        return output

    try:
        original_config_text = config_path.read_text(encoding="utf-8")
    except Exception:
        output["reason"] = "runtime_config_load_failed"
        return output

    temp_bundle_paths: List[pathlib.Path] = []
    mode_reports: Dict[str, Dict[str, Any]] = {}
    compare_nonce = uuid.uuid4().hex[:10]
    try:
        for mode_name, enabled in (("off", False), ("on", True)):
            mode_bundle_payload = with_phase4_flags(base_bundle_payload, enabled=enabled)
            mode_bundle_path = (
                config_path.parent
                / f"probabilistic_runtime_bundle_v2_phase4_{mode_name}_{compare_nonce}.json"
            )
            temp_bundle_paths.append(mode_bundle_path)
            mode_bundle_path.write_text(
                json.dumps(mode_bundle_payload, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
                newline="\n",
            )

            mode_config_payload = copy.deepcopy(config_payload) if isinstance(config_payload, dict) else {}
            trading_cfg = mode_config_payload.get("trading", {})
            if not isinstance(trading_cfg, dict):
                trading_cfg = {}
            trading_cfg["enable_probabilistic_runtime_model"] = True
            trading_cfg["probabilistic_runtime_primary_mode"] = True
            trading_cfg["probabilistic_runtime_bundle_path"] = str(mode_bundle_path)
            mode_config_payload["trading"] = trading_cfg
            config_path.write_text(
                json.dumps(mode_config_payload, ensure_ascii=False, indent=4) + "\n",
                encoding="utf-8",
                newline="\n",
            )

            mode_rows: List[Dict[str, Any]] = []
            mode_phase4_diags: List[Dict[str, Any]] = []
            mode_cluster_map = extract_phase4_market_cluster_map(mode_bundle_payload)
            for dataset_path in dataset_paths:
                result = run_backtest(
                    exe_path=exe_path,
                    dataset_path=dataset_path,
                    config_path=config_path,
                    require_higher_tf_companions=bool(require_higher_tf_companions),
                    disable_adaptive_state_io=bool(disable_adaptive_state_io),
                )
                mode_rows.append(build_verification_row(dataset_path.name, result))
                mode_phase4_diags.append(
                    build_phase4_portfolio_diagnostics(
                        result,
                        phase4_market_cluster_map=mode_cluster_map,
                    )
                )

            mode_reports[mode_name] = {
                "aggregates": summarize_verification_rows(mode_rows),
                "rows": mode_rows,
                "phase4_portfolio_diagnostics": aggregate_phase4_compare_diags(mode_phase4_diags),
            }
    except Exception as exc:
        output["reason"] = f"phase4_off_on_compare_failed:{exc}"
    finally:
        try:
            config_path.write_text(original_config_text, encoding="utf-8", newline="\n")
        except Exception:
            pass
        for path_value in temp_bundle_paths:
            try:
                path_value.unlink(missing_ok=True)
            except Exception:
                pass

    off_report = mode_reports.get("off", {})
    on_report = mode_reports.get("on", {})
    if not off_report or not on_report:
        if not output.get("reason"):
            output["reason"] = "phase4_off_on_compare_failed_missing_mode_reports"
        return output

    off_aggregates = off_report.get("aggregates", {}) if isinstance(off_report, dict) else {}
    on_aggregates = on_report.get("aggregates", {}) if isinstance(on_report, dict) else {}
    off_diag = (
        off_report.get("phase4_portfolio_diagnostics", {})
        if isinstance(off_report, dict)
        else {}
    )
    on_diag = (
        on_report.get("phase4_portfolio_diagnostics", {})
        if isinstance(on_report, dict)
        else {}
    )
    off_selection = off_diag.get("selection_breakdown", {}) if isinstance(off_diag, dict) else {}
    on_selection = on_diag.get("selection_breakdown", {}) if isinstance(on_diag, dict) else {}

    output.update(
        {
            "available": True,
            "reason": "",
            "coverage": {
                "cost_proxy": "avg_fee_krw",
                "exposure_distribution_available": False,
                "exposure_distribution_note": "phase4 exposure-by-cluster is not emitted in backtest JSON yet",
            },
            "off": off_report,
            "on": on_report,
            "deltas": {
                "avg_total_trades": round(
                    to_float(on_aggregates.get("avg_total_trades", 0.0))
                    - to_float(off_aggregates.get("avg_total_trades", 0.0)),
                    6,
                ),
                "avg_profit_factor": round(
                    to_float(on_aggregates.get("avg_profit_factor", 0.0))
                    - to_float(off_aggregates.get("avg_profit_factor", 0.0)),
                    6,
                ),
                "avg_expectancy_krw": round(
                    to_float(on_aggregates.get("avg_expectancy_krw", 0.0))
                    - to_float(off_aggregates.get("avg_expectancy_krw", 0.0)),
                    6,
                ),
                "peak_max_drawdown_pct": round(
                    to_float(on_aggregates.get("peak_max_drawdown_pct", 0.0))
                    - to_float(off_aggregates.get("peak_max_drawdown_pct", 0.0)),
                    6,
                ),
                "avg_fee_krw": round(
                    to_float(on_aggregates.get("avg_fee_krw", 0.0))
                    - to_float(off_aggregates.get("avg_fee_krw", 0.0)),
                    6,
                ),
                "phase4_selection_rate": round(
                    to_float(on_selection.get("selection_rate", 0.0))
                    - to_float(off_selection.get("selection_rate", 0.0)),
                    6,
                ),
                "phase4_selected_total": (
                    to_int(on_selection.get("selected_total", 0))
                    - to_int(off_selection.get("selected_total", 0))
                ),
                "phase4_rejected_by_budget": (
                    to_int(on_selection.get("rejected_by_budget", 0))
                    - to_int(off_selection.get("rejected_by_budget", 0))
                ),
                "phase4_rejected_by_cluster_cap": (
                    to_int(on_selection.get("rejected_by_cluster_cap", 0))
                    - to_int(off_selection.get("rejected_by_cluster_cap", 0))
                ),
                "phase4_rejected_by_execution_cap": (
                    to_int(on_selection.get("rejected_by_execution_cap", 0))
                    - to_int(off_selection.get("rejected_by_execution_cap", 0))
                ),
                "phase4_rejected_by_drawdown_governor": (
                    to_int(on_selection.get("rejected_by_drawdown_governor", 0))
                    - to_int(off_selection.get("rejected_by_drawdown_governor", 0))
                ),
            },
        }
    )
    return output


def parse_backtest_json(proc: subprocess.CompletedProcess) -> Dict[str, Any]:
    lines: List[str] = []
    if proc.stdout:
        lines.extend(proc.stdout.splitlines())
    if proc.stderr:
        lines.extend(proc.stderr.splitlines())
    parsed: Dict[str, Any] = {}
    has_parsed = False
    for line in reversed(lines):
        text = line.strip()
        if text.startswith("{") and text.endswith("}"):
            try:
                value = json.loads(text)
            except Exception:
                continue
            if isinstance(value, dict):
                parsed = value
                has_parsed = True
                break
    if int(proc.returncode) != 0:
        tail = " || ".join([x.strip() for x in lines[-5:] if str(x).strip()])[:800]
        parsed_hint = ""
        if has_parsed:
            parsed_hint = f" parsed_json={json.dumps(parsed, ensure_ascii=False)[:400]}"
        raise RuntimeError(
            f"Backtest failed (exit={proc.returncode}).{parsed_hint} tail={tail}"
        )
    if has_parsed:
        return parsed
    raise RuntimeError(f"Backtest JSON output not found (exit={proc.returncode})")


def resolve_runtime_logs_dir(
    exe_dir: pathlib.Path,
    run_dir: Optional[pathlib.Path] = None,
) -> pathlib.Path:
    if isinstance(run_dir, pathlib.Path):
        return run_dir.resolve()
    return (exe_dir / "logs").resolve()


def run_backtest(
    exe_path: pathlib.Path,
    dataset_path: pathlib.Path,
    config_path: pathlib.Path,
    require_higher_tf_companions: bool,
    disable_adaptive_state_io: bool,
    run_dir: Optional[pathlib.Path] = None,
    evaluation_start_ts: Optional[int] = None,
    evaluation_end_ts: Optional[int] = None,
    cumulative_start_ts: Optional[int] = None,
    cumulative_end_ts: Optional[int] = None,
) -> Dict[str, Any]:
    cmd = [
        str(exe_path),
        "--backtest",
        str(dataset_path),
        "--json",
        "--config-path",
        str(config_path),
    ]
    if isinstance(run_dir, pathlib.Path):
        cmd.extend(["--run-dir", str(run_dir)])
    if require_higher_tf_companions and is_upbit_primary_1m_dataset(dataset_path):
        cmd.append("--require-higher-tf-companions")
    env = os.environ.copy()
    if disable_adaptive_state_io:
        env["AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO"] = "1"
    else:
        env.pop("AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO", None)
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        cwd=str(exe_path.parent),
        env=env,
    )
    parsed = parse_backtest_json(proc)
    parsed["runtime_logs_dir"] = str(resolve_runtime_logs_dir(exe_path.parent, run_dir))
    parsed["phase3_pass_ev_samples"] = load_policy_decision_ev_samples(exe_path.parent, run_dir=run_dir)
    parsed["policy_decision_volatility_samples"] = load_policy_decision_volatility_samples(
        exe_path.parent,
        run_dir=run_dir,
    )
    parsed["split_eval_stats"] = load_policy_decision_range_stats(
        exe_dir=exe_path.parent,
        run_dir=run_dir,
        start_ts=evaluation_start_ts,
        end_ts=evaluation_end_ts,
    )
    parsed["split_cumulative_stats"] = load_policy_decision_range_stats(
        exe_dir=exe_path.parent,
        run_dir=run_dir,
        start_ts=cumulative_start_ts,
        end_ts=cumulative_end_ts,
    )
    parsed["split_trade_eval_stats"] = load_execution_trade_range_stats(
        exe_dir=exe_path.parent,
        run_dir=run_dir,
        start_ts=evaluation_start_ts,
        end_ts=evaluation_end_ts,
    )
    parsed["split_trade_cumulative_stats"] = load_execution_trade_range_stats(
        exe_dir=exe_path.parent,
        run_dir=run_dir,
        start_ts=cumulative_start_ts,
        end_ts=cumulative_end_ts,
    )
    parsed["split_stage_funnel_eval_stats"] = load_entry_stage_funnel_range_stats(
        exe_dir=exe_path.parent,
        run_dir=run_dir,
        start_ts=evaluation_start_ts,
        end_ts=evaluation_end_ts,
    )
    parsed["split_stage_funnel_cumulative_stats"] = load_entry_stage_funnel_range_stats(
        exe_dir=exe_path.parent,
        run_dir=run_dir,
        start_ts=cumulative_start_ts,
        end_ts=cumulative_end_ts,
    )
    parsed["quality_filter_samples"] = load_quality_filter_range_samples(
        exe_dir=exe_path.parent,
        run_dir=run_dir,
        start_ts=evaluation_start_ts,
        end_ts=evaluation_end_ts,
    )
    runtime_logs_dir = resolve_runtime_logs_dir(exe_path.parent, run_dir)
    parsed["run_provenance"] = load_report_json(runtime_logs_dir / "run_provenance.json")
    parsed["run_params_dump"] = load_report_json(runtime_logs_dir / "run_params_dump.json")
    parsed["vnext_ev_samples"] = load_vnext_ev_samples(
        exe_dir=exe_path.parent,
        run_dir=run_dir,
        start_ts=evaluation_start_ts,
        end_ts=evaluation_end_ts,
        limit=50,
    )
    parsed["ev_sample_provenance"] = write_ev_sample_provenance_artifact(
        exe_dir=exe_path.parent,
        run_dir=run_dir,
        evaluation_start_ts=evaluation_start_ts,
        evaluation_end_ts=evaluation_end_ts,
    )
    return parsed


def load_ranging_shadow_events(
    exe_dir: pathlib.Path,
    dataset_name: str,
    run_dir: Optional[pathlib.Path] = None,
) -> List[Dict[str, Any]]:
    artifact_path = (resolve_runtime_logs_dir(exe_dir, run_dir) / "ranging_shadow_signals.jsonl").resolve()
    if not artifact_path.exists():
        return []
    events: List[Dict[str, Any]] = []
    try:
        with artifact_path.open("r", encoding="utf-8", errors="ignore") as fp:
            for raw_line in fp:
                line = str(raw_line).strip()
                if not line:
                    continue
                try:
                    payload = json.loads(line)
                except Exception:
                    continue
                if not isinstance(payload, dict):
                    continue
                row = dict(payload)
                row["dataset"] = str(dataset_name).strip()
                events.append(row)
    except Exception:
        return []
    return events


def write_ranging_shadow_events_jsonl(
    output_path: pathlib.Path,
    events: List[Dict[str, Any]],
) -> None:
    ensure_parent(output_path)
    with output_path.open("w", encoding="utf-8", newline="\n") as fp:
        for row in events:
            if not isinstance(row, dict):
                continue
            fp.write(json.dumps(row, ensure_ascii=False) + "\n")


def load_policy_decision_ev_samples(
    exe_dir: pathlib.Path,
    run_dir: Optional[pathlib.Path] = None,
) -> List[Dict[str, Any]]:
    artifact_path = resolve_policy_decision_artifact_path(exe_dir, run_dir)
    if not artifact_path.exists():
        return []
    samples: List[Dict[str, Any]] = []
    try:
        with artifact_path.open("r", encoding="utf-8", errors="ignore") as fp:
            for raw_line in fp:
                line = str(raw_line).strip()
                if not line:
                    continue
                try:
                    payload = json.loads(line)
                except Exception:
                    continue
                if not isinstance(payload, dict):
                    continue
                ts_value = max(0, to_int(payload.get("ts", 0)))
                decisions = payload.get("decisions", [])
                if not isinstance(decisions, list):
                    continue
                for row in decisions:
                    if not isinstance(row, dict):
                        continue
                    expected_pct = to_float(row.get("expected_value", 0.0))
                    if not math.isfinite(expected_pct):
                        continue
                    expected_bps = expected_pct * 10000.0
                    expected_edge_calibrated_bps = to_float(
                        row.get("expected_edge_calibrated_bps", expected_bps)
                    )
                    if not math.isfinite(expected_edge_calibrated_bps):
                        expected_edge_calibrated_bps = expected_bps
                    expected_edge_used_for_gate_bps = to_float(
                        row.get("expected_edge_used_for_gate_bps", expected_bps)
                    )
                    if not math.isfinite(expected_edge_used_for_gate_bps):
                        expected_edge_used_for_gate_bps = expected_bps
                    cost_bps_estimate_used = to_float(row.get("cost_bps_estimate_used", 0.0))
                    if not math.isfinite(cost_bps_estimate_used):
                        cost_bps_estimate_used = 0.0
                    edge_semantics = str(row.get("edge_semantics", "net")).strip().lower()
                    if edge_semantics not in ("net", "gross"):
                        edge_semantics = "unknown"
                    samples.append(
                        {
                            "ts": int(ts_value),
                            "market": str(row.get("market", "")).strip(),
                            "strategy": str(row.get("strategy", "")).strip(),
                            "selected": bool(row.get("selected", False)),
                            "reason": str(row.get("reason", "")).strip(),
                            "expected_edge_after_cost_pct": round(float(expected_pct), 10),
                            "expected_edge_after_cost_bps": round(float(expected_bps), 6),
                            "expected_edge_calibrated_bps": round(float(expected_edge_calibrated_bps), 6),
                            "expected_edge_used_for_gate_bps": round(float(expected_edge_used_for_gate_bps), 6),
                            "cost_bps_estimate_used": round(float(cost_bps_estimate_used), 6),
                            "edge_semantics": edge_semantics,
                            "root_cost_model_enabled_configured": bool(
                                row.get("root_cost_model_enabled_configured", False)
                            ),
                            "phase3_cost_model_enabled_configured": bool(
                                row.get("phase3_cost_model_enabled_configured", False)
                            ),
                            "root_cost_model_enabled_effective": bool(
                                row.get("root_cost_model_enabled_effective", False)
                            ),
                            "phase3_cost_model_enabled_effective": bool(
                                row.get("phase3_cost_model_enabled_effective", False)
                            ),
                            "edge_semantics_guard_violation": bool(
                                row.get("edge_semantics_guard_violation", False)
                            ),
                            "edge_semantics_guard_forced_off": bool(
                                row.get("edge_semantics_guard_forced_off", False)
                            ),
                            "edge_semantics_guard_action": str(
                                row.get("edge_semantics_guard_action", "none")
                            ).strip()
                            or "none",
                        }
                    )
    except Exception:
        return []
    if not samples:
        return []
    samples.sort(
        key=lambda item: (
            int(item.get("ts", 0)),
            str(item.get("market", "")),
            str(item.get("strategy", "")),
        )
    )
    return samples[:5000]


def load_policy_decision_volatility_samples(
    exe_dir: pathlib.Path,
    *,
    run_dir: Optional[pathlib.Path] = None,
    sample_cap: int = 120000,
) -> List[Dict[str, Any]]:
    artifact_path = resolve_policy_decision_artifact_path(exe_dir, run_dir)
    if not artifact_path.exists():
        return []
    samples: List[Dict[str, Any]] = []
    try:
        with artifact_path.open("r", encoding="utf-8", errors="ignore") as fp:
            for raw_line in fp:
                line = str(raw_line).strip()
                if not line:
                    continue
                try:
                    payload = json.loads(line)
                except Exception:
                    continue
                if not isinstance(payload, dict):
                    continue
                ts_value = max(0, to_int(payload.get("ts", 0)))
                if ts_value <= 0:
                    continue
                dominant_regime = str(payload.get("dominant_regime", "")).strip()
                decisions = payload.get("decisions", [])
                if not isinstance(decisions, list):
                    continue
                for decision in decisions:
                    if not isinstance(decision, dict):
                        continue
                    market = str(decision.get("market", "")).strip().upper()
                    if not market:
                        continue
                    volatility = to_float(decision.get("volatility", float("nan")))
                    if not math.isfinite(volatility):
                        continue
                    regime = str(decision.get("regime", dominant_regime)).strip()
                    if not regime:
                        regime = dominant_regime
                    samples.append(
                        {
                            "ts": int(ts_value),
                            "market": market,
                            "regime": regime or "UNKNOWN",
                            "volatility": round(float(volatility), 10),
                        }
                    )
    except Exception:
        return []
    if not samples:
        return []
    samples.sort(
        key=lambda item: (
            str(item.get("market", "")),
            int(item.get("ts", 0)),
            str(item.get("regime", "")),
        )
    )
    deduped: List[Dict[str, Any]] = []
    last_market = ""
    last_ts = -1
    for row in samples:
        market = str(row.get("market", ""))
        ts_value = int(row.get("ts", 0))
        if market == last_market and ts_value == last_ts and deduped:
            deduped[-1] = row
            continue
        deduped.append(row)
        last_market = market
        last_ts = ts_value
    cap = max(0, int(sample_cap))
    if cap > 0 and len(deduped) > cap:
        return deduped[:cap]
    return deduped


def load_policy_decision_range_stats(
    exe_dir: pathlib.Path,
    run_dir: Optional[pathlib.Path],
    start_ts: Optional[int],
    end_ts: Optional[int],
) -> Dict[str, Any]:
    start = int(start_ts) if start_ts is not None else 0
    end = int(end_ts) if end_ts is not None else 0
    enabled = bool(start > 0 and end > 0 and end >= start)
    artifact_path = resolve_policy_decision_artifact_path(exe_dir, run_dir)
    out = {
        "enabled": enabled,
        "start_ts": int(start),
        "end_ts": int(end),
        "path": str(artifact_path),
        "line_count_total": 0,
        "line_count_in_range": 0,
        "decision_count_total": 0,
        "decision_count_in_range": 0,
        "selected_decision_count_in_range": 0,
        "ts_min_total": None,
        "ts_max_total": None,
        "ts_min_in_range": None,
        "ts_max_in_range": None,
    }
    if not artifact_path.exists():
        out["reason"] = "policy_decisions_log_missing"
        return out
    try:
        with artifact_path.open("r", encoding="utf-8", errors="ignore") as fp:
            for raw_line in fp:
                line = str(raw_line).strip()
                if not line:
                    continue
                try:
                    payload = json.loads(line)
                except Exception:
                    continue
                if not isinstance(payload, dict):
                    continue
                ts_value = max(0, to_int(payload.get("ts", 0)))
                decisions = payload.get("decisions", [])
                if not isinstance(decisions, list):
                    decisions = []
                out["line_count_total"] = int(out["line_count_total"]) + 1
                out["decision_count_total"] = int(out["decision_count_total"]) + int(len(decisions))
                if ts_value > 0:
                    if out["ts_min_total"] is None or ts_value < int(out["ts_min_total"]):
                        out["ts_min_total"] = int(ts_value)
                    if out["ts_max_total"] is None or ts_value > int(out["ts_max_total"]):
                        out["ts_max_total"] = int(ts_value)
                in_range = bool(enabled and ts_value > 0 and start <= ts_value <= end)
                if in_range:
                    out["line_count_in_range"] = int(out["line_count_in_range"]) + 1
                    out["decision_count_in_range"] = int(out["decision_count_in_range"]) + int(len(decisions))
                    out["selected_decision_count_in_range"] = int(out["selected_decision_count_in_range"]) + int(
                        sum(1 for row in decisions if isinstance(row, dict) and bool(row.get("selected", False)))
                    )
                    if out["ts_min_in_range"] is None or ts_value < int(out["ts_min_in_range"]):
                        out["ts_min_in_range"] = int(ts_value)
                    if out["ts_max_in_range"] is None or ts_value > int(out["ts_max_in_range"]):
                        out["ts_max_in_range"] = int(ts_value)
    except Exception:
        out["reason"] = "policy_decisions_log_parse_failed"
        return out
    return out


def load_execution_trade_range_stats(
    exe_dir: pathlib.Path,
    run_dir: Optional[pathlib.Path],
    start_ts: Optional[int],
    end_ts: Optional[int],
) -> Dict[str, Any]:
    start = int(start_ts) if start_ts is not None else 0
    end = int(end_ts) if end_ts is not None else 0
    enabled = bool(start > 0 and end > 0 and end >= start)
    artifact_path = (resolve_runtime_logs_dir(exe_dir, run_dir) / "execution_updates_backtest.jsonl").resolve()
    out = {
        "enabled": enabled,
        "start_ts": int(start),
        "end_ts": int(end),
        "path": str(artifact_path),
        "events_total": 0,
        "events_in_range": 0,
        "buy_submitted_total": 0,
        "buy_submitted_in_range": 0,
        "buy_filled_total": 0,
        "buy_filled_in_range": 0,
        "sell_submitted_total": 0,
        "sell_submitted_in_range": 0,
        "sell_filled_total": 0,
        "sell_filled_in_range": 0,
        "terminal_sell_fills_total": 0,
        "terminal_sell_fills_in_range": 0,
        "ts_min_total": None,
        "ts_max_total": None,
        "ts_min_in_range": None,
        "ts_max_in_range": None,
    }
    if not artifact_path.exists():
        out["reason"] = "execution_updates_log_missing"
        return out
    try:
        with artifact_path.open("r", encoding="utf-8", errors="ignore") as fp:
            for raw_line in fp:
                line = str(raw_line).strip()
                if not line:
                    continue
                try:
                    payload = json.loads(line)
                except Exception:
                    continue
                if not isinstance(payload, dict):
                    continue
                ts_value = max(0, to_int(payload.get("ts_ms", payload.get("ts", 0))))
                event = str(payload.get("event", "")).strip().lower()
                side = str(payload.get("side", "")).strip().upper()
                terminal = bool(payload.get("terminal", False))
                is_terminal_sell_fill = bool(event == "filled" and side == "SELL" and terminal)
                is_buy_submitted = bool(event == "submitted" and side == "BUY")
                is_buy_filled = bool(event == "filled" and side == "BUY")
                is_sell_submitted = bool(event == "submitted" and side == "SELL")
                is_sell_filled = bool(event == "filled" and side == "SELL")
                out["events_total"] = int(out["events_total"]) + 1
                if is_buy_submitted:
                    out["buy_submitted_total"] = int(out["buy_submitted_total"]) + 1
                if is_buy_filled:
                    out["buy_filled_total"] = int(out["buy_filled_total"]) + 1
                if is_sell_submitted:
                    out["sell_submitted_total"] = int(out["sell_submitted_total"]) + 1
                if is_sell_filled:
                    out["sell_filled_total"] = int(out["sell_filled_total"]) + 1
                if is_terminal_sell_fill:
                    out["terminal_sell_fills_total"] = int(out["terminal_sell_fills_total"]) + 1
                if ts_value > 0:
                    if out["ts_min_total"] is None or ts_value < int(out["ts_min_total"]):
                        out["ts_min_total"] = int(ts_value)
                    if out["ts_max_total"] is None or ts_value > int(out["ts_max_total"]):
                        out["ts_max_total"] = int(ts_value)
                in_range = bool(enabled and ts_value > 0 and start <= ts_value <= end)
                if in_range:
                    out["events_in_range"] = int(out["events_in_range"]) + 1
                    if is_buy_submitted:
                        out["buy_submitted_in_range"] = int(out["buy_submitted_in_range"]) + 1
                    if is_buy_filled:
                        out["buy_filled_in_range"] = int(out["buy_filled_in_range"]) + 1
                    if is_sell_submitted:
                        out["sell_submitted_in_range"] = int(out["sell_submitted_in_range"]) + 1
                    if is_sell_filled:
                        out["sell_filled_in_range"] = int(out["sell_filled_in_range"]) + 1
                    if is_terminal_sell_fill:
                        out["terminal_sell_fills_in_range"] = int(out["terminal_sell_fills_in_range"]) + 1
                    if out["ts_min_in_range"] is None or ts_value < int(out["ts_min_in_range"]):
                        out["ts_min_in_range"] = int(ts_value)
                    if out["ts_max_in_range"] is None or ts_value > int(out["ts_max_in_range"]):
                        out["ts_max_in_range"] = int(ts_value)
    except Exception:
        out["reason"] = "execution_updates_log_parse_failed"
        return out
    return out


def load_entry_stage_funnel_range_stats(
    exe_dir: pathlib.Path,
    run_dir: Optional[pathlib.Path],
    start_ts: Optional[int],
    end_ts: Optional[int],
) -> Dict[str, Any]:
    start = int(start_ts) if start_ts is not None else 0
    end = int(end_ts) if end_ts is not None else 0
    enabled = bool(start > 0 and end > 0 and end >= start)
    artifact_path = (resolve_runtime_logs_dir(exe_dir, run_dir) / "entry_stage_funnel_backtest.jsonl").resolve()
    out: Dict[str, Any] = {
        "enabled": enabled,
        "start_ts": int(start),
        "end_ts": int(end),
        "path": str(artifact_path),
        "entry_rounds_total": 0,
        "entry_rounds_in_range": 0,
        "candidates_total_in_range": 0,
        "candidates_after_sizing_in_range": 0,
        "candidates_after_quality_topk_in_range": 0,
        "candidates_after_manager_in_range": 0,
        "candidates_after_portfolio_in_range": 0,
        "orders_submitted_in_range": 0,
        "orders_filled_in_range": 0,
        "ev_samples_in_range": 0,
        "ev_at_manager_pass_sum_in_range": 0.0,
        "ev_at_order_submit_check_sum_in_range": 0.0,
        "ev_at_manager_pass_avg_in_range": 0.0,
        "ev_at_order_submit_check_avg_in_range": 0.0,
        "ev_mismatch_count_in_range": 0,
        "order_block_rounds_in_range": 0,
        "order_block_reason_counts_in_range": {},
        "top_order_block_reasons_in_range": [],
        "no_signal_generated_in_range": 0,
        "no_signal_reason_counts_in_range": {},
        "top_no_signal_reasons_in_range": [],
        "no_signal_reason_event_rows_in_range": 0,
        "no_signal_reason_event_rows_total": 0,
        "no_signal_reason_event_rows_with_ts_total": 0,
        "no_signal_reason_event_rows_missing_ts_total": 0,
        "no_signal_reason_timestamp_coverage_total": 0.0,
        "ts_min_total": None,
        "ts_max_total": None,
        "ts_min_in_range": None,
        "ts_max_in_range": None,
    }
    if not artifact_path.exists():
        out["reason"] = "entry_stage_funnel_log_missing"
        return out

    def _is_order_block_reason(reason: str) -> bool:
        reason_key = str(reason).strip().lower()
        return bool(
            reason_key.startswith("blocked_")
            or reason_key in {"reject_expected_edge_negative_count", "reject_expected_edge_negative"}
        )

    def _is_no_signal_reason(reason: str) -> bool:
        reason_key = str(reason).strip().lower()
        if not reason_key or reason_key == "no_signal_generated":
            return False
        return bool(
            reason_key.startswith("foundation_no_signal_")
            or reason_key.startswith("no_signal_")
            or reason_key.startswith("probabilistic_")
        )

    block_reason_counts: Dict[str, int] = {}
    no_signal_reason_counts: Dict[str, int] = {}
    no_signal_event_rows_total = 0
    no_signal_event_rows_with_ts_total = 0
    no_signal_event_rows_missing_ts_total = 0
    try:
        with artifact_path.open("r", encoding="utf-8", errors="ignore") as fp:
            for raw_line in fp:
                line = str(raw_line).strip()
                if not line:
                    continue
                try:
                    payload = json.loads(line)
                except Exception:
                    continue
                if not isinstance(payload, dict):
                    continue
                ts_value = max(0, to_int(payload.get("ts", 0)))
                stages = payload.get("stages", {})
                if not isinstance(stages, dict):
                    stages = {}
                stage_vnext = payload.get("stage_funnel_vnext", {})
                if not isinstance(stage_vnext, dict):
                    stage_vnext = {}
                out["entry_rounds_total"] = int(out["entry_rounds_total"]) + 1
                if ts_value > 0:
                    if out["ts_min_total"] is None or ts_value < int(out["ts_min_total"]):
                        out["ts_min_total"] = int(ts_value)
                    if out["ts_max_total"] is None or ts_value > int(out["ts_max_total"]):
                        out["ts_max_total"] = int(ts_value)
                reason_counts = payload.get("reject_reason_counts", {})
                if not isinstance(reason_counts, dict):
                    reason_counts = {}

                has_no_signal_reason = False
                for reason, value in reason_counts.items():
                    reason_key = str(reason).strip()
                    if not reason_key:
                        continue
                    count = max(0, to_int(value))
                    if count <= 0:
                        continue
                    if str(reason_key).strip().lower() == "no_signal_generated":
                        has_no_signal_reason = True
                        continue
                    if _is_no_signal_reason(reason_key):
                        has_no_signal_reason = True
                if has_no_signal_reason:
                    no_signal_event_rows_total += 1
                    if ts_value > 0:
                        no_signal_event_rows_with_ts_total += 1
                    else:
                        no_signal_event_rows_missing_ts_total += 1

                in_range = bool(enabled and ts_value > 0 and start <= ts_value <= end)
                if not in_range:
                    continue
                out["entry_rounds_in_range"] = int(out["entry_rounds_in_range"]) + 1
                out["candidates_total_in_range"] = int(out["candidates_total_in_range"]) + max(
                    0,
                    to_int(
                        stage_vnext.get(
                            "s0_snapshots_valid",
                            stages.get("candidates_total", 0),
                        )
                    ),
                )
                after_quality_topk = max(
                    0,
                    to_int(
                        stage_vnext.get(
                            "s1_selected_topk",
                            stages.get(
                                "candidates_after_quality_topk",
                                stages.get("candidates_after_quality_topk", 0),
                            ),
                        )
                    ),
                )
                after_sizing = max(
                    0,
                    to_int(
                        stage_vnext.get(
                            "s2_sized_count",
                            stages.get(
                                "candidates_after_sizing",
                                stages.get("candidates_after_manager", 0),
                            ),
                        )
                    ),
                )
                out["candidates_after_quality_topk_in_range"] = int(
                    out["candidates_after_quality_topk_in_range"]
                ) + int(after_quality_topk)
                out["candidates_after_sizing_in_range"] = int(
                    out["candidates_after_sizing_in_range"]
                ) + int(after_sizing)
                out["candidates_after_manager_in_range"] = int(
                    out["candidates_after_manager_in_range"]
                ) + int(after_sizing)
                out["candidates_after_portfolio_in_range"] = int(
                    out["candidates_after_portfolio_in_range"]
                ) + max(
                    0,
                    to_int(
                        stage_vnext.get(
                            "s3_exec_gate_pass",
                            stages.get("candidates_after_portfolio", 0),
                        )
                    ),
                )
                orders_submitted = max(
                    0,
                    to_int(stage_vnext.get("s4_submitted", stages.get("orders_submitted", 0))),
                )
                orders_filled = max(
                    0,
                    to_int(stage_vnext.get("s5_filled", stages.get("orders_filled", 0))),
                )
                out["orders_submitted_in_range"] = int(out["orders_submitted_in_range"]) + int(
                    orders_submitted
                )
                out["orders_filled_in_range"] = int(out["orders_filled_in_range"]) + int(
                    orders_filled
                )
                if out["ts_min_in_range"] is None or ts_value < int(out["ts_min_in_range"]):
                    out["ts_min_in_range"] = int(ts_value)
                if out["ts_max_in_range"] is None or ts_value > int(out["ts_max_in_range"]):
                    out["ts_max_in_range"] = int(ts_value)

                no_signal_count_in_row = 0
                for reason, value in reason_counts.items():
                    reason_key = str(reason).strip()
                    if not reason_key:
                        continue
                    count = max(0, to_int(value))
                    if count <= 0:
                        continue
                    reason_key_lc = str(reason_key).strip().lower()
                    if reason_key_lc == "no_signal_generated":
                        no_signal_count_in_row += count
                        out["no_signal_generated_in_range"] = int(
                            out["no_signal_generated_in_range"]
                        ) + int(count)
                        continue
                    if _is_no_signal_reason(reason_key):
                        no_signal_count_in_row += count
                        no_signal_reason_counts[reason_key] = (
                            no_signal_reason_counts.get(reason_key, 0) + int(count)
                        )
                if no_signal_count_in_row > 0:
                    out["no_signal_reason_event_rows_in_range"] = int(
                        out["no_signal_reason_event_rows_in_range"]
                    ) + 1

                ev_consistency = payload.get("ev_consistency", {})
                if isinstance(ev_consistency, dict):
                    raw_ev_manager = ev_consistency.get(
                        "ev_at_selection",
                        ev_consistency.get("ev_at_manager_pass", None),
                    )
                    raw_ev_order = ev_consistency.get("ev_at_order_submit_check", None)
                    try:
                        ev_manager = float(raw_ev_manager) if raw_ev_manager is not None else math.nan
                    except Exception:
                        ev_manager = math.nan
                    try:
                        ev_order = float(raw_ev_order) if raw_ev_order is not None else math.nan
                    except Exception:
                        ev_order = math.nan
                    if not math.isfinite(ev_manager):
                        ev_manager = math.nan
                    if not math.isfinite(ev_order):
                        ev_order = math.nan
                    if math.isfinite(ev_manager) and math.isfinite(ev_order):
                        out["ev_samples_in_range"] = int(out["ev_samples_in_range"]) + 1
                        out["ev_at_manager_pass_sum_in_range"] = float(
                            out["ev_at_manager_pass_sum_in_range"]
                        ) + float(ev_manager)
                        out["ev_at_order_submit_check_sum_in_range"] = float(
                            out["ev_at_order_submit_check_sum_in_range"]
                        ) + float(ev_order)
                    out["ev_mismatch_count_in_range"] = int(
                        out["ev_mismatch_count_in_range"]
                    ) + max(0, to_int(ev_consistency.get("ev_mismatch_count", 0)))

                stage_after_portfolio = max(
                    0, to_int(stages.get("candidates_after_portfolio", 0))
                )
                if stage_after_portfolio > 0 and orders_submitted <= 0:
                    out["order_block_rounds_in_range"] = int(out["order_block_rounds_in_range"]) + 1
                    for reason, value in reason_counts.items():
                        reason_key = str(reason).strip()
                        if not reason_key or not _is_order_block_reason(reason_key):
                            continue
                        block_reason_counts[reason_key] = block_reason_counts.get(
                            reason_key, 0
                        ) + max(0, to_int(value))
    except Exception:
        out["reason"] = "entry_stage_funnel_log_parse_failed"
        return out

    out["order_block_reason_counts_in_range"] = {
        str(k): int(v) for k, v in block_reason_counts.items() if int(v) > 0
    }
    out["top_order_block_reasons_in_range"] = sorted(
        [
            {"reason": str(k), "count": int(v)}
            for k, v in block_reason_counts.items()
            if int(v) > 0
        ],
        key=lambda item: (-int(item["count"]), str(item["reason"])),
    )[:3]
    out["no_signal_reason_counts_in_range"] = {
        str(k): int(v) for k, v in no_signal_reason_counts.items() if int(v) > 0
    }
    out["top_no_signal_reasons_in_range"] = sorted(
        [
            {"reason": str(k), "count": int(v)}
            for k, v in no_signal_reason_counts.items()
            if int(v) > 0
        ],
        key=lambda item: (-int(item["count"]), str(item["reason"])),
    )[:8]
    out["no_signal_reason_event_rows_total"] = int(max(0, no_signal_event_rows_total))
    out["no_signal_reason_event_rows_with_ts_total"] = int(max(0, no_signal_event_rows_with_ts_total))
    out["no_signal_reason_event_rows_missing_ts_total"] = int(
        max(0, no_signal_event_rows_missing_ts_total)
    )
    out["no_signal_reason_timestamp_coverage_total"] = round(
        float(no_signal_event_rows_with_ts_total) / float(max(1, no_signal_event_rows_total)),
        6,
    ) if no_signal_event_rows_total > 0 else 0.0
    ev_samples = max(0, to_int(out.get("ev_samples_in_range", 0)))
    if ev_samples > 0:
        out["ev_at_manager_pass_avg_in_range"] = (
            float(out.get("ev_at_manager_pass_sum_in_range", 0.0)) / float(ev_samples)
        )
        out["ev_at_order_submit_check_avg_in_range"] = (
            float(out.get("ev_at_order_submit_check_sum_in_range", 0.0))
            / float(ev_samples)
        )
    else:
        out["ev_at_manager_pass_avg_in_range"] = 0.0
        out["ev_at_order_submit_check_avg_in_range"] = 0.0
    return out


def load_vnext_ev_samples(
    exe_dir: pathlib.Path,
    run_dir: Optional[pathlib.Path],
    start_ts: Optional[int],
    end_ts: Optional[int],
    limit: int = 50,
) -> List[Dict[str, Any]]:
    artifact_path = (resolve_runtime_logs_dir(exe_dir, run_dir) / "vnext_ev_samples.json").resolve()
    if not artifact_path.exists():
        return []
    start = int(start_ts) if start_ts is not None else 0
    end = int(end_ts) if end_ts is not None else 0
    enabled_range = bool(start > 0 and end > 0 and end >= start)
    try:
        loaded = load_report_json(artifact_path)
    except Exception:
        return []
    rows = loaded if isinstance(loaded, list) else []
    out: List[Dict[str, Any]] = []
    for raw in rows:
        if not isinstance(raw, dict):
            continue
        ts_ms = max(0, to_int(raw.get("ts_ms", 0)))
        if enabled_range and (ts_ms <= 0 or ts_ms < start or ts_ms > end):
            continue
        out.append(
            {
                "ts_ms": int(ts_ms),
                "market": str(raw.get("market", "")).strip(),
                "regime": str(raw.get("regime", "")).strip(),
                "snapshot_id": str(raw.get("snapshot_id", "")).strip(),
                "backend_request": str(raw.get("backend_request", "")).strip().lower(),
                "backend_effective": str(raw.get("backend_effective", "")).strip().lower(),
                "lgbm_model_sha256": str(raw.get("lgbm_model_sha256", "")).strip().lower(),
                "sample_stage": str(raw.get("sample_stage", "")).strip(),
                "snapshot_valid": bool(raw.get("snapshot_valid", False)),
                "snapshot_fail_reason": str(raw.get("snapshot_fail_reason", "")).strip(),
                "p_raw": (
                    None
                    if raw.get("p_raw", None) is None
                    else round(to_float(raw.get("p_raw", 0.0)), 10)
                ),
                "p_cal": round(
                    to_float(raw.get("p_cal", raw.get("p_calibrated", 0.0))),
                    10,
                ),
                "p_calibrated": round(to_float(raw.get("p_calibrated", 0.0)), 10),
                "threshold": round(
                    to_float(raw.get("threshold", raw.get("selection_threshold", 0.0))),
                    10,
                ),
                "selection_threshold": round(
                    to_float(raw.get("selection_threshold", raw.get("threshold", 0.0))),
                    10,
                ),
                "margin": round(to_float(raw.get("margin", 0.0)), 10),
                "tp_pct": round(to_float(raw.get("tp_pct", 0.0)), 10),
                "sl_pct": round(to_float(raw.get("sl_pct", 0.0)), 10),
                "label_cost_bps": round(to_float(raw.get("label_cost_bps", 0.0)), 10),
                "expected_value_from_prob_bps": round(
                    to_float(raw.get("expected_value_from_prob_bps", 0.0)),
                    10,
                ),
                "stop_loss_trending_multiplier_effective": round(
                    to_float(raw.get("stop_loss_trending_multiplier_effective", 0.0)),
                    10,
                ),
                "tp_distance_trending_multiplier_effective": round(
                    to_float(raw.get("tp_distance_trending_multiplier_effective", 0.0)),
                    10,
                ),
                "expected_edge_calibrated_bps": round(
                    to_float(raw.get("expected_edge_calibrated_bps", 0.0)),
                    10,
                ),
                "expected_edge_used_for_gate_bps": round(
                    to_float(raw.get("expected_edge_used_for_gate_bps", 0.0)),
                    10,
                ),
                "expected_edge_used_for_gate_source": str(
                    raw.get("expected_edge_used_for_gate_source", "")
                ).strip(),
                "edge_regressor_available": bool(raw.get("edge_regressor_available", False)),
                "edge_regressor_used": bool(raw.get("edge_regressor_used", False)),
                "edge_profile_used": bool(raw.get("edge_profile_used", False)),
                "signal_expected_value": round(
                    to_float(raw.get("signal_expected_value", 0.0)),
                    10,
                ),
                "edge_bps_from_snapshot": round(
                    to_float(raw.get("edge_bps_from_snapshot", 0.0)),
                    10,
                ),
                "ev_in_bps": round(
                    to_float(raw.get("ev_in_bps", raw.get("expected_edge_used_for_gate_bps", 0.0))),
                    10,
                ),
                "ev_for_size_bps": round(
                    to_float(raw.get("ev_for_size_bps", raw.get("expected_value_vnext_bps", 0.0))),
                    10,
                ),
                "expected_value_vnext_bps": round(
                    to_float(raw.get("expected_value_vnext_bps", 0.0)),
                    10,
                ),
                "size_fraction": round(to_float(raw.get("size_fraction", 0.0)), 10),
                "execution_gate_pass": bool(raw.get("execution_gate_pass", False)),
                "execution_reject_reason": str(raw.get("execution_reject_reason", "")).strip(),
            }
        )
        if len(out) >= max(1, int(limit)):
            break
    return out


def sha256_of_file(path: pathlib.Path) -> str:
    if not path.exists():
        return ""
    h = hashlib.sha256()
    with path.open("rb") as fp:
        while True:
            chunk = fp.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest().lower()


def write_ev_sample_provenance_artifact(
    exe_dir: pathlib.Path,
    run_dir: Optional[pathlib.Path],
    evaluation_start_ts: Optional[int],
    evaluation_end_ts: Optional[int],
) -> Dict[str, Any]:
    runtime_logs_dir = resolve_runtime_logs_dir(exe_dir, run_dir)
    runtime_logs_dir.mkdir(parents=True, exist_ok=True)

    ev_samples_path = (runtime_logs_dir / "vnext_ev_samples.json").resolve()
    run_provenance_path = (runtime_logs_dir / "run_provenance.json").resolve()
    stage_funnel_vnext_path = (runtime_logs_dir / "stage_funnel_vnext.json").resolve()

    run_provenance = (
        load_report_json(run_provenance_path)
        if run_provenance_path.exists()
        else {}
    )
    if not isinstance(run_provenance, dict):
        run_provenance = {}
    stage_funnel_payload = (
        load_report_json(stage_funnel_vnext_path)
        if stage_funnel_vnext_path.exists()
        else {}
    )
    if not isinstance(stage_funnel_payload, dict):
        stage_funnel_payload = {}

    ev_rows: List[Dict[str, Any]] = []
    if ev_samples_path.exists():
        try:
            with ev_samples_path.open("r", encoding="utf-8") as fp:
                loaded_any = json.load(fp)
            if isinstance(loaded_any, list):
                ev_rows = [row for row in loaded_any if isinstance(row, dict)]
        except Exception:
            ev_rows = []

    compact_rows = [
        json.dumps(row, ensure_ascii=False, separators=(",", ":"))
        for row in ev_rows
    ]
    start_ts_ms = (
        int(evaluation_start_ts)
        if evaluation_start_ts is not None
        else int(to_int(run_provenance.get("start_ts_ms", 0)))
    )
    end_ts_ms = (
        int(evaluation_end_ts)
        if evaluation_end_ts is not None
        else int(to_int(stage_funnel_payload.get("generated_at_ms", 0)))
    )

    payload: Dict[str, Any] = {
        "run_dir": str(runtime_logs_dir),
        "config_path": str(run_provenance.get("config_path", "")),
        "bundle_path": str(run_provenance.get("bundle_path", "")),
        "backend_request": str(run_provenance.get("prob_model_backend", "unknown")).strip().lower(),
        "backend_effective": str(stage_funnel_payload.get("backend_effective", "unknown")).strip().lower(),
        "start_ts_ms": start_ts_ms if start_ts_ms > 0 else None,
        "end_ts_ms": end_ts_ms if end_ts_ms > 0 else None,
        "vnext_ev_samples_path": str(ev_samples_path),
        "vnext_ev_samples_sha256": sha256_of_file(ev_samples_path),
        "vnext_ev_samples_line_count": int(len(ev_rows)),
        "first_3_rows": compact_rows[:3],
        "last_3_rows": compact_rows[-3:] if compact_rows else [],
    }

    output_path = (runtime_logs_dir / "ev_sample_provenance.json").resolve()
    with output_path.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(payload, fp, ensure_ascii=False, indent=4)
    return payload


def load_quality_filter_range_samples(
    exe_dir: pathlib.Path,
    run_dir: Optional[pathlib.Path],
    start_ts: Optional[int],
    end_ts: Optional[int],
) -> Dict[str, Any]:
    start = int(start_ts) if start_ts is not None else 0
    end = int(end_ts) if end_ts is not None else 0
    enabled = bool(start > 0 and end > 0 and end >= start)
    artifact_path = (resolve_runtime_logs_dir(exe_dir, run_dir) / "quality_filter_backtest.jsonl").resolve()
    out: Dict[str, Any] = {
        "enabled": enabled,
        "start_ts": int(start),
        "end_ts": int(end),
        "path": str(artifact_path),
        "line_count_total": 0,
        "line_count_in_range": 0,
        "sample_count_total": 0,
        "sample_count_in_range": 0,
        "quality_filter_enabled_count_in_range": 0,
        "quality_filter_fail_count_in_range": 0,
        "samples": [],
    }
    if not artifact_path.exists():
        out["reason"] = "quality_filter_log_missing"
        return out
    try:
        with artifact_path.open("r", encoding="utf-8", errors="ignore") as fp:
            for raw_line in fp:
                line = str(raw_line).strip()
                if not line:
                    continue
                try:
                    payload = json.loads(line)
                except Exception:
                    continue
                if not isinstance(payload, dict):
                    continue
                ts_value = max(0, to_int(payload.get("ts", 0)))
                samples = payload.get("samples", [])
                if not isinstance(samples, list):
                    samples = []
                out["line_count_total"] = int(out["line_count_total"]) + 1
                out["sample_count_total"] = int(out["sample_count_total"]) + int(len(samples))
                in_range = bool(enabled and ts_value > 0 and start <= ts_value <= end)
                if not in_range:
                    continue
                out["line_count_in_range"] = int(out["line_count_in_range"]) + 1
                out["sample_count_in_range"] = int(out["sample_count_in_range"]) + int(len(samples))
                for sample in samples:
                    if not isinstance(sample, dict):
                        continue
                    row = {
                        "ts": int(ts_value),
                        "market": str(sample.get("market", payload.get("market", ""))).strip().upper(),
                        "strategy_name": str(sample.get("strategy_name", "")).strip(),
                        "regime": str(sample.get("regime", "")).strip(),
                        "quality_filter_enabled": bool(sample.get("quality_filter_enabled", False)),
                        "quality_filter_pass": bool(sample.get("quality_filter_pass", False)),
                        "sizing_pass": bool(sample.get("sizing_pass", False)),
                        "margin_pass": bool(sample.get("margin_pass", True)),
                        "ev_confidence_pass": bool(sample.get("ev_confidence_pass", True)),
                        "cost_tail_pass": bool(sample.get("cost_tail_pass", True)),
                        "manager_pass": bool(sample.get("manager_pass", False)),
                        "expected_value_observed": to_float(sample.get("expected_value_observed", float("nan"))),
                        "required_expected_value": to_float(sample.get("required_expected_value", float("nan"))),
                        "expected_value_slack": to_float(sample.get("expected_value_slack", float("nan"))),
                        "margin_observed": to_float(sample.get("margin_observed", float("nan"))),
                        "margin_floor": to_float(sample.get("margin_floor", float("nan"))),
                        "margin_slack": to_float(sample.get("margin_slack", float("nan"))),
                        "ev_confidence_observed": to_float(sample.get("ev_confidence_observed", float("nan"))),
                        "ev_confidence_floor": to_float(sample.get("ev_confidence_floor", float("nan"))),
                        "ev_confidence_slack": to_float(sample.get("ev_confidence_slack", float("nan"))),
                        "cost_tail_observed": to_float(sample.get("cost_tail_observed", float("nan"))),
                        "cost_tail_limit": to_float(sample.get("cost_tail_limit", float("nan"))),
                        "cost_tail_slack": to_float(sample.get("cost_tail_slack", float("nan"))),
                    }
                    if bool(row["quality_filter_enabled"]):
                        out["quality_filter_enabled_count_in_range"] = int(
                            out["quality_filter_enabled_count_in_range"]
                        ) + 1
                    if bool(row["quality_filter_enabled"]) and not bool(row["quality_filter_pass"]):
                        out["quality_filter_fail_count_in_range"] = int(out["quality_filter_fail_count_in_range"]) + 1
                    out["samples"].append(row)
    except Exception:
        out["reason"] = "quality_filter_log_parse_failed"
        return out
    return out


def resolve_vol_bucket_pct_policy(raw_policy: Dict[str, Any]) -> Dict[str, Any]:
    window_size = max(5, to_int(raw_policy.get("window_size", 240)))
    min_points_raw = to_int(raw_policy.get("min_points", 0))
    auto_min_points = int(math.ceil(float(window_size) * 0.7))
    min_points = max(1, min_points_raw) if min_points_raw > 0 else max(1, auto_min_points)
    min_points = min(window_size, min_points)
    lower_q = clamp_value(to_float(raw_policy.get("lower_q", 0.33)), 0.01, 0.49)
    upper_q = clamp_value(to_float(raw_policy.get("upper_q", 0.67)), 0.51, 0.99)
    if upper_q <= lower_q:
        upper_q = min(0.99, lower_q + 0.01)
    fixed_low_cut = to_float(raw_policy.get("fixed_low_cut", 1.8))
    fixed_high_cut = to_float(raw_policy.get("fixed_high_cut", 3.5))
    if fixed_high_cut < fixed_low_cut:
        fixed_high_cut = fixed_low_cut
    return {
        "window_size": int(window_size),
        "min_points": int(min_points),
        "lower_q": float(lower_q),
        "upper_q": float(upper_q),
        "fixed_low_cut": float(fixed_low_cut),
        "fixed_high_cut": float(fixed_high_cut),
        "source": "verification_policy",
    }


def quantile_from_sorted_values(sorted_values: List[float], q: float) -> float:
    if not sorted_values:
        return 0.0
    qv = clamp_value(float(q), 0.0, 1.0)
    if len(sorted_values) == 1:
        return float(sorted_values[0])
    pos = qv * float(len(sorted_values) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(sorted_values[lo])
    weight = pos - float(lo)
    return float(sorted_values[lo]) + (float(sorted_values[hi]) - float(sorted_values[lo])) * float(weight)


def make_bucket_counts() -> Dict[str, int]:
    return {
        VOL_BUCKET_LOW: 0,
        VOL_BUCKET_MID: 0,
        VOL_BUCKET_HIGH: 0,
        VOL_BUCKET_NONE: 0,
    }


def bucket_ratios_from_counts(counts: Dict[str, int]) -> Dict[str, float]:
    total = sum(max(0, to_int(v)) for v in counts.values())
    denom = float(max(1, total))
    return {
        VOL_BUCKET_LOW: round(float(max(0, to_int(counts.get(VOL_BUCKET_LOW, 0)))) / denom, 6),
        VOL_BUCKET_MID: round(float(max(0, to_int(counts.get(VOL_BUCKET_MID, 0)))) / denom, 6),
        VOL_BUCKET_HIGH: round(float(max(0, to_int(counts.get(VOL_BUCKET_HIGH, 0)))) / denom, 6),
        VOL_BUCKET_NONE: round(float(max(0, to_int(counts.get(VOL_BUCKET_NONE, 0)))) / denom, 6),
    }


def summarize_values_quantiles(values: List[float]) -> Dict[str, Any]:
    clean = [float(v) for v in values if math.isfinite(to_float(v))]
    clean.sort()
    if not clean:
        return {
            "count": 0,
            "p10": 0.0,
            "p25": 0.0,
            "p50": 0.0,
            "p75": 0.0,
            "p90": 0.0,
        }
    return {
        "count": int(len(clean)),
        "p10": round(quantile_from_sorted_values(clean, 0.10), 8),
        "p25": round(quantile_from_sorted_values(clean, 0.25), 8),
        "p50": round(quantile_from_sorted_values(clean, 0.50), 8),
        "p75": round(quantile_from_sorted_values(clean, 0.75), 8),
        "p90": round(quantile_from_sorted_values(clean, 0.90), 8),
    }


def classify_fixed_vol_bucket(volatility: float, low_cut: float, high_cut: float) -> str:
    if not math.isfinite(to_float(volatility)):
        return VOL_BUCKET_NONE
    value = float(volatility)
    if value <= float(low_cut):
        return VOL_BUCKET_LOW
    if value <= float(high_cut):
        return VOL_BUCKET_MID
    return VOL_BUCKET_HIGH


def classify_percentile_vol_bucket(volatility: float, p_low: float, p_high: float) -> str:
    if (
        not math.isfinite(to_float(volatility))
        or not math.isfinite(to_float(p_low))
        or not math.isfinite(to_float(p_high))
    ):
        return VOL_BUCKET_NONE
    low = float(min(p_low, p_high))
    high = float(max(p_low, p_high))
    value = float(volatility)
    if value <= low:
        return VOL_BUCKET_LOW
    if value <= high:
        return VOL_BUCKET_MID
    return VOL_BUCKET_HIGH


def normalize_ts_range(ts_range: Any) -> Optional[List[int]]:
    if not isinstance(ts_range, list) or len(ts_range) != 2:
        return None
    start_ts = max(0, to_int(ts_range[0]))
    end_ts = max(0, to_int(ts_range[1]))
    if start_ts <= 0 or end_ts <= 0 or end_ts < start_ts:
        return None
    return [int(start_ts), int(end_ts)]


def filter_rows_by_ts_range(
    rows: Any,
    ts_range: Any,
) -> List[Dict[str, Any]]:
    if not isinstance(rows, list):
        return []
    normalized = normalize_ts_range(ts_range)
    if not normalized:
        return [dict(x) for x in rows if isinstance(x, dict)]
    start_ts, end_ts = int(normalized[0]), int(normalized[1])
    out: List[Dict[str, Any]] = []
    for raw in rows:
        if not isinstance(raw, dict):
            continue
        ts_value = max(0, to_int(raw.get("ts", 0)))
        if ts_value <= 0:
            continue
        if start_ts <= ts_value <= end_ts:
            out.append(dict(raw))
    return out


def count_valid_quantile_points(rolling_samples: Any) -> int:
    if not isinstance(rolling_samples, list):
        return 0
    count = 0
    for raw in rolling_samples:
        if not isinstance(raw, dict):
            continue
        p33 = raw.get("p33", None)
        p67 = raw.get("p67", None)
        if (
            p33 is not None
            and p67 is not None
            and math.isfinite(to_float(p33))
            and math.isfinite(to_float(p67))
        ):
            count += 1
    return int(count)


def load_candle_series_volatility_samples(
    dataset_path: Any,
    *,
    execution_range_used: Any = None,
    atr_period: int = 14,
    sample_cap: int = 240000,
) -> List[Dict[str, Any]]:
    if not isinstance(dataset_path, pathlib.Path):
        return []
    path_obj = dataset_path.resolve()
    if not path_obj.exists() or not path_obj.is_file():
        return []
    atr_window = max(2, int(atr_period))
    cap = max(0, int(sample_cap))
    market = extract_upbit_market_from_dataset_name(path_obj.name)
    if not market:
        market = str(path_obj.stem).strip().upper() or "UNKNOWN"
    normalized_range = normalize_ts_range(execution_range_used)
    range_start = int(normalized_range[0]) if normalized_range else 0
    range_end = int(normalized_range[1]) if normalized_range else 0

    candle_rows: List[Dict[str, Any]] = []
    try:
        with path_obj.open("r", encoding="utf-8", errors="ignore", newline="") as fp:
            reader = csv.DictReader(fp)
            if reader.fieldnames is None:
                return []
            for raw in reader:
                if not isinstance(raw, dict):
                    continue
                ts_cell = (
                    raw.get("timestamp")
                    or raw.get("ts")
                    or raw.get("time")
                    or raw.get("datetime")
                    or ""
                )
                ts_raw = parse_timestamp_cell(str(ts_cell))
                if ts_raw is None:
                    continue
                ts_value = normalize_timestamp_ms(int(ts_raw))
                if normalized_range and not (range_start <= ts_value <= range_end):
                    continue
                high = to_float(raw.get("high", float("nan")))
                low = to_float(raw.get("low", float("nan")))
                close = to_float(raw.get("close", float("nan")))
                if (
                    not math.isfinite(high)
                    or not math.isfinite(low)
                    or not math.isfinite(close)
                    or close <= 0.0
                ):
                    continue
                candle_rows.append(
                    {
                        "ts": int(ts_value),
                        "high": float(high),
                        "low": float(low),
                        "close": float(close),
                    }
                )
    except Exception:
        return []

    if not candle_rows:
        return []
    candle_rows.sort(key=lambda item: int(item.get("ts", 0)))
    tr_window: deque = deque()
    tr_sum = 0.0
    prev_close: Optional[float] = None
    output: List[Dict[str, Any]] = []
    for row in candle_rows:
        high = float(row.get("high", 0.0))
        low = float(row.get("low", 0.0))
        close = float(row.get("close", 0.0))
        tr = high - low
        if prev_close is not None and math.isfinite(prev_close):
            tr = max(tr, abs(high - prev_close), abs(low - prev_close))
        tr = max(0.0, float(tr))
        tr_window.append(tr)
        tr_sum += tr
        if len(tr_window) > atr_window:
            tr_sum -= float(tr_window.popleft())
        window_count = len(tr_window)
        if window_count <= 0:
            prev_close = close
            continue
        atr = float(tr_sum) / float(window_count)
        atr_pct = (atr / close) * 100.0 if close > 0.0 else float("nan")
        if math.isfinite(atr_pct):
            output.append(
                {
                    "ts": int(row.get("ts", 0)),
                    "market": market,
                    "regime": "UNKNOWN",
                    "volatility": round(float(atr_pct), 10),
                }
            )
        prev_close = close

    if not output:
        return []
    deduped: List[Dict[str, Any]] = []
    last_ts = -1
    for item in output:
        ts_value = int(item.get("ts", 0))
        if deduped and ts_value == last_ts:
            deduped[-1] = item
        else:
            deduped.append(item)
        last_ts = ts_value
    if cap > 0 and len(deduped) > cap:
        return deduped[:cap]
    return deduped


def build_rolling_percentile_vol_bucket_samples(
    volatility_samples: Any,
    policy: Dict[str, Any],
) -> List[Dict[str, Any]]:
    if not isinstance(volatility_samples, list):
        return []
    resolved_policy = resolve_vol_bucket_pct_policy(policy if isinstance(policy, dict) else {})
    by_market: Dict[str, List[Dict[str, Any]]] = {}
    for raw in volatility_samples:
        if not isinstance(raw, dict):
            continue
        market = str(raw.get("market", "")).strip().upper()
        ts_value = max(0, to_int(raw.get("ts", 0)))
        volatility = to_float(raw.get("volatility", float("nan")))
        if not market or ts_value <= 0 or not math.isfinite(volatility):
            continue
        regime = str(raw.get("regime", "")).strip() or "UNKNOWN"
        by_market.setdefault(market, []).append(
            {
                "market": market,
                "ts": int(ts_value),
                "regime": regime,
                "volatility": float(volatility),
            }
        )
    output_rows: List[Dict[str, Any]] = []
    for market in sorted(by_market.keys()):
        rows = sorted(
            by_market.get(market, []),
            key=lambda item: (int(item.get("ts", 0)), str(item.get("regime", ""))),
        )
        if not rows:
            continue
        rolling_window_values = deque()
        sorted_window_values: List[float] = []
        window_size = int(resolved_policy.get("window_size", 240))
        min_points = int(resolved_policy.get("min_points", max(1, int(math.ceil(window_size * 0.7)))))
        lower_q = float(resolved_policy.get("lower_q", 0.33))
        upper_q = float(resolved_policy.get("upper_q", 0.67))
        fixed_low_cut = float(resolved_policy.get("fixed_low_cut", 1.8))
        fixed_high_cut = float(resolved_policy.get("fixed_high_cut", 3.5))
        for row in rows:
            volatility = float(to_float(row.get("volatility", float("nan"))))
            rolling_window_values.append(volatility)
            bisect.insort(sorted_window_values, volatility)
            if len(rolling_window_values) > window_size:
                removed = float(rolling_window_values.popleft())
                remove_idx = bisect.bisect_left(sorted_window_values, removed)
                if 0 <= remove_idx < len(sorted_window_values):
                    sorted_window_values.pop(remove_idx)
            sample_count = len(sorted_window_values)
            p33_value = None
            p67_value = None
            pct_bucket = VOL_BUCKET_NONE
            if sample_count >= min_points:
                p33_value = quantile_from_sorted_values(sorted_window_values, lower_q)
                p67_value = quantile_from_sorted_values(sorted_window_values, upper_q)
                pct_bucket = classify_percentile_vol_bucket(volatility, p33_value, p67_value)
            fixed_bucket = classify_fixed_vol_bucket(volatility, fixed_low_cut, fixed_high_cut)
            output_rows.append(
                {
                    "market": market,
                    "ts": int(row.get("ts", 0)),
                    "regime": str(row.get("regime", "")).strip() or "UNKNOWN",
                    "volatility": round(volatility, 10),
                    "p33": round(float(p33_value), 10) if p33_value is not None else None,
                    "p67": round(float(p67_value), 10) if p67_value is not None else None,
                    "window_sample_size": int(sample_count),
                    "vol_bucket_fixed": fixed_bucket,
                    "vol_bucket_pct": pct_bucket,
                }
            )
    output_rows.sort(
        key=lambda item: (
            str(item.get("market", "")),
            int(item.get("ts", 0)),
            str(item.get("regime", "")),
        )
    )
    return output_rows


def build_vol_bucket_pct_distribution_summary(
    rolling_samples: Any,
) -> Dict[str, Any]:
    if not isinstance(rolling_samples, list):
        return {
            "sample_count_total": 0,
            "bucket_counts": make_bucket_counts(),
            "bucket_ratios": bucket_ratios_from_counts(make_bucket_counts()),
            "coverage": 0.0,
            "by_market": [],
            "p33_summary": summarize_values_quantiles([]),
            "p67_summary": summarize_values_quantiles([]),
        }

    overall_counts = make_bucket_counts()
    market_counts: Dict[str, Dict[str, int]] = {}
    market_p33_values: Dict[str, List[float]] = {}
    market_p67_values: Dict[str, List[float]] = {}
    all_p33_values: List[float] = []
    all_p67_values: List[float] = []
    for raw in rolling_samples:
        if not isinstance(raw, dict):
            continue
        market = str(raw.get("market", "")).strip().upper()
        bucket = str(raw.get("vol_bucket_pct", VOL_BUCKET_NONE)).strip().upper() or VOL_BUCKET_NONE
        if bucket not in VOL_BUCKET_ORDER:
            bucket = VOL_BUCKET_NONE
        overall_counts[bucket] = overall_counts.get(bucket, 0) + 1
        if market:
            slot = market_counts.setdefault(market, make_bucket_counts())
            slot[bucket] = slot.get(bucket, 0) + 1
            p33 = raw.get("p33", raw.get("vol_bucket_pct_ref_p33", None))
            p67 = raw.get("p67", raw.get("vol_bucket_pct_ref_p67", None))
            if p33 is not None and math.isfinite(to_float(p33)):
                market_p33_values.setdefault(market, []).append(float(p33))
                all_p33_values.append(float(p33))
            if p67 is not None and math.isfinite(to_float(p67)):
                market_p67_values.setdefault(market, []).append(float(p67))
                all_p67_values.append(float(p67))

    market_rows: List[Dict[str, Any]] = []
    for market in sorted(market_counts.keys()):
        counts = market_counts.get(market, make_bucket_counts())
        total = sum(max(0, to_int(v)) for v in counts.values())
        covered = (
            max(0, to_int(counts.get(VOL_BUCKET_LOW, 0)))
            + max(0, to_int(counts.get(VOL_BUCKET_MID, 0)))
            + max(0, to_int(counts.get(VOL_BUCKET_HIGH, 0)))
        )
        market_rows.append(
            {
                "market": market,
                "sample_count": int(total),
                "bucket_counts": counts,
                "bucket_ratios": bucket_ratios_from_counts(counts),
                "coverage": round(float(covered) / float(max(1, total)), 6),
                "none_ratio": round(
                    float(max(0, to_int(counts.get(VOL_BUCKET_NONE, 0)))) / float(max(1, total)),
                    6,
                ),
                "p33_summary": summarize_values_quantiles(market_p33_values.get(market, [])),
                "p67_summary": summarize_values_quantiles(market_p67_values.get(market, [])),
            }
        )

    total_count = sum(max(0, to_int(v)) for v in overall_counts.values())
    covered_total = (
        max(0, to_int(overall_counts.get(VOL_BUCKET_LOW, 0)))
        + max(0, to_int(overall_counts.get(VOL_BUCKET_MID, 0)))
        + max(0, to_int(overall_counts.get(VOL_BUCKET_HIGH, 0)))
    )
    return {
        "sample_count_total": int(total_count),
        "bucket_counts": overall_counts,
        "bucket_ratios": bucket_ratios_from_counts(overall_counts),
        "coverage": round(float(covered_total) / float(max(1, total_count)), 6),
        "none_ratio": round(
            float(max(0, to_int(overall_counts.get(VOL_BUCKET_NONE, 0)))) / float(max(1, total_count)),
            6,
        ),
        "by_market": market_rows,
        "p33_summary": summarize_values_quantiles(all_p33_values),
        "p67_summary": summarize_values_quantiles(all_p67_values),
    }


def build_trade_rows_for_vol_bucket_diagnostics(
    trade_history_samples: Any,
    *,
    fixed_low_cut: float,
    fixed_high_cut: float,
) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    if not isinstance(trade_history_samples, list):
        return rows
    for raw in trade_history_samples:
        if not isinstance(raw, dict):
            continue
        market = str(raw.get("market", "")).strip().upper()
        if not market:
            continue
        entry_time = max(0, to_int(raw.get("entry_time", raw.get("entry_ts", raw.get("ts", 0)))))
        exit_time = max(0, to_int(raw.get("exit_time", 0)))
        ts_value = entry_time if entry_time > 0 else exit_time
        regime = str(raw.get("regime", "")).strip() or "UNKNOWN"
        volatility = to_float(raw.get("volatility", float("nan")))
        fixed_bucket = classify_fixed_vol_bucket(volatility, fixed_low_cut, fixed_high_cut)
        entry_price = to_float(raw.get("entry_price", float("nan")))
        quantity = to_float(raw.get("quantity", float("nan")))
        fee_paid_krw = to_float(raw.get("fee_paid_krw", float("nan")))
        expected_edge_pct = to_float(raw.get("expected_value", float("nan")))
        probabilistic_h5_calibrated = to_float(raw.get("probabilistic_h5_calibrated", float("nan")))
        probabilistic_h5_margin = to_float(raw.get("probabilistic_h5_margin", float("nan")))
        liquidity_score = to_float(raw.get("liquidity_score", float("nan")))
        reward_risk_ratio = to_float(raw.get("reward_risk_ratio", float("nan")))
        signal_filter = to_float(raw.get("signal_filter", float("nan")))
        signal_strength = to_float(raw.get("signal_strength", float("nan")))
        holding_minutes = to_float(raw.get("holding_minutes", float("nan")))
        notional_krw = float("nan")
        if math.isfinite(entry_price) and math.isfinite(quantity):
            notional_krw = abs(float(entry_price) * float(quantity))
        realized_cost_bps_proxy = float("nan")
        if (
            math.isfinite(fee_paid_krw)
            and math.isfinite(notional_krw)
            and float(notional_krw) > 0.0
        ):
            realized_cost_bps_proxy = (float(fee_paid_krw) / float(notional_krw)) * 10000.0
        expected_edge_bps = (
            float(expected_edge_pct) * 10000.0
            if math.isfinite(expected_edge_pct)
            else float("nan")
        )
        profit_loss_krw = to_float(raw.get("profit_loss_krw", 0.0))
        profit_loss_pct = to_float(raw.get("profit_loss_pct", 0.0))
        rows.append(
            {
                "market": market,
                "ts": int(ts_value),
                "entry_time": int(entry_time),
                "exit_time": int(exit_time),
                "regime": regime,
                "volatility": float(volatility) if math.isfinite(volatility) else float("nan"),
                "profit_loss_krw": round(float(profit_loss_krw), 8),
                "profit_loss_pct": round(float(profit_loss_pct), 10),
                "notional_krw": (
                    round(float(notional_krw), 8) if math.isfinite(notional_krw) else None
                ),
                "fee_paid_krw": round(float(fee_paid_krw), 8) if math.isfinite(fee_paid_krw) else None,
                "realized_cost_bps_proxy": (
                    round(float(realized_cost_bps_proxy), 8)
                    if math.isfinite(realized_cost_bps_proxy)
                    else None
                ),
                "estimated_cost_bps": None,
                "expected_edge_after_cost_pct": (
                    round(float(expected_edge_pct), 10)
                    if math.isfinite(expected_edge_pct)
                    else None
                ),
                "expected_edge_after_cost_bps": (
                    round(float(expected_edge_bps), 6)
                    if math.isfinite(expected_edge_bps)
                    else None
                ),
                "expected_value": (
                    round(float(expected_edge_pct), 10)
                    if math.isfinite(expected_edge_pct)
                    else None
                ),
                "probabilistic_h5_calibrated": (
                    round(float(probabilistic_h5_calibrated), 10)
                    if math.isfinite(probabilistic_h5_calibrated)
                    else None
                ),
                "probabilistic_h5_margin": (
                    round(float(probabilistic_h5_margin), 10)
                    if math.isfinite(probabilistic_h5_margin)
                    else None
                ),
                "required_margin": None,
                "margin_slack": (
                    round(float(probabilistic_h5_margin), 10)
                    if math.isfinite(probabilistic_h5_margin)
                    else None
                ),
                "required_ev": 0.0,
                "ev_slack": (
                    round(float(expected_edge_pct), 10)
                    if math.isfinite(expected_edge_pct)
                    else None
                ),
                "liquidity_score": (
                    round(float(liquidity_score), 8)
                    if math.isfinite(liquidity_score)
                    else None
                ),
                "reward_risk_ratio": (
                    round(float(reward_risk_ratio), 8)
                    if math.isfinite(reward_risk_ratio)
                    else None
                ),
                "signal_filter": (
                    round(float(signal_filter), 8)
                    if math.isfinite(signal_filter)
                    else None
                ),
                "signal_strength": (
                    round(float(signal_strength), 8)
                    if math.isfinite(signal_strength)
                    else None
                ),
                "holding_minutes": (
                    round(float(holding_minutes), 8)
                    if math.isfinite(holding_minutes)
                    else None
                ),
                "vol_bucket_fixed": fixed_bucket,
                "vol_bucket_pct": VOL_BUCKET_NONE,
                "vol_bucket_pct_ref_ts": 0,
                "vol_bucket_pct_ref_p33": None,
                "vol_bucket_pct_ref_p67": None,
                "strategy_name": str(raw.get("strategy_name", "")).strip(),
                "entry_archetype": str(raw.get("entry_archetype", "")).strip(),
                "exit_reason": str(raw.get("exit_reason", "")).strip(),
            }
        )
    rows.sort(
        key=lambda item: (
            str(item.get("market", "")),
            int(item.get("ts", 0)),
            str(item.get("regime", "")),
            float(item.get("profit_loss_krw", 0.0)),
        )
    )
    return rows


def build_decision_rows_for_vol_bucket_diagnostics(
    volatility_samples: Any,
    *,
    fixed_low_cut: float,
    fixed_high_cut: float,
) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    if not isinstance(volatility_samples, list):
        return rows
    for raw in volatility_samples:
        if not isinstance(raw, dict):
            continue
        market = str(raw.get("market", "")).strip().upper()
        if not market:
            continue
        ts_value = max(0, to_int(raw.get("ts", 0)))
        if ts_value <= 0:
            continue
        regime = str(raw.get("regime", "")).strip() or "UNKNOWN"
        volatility = to_float(raw.get("volatility", float("nan")))
        fixed_bucket = classify_fixed_vol_bucket(volatility, fixed_low_cut, fixed_high_cut)
        rows.append(
            {
                "market": market,
                "ts": int(ts_value),
                "regime": regime,
                "volatility": float(volatility) if math.isfinite(volatility) else float("nan"),
                "vol_bucket_fixed": fixed_bucket,
                "vol_bucket_pct": VOL_BUCKET_NONE,
                "vol_bucket_pct_ref_ts": 0,
                "vol_bucket_pct_ref_p33": None,
                "vol_bucket_pct_ref_p67": None,
                "event_source": "policy_decision",
            }
        )
    rows.sort(
        key=lambda item: (
            str(item.get("market", "")),
            int(item.get("ts", 0)),
            str(item.get("regime", "")),
        )
    )
    return rows


def attach_percentile_bucket_to_trade_rows(
    trade_rows: Any,
    rolling_samples: Any,
) -> List[Dict[str, Any]]:
    if not isinstance(trade_rows, list):
        return []
    market_rows: Dict[str, List[Dict[str, Any]]] = {}
    if isinstance(rolling_samples, list):
        for raw in rolling_samples:
            if not isinstance(raw, dict):
                continue
            market = str(raw.get("market", "")).strip().upper()
            ts_value = max(0, to_int(raw.get("ts", 0)))
            if not market or ts_value <= 0:
                continue
            market_rows.setdefault(market, []).append(raw)
    market_index: Dict[str, Dict[str, Any]] = {}
    for market in sorted(market_rows.keys()):
        rows = sorted(
            market_rows.get(market, []),
            key=lambda item: int(item.get("ts", 0)),
        )
        market_index[market] = {
            "ts": [int(x.get("ts", 0)) for x in rows],
            "rows": rows,
        }

    out: List[Dict[str, Any]] = []
    for raw_trade in trade_rows:
        if not isinstance(raw_trade, dict):
            continue
        trade = dict(raw_trade)
        trade["vol_bucket_pct"] = VOL_BUCKET_NONE
        trade["vol_bucket_pct_ref_ts"] = 0
        trade["vol_bucket_pct_ref_p33"] = None
        trade["vol_bucket_pct_ref_p67"] = None
        market = str(trade.get("market", "")).strip().upper()
        ts_value = max(0, to_int(trade.get("ts", 0)))
        volatility = to_float(trade.get("volatility", float("nan")))
        if market and ts_value > 0 and math.isfinite(volatility):
            lookup = market_index.get(market, {})
            ts_list = lookup.get("ts", [])
            row_list = lookup.get("rows", [])
            if isinstance(ts_list, list) and isinstance(row_list, list) and ts_list and row_list:
                ref_idx = bisect.bisect_right(ts_list, ts_value) - 1
                if ref_idx >= 0 and ref_idx < len(row_list):
                    ref = row_list[ref_idx]
                    p33 = ref.get("p33", None)
                    p67 = ref.get("p67", None)
                    trade["vol_bucket_pct_ref_ts"] = int(ref.get("ts", 0))
                    trade["vol_bucket_pct_ref_p33"] = (
                        round(float(to_float(p33)), 10)
                        if p33 is not None and math.isfinite(to_float(p33))
                        else None
                    )
                    trade["vol_bucket_pct_ref_p67"] = (
                        round(float(to_float(p67)), 10)
                        if p67 is not None and math.isfinite(to_float(p67))
                        else None
                    )
                    if (
                        str(ref.get("vol_bucket_pct", VOL_BUCKET_NONE)).strip().upper() != VOL_BUCKET_NONE
                        and p33 is not None
                        and p67 is not None
                        and math.isfinite(to_float(p33))
                        and math.isfinite(to_float(p67))
                    ):
                        trade["vol_bucket_pct"] = classify_percentile_vol_bucket(
                            volatility,
                            to_float(p33),
                            to_float(p67),
                        )
        out.append(trade)
    out.sort(
        key=lambda item: (
            str(item.get("market", "")),
            int(item.get("ts", 0)),
            str(item.get("regime", "")),
            float(item.get("profit_loss_krw", 0.0)),
        )
    )
    return out


def build_quantile_exec_coverage_debug(
    *,
    execution_rolling_samples: Any,
    evaluation_rows_mapped: Any,
    execution_range_used: Any,
    evaluation_range_used: Any,
    policy: Optional[Dict[str, Any]] = None,
    fallback_used_source: str = "policy_decisions",
) -> Dict[str, Any]:
    resolved_policy = resolve_vol_bucket_pct_policy(policy if isinstance(policy, dict) else {})
    window_size = int(resolved_policy.get("window_size", 240))
    min_points = int(resolved_policy.get("min_points", max(1, int(math.ceil(float(window_size) * 0.7))))
    )
    rolling_rows = execution_rolling_samples if isinstance(execution_rolling_samples, list) else []
    eval_rows = evaluation_rows_mapped if isinstance(evaluation_rows_mapped, list) else []

    by_market_exec: Dict[str, Dict[str, int]] = {}
    exec_total = 0
    exec_valid = 0
    for raw in rolling_rows:
        if not isinstance(raw, dict):
            continue
        market = str(raw.get("market", "")).strip().upper() or "UNKNOWN"
        slot = by_market_exec.setdefault(
            market,
            {
                "exec_points_total": 0,
                "exec_points_valid_for_quantile": 0,
            },
        )
        slot["exec_points_total"] += 1
        exec_total += 1
        p33 = raw.get("p33", None)
        p67 = raw.get("p67", None)
        valid = bool(
            p33 is not None
            and p67 is not None
            and math.isfinite(to_float(p33))
            and math.isfinite(to_float(p67))
        )
        if valid:
            slot["exec_points_valid_for_quantile"] += 1
            exec_valid += 1

    by_market_eval: Dict[str, Dict[str, int]] = {}
    eval_total = 0
    eval_bucketed = 0
    eval_mapped = 0
    mapping_miss_samples: List[Dict[str, Any]] = []
    mapped_bucket_samples: List[Dict[str, Any]] = []
    for raw in eval_rows:
        if not isinstance(raw, dict):
            continue
        market = str(raw.get("market", "")).strip().upper() or "UNKNOWN"
        slot = by_market_eval.setdefault(
            market,
            {
                "evaluation_events_total": 0,
                "evaluation_events_mapped": 0,
                "evaluation_events_bucketed": 0,
                "evaluation_events_none": 0,
            },
        )
        slot["evaluation_events_total"] += 1
        eval_total += 1
        ref_ts = max(0, to_int(raw.get("vol_bucket_pct_ref_ts", 0)))
        if ref_ts > 0:
            slot["evaluation_events_mapped"] += 1
            eval_mapped += 1
            if len(mapped_bucket_samples) < 10:
                eval_vol = to_float(raw.get("volatility", float("nan")))
                mapped_bucket_samples.append(
                    {
                        "market": market,
                        "eval_ts": max(0, to_int(raw.get("ts", 0))),
                        "mapped_ts": int(ref_ts),
                        "atr_pct": round(float(eval_vol), 10) if math.isfinite(eval_vol) else None,
                        "bucket": str(raw.get("vol_bucket_pct", VOL_BUCKET_NONE)).strip().upper()
                        or VOL_BUCKET_NONE,
                    }
                )
        else:
            if len(mapping_miss_samples) < 10:
                miss_vol = to_float(raw.get("volatility", float("nan")))
                mapping_miss_samples.append(
                    {
                        "market": market,
                        "ts": max(0, to_int(raw.get("ts", 0))),
                        "volatility": round(float(miss_vol), 10) if math.isfinite(miss_vol) else None,
                    }
                )
        bucket = str(raw.get("vol_bucket_pct", VOL_BUCKET_NONE)).strip().upper() or VOL_BUCKET_NONE
        if bucket not in VOL_BUCKET_ORDER:
            bucket = VOL_BUCKET_NONE
        if bucket == VOL_BUCKET_NONE:
            slot["evaluation_events_none"] += 1
        else:
            slot["evaluation_events_bucketed"] += 1
            eval_bucketed += 1

    market_rows: List[Dict[str, Any]] = []
    markets_below_min_points: List[str] = []
    for market in sorted(set(by_market_exec.keys()) | set(by_market_eval.keys())):
        exec_slot = by_market_exec.get(
            market,
            {
                "exec_points_total": 0,
                "exec_points_valid_for_quantile": 0,
            },
        )
        eval_slot = by_market_eval.get(
            market,
            {
                "evaluation_events_total": 0,
                "evaluation_events_mapped": 0,
                "evaluation_events_bucketed": 0,
                "evaluation_events_none": 0,
            },
        )
        eval_total_market = max(1, to_int(eval_slot.get("evaluation_events_total", 0)))
        market_rows.append(
            {
                "market": market,
                "exec_points_total": int(exec_slot.get("exec_points_total", 0)),
                "exec_points_valid_for_quantile": int(exec_slot.get("exec_points_valid_for_quantile", 0)),
                "evaluation_events_total": int(eval_slot.get("evaluation_events_total", 0)),
                "evaluation_events_mapped": int(eval_slot.get("evaluation_events_mapped", 0)),
                "evaluation_events_bucketed": int(eval_slot.get("evaluation_events_bucketed", 0)),
                "evaluation_events_none": int(eval_slot.get("evaluation_events_none", 0)),
                "evaluation_bucket_coverage": round(
                    float(to_int(eval_slot.get("evaluation_events_bucketed", 0)))
                    / float(eval_total_market),
                    6,
                ),
            }
        )
        if int(exec_slot.get("exec_points_total", 0)) < int(min_points):
            markets_below_min_points.append(str(market))

    normalized_execution_range = normalize_ts_range(execution_range_used)
    normalized_evaluation_range = normalize_ts_range(evaluation_range_used)
    eval_total_safe = max(1, int(eval_total))
    return {
        "policy": {
            "window_size": int(window_size),
            "min_points": int(min_points),
            "lower_q": round(to_float(resolved_policy.get("lower_q", 0.33)), 4),
            "upper_q": round(to_float(resolved_policy.get("upper_q", 0.67)), 4),
        },
        "fallback_used_source": str(fallback_used_source or "policy_decisions"),
        "execution_range_used": normalized_execution_range or [],
        "evaluation_range_used": normalized_evaluation_range or [],
        "exec_points_total": int(exec_total),
        "exec_points_valid_for_quantile": int(exec_valid),
        "exec_points_valid_ratio": round(float(exec_valid) / float(max(1, exec_total)), 6),
        "evaluation_events_total": int(eval_total),
        "evaluation_events_mapped": int(eval_mapped),
        "evaluation_events_bucketed": int(eval_bucketed),
        "coverage": round(float(eval_bucketed) / float(eval_total_safe), 6),
        "none_ratio": round(float(max(0, eval_total - eval_bucketed)) / float(eval_total_safe), 6),
        "mapping_miss_count": int(max(0, eval_total - eval_mapped)),
        "mapping_miss_samples": mapping_miss_samples,
        "mapped_bucket_samples": mapped_bucket_samples,
        "markets_below_min_points": markets_below_min_points,
        "insufficient_exec_points_for_quantile": bool(exec_valid <= 0),
        "by_market": market_rows,
    }


def summarize_numeric_values(values: List[float]) -> Dict[str, Any]:
    clean = [float(v) for v in values if math.isfinite(to_float(v))]
    clean.sort()
    if not clean:
        return {
            "count": 0,
            "mean": None,
            "median": None,
            "p10": None,
            "p25": None,
            "p75": None,
            "p90": None,
            "min": None,
            "max": None,
        }
    mean_value = float(sum(clean)) / float(len(clean))
    return {
        "count": int(len(clean)),
        "mean": round(float(mean_value), 10),
        "median": round(quantile_from_sorted_values(clean, 0.50), 10),
        "p10": round(quantile_from_sorted_values(clean, 0.10), 10),
        "p25": round(quantile_from_sorted_values(clean, 0.25), 10),
        "p75": round(quantile_from_sorted_values(clean, 0.75), 10),
        "p90": round(quantile_from_sorted_values(clean, 0.90), 10),
        "min": round(float(clean[0]), 10),
        "max": round(float(clean[-1]), 10),
    }


def build_cell_root_cause_report(
    dataset_profiles: Any,
    *,
    split_name: str,
    top_n_cells: int = 3,
) -> Dict[str, Any]:
    if not isinstance(dataset_profiles, list):
        dataset_profiles = []
    trade_rows: List[Dict[str, Any]] = []
    for profile in dataset_profiles:
        if not isinstance(profile, dict):
            continue
        dataset = str(profile.get("dataset", "")).strip()
        rows = profile.get("trade_rows", [])
        if not isinstance(rows, list):
            continue
        for row in rows:
            if not isinstance(row, dict):
                continue
            market = str(row.get("market", "")).strip().upper()
            regime = str(row.get("regime", "")).strip() or "UNKNOWN"
            bucket = str(row.get("vol_bucket_pct", VOL_BUCKET_NONE)).strip().upper() or VOL_BUCKET_NONE
            if not market or bucket not in (VOL_BUCKET_LOW, VOL_BUCKET_MID, VOL_BUCKET_HIGH):
                continue
            pnl = to_float(row.get("profit_loss_krw", float("nan")))
            if not math.isfinite(pnl):
                continue
            trade_rows.append(
                {
                    "dataset": dataset,
                    **row,
                    "market": market,
                    "regime": regime,
                    "vol_bucket_pct": bucket,
                }
            )

    if not trade_rows:
        return {
            "enabled": False,
            "split_name": str(split_name).strip(),
            "reason": "no_trade_rows_with_percentile_bucket",
            "target_cells": [],
        }

    liquidity_values = [
        to_float(x.get("liquidity_score", float("nan")))
        for x in trade_rows
        if math.isfinite(to_float(x.get("liquidity_score", float("nan"))))
    ]
    liquidity_values.sort()
    liq_q33 = quantile_from_sorted_values(liquidity_values, 0.33) if liquidity_values else None
    liq_q67 = quantile_from_sorted_values(liquidity_values, 0.67) if liquidity_values else None

    def _liq_bucket(score_value: Any) -> str:
        score = to_float(score_value)
        if not math.isfinite(score):
            return "unknown"
        if liq_q33 is None or liq_q67 is None:
            return "unknown"
        if score <= float(min(liq_q33, liq_q67)):
            return "low_liquidity"
        if score <= float(max(liq_q33, liq_q67)):
            return "mid_liquidity"
        return "high_liquidity"

    ev_slack_all = [
        to_float(x.get("ev_slack", float("nan")))
        for x in trade_rows
        if math.isfinite(to_float(x.get("ev_slack", float("nan"))))
    ]
    ev_slack_all.sort()
    ev_slack_p25 = quantile_from_sorted_values(ev_slack_all, 0.25) if ev_slack_all else None
    margin_slack_all = [
        to_float(x.get("margin_slack", float("nan")))
        for x in trade_rows
        if math.isfinite(to_float(x.get("margin_slack", float("nan"))))
    ]
    margin_slack_all.sort()
    margin_slack_p25 = (
        quantile_from_sorted_values(margin_slack_all, 0.25) if margin_slack_all else None
    )

    grouped: Dict[str, List[Dict[str, Any]]] = {}
    for row in trade_rows:
        key = "|".join(
            [
                str(row.get("market", "")).strip().upper(),
                str(row.get("regime", "")).strip().upper(),
                str(row.get("vol_bucket_pct", "")).strip().upper(),
            ]
        )
        grouped.setdefault(key, []).append(row)

    cell_rows: List[Dict[str, Any]] = []
    for key, rows in grouped.items():
        if not rows:
            continue
        first = rows[0]
        market = str(first.get("market", "")).strip().upper()
        regime = str(first.get("regime", "")).strip() or "UNKNOWN"
        vol_bucket_pct = str(first.get("vol_bucket_pct", VOL_BUCKET_NONE)).strip().upper() or VOL_BUCKET_NONE
        pnl_values = [to_float(x.get("profit_loss_krw", 0.0)) for x in rows]
        pnl_sum = float(sum(pnl_values))
        avg_pnl = float(pnl_sum / float(max(1, len(pnl_values))))
        expected_edge_pct_values = [
            to_float(x.get("expected_edge_after_cost_pct", float("nan")))
            for x in rows
            if x.get("expected_edge_after_cost_pct", None) is not None
            and math.isfinite(to_float(x.get("expected_edge_after_cost_pct", float("nan"))))
        ]
        expected_edge_bps_values = [
            to_float(x.get("expected_edge_after_cost_bps", float("nan")))
            for x in rows
            if x.get("expected_edge_after_cost_bps", None) is not None
            and math.isfinite(to_float(x.get("expected_edge_after_cost_bps", float("nan"))))
        ]
        realized_cost_bps_values = [
            to_float(x.get("realized_cost_bps_proxy", float("nan")))
            for x in rows
            if x.get("realized_cost_bps_proxy", None) is not None
            and math.isfinite(to_float(x.get("realized_cost_bps_proxy", float("nan"))))
        ]
        p_calibrated_values = [
            to_float(x.get("probabilistic_h5_calibrated", float("nan")))
            for x in rows
            if x.get("probabilistic_h5_calibrated", None) is not None
            and math.isfinite(to_float(x.get("probabilistic_h5_calibrated", float("nan"))))
        ]
        margin_values = [
            to_float(x.get("probabilistic_h5_margin", float("nan")))
            for x in rows
            if x.get("probabilistic_h5_margin", None) is not None
            and math.isfinite(to_float(x.get("probabilistic_h5_margin", float("nan"))))
        ]
        ev_slack_values = [
            to_float(x.get("ev_slack", float("nan")))
            for x in rows
            if x.get("ev_slack", None) is not None
            and math.isfinite(to_float(x.get("ev_slack", float("nan"))))
        ]
        margin_slack_values = [
            to_float(x.get("margin_slack", float("nan")))
            for x in rows
            if x.get("margin_slack", None) is not None
            and math.isfinite(to_float(x.get("margin_slack", float("nan"))))
        ]
        realized_edge_pct_values = [
            to_float(x.get("profit_loss_pct", float("nan")))
            for x in rows
            if math.isfinite(to_float(x.get("profit_loss_pct", float("nan"))))
        ]

        execution_bucket_counts = {
            "low_liquidity": 0,
            "mid_liquidity": 0,
            "high_liquidity": 0,
            "unknown": 0,
        }
        for row in rows:
            bucket = _liq_bucket(row.get("liquidity_score", None))
            execution_bucket_counts[bucket] = execution_bucket_counts.get(bucket, 0) + 1

        expected_edge_pct_summary = summarize_numeric_values(expected_edge_pct_values)
        ev_slack_summary = summarize_numeric_values(ev_slack_values)
        margin_slack_summary = summarize_numeric_values(margin_slack_values)
        realized_edge_pct_summary = summarize_numeric_values(realized_edge_pct_values)

        mean_expected_pct = expected_edge_pct_summary.get("mean", None)
        mean_realized_pct = realized_edge_pct_summary.get("mean", None)
        median_ev_slack = ev_slack_summary.get("median", None)
        median_margin_slack = margin_slack_summary.get("median", None)
        root_cause = "mixed_or_unclassified"
        root_rationale = (
            "No single dominant mismatch pattern detected from expected-edge/margin/cost proxies."
        )
        if (
            mean_expected_pct is not None
            and math.isfinite(to_float(mean_expected_pct))
            and float(mean_expected_pct) <= 0.0
        ):
            root_cause = "expected_edge_negative_but_entered"
            root_rationale = (
                "Expected edge after cost is non-positive on average while entries were executed."
            )
        elif (
            mean_expected_pct is not None
            and mean_realized_pct is not None
            and math.isfinite(to_float(mean_expected_pct))
            and math.isfinite(to_float(mean_realized_pct))
            and float(mean_expected_pct) > 0.0
            and float(mean_realized_pct) < 0.0
        ):
            root_cause = "execution_cost_or_market_mismatch"
            root_rationale = (
                "Expected edge after cost is positive, but realized edge is negative; "
                "execution/cost or regime slippage mismatch is likely."
            )
        elif (
            ev_slack_p25 is not None
            and median_ev_slack is not None
            and math.isfinite(to_float(ev_slack_p25))
            and math.isfinite(to_float(median_ev_slack))
            and float(median_ev_slack) <= float(ev_slack_p25)
        ):
            root_cause = "low_ev_slack_entry_quality_issue"
            root_rationale = (
                "Entry EV slack sits near the lower quartile of observed trades, suggesting low-quality acceptance."
            )
        elif (
            margin_slack_p25 is not None
            and median_margin_slack is not None
            and math.isfinite(to_float(margin_slack_p25))
            and math.isfinite(to_float(median_margin_slack))
            and float(median_margin_slack) <= float(margin_slack_p25)
        ):
            root_cause = "low_margin_slack_entry_quality_issue"
            root_rationale = (
                "Probability margin slack sits near the lower quartile, indicating thin-confidence entries."
            )

        cell_rows.append(
            {
                "cell_key": key,
                "market": market,
                "regime": regime,
                "vol_bucket_pct": (
                    "vol_low_pct"
                    if vol_bucket_pct == VOL_BUCKET_LOW
                    else (
                        "vol_mid_pct"
                        if vol_bucket_pct == VOL_BUCKET_MID
                        else ("vol_high_pct" if vol_bucket_pct == VOL_BUCKET_HIGH else "none")
                    )
                ),
                "trades_count": int(len(rows)),
                "avg_pnl_krw": round(float(avg_pnl), 8),
                "pnl_sum_krw": round(float(pnl_sum), 8),
                "metrics": {
                    "estimated_cost_bps": {
                        "available": False,
                        "summary": summarize_numeric_values([]),
                        "note": "estimated_cost_bps_not_emitted_by_runtime_trade_samples",
                    },
                    "realized_cost_bps_proxy": {
                        "available": bool(realized_cost_bps_values),
                        "summary": summarize_numeric_values(realized_cost_bps_values),
                    },
                    "expected_edge_after_cost_pct": summarize_numeric_values(expected_edge_pct_values),
                    "expected_edge_after_cost_bps": summarize_numeric_values(expected_edge_bps_values),
                    "p_calibrated_summary": summarize_numeric_values(p_calibrated_values),
                    "margin_summary": summarize_numeric_values(margin_values),
                    "required_ev_summary": {
                        "available": True,
                        "baseline": 0.0,
                    },
                    "required_margin_summary": {
                        "available": False,
                        "baseline": None,
                        "note": "required_margin_not_emitted_by_runtime_trade_samples",
                    },
                    "ev_slack_summary": ev_slack_summary,
                    "margin_slack_summary": margin_slack_summary,
                    "execution_bucket_distribution": execution_bucket_counts,
                    "realized_edge_pct_summary": realized_edge_pct_summary,
                    "post_entry_realized_edge_nmin": {
                        "available": False,
                        "note": "n_minute_realized_edge_not_emitted_in_trade_history_samples",
                    },
                },
                "root_cause_top1": {
                    "label": root_cause,
                    "rationale": root_rationale,
                },
            }
        )

    cell_rows.sort(
        key=lambda item: (
            to_float(item.get("pnl_sum_krw", 0.0)),
            -to_int(item.get("trades_count", 0)),
            str(item.get("market", "")),
            str(item.get("regime", "")),
            str(item.get("vol_bucket_pct", "")),
        )
    )
    negative_cells = [x for x in cell_rows if to_float(x.get("pnl_sum_krw", 0.0)) < 0.0]
    target_base = negative_cells if negative_cells else cell_rows
    top_count = max(1, int(top_n_cells))
    target_cells = target_base[:top_count]
    for idx, row in enumerate(target_cells, start=1):
        row["rank"] = int(idx)

    return {
        "enabled": True,
        "split_name": str(split_name).strip(),
        "source": "trade_rows_with_percentile_bucket",
        "target_selection": {
            "mode": "lowest_total_pnl_cells",
            "top_n_cells": int(top_count),
            "candidate_cells_total": int(len(cell_rows)),
            "negative_cells_total": int(len(negative_cells)),
        },
        "global_context": {
            "trade_count_total": int(len(trade_rows)),
            "liquidity_score_quantiles": summarize_numeric_values(liquidity_values),
            "liquidity_q33": round(float(liq_q33), 10) if liq_q33 is not None else None,
            "liquidity_q67": round(float(liq_q67), 10) if liq_q67 is not None else None,
            "ev_slack_p25": round(float(ev_slack_p25), 10) if ev_slack_p25 is not None else None,
            "margin_slack_p25": (
                round(float(margin_slack_p25), 10) if margin_slack_p25 is not None else None
            ),
        },
        "target_cells": target_cells,
        "all_cells_sorted_by_pnl": cell_rows[: max(12, top_count * 4)],
    }


def summarize_trade_pnl_by_regime_and_bucket(
    trade_rows: Any,
    *,
    bucket_field: str,
    include_none: bool = False,
) -> Dict[str, Any]:
    if not isinstance(trade_rows, list):
        return {
            "trade_count_total": 0,
            "trade_count_used": 0,
            "excluded_trade_count": 0,
            "negative_profit_abs_krw": 0.0,
            "ranging_low_loss_contribution": 0.0,
            "cells": [],
            "by_market": [],
        }

    total_count = 0
    used_count = 0
    excluded_count = 0
    by_cell: Dict[str, Dict[str, Any]] = {}
    by_market_cell: Dict[str, Dict[str, Any]] = {}

    def _cell_key(regime: str, bucket: str) -> str:
        return f"{regime}|{bucket}"

    def _market_cell_key(market: str, regime: str, bucket: str) -> str:
        return f"{market}|{regime}|{bucket}"

    for raw in trade_rows:
        if not isinstance(raw, dict):
            continue
        total_count += 1
        market = str(raw.get("market", "")).strip().upper() or "UNKNOWN"
        regime = str(raw.get("regime", "")).strip() or "UNKNOWN"
        bucket = str(raw.get(bucket_field, VOL_BUCKET_NONE)).strip().upper() or VOL_BUCKET_NONE
        if bucket not in VOL_BUCKET_ORDER:
            bucket = VOL_BUCKET_NONE
        if not include_none and bucket == VOL_BUCKET_NONE:
            excluded_count += 1
            continue
        used_count += 1
        profit_krw = to_float(raw.get("profit_loss_krw", 0.0))
        profit_pct = to_float(raw.get("profit_loss_pct", 0.0))

        key = _cell_key(regime, bucket)
        cell = by_cell.setdefault(
            key,
            {
                "regime": regime,
                "vol_bucket": bucket,
                "trade_count": 0,
                "total_profit_krw": 0.0,
                "total_profit_pct": 0.0,
            },
        )
        cell["trade_count"] += 1
        cell["total_profit_krw"] += float(profit_krw)
        cell["total_profit_pct"] += float(profit_pct)

        market_key = _market_cell_key(market, regime, bucket)
        market_cell = by_market_cell.setdefault(
            market_key,
            {
                "market": market,
                "regime": regime,
                "vol_bucket": bucket,
                "trade_count": 0,
                "total_profit_krw": 0.0,
                "total_profit_pct": 0.0,
            },
        )
        market_cell["trade_count"] += 1
        market_cell["total_profit_krw"] += float(profit_krw)
        market_cell["total_profit_pct"] += float(profit_pct)

    cells: List[Dict[str, Any]] = []
    negative_total_abs = 0.0
    for raw in by_cell.values():
        total_profit_krw = float(to_float(raw.get("total_profit_krw", 0.0)))
        if total_profit_krw < 0.0:
            negative_total_abs += abs(total_profit_krw)
    for raw in by_cell.values():
        trade_count = max(0, to_int(raw.get("trade_count", 0)))
        total_profit_krw = float(to_float(raw.get("total_profit_krw", 0.0)))
        total_profit_pct = float(to_float(raw.get("total_profit_pct", 0.0)))
        cells.append(
            {
                "regime": str(raw.get("regime", "")),
                "vol_bucket": str(raw.get("vol_bucket", VOL_BUCKET_NONE)),
                "trade_count": int(trade_count),
                "avg_profit_krw": round(total_profit_krw / float(max(1, trade_count)), 8),
                "avg_profit_pct": round(total_profit_pct / float(max(1, trade_count)), 10),
                "total_profit_krw": round(total_profit_krw, 8),
                "total_profit_pct": round(total_profit_pct, 10),
                "loss_contribution": round(
                    (abs(total_profit_krw) / float(negative_total_abs))
                    if total_profit_krw < 0.0 and negative_total_abs > 0.0
                    else 0.0,
                    6,
                ),
            }
        )
    cells.sort(
        key=lambda item: (
            to_float(item.get("total_profit_krw", 0.0)),
            -to_int(item.get("trade_count", 0)),
            str(item.get("regime", "")),
            str(item.get("vol_bucket", "")),
        )
    )

    market_grouped_cells: Dict[str, List[Dict[str, Any]]] = {}
    for raw in by_market_cell.values():
        market = str(raw.get("market", "")).strip().upper() or "UNKNOWN"
        trade_count = max(0, to_int(raw.get("trade_count", 0)))
        total_profit_krw = float(to_float(raw.get("total_profit_krw", 0.0)))
        total_profit_pct = float(to_float(raw.get("total_profit_pct", 0.0)))
        market_grouped_cells.setdefault(market, []).append(
            {
                "market": market,
                "regime": str(raw.get("regime", "")),
                "vol_bucket": str(raw.get("vol_bucket", VOL_BUCKET_NONE)),
                "trade_count": int(trade_count),
                "avg_profit_krw": round(total_profit_krw / float(max(1, trade_count)), 8),
                "avg_profit_pct": round(total_profit_pct / float(max(1, trade_count)), 10),
                "total_profit_krw": round(total_profit_krw, 8),
                "total_profit_pct": round(total_profit_pct, 10),
            }
        )

    by_market_rows: List[Dict[str, Any]] = []
    for market in sorted(market_grouped_cells.keys()):
        market_cells = market_grouped_cells.get(market, [])
        market_cells.sort(
            key=lambda item: (
                to_float(item.get("total_profit_krw", 0.0)),
                -to_int(item.get("trade_count", 0)),
                str(item.get("regime", "")),
                str(item.get("vol_bucket", "")),
            )
        )
        market_negative_abs = sum(
            abs(to_float(x.get("total_profit_krw", 0.0)))
            for x in market_cells
            if to_float(x.get("total_profit_krw", 0.0)) < 0.0
        )
        for cell in market_cells:
            cell_total = to_float(cell.get("total_profit_krw", 0.0))
            cell["loss_contribution_in_market"] = round(
                (abs(cell_total) / float(market_negative_abs))
                if cell_total < 0.0 and market_negative_abs > 0.0
                else 0.0,
                6,
            )
        by_market_rows.append(
            {
                "market": market,
                "negative_profit_abs_krw": round(market_negative_abs, 8),
                "cells": market_cells,
            }
        )

    ranging_low_loss = 0.0
    for cell in cells:
        if str(cell.get("regime", "")).upper() == "RANGING" and str(cell.get("vol_bucket", "")) == VOL_BUCKET_LOW:
            ranging_low_loss = to_float(cell.get("loss_contribution", 0.0))
            break

    return {
        "trade_count_total": int(total_count),
        "trade_count_used": int(used_count),
        "excluded_trade_count": int(excluded_count),
        "negative_profit_abs_krw": round(negative_total_abs, 8),
        "ranging_low_loss_contribution": round(ranging_low_loss, 6),
        "cells": cells,
        "by_market": by_market_rows,
    }


def project_pnl_summary_with_bucket_field(
    summary: Dict[str, Any],
    *,
    bucket_field_name: str,
) -> Dict[str, Any]:
    def _format_bucket(bucket_value: Any) -> str:
        token = str(bucket_value).strip().upper() or VOL_BUCKET_NONE
        if bucket_field_name == "vol_bucket_pct":
            if token == VOL_BUCKET_LOW:
                return "vol_low_pct"
            if token == VOL_BUCKET_MID:
                return "vol_mid_pct"
            if token == VOL_BUCKET_HIGH:
                return "vol_high_pct"
            return "none"
        if token == VOL_BUCKET_LOW:
            return "vol_low"
        if token == VOL_BUCKET_MID:
            return "vol_mid"
        if token == VOL_BUCKET_HIGH:
            return "vol_high"
        return "none"

    cells_in = summary.get("cells", []) if isinstance(summary, dict) else []
    by_market_in = summary.get("by_market", []) if isinstance(summary, dict) else []
    cells_out: List[Dict[str, Any]] = []
    if isinstance(cells_in, list):
        for raw in cells_in:
            if not isinstance(raw, dict):
                continue
            row = {k: v for k, v in raw.items() if k != "vol_bucket"}
            row[bucket_field_name] = _format_bucket(raw.get("vol_bucket", VOL_BUCKET_NONE))
            cells_out.append(row)
    by_market_out: List[Dict[str, Any]] = []
    if isinstance(by_market_in, list):
        for raw_market in by_market_in:
            if not isinstance(raw_market, dict):
                continue
            cells = raw_market.get("cells", [])
            projected_cells: List[Dict[str, Any]] = []
            if isinstance(cells, list):
                for raw_cell in cells:
                    if not isinstance(raw_cell, dict):
                        continue
                    row = {k: v for k, v in raw_cell.items() if k != "vol_bucket"}
                    row[bucket_field_name] = _format_bucket(raw_cell.get("vol_bucket", VOL_BUCKET_NONE))
                    projected_cells.append(row)
            out_market = {k: v for k, v in raw_market.items() if k != "cells"}
            out_market["cells"] = projected_cells
            by_market_out.append(out_market)
    output = {k: v for k, v in summary.items()} if isinstance(summary, dict) else {}
    output["cells"] = cells_out
    output["by_market"] = by_market_out
    return output


def build_top_loss_cells_by_market(
    pct_summary: Dict[str, Any],
    *,
    top_n: int = 3,
) -> Dict[str, Any]:
    def _pct_bucket_label(bucket_value: Any) -> str:
        token = str(bucket_value).strip().upper() or VOL_BUCKET_NONE
        if token == VOL_BUCKET_LOW:
            return "vol_low_pct"
        if token == VOL_BUCKET_MID:
            return "vol_mid_pct"
        if token == VOL_BUCKET_HIGH:
            return "vol_high_pct"
        return "none"

    out_markets: List[Dict[str, Any]] = []
    overall_candidates: List[Dict[str, Any]] = []
    by_market = pct_summary.get("by_market", []) if isinstance(pct_summary, dict) else []
    if not isinstance(by_market, list):
        by_market = []
    top_count = max(1, int(top_n))
    for raw_market in by_market:
        if not isinstance(raw_market, dict):
            continue
        market = str(raw_market.get("market", "")).strip().upper() or "UNKNOWN"
        cells = raw_market.get("cells", [])
        if not isinstance(cells, list):
            cells = []
        ordered_cells = sorted(
            [x for x in cells if isinstance(x, dict)],
            key=lambda item: (
                to_float(item.get("total_profit_krw", 0.0)),
                -to_int(item.get("trade_count", 0)),
                str(item.get("regime", "")),
                str(item.get("vol_bucket", "")),
            ),
        )
        negative_cells = [x for x in ordered_cells if to_float(x.get("total_profit_krw", 0.0)) < 0.0]
        selected = negative_cells[:top_count] if negative_cells else ordered_cells[:top_count]
        top_rows: List[Dict[str, Any]] = []
        for rank, cell in enumerate(selected, start=1):
            row = {
                "rank": int(rank),
                "market": market,
                "regime": str(cell.get("regime", "")),
                "vol_bucket_pct": _pct_bucket_label(cell.get("vol_bucket", VOL_BUCKET_NONE)),
                "trade_count": int(to_int(cell.get("trade_count", 0))),
                "avg_profit_krw": round(to_float(cell.get("avg_profit_krw", 0.0)), 8),
                "total_profit_krw": round(to_float(cell.get("total_profit_krw", 0.0)), 8),
                "loss_contribution_in_market": round(
                    to_float(cell.get("loss_contribution_in_market", 0.0)),
                    6,
                ),
            }
            top_rows.append(row)
            overall_candidates.append(row)
        out_markets.append(
            {
                "market": market,
                "top_loss_cells": top_rows,
            }
        )
    overall_candidates.sort(
        key=lambda item: (
            to_float(item.get("total_profit_krw", 0.0)),
            -to_int(item.get("trade_count", 0)),
            str(item.get("market", "")),
            str(item.get("regime", "")),
        )
    )
    return {
        "markets": out_markets,
        "overall_top_loss_cells": overall_candidates[: max(6, top_count * 4)],
    }


def build_vol_bucket_pct_dataset_profile(
    dataset_name: str,
    backtest_result: Dict[str, Any],
    policy: Dict[str, Any],
    *,
    dataset_path: Optional[pathlib.Path] = None,
    execution_range_used: Any = None,
    evaluation_range_used: Any = None,
) -> Dict[str, Any]:
    resolved_policy = resolve_vol_bucket_pct_policy(policy if isinstance(policy, dict) else {})
    decision_execution_samples = filter_rows_by_ts_range(
        backtest_result.get("policy_decision_volatility_samples", []),
        execution_range_used,
    )
    quantile_source = "policy_decisions"
    execution_samples = decision_execution_samples
    rolling_samples = build_rolling_percentile_vol_bucket_samples(
        execution_samples,
        resolved_policy,
    )
    valid_quantile_points = count_valid_quantile_points(rolling_samples)
    if valid_quantile_points <= 0:
        candle_execution_samples = load_candle_series_volatility_samples(
            dataset_path,
            execution_range_used=execution_range_used,
            atr_period=14,
            sample_cap=240000,
        )
        candle_rolling_samples = build_rolling_percentile_vol_bucket_samples(
            candle_execution_samples,
            resolved_policy,
        )
        candle_valid_quantile_points = count_valid_quantile_points(candle_rolling_samples)
        if candle_valid_quantile_points > 0:
            execution_samples = candle_execution_samples
            rolling_samples = candle_rolling_samples
            valid_quantile_points = candle_valid_quantile_points
            quantile_source = "candle_series"
    evaluation_samples = filter_rows_by_ts_range(
        decision_execution_samples,
        evaluation_range_used,
    )
    evaluation_decision_rows = build_decision_rows_for_vol_bucket_diagnostics(
        evaluation_samples,
        fixed_low_cut=float(resolved_policy.get("fixed_low_cut", 1.8)),
        fixed_high_cut=float(resolved_policy.get("fixed_high_cut", 3.5)),
    )
    evaluation_decision_rows = attach_percentile_bucket_to_trade_rows(
        evaluation_decision_rows,
        rolling_samples,
    )
    trade_rows_raw = build_trade_rows_for_vol_bucket_diagnostics(
        backtest_result.get("trade_history_samples", []),
        fixed_low_cut=float(resolved_policy.get("fixed_low_cut", 1.8)),
        fixed_high_cut=float(resolved_policy.get("fixed_high_cut", 3.5)),
    )
    trade_rows = filter_rows_by_ts_range(trade_rows_raw, evaluation_range_used)
    trade_rows = attach_percentile_bucket_to_trade_rows(trade_rows, rolling_samples)
    distribution = build_vol_bucket_pct_distribution_summary(evaluation_decision_rows)
    pnl_fixed = summarize_trade_pnl_by_regime_and_bucket(
        trade_rows,
        bucket_field="vol_bucket_fixed",
        include_none=False,
    )
    pnl_pct = summarize_trade_pnl_by_regime_and_bucket(
        trade_rows,
        bucket_field="vol_bucket_pct",
        include_none=False,
    )
    top_loss_cells = build_top_loss_cells_by_market(pnl_pct, top_n=3)
    quantile_exec_coverage_debug = build_quantile_exec_coverage_debug(
        execution_rolling_samples=rolling_samples,
        evaluation_rows_mapped=evaluation_decision_rows,
        execution_range_used=execution_range_used,
        evaluation_range_used=evaluation_range_used,
        policy=resolved_policy,
        fallback_used_source=quantile_source,
    )
    return {
        "dataset": str(dataset_name),
        "enabled": bool(rolling_samples or evaluation_decision_rows or trade_rows),
        "policy": resolved_policy,
        "quantile_source": quantile_source,
        "policy_decision_exec_points_total": int(len(decision_execution_samples)),
        "quantile_exec_points_total": int(len(execution_samples)),
        "quantile_valid_points_total": int(valid_quantile_points),
        "execution_range_used": normalize_ts_range(execution_range_used) or [],
        "evaluation_range_used": normalize_ts_range(evaluation_range_used) or [],
        "distribution": distribution,
        "pnl_fixed": pnl_fixed,
        "pnl_pct": pnl_pct,
        "top_loss_cells_regime_vol_pct": top_loss_cells,
        "rolling_samples": rolling_samples,
        "distribution_rows": evaluation_decision_rows,
        "trade_rows": trade_rows,
        "quantile_exec_coverage_debug": quantile_exec_coverage_debug,
    }


def build_quarantine_vol_bucket_pct_artifacts(
    dataset_profiles: Any,
    *,
    split_name: str,
) -> Dict[str, Any]:
    if not isinstance(dataset_profiles, list):
        dataset_profiles = []
    valid_profiles: List[Dict[str, Any]] = [
        x for x in dataset_profiles if isinstance(x, dict)
    ]
    profile_names = [str(x.get("dataset", "")).strip() for x in valid_profiles]
    combined_distribution_rows: List[Dict[str, Any]] = []
    combined_trade_rows: List[Dict[str, Any]] = []
    per_dataset_distribution: List[Dict[str, Any]] = []
    per_dataset_fixed: List[Dict[str, Any]] = []
    per_dataset_pct: List[Dict[str, Any]] = []
    quantile_exec_coverage_per_dataset: List[Dict[str, Any]] = []

    for profile in valid_profiles:
        dataset = str(profile.get("dataset", "")).strip()
        distribution = profile.get("distribution", {})
        pnl_fixed = profile.get("pnl_fixed", {})
        pnl_pct = profile.get("pnl_pct", {})
        if isinstance(distribution, dict):
            per_dataset_distribution.append({"dataset": dataset, **distribution})
        if isinstance(pnl_fixed, dict):
            per_dataset_fixed.append({"dataset": dataset, **pnl_fixed})
        if isinstance(pnl_pct, dict):
            per_dataset_pct.append({"dataset": dataset, **pnl_pct})
        distribution_rows = profile.get("distribution_rows", [])
        if isinstance(distribution_rows, list):
            for row in distribution_rows:
                if not isinstance(row, dict):
                    continue
                combined_distribution_rows.append({"dataset": dataset, **row})
        trade_rows = profile.get("trade_rows", [])
        if isinstance(trade_rows, list):
            for row in trade_rows:
                if not isinstance(row, dict):
                    continue
                combined_trade_rows.append({"dataset": dataset, **row})
        quantile_debug = profile.get("quantile_exec_coverage_debug", {})
        if isinstance(quantile_debug, dict):
            quantile_exec_coverage_per_dataset.append(
                {
                    "dataset": dataset,
                    **quantile_debug,
                }
            )

    aggregate_distribution = build_vol_bucket_pct_distribution_summary(combined_distribution_rows)
    aggregate_fixed_raw = summarize_trade_pnl_by_regime_and_bucket(
        combined_trade_rows,
        bucket_field="vol_bucket_fixed",
        include_none=False,
    )
    aggregate_pct_raw = summarize_trade_pnl_by_regime_and_bucket(
        combined_trade_rows,
        bucket_field="vol_bucket_pct",
        include_none=False,
    )
    aggregate_fixed = project_pnl_summary_with_bucket_field(
        aggregate_fixed_raw,
        bucket_field_name="vol_bucket_fixed",
    )
    aggregate_pct = project_pnl_summary_with_bucket_field(
        aggregate_pct_raw,
        bucket_field_name="vol_bucket_pct",
    )
    top_loss_pct = build_top_loss_cells_by_market(aggregate_pct_raw, top_n=3)

    dist_payload = {
        "split_name": str(split_name).strip(),
        "dataset_count": len(valid_profiles),
        "datasets": profile_names,
        "policy": (
            valid_profiles[0].get("policy", {})
            if valid_profiles and isinstance(valid_profiles[0].get("policy", {}), dict)
            else {}
        ),
        "overall": aggregate_distribution,
        "per_dataset": per_dataset_distribution,
    }
    fixed_payload = {
        "split_name": str(split_name).strip(),
        "dataset_count": len(valid_profiles),
        "datasets": profile_names,
        "overall": aggregate_fixed,
        "per_dataset": [
            {
                "dataset": str(x.get("dataset", "")),
                **project_pnl_summary_with_bucket_field(
                    x,
                    bucket_field_name="vol_bucket_fixed",
                ),
            }
            for x in per_dataset_fixed
        ],
    }
    if isinstance(fixed_payload.get("overall", {}), dict):
        fixed_payload["overall"]["ranging_vol_low_loss_contribution"] = round(
            to_float(aggregate_fixed_raw.get("ranging_low_loss_contribution", 0.0)),
            6,
        )
    pct_payload = {
        "split_name": str(split_name).strip(),
        "dataset_count": len(valid_profiles),
        "datasets": profile_names,
        "overall": aggregate_pct,
        "per_dataset": [
            {
                "dataset": str(x.get("dataset", "")),
                **project_pnl_summary_with_bucket_field(
                    x,
                    bucket_field_name="vol_bucket_pct",
                ),
            }
            for x in per_dataset_pct
        ],
    }
    if isinstance(pct_payload.get("overall", {}), dict):
        pct_payload["overall"]["ranging_vol_low_pct_loss_contribution"] = round(
            to_float(aggregate_pct_raw.get("ranging_low_loss_contribution", 0.0)),
            6,
        )
    top_loss_payload = {
        "split_name": str(split_name).strip(),
        "dataset_count": len(valid_profiles),
        "datasets": profile_names,
        "top_loss_cells_by_market": top_loss_pct.get("markets", []),
        "overall_top_loss_cells": top_loss_pct.get("overall_top_loss_cells", []),
    }

    quantile_exec_coverage_by_market: Dict[str, Dict[str, int]] = {}
    quantile_source_counts: Dict[str, int] = {}
    total_exec_points = 0
    total_exec_valid = 0
    total_eval_events = 0
    total_eval_mapped = 0
    total_eval_bucketed = 0
    total_mapping_miss = 0
    mapping_miss_samples: List[Dict[str, Any]] = []
    for row in quantile_exec_coverage_per_dataset:
        if not isinstance(row, dict):
            continue
        total_exec_points += max(0, to_int(row.get("exec_points_total", 0)))
        total_exec_valid += max(0, to_int(row.get("exec_points_valid_for_quantile", 0)))
        total_eval_events += max(0, to_int(row.get("evaluation_events_total", 0)))
        total_eval_mapped += max(0, to_int(row.get("evaluation_events_mapped", 0)))
        total_eval_bucketed += max(0, to_int(row.get("evaluation_events_bucketed", 0)))
        total_mapping_miss += max(0, to_int(row.get("mapping_miss_count", 0)))
        source_token = str(row.get("fallback_used_source", "")).strip() or "unknown"
        quantile_source_counts[source_token] = quantile_source_counts.get(source_token, 0) + 1
        miss_rows = row.get("mapping_miss_samples", [])
        if isinstance(miss_rows, list):
            for miss in miss_rows:
                if len(mapping_miss_samples) >= 10:
                    break
                if isinstance(miss, dict):
                    mapping_miss_samples.append({"dataset": row.get("dataset", ""), **miss})
        market_rows = row.get("by_market", [])
        if isinstance(market_rows, list):
            for market_row in market_rows:
                if not isinstance(market_row, dict):
                    continue
                market = str(market_row.get("market", "")).strip().upper() or "UNKNOWN"
                slot = quantile_exec_coverage_by_market.setdefault(
                    market,
                    {
                        "exec_points_total": 0,
                        "exec_points_valid_for_quantile": 0,
                        "evaluation_events_total": 0,
                        "evaluation_events_mapped": 0,
                        "evaluation_events_bucketed": 0,
                        "evaluation_events_none": 0,
                    },
                )
                slot["exec_points_total"] += max(0, to_int(market_row.get("exec_points_total", 0)))
                slot["exec_points_valid_for_quantile"] += max(
                    0, to_int(market_row.get("exec_points_valid_for_quantile", 0))
                )
                slot["evaluation_events_total"] += max(0, to_int(market_row.get("evaluation_events_total", 0)))
                slot["evaluation_events_mapped"] += max(0, to_int(market_row.get("evaluation_events_mapped", 0)))
                slot["evaluation_events_bucketed"] += max(
                    0, to_int(market_row.get("evaluation_events_bucketed", 0))
                )
                slot["evaluation_events_none"] += max(0, to_int(market_row.get("evaluation_events_none", 0)))

    quantile_exec_coverage_market_rows: List[Dict[str, Any]] = []
    for market in sorted(quantile_exec_coverage_by_market.keys()):
        slot = quantile_exec_coverage_by_market.get(market, {})
        eval_market_total = max(1, to_int(slot.get("evaluation_events_total", 0)))
        quantile_exec_coverage_market_rows.append(
            {
                "market": market,
                "exec_points_total": int(slot.get("exec_points_total", 0)),
                "exec_points_valid_for_quantile": int(slot.get("exec_points_valid_for_quantile", 0)),
                "evaluation_events_total": int(slot.get("evaluation_events_total", 0)),
                "evaluation_events_mapped": int(slot.get("evaluation_events_mapped", 0)),
                "evaluation_events_bucketed": int(slot.get("evaluation_events_bucketed", 0)),
                "evaluation_events_none": int(slot.get("evaluation_events_none", 0)),
                "evaluation_bucket_coverage": round(
                    float(to_int(slot.get("evaluation_events_bucketed", 0))) / float(eval_market_total),
                    6,
                ),
            }
        )
    quantile_exec_coverage_payload = {
        "split_name": str(split_name).strip(),
        "dataset_count": len(valid_profiles),
        "datasets": profile_names,
        "overall": {
            "exec_points_total": int(total_exec_points),
            "exec_points_valid_for_quantile": int(total_exec_valid),
            "exec_points_valid_ratio": round(
                float(total_exec_valid) / float(max(1, total_exec_points)),
                6,
            ),
            "evaluation_events_total": int(total_eval_events),
            "evaluation_events_mapped": int(total_eval_mapped),
            "evaluation_events_bucketed": int(total_eval_bucketed),
            "coverage": round(
                float(total_eval_bucketed) / float(max(1, total_eval_events)),
                6,
            ),
            "none_ratio": round(
                float(max(0, total_eval_events - total_eval_bucketed)) / float(max(1, total_eval_events)),
                6,
            ),
            "mapping_miss_count": int(total_mapping_miss),
            "mapping_miss_samples": mapping_miss_samples,
            "fallback_used_source": (
                next(iter(quantile_source_counts.keys()))
                if len(quantile_source_counts) == 1
                else "mixed"
            ),
            "fallback_used_source_counts": quantile_source_counts,
        },
        "by_market": quantile_exec_coverage_market_rows,
        "per_dataset": quantile_exec_coverage_per_dataset,
    }
    summary = {
        "enabled": bool(valid_profiles),
        "dataset_count": len(valid_profiles),
        "coverage": round(to_float(aggregate_distribution.get("coverage", 0.0)), 6),
        "none_ratio": round(to_float(aggregate_distribution.get("none_ratio", 0.0)), 6),
        "ranging_vol_low_loss_contribution_fixed": round(
            to_float(aggregate_fixed_raw.get("ranging_low_loss_contribution", 0.0)),
            6,
        ),
        "ranging_vol_low_loss_contribution_pct": round(
            to_float(aggregate_pct_raw.get("ranging_low_loss_contribution", 0.0)),
            6,
        ),
        "overall_vol_bucket_pct_ratios": aggregate_distribution.get("bucket_ratios", {}),
    }
    return {
        "distribution_payload": dist_payload,
        "pnl_fixed_payload": fixed_payload,
        "pnl_pct_payload": pct_payload,
        "top_loss_payload": top_loss_payload,
        "quantile_exec_coverage_payload": quantile_exec_coverage_payload,
        "summary": summary,
    }


def resolve_core_dataset_paths(
    split_manifest_payload: Dict[str, Any],
    split_name: str,
    dataset_paths: List[pathlib.Path],
) -> Dict[str, Any]:
    split_token = str(split_name or "").strip().lower()
    if split_token not in ("development", "validation", "quarantine"):
        return {
            "available": False,
            "reason": "split_name_not_supported_for_core_direct",
            "core_dir": "",
            "dataset_paths": [],
            "missing_datasets": [],
        }
    paths = split_manifest_payload.get("paths", {}) if isinstance(split_manifest_payload, dict) else {}
    if not isinstance(paths, dict):
        paths = {}
    core_dir_key = f"{split_token}_core_dir"
    core_dir_value = str(paths.get(core_dir_key, "")).strip()
    if not core_dir_value:
        return {
            "available": False,
            "reason": f"manifest_missing_{core_dir_key}",
            "core_dir": "",
            "dataset_paths": [],
            "missing_datasets": [],
        }
    core_dir = pathlib.Path(core_dir_value)
    if not core_dir.is_absolute():
        core_dir = (pathlib.Path.cwd() / core_dir).resolve()
    if not core_dir.exists() or not core_dir.is_dir():
        return {
            "available": False,
            "reason": "core_dir_missing_or_invalid",
            "core_dir": str(core_dir),
            "dataset_paths": [],
            "missing_datasets": [],
        }
    resolved: List[pathlib.Path] = []
    missing: List[str] = []
    for source_dataset in dataset_paths:
        candidate = (core_dir / source_dataset.name).resolve()
        if candidate.exists() and candidate.is_file():
            resolved.append(candidate)
        else:
            missing.append(source_dataset.name)
    if missing:
        return {
            "available": False,
            "reason": "core_dataset_missing",
            "core_dir": str(core_dir),
            "dataset_paths": [str(x) for x in resolved],
            "missing_datasets": missing,
        }
    return {
        "available": bool(resolved),
        "reason": "" if resolved else "core_dataset_empty",
        "core_dir": str(core_dir),
        "dataset_paths": resolved,
        "missing_datasets": [],
    }


def run_core_window_direct_report(
    exe_path: pathlib.Path,
    config_path: pathlib.Path,
    dataset_paths: List[pathlib.Path],
    require_higher_tf_companions: bool,
    disable_adaptive_state_io: bool,
    phase4_market_cluster_map: Dict[str, str],
    risk_adjusted_score_policy: Dict[str, Any],
    bundle_meta: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    dataset_diagnostics: List[Dict[str, Any]] = []
    adaptive_dataset_profiles: List[Dict[str, Any]] = []
    for dataset_path in dataset_paths:
        result = run_backtest(
            exe_path=exe_path,
            dataset_path=dataset_path,
            config_path=config_path,
            require_higher_tf_companions=bool(require_higher_tf_companions),
            disable_adaptive_state_io=bool(disable_adaptive_state_io),
        )
        rows.append(build_verification_row(dataset_path.name, result))
        dataset_diagnostics.append(
            build_dataset_diagnostics(
                dataset_name=dataset_path.name,
                backtest_result=result,
                phase4_market_cluster_map=phase4_market_cluster_map,
                bundle_meta=bundle_meta if isinstance(bundle_meta, dict) else {},
            )
        )
        adaptive_dataset_profiles.append(
            build_adaptive_dataset_profile(
                dataset_name=dataset_path.name,
                backtest_result=result,
                risk_adjusted_score_policy=risk_adjusted_score_policy,
            )
        )
    summary = summarize_verification_rows(rows)
    aggregate_diagnostics = aggregate_dataset_diagnostics(dataset_diagnostics)
    adaptive_aggregates = aggregate_adaptive_validation(adaptive_dataset_profiles)
    return {
        "available": True,
        "source": "core_dataset_direct",
        "dataset_count": len(rows),
        "datasets": [x["dataset"] for x in rows],
        "aggregates": summary,
        "rows": rows,
        "diagnostics": {
            "aggregate": aggregate_diagnostics,
            "per_dataset": dataset_diagnostics,
        },
        "adaptive_validation": {
            "aggregates": adaptive_aggregates,
            "per_dataset": adaptive_dataset_profiles,
        },
    }


def safe_avg(values: List[float]) -> float:
    return sum(values) / float(len(values)) if values else 0.0


def safe_weighted_avg(values: List[float], weights: List[float]) -> float:
    if not values or not weights or len(values) != len(weights):
        return safe_avg(values)
    positive_weights = [max(0.0, float(x)) for x in weights]
    total_weight = sum(positive_weights)
    if total_weight <= 0.0:
        return safe_avg(values)
    weighted_sum = 0.0
    for value, weight in zip(values, positive_weights):
        weighted_sum += float(value) * weight
    return weighted_sum / total_weight


def to_int(value: Any) -> int:
    try:
        return int(float(value))
    except Exception:
        return 0


def to_float(value: Any) -> float:
    try:
        return float(value)
    except Exception:
        return 0.0


def nested_get(payload: Any, path: List[str], default: Any) -> Any:
    current = payload
    for key in path:
        if not isinstance(current, dict):
            return default
        current = current.get(key)
    if current is None:
        return default
    return current


def load_report_json(path_value: pathlib.Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {}
    try:
        with path_value.open("r", encoding="utf-8") as fp:
            payload = json.load(fp)
    except Exception:
        return {}
    return payload if isinstance(payload, dict) else {}


def clamp_value(value: float, low: float, high: float) -> float:
    return max(float(low), min(float(high), float(value)))


def quantile(values: List[float], q: float) -> float:
    clean = sorted(
        float(x)
        for x in values
        if math.isfinite(to_float(x))
    )
    if not clean:
        return 0.0
    qv = clamp_value(float(q), 0.0, 1.0)
    if len(clean) == 1:
        return float(clean[0])
    pos = qv * float(len(clean) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(clean[lo])
    weight = pos - float(lo)
    return float(clean[lo]) + (float(clean[hi]) - float(clean[lo])) * float(weight)


def dominant_label_from_rows(rows: Any, default: str = "ANY") -> str:
    if not isinstance(rows, list):
        return str(default)
    best_label = str(default)
    best_count = -1
    for row in rows:
        if not isinstance(row, dict):
            continue
        label = str(row.get("label", "")).strip()
        if not label:
            continue
        candidate_total = max(0, to_int(row.get("candidate_total", 0)))
        if candidate_total > best_count or (candidate_total == best_count and label < best_label):
            best_label = label
            best_count = candidate_total
    return best_label if best_count >= 0 else str(default)


def match_bucket_token(value: str, pattern: Any) -> bool:
    token = str(pattern).strip().upper()
    if token in ("", "ANY", "*"):
        return True
    return str(value).strip().upper() == token


def match_fallback_mode_token(fallback_mode: bool, pattern: Any) -> bool:
    token = str(pattern).strip().lower()
    if token in ("", "any", "*"):
        return True
    if token in ("fallback", "fallback_core_zero", "true", "1", "yes"):
        return bool(fallback_mode)
    if token in ("core", "normal", "false", "0", "no"):
        return not bool(fallback_mode)
    return True


def combine_limits(lookup_value: float, dist_value: float, mode: str) -> float:
    token = str(mode).strip().lower()
    if token == "min":
        return float(min(float(lookup_value), float(dist_value)))
    if token == "mean":
        return float((float(lookup_value) + float(dist_value)) * 0.5)
    return float(max(float(lookup_value), float(dist_value)))


def lookup_limit_with_context(
    rows: Any,
    *,
    regime: str,
    volatility_bucket: str,
    liquidity_bucket: str,
    fallback_mode: bool,
    default_limit: float,
) -> float:
    if not isinstance(rows, list):
        return float(default_limit)
    best_score = -1
    best_limits: List[float] = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        rg = str(row.get("regime", "ANY")).strip()
        vb = str(row.get("volatility_bucket", row.get("vol_bucket", "ANY"))).strip()
        lb = str(row.get("liquidity_bucket", row.get("liq_bucket", "ANY"))).strip()
        fb = row.get("fallback_mode", row.get("core_mode", "ANY"))
        if not match_bucket_token(regime, rg):
            continue
        if not match_bucket_token(volatility_bucket, vb):
            continue
        if not match_bucket_token(liquidity_bucket, lb):
            continue
        if not match_fallback_mode_token(fallback_mode, fb):
            continue
        score = 0
        if str(rg).strip().upper() not in ("", "ANY", "*"):
            score += 1
        if str(vb).strip().upper() not in ("", "ANY", "*"):
            score += 1
        if str(lb).strip().upper() not in ("", "ANY", "*"):
            score += 1
        if str(fb).strip().lower() not in ("", "any", "*"):
            score += 1
        limit_value = to_float(row.get("limit", default_limit))
        if score > best_score:
            best_score = score
            best_limits = [float(limit_value)]
        elif score == best_score:
            best_limits.append(float(limit_value))
    if not best_limits:
        return float(default_limit)
    return float(max(best_limits))


def build_risk_score_guard_context(
    aggregate_diagnostics: Dict[str, Any],
    core_window_direct_report: Dict[str, Any],
) -> Dict[str, Any]:
    phase3_diag = nested_get(
        aggregate_diagnostics,
        ["phase3_diagnostics_v2"],
        {},
    )
    if not isinstance(phase3_diag, dict):
        phase3_diag = {}
    regime_rows = phase3_diag.get("pass_rate_by_regime", [])
    vol_rows = phase3_diag.get("pass_rate_by_vol_bucket", [])
    liq_rows = phase3_diag.get("pass_rate_by_liquidity_bucket", [])
    report_candidate_total = max(
        0,
        to_int(
            nested_get(
                phase3_diag,
                ["funnel_breakdown", "candidate_total"],
                0,
            )
        ),
    )
    core_candidate_total = -1
    fallback_core_zero_mode = False
    if isinstance(core_window_direct_report, dict) and bool(core_window_direct_report.get("available", False)):
        core_candidate_total = max(
            0,
            to_int(
                nested_get(
                    core_window_direct_report,
                    ["diagnostics", "aggregate", "phase3_diagnostics_v2", "funnel_breakdown", "candidate_total"],
                    0,
                )
            ),
        )
        fallback_core_zero_mode = bool(core_candidate_total <= 0 and report_candidate_total > 0)
    return {
        "regime": dominant_label_from_rows(regime_rows, "ANY"),
        "volatility_bucket": dominant_label_from_rows(vol_rows, "ANY"),
        "liquidity_bucket": dominant_label_from_rows(liq_rows, "ANY"),
        "fallback_core_zero_mode": bool(fallback_core_zero_mode),
        "core_candidate_total": int(core_candidate_total),
        "report_candidate_total": int(report_candidate_total),
    }


def load_risk_score_guard_history(
    policy: Dict[str, Any],
) -> Dict[str, Any]:
    if not isinstance(policy, dict):
        return {
            "scores": [],
            "ess_values": [],
            "sources": [],
        }
    raw_paths = policy.get("history_report_jsons", [])
    if not isinstance(raw_paths, list):
        raw_paths = []
    split_allowlist = {
        str(x).strip().lower()
        for x in policy.get("history_split_allowlist", ["development", "validation"])
        if str(x).strip()
    }
    split_denylist = {
        str(x).strip().lower()
        for x in policy.get("history_split_denylist", [])
        if str(x).strip()
    }
    if bool(policy.get("exclude_quarantine", True)):
        split_denylist.add("quarantine")
    use_core_window_direct = bool(policy.get("history_use_core_window_direct", True))

    scores: List[float] = []
    ess_values: List[float] = []
    sources: List[Dict[str, Any]] = []
    for raw_path in raw_paths:
        token = str(raw_path).strip()
        if not token:
            continue
        path_obj = pathlib.Path(token)
        if not path_obj.is_absolute():
            path_obj = (pathlib.Path.cwd() / path_obj).resolve()
        report = load_report_json(path_obj)
        if not isinstance(report, dict) or not report:
            continue
        split_name = str(
            nested_get(report, ["protocol_split", "split_name"], "")
        ).strip().lower()
        if split_allowlist and split_name and split_name not in split_allowlist:
            continue
        if split_name in split_denylist:
            continue

        source_mode = "adaptive_validation"
        score = to_float(
            nested_get(
                report,
                ["adaptive_validation", "aggregates", "avg_risk_adjusted_score"],
                0.0,
            )
        )
        ess_value = to_float(
            nested_get(
                report,
                ["adaptive_validation", "aggregates", "loss_tail_aggregate", "tail_metric_ess"],
                0.0,
            )
        )
        if use_core_window_direct and bool(nested_get(report, ["core_window_direct", "available"], False)):
            source_mode = "core_window_direct"
            score = to_float(
                nested_get(
                    report,
                    ["core_window_direct", "adaptive_validation", "aggregates", "avg_risk_adjusted_score"],
                    score,
                )
            )
            ess_value = to_float(
                nested_get(
                    report,
                    ["core_window_direct", "adaptive_validation", "aggregates", "loss_tail_aggregate", "tail_metric_ess"],
                    ess_value,
                )
            )
        if not math.isfinite(float(score)):
            continue
        scores.append(float(score))
        if math.isfinite(float(ess_value)):
            ess_values.append(max(0.0, float(ess_value)))
        sources.append(
            {
                "path": str(path_obj),
                "split_name": split_name,
                "source_mode": source_mode,
                "score": round(float(score), 6),
                "tail_metric_ess": round(max(0.0, float(ess_value)), 6),
            }
        )
    return {
        "scores": scores,
        "ess_values": ess_values,
        "sources": sources,
    }


def evaluate_risk_adjusted_score_guard(
    *,
    avg_score: float,
    min_risk_adjusted_score: float,
    policy: Dict[str, Any],
    context: Dict[str, Any],
    observed_ess: float,
    history_scores: List[float],
    history_ess_values: List[float],
) -> Dict[str, Any]:
    guard_policy = policy if isinstance(policy, dict) else {}
    dynamic_enabled = bool(guard_policy.get("enabled", False))
    regime = str(context.get("regime", "ANY")).strip() or "ANY"
    volatility_bucket = str(context.get("volatility_bucket", "ANY")).strip() or "ANY"
    liquidity_bucket = str(context.get("liquidity_bucket", "ANY")).strip() or "ANY"
    fallback_mode = bool(context.get("fallback_core_zero_mode", False))
    observed_score = float(avg_score)
    if not dynamic_enabled:
        final_limit = float(min_risk_adjusted_score)
        hard_fail = observed_score < final_limit
        return {
            "enabled": False,
            "limit_mode": "absolute_threshold_static",
            "avg_risk_adjusted_score": round(observed_score, 6),
            "lookup_limit": round(final_limit, 6),
            "dist_limit": round(final_limit, 6),
            "dist_quantile_p": 0.0,
            "dist_available": False,
            "uncertainty_component": {
                "enabled": False,
                "metric": "ess",
                "observed": round(max(0.0, float(observed_ess)), 6),
                "lookup_limit": 0.0,
                "dist_limit": 0.0,
                "dist_available": False,
                "final_limit": 0.0,
                "sufficient": True,
                "low_conf_action": "none",
                "dist_weight_when_low_conf": 1.0,
            },
            "final_limit": round(final_limit, 6),
            "hard_fail": bool(hard_fail),
            "soft_fail": False,
            "promotion_hold": False,
            "status": "hard_fail" if hard_fail else "pass",
            "guard_pass": not bool(hard_fail),
            "fallback_core_zero_mode": bool(fallback_mode),
            "history_count": 0,
            "history_ess_count": 0,
        }

    lookup_default = to_float(
        guard_policy.get("lookup_limit_default", min_risk_adjusted_score)
    )
    lookup_limit = lookup_limit_with_context(
        guard_policy.get("lookup_limits", []),
        regime=regime,
        volatility_bucket=volatility_bucket,
        liquidity_bucket=liquidity_bucket,
        fallback_mode=fallback_mode,
        default_limit=lookup_default,
    )
    dist_min_samples = max(1, to_int(guard_policy.get("dist_min_samples", 2)))
    dist_q = clamp_value(to_float(guard_policy.get("dist_quantile_p", 0.3)), 0.0, 1.0)
    clean_history_scores = [
        float(x)
        for x in history_scores
        if math.isfinite(to_float(x))
    ]
    dist_available = len(clean_history_scores) >= dist_min_samples
    dist_limit = float(lookup_limit)
    if dist_available:
        dist_limit = quantile(clean_history_scores, dist_q)

    combine_mode = str(guard_policy.get("combine_mode", "max")).strip().lower() or "max"
    pre_uncertainty_limit = combine_limits(
        float(lookup_limit),
        float(dist_limit),
        combine_mode,
    )

    uncertainty = guard_policy.get("uncertainty_component", {})
    if not isinstance(uncertainty, dict):
        uncertainty = {}
    uncertainty_enabled = bool(uncertainty.get("enabled", True))
    uncertainty_metric = str(uncertainty.get("metric", "ess")).strip().lower() or "ess"
    observed_confidence = max(0.0, float(observed_ess))
    clean_history_ess = [
        max(0.0, float(x))
        for x in history_ess_values
        if math.isfinite(to_float(x))
    ]
    confidence_lookup = 0.0
    confidence_dist = 0.0
    confidence_dist_available = False
    confidence_final_limit = 0.0
    confidence_sufficient = True
    low_conf_action = "none"
    dist_weight_when_low_conf = 1.0
    dist_for_combine = float(dist_limit)

    if uncertainty_enabled and uncertainty_metric in ("ess", "effective_sample_size"):
        confidence_lookup_default = max(
            0.0,
            to_float(uncertainty.get("lookup_limit_default", 1.0)),
        )
        confidence_lookup = lookup_limit_with_context(
            uncertainty.get("lookup_limits", []),
            regime=regime,
            volatility_bucket=volatility_bucket,
            liquidity_bucket=liquidity_bucket,
            fallback_mode=fallback_mode,
            default_limit=confidence_lookup_default,
        )
        confidence_dist = float(confidence_lookup)
        confidence_min_samples = max(1, to_int(uncertainty.get("dist_min_samples", 2)))
        confidence_q = clamp_value(
            to_float(uncertainty.get("dist_quantile_p", 0.2)),
            0.0,
            1.0,
        )
        confidence_dist_available = len(clean_history_ess) >= confidence_min_samples
        if confidence_dist_available:
            confidence_dist = quantile(clean_history_ess, confidence_q)
        confidence_combine_mode = str(
            uncertainty.get("combine_mode", "max")
        ).strip().lower() or "max"
        confidence_final_limit = combine_limits(
            float(confidence_lookup),
            float(confidence_dist),
            confidence_combine_mode,
        )
        confidence_sufficient = observed_confidence >= confidence_final_limit
        low_conf_action = str(
            uncertainty.get("low_conf_action", "promotion_hold")
        ).strip().lower() or "promotion_hold"
        if low_conf_action not in ("promotion_hold", "soft_fail", "hard_fail"):
            low_conf_action = "promotion_hold"
        dist_weight_when_low_conf = clamp_value(
            to_float(uncertainty.get("low_conf_dist_weight", 0.0)),
            0.0,
            1.0,
        )
        if not confidence_sufficient:
            dist_for_combine = float(lookup_limit) + (
                float(dist_limit) - float(lookup_limit)
            ) * float(dist_weight_when_low_conf)

    final_limit = combine_limits(
        float(lookup_limit),
        float(dist_for_combine),
        combine_mode,
    )
    hard_fail = False
    soft_fail = False
    promotion_hold = False
    status = "pass"
    if observed_score < final_limit:
        if uncertainty_enabled and not confidence_sufficient:
            if low_conf_action == "promotion_hold":
                promotion_hold = True
                status = "promotion_hold"
            elif low_conf_action == "soft_fail":
                soft_fail = True
                status = "soft_fail"
            else:
                hard_fail = True
                status = "hard_fail"
        else:
            hard_fail = True
            status = "hard_fail"

    return {
        "enabled": True,
        "limit_mode": "dynamic_lookup_dist_uncertainty",
        "avg_risk_adjusted_score": round(observed_score, 6),
        "lookup_limit": round(float(lookup_limit), 6),
        "dist_limit": round(float(dist_limit), 6),
        "dist_quantile_p": round(float(dist_q), 6),
        "dist_available": bool(dist_available),
        "pre_uncertainty_limit": round(float(pre_uncertainty_limit), 6),
        "uncertainty_component": {
            "enabled": bool(uncertainty_enabled),
            "metric": uncertainty_metric,
            "observed": round(float(observed_confidence), 6),
            "lookup_limit": round(float(confidence_lookup), 6),
            "dist_limit": round(float(confidence_dist), 6),
            "dist_available": bool(confidence_dist_available),
            "final_limit": round(float(confidence_final_limit), 6),
            "sufficient": bool(confidence_sufficient),
            "low_conf_action": low_conf_action,
            "dist_weight_when_low_conf": round(float(dist_weight_when_low_conf), 6),
        },
        "final_limit": round(float(final_limit), 6),
        "hard_fail": bool(hard_fail),
        "soft_fail": bool(soft_fail),
        "promotion_hold": bool(promotion_hold),
        "status": status,
        "guard_pass": not bool(hard_fail),
        "fallback_core_zero_mode": bool(fallback_mode),
        "context": {
            "regime": regime,
            "volatility_bucket": volatility_bucket,
            "liquidity_bucket": liquidity_bucket,
        },
        "history_count": int(len(clean_history_scores)),
        "history_ess_count": int(len(clean_history_ess)),
        "combine_mode": combine_mode,
    }


def normalize_dataset_list(value: Any) -> List[str]:
    if not isinstance(value, list):
        return []
    out: List[str] = []
    for item in value:
        token = str(item).strip()
        if token:
            out.append(token)
    return out


def extract_report_metrics(report: Dict[str, Any]) -> Dict[str, Any]:
    failure_primary_group = nested_get(
        report,
        ["diagnostics", "failure_attribution", "primary_non_execution_group"],
        "",
    )
    if not str(failure_primary_group).strip():
        failure_primary_group = nested_get(
            report,
            ["diagnostics", "aggregate", "primary_non_execution_group", "name"],
            "unknown",
        )
    return {
        "dataset_count": max(0, to_int(report.get("dataset_count", 0))),
        "datasets": normalize_dataset_list(report.get("datasets", [])),
        "avg_profit_factor": to_float(nested_get(report, ["aggregates", "avg_profit_factor"], 0.0)),
        "avg_expectancy_krw": to_float(nested_get(report, ["aggregates", "avg_expectancy_krw"], 0.0)),
        "avg_total_trades": to_float(nested_get(report, ["aggregates", "avg_total_trades"], 0.0)),
        "avg_fee_krw": to_float(nested_get(report, ["aggregates", "avg_fee_krw"], 0.0)),
        "peak_max_drawdown_pct": to_float(
            nested_get(report, ["aggregates", "peak_max_drawdown_pct"], 0.0)
        ),
        "avg_primary_candidate_conversion": to_float(
            nested_get(
                report,
                ["adaptive_validation", "aggregates", "avg_primary_candidate_conversion"],
                0.0,
            )
        ),
        "avg_shadow_candidate_conversion": to_float(
            nested_get(
                report,
                ["adaptive_validation", "aggregates", "avg_shadow_candidate_conversion"],
                0.0,
            )
        ),
        "avg_shadow_candidate_supply_lift": to_float(
            nested_get(
                report,
                ["adaptive_validation", "aggregates", "avg_shadow_candidate_supply_lift"],
                0.0,
            )
        ),
        "overall_gate_pass": bool(report.get("overall_gate_pass", False)),
        "adaptive_verdict": str(
            nested_get(report, ["adaptive_validation", "verdict", "verdict"], "fail")
        ).strip()
        or "fail",
        "primary_non_execution_group": str(failure_primary_group).strip() or "unknown",
        "generated_at": str(report.get("generated_at", "")).strip(),
    }


def build_baseline_comparison(
    current_report: Dict[str, Any],
    baseline_report_path: pathlib.Path,
) -> Dict[str, Any]:
    baseline_report = load_report_json(baseline_report_path)
    output: Dict[str, Any] = {
        "baseline_report_path": str(baseline_report_path),
        "available": bool(baseline_report),
        "reason": "",
    }
    if not baseline_report:
        output["reason"] = "baseline_report_missing_or_invalid"
        return output

    current_metrics = extract_report_metrics(current_report)
    baseline_metrics = extract_report_metrics(baseline_report)
    current_datasets = normalize_dataset_list(current_metrics.get("datasets", []))
    baseline_datasets = normalize_dataset_list(baseline_metrics.get("datasets", []))
    current_dataset_counter = Counter(current_datasets)
    baseline_dataset_counter = Counter(baseline_datasets)
    comparable_dataset_set = (
        bool(current_datasets)
        and bool(baseline_datasets)
        and (current_dataset_counter == baseline_dataset_counter)
    )
    overlap_count = int(sum((current_dataset_counter & baseline_dataset_counter).values()))
    missing_in_current = sorted(list((baseline_dataset_counter - current_dataset_counter).elements()))
    added_in_current = sorted(list((current_dataset_counter - baseline_dataset_counter).elements()))

    deltas = {
        "avg_profit_factor": round(
            to_float(current_metrics.get("avg_profit_factor", 0.0))
            - to_float(baseline_metrics.get("avg_profit_factor", 0.0)),
            6,
        ),
        "avg_expectancy_krw": round(
            to_float(current_metrics.get("avg_expectancy_krw", 0.0))
            - to_float(baseline_metrics.get("avg_expectancy_krw", 0.0)),
            6,
        ),
        "avg_total_trades": round(
            to_float(current_metrics.get("avg_total_trades", 0.0))
            - to_float(baseline_metrics.get("avg_total_trades", 0.0)),
            6,
        ),
        "peak_max_drawdown_pct": round(
            to_float(current_metrics.get("peak_max_drawdown_pct", 0.0))
            - to_float(baseline_metrics.get("peak_max_drawdown_pct", 0.0)),
            6,
        ),
        "avg_fee_krw": round(
            to_float(current_metrics.get("avg_fee_krw", 0.0))
            - to_float(baseline_metrics.get("avg_fee_krw", 0.0)),
            6,
        ),
        "avg_primary_candidate_conversion": round(
            to_float(current_metrics.get("avg_primary_candidate_conversion", 0.0))
            - to_float(baseline_metrics.get("avg_primary_candidate_conversion", 0.0)),
            6,
        ),
        "avg_shadow_candidate_conversion": round(
            to_float(current_metrics.get("avg_shadow_candidate_conversion", 0.0))
            - to_float(baseline_metrics.get("avg_shadow_candidate_conversion", 0.0)),
            6,
        ),
        "avg_shadow_candidate_supply_lift": round(
            to_float(current_metrics.get("avg_shadow_candidate_supply_lift", 0.0))
            - to_float(baseline_metrics.get("avg_shadow_candidate_supply_lift", 0.0)),
            6,
        ),
    }

    primary_candidate_conversion_tolerance = 0.0010
    checks: Dict[str, Any] = {}
    failed_checks: List[str] = []
    if comparable_dataset_set:
        checks = {
            "profit_factor_non_degrade_pass": to_float(current_metrics.get("avg_profit_factor", 0.0))
            >= to_float(baseline_metrics.get("avg_profit_factor", 0.0)),
            "expectancy_non_degrade_pass": to_float(current_metrics.get("avg_expectancy_krw", 0.0))
            >= to_float(baseline_metrics.get("avg_expectancy_krw", 0.0)),
            "drawdown_non_worse_pass": to_float(current_metrics.get("peak_max_drawdown_pct", 0.0))
            <= to_float(baseline_metrics.get("peak_max_drawdown_pct", 0.0)),
            "primary_candidate_conversion_non_degrade_pass": to_float(
                current_metrics.get("avg_primary_candidate_conversion", 0.0)
            )
            >= (
                to_float(baseline_metrics.get("avg_primary_candidate_conversion", 0.0))
                - primary_candidate_conversion_tolerance
            ),
            "shadow_supply_lift_non_degrade_pass": to_float(
                current_metrics.get("avg_shadow_candidate_supply_lift", 0.0)
            )
            >= to_float(baseline_metrics.get("avg_shadow_candidate_supply_lift", 0.0)),
        }
        failed_checks = [key for key, value in checks.items() if not bool(value)]

    output.update(
        {
            "baseline_generated_at": str(baseline_metrics.get("generated_at", "")),
            "current_generated_at": str(current_metrics.get("generated_at", "")),
            "comparable_dataset_set": comparable_dataset_set,
            "dataset_overlap_count": int(overlap_count),
            "dataset_missing_in_current": missing_in_current,
            "dataset_added_in_current": added_in_current,
            "current": current_metrics,
            "baseline": baseline_metrics,
            "deltas": deltas,
            "status_changes": {
                "overall_gate_pass_changed": bool(current_metrics.get("overall_gate_pass", False))
                != bool(baseline_metrics.get("overall_gate_pass", False)),
                "adaptive_verdict_changed": str(current_metrics.get("adaptive_verdict", "fail"))
                != str(baseline_metrics.get("adaptive_verdict", "fail")),
                "primary_non_execution_group_changed": str(
                    current_metrics.get("primary_non_execution_group", "unknown")
                )
                != str(baseline_metrics.get("primary_non_execution_group", "unknown")),
            },
            "non_degradation_contract": {
                "applied": comparable_dataset_set,
                "all_pass": (len(failed_checks) == 0) if comparable_dataset_set else None,
                "checks": checks,
                "tolerances": {
                    "primary_candidate_conversion_abs": primary_candidate_conversion_tolerance
                },
                "failed_checks": failed_checks,
                "reason": "" if comparable_dataset_set else "dataset_set_mismatch",
            },
        }
    )
    return output


def normalized_reason_counts(value: Any) -> Dict[str, int]:
    out: Dict[str, int] = {}
    if not isinstance(value, dict):
        return out
    for k, v in value.items():
        key = str(k).strip()
        if not key:
            continue
        out[key] = out.get(key, 0) + max(0, to_int(v))
    return out


def top_reason_rows(reason_counts: Dict[str, int], limit: int = 5) -> List[Dict[str, Any]]:
    if not reason_counts:
        return []
    ordered = sorted(reason_counts.items(), key=lambda item: (-int(item[1]), item[0]))
    return [{"reason": key, "count": int(count)} for key, count in ordered[: max(1, int(limit))]]


def filter_reason_counts_by_prefix(
    reason_counts: Dict[str, int],
    prefixes: List[str],
    exclude_exact: List[str] = None,
) -> Dict[str, int]:
    out: Dict[str, int] = {}
    norm_prefixes = [str(x).strip().lower() for x in prefixes if str(x).strip()]
    exclude_set = {
        str(x).strip().lower()
        for x in (exclude_exact or [])
        if str(x).strip()
    }
    if not norm_prefixes:
        return out
    for key, value in reason_counts.items():
        reason = str(key).strip()
        if not reason:
            continue
        reason_lc = reason.lower()
        if reason_lc in exclude_set:
            continue
        if any(reason_lc.startswith(prefix) for prefix in norm_prefixes):
            out[reason] = out.get(reason, 0) + max(0, to_int(value))
    return out


def parse_phase3_slice_rows(
    reason_counts: Dict[str, int],
    prefix: str,
) -> List[Dict[str, Any]]:
    candidate_counts: Dict[str, int] = {}
    pass_counts: Dict[str, int] = {}
    token = f"{str(prefix).strip()}::"
    for key, value in reason_counts.items():
        name = str(key).strip()
        if not name.startswith(token):
            continue
        tail = name[len(token) :]
        parts = tail.split("::")
        if len(parts) < 2:
            continue
        label = str("::".join(parts[:-1])).strip()
        metric = str(parts[-1]).strip().lower()
        if not label:
            continue
        count = max(0, to_int(value))
        if metric == "candidate":
            candidate_counts[label] = candidate_counts.get(label, 0) + count
        elif metric == "pass":
            pass_counts[label] = pass_counts.get(label, 0) + count

    labels = sorted(set(candidate_counts.keys()) | set(pass_counts.keys()))
    rows: List[Dict[str, Any]] = []
    for label in labels:
        candidate_total = max(0, to_int(candidate_counts.get(label, 0)))
        pass_total = max(0, to_int(pass_counts.get(label, 0)))
        rows.append(
            {
                "label": label,
                "candidate_total": int(candidate_total),
                "pass_total": int(pass_total),
                "pass_rate": round(float(pass_total) / float(max(1, candidate_total)), 4),
            }
        )
    rows.sort(key=lambda item: (-int(item["candidate_total"]), str(item["label"])))
    return rows


def build_phase3_diagnostics_v2(reason_counts: Dict[str, int]) -> Dict[str, Any]:
    counters = {
        "candidate_total": max(0, to_int(reason_counts.get("candidate_total", 0))),
        "pass_total": max(0, to_int(reason_counts.get("pass_total", 0))),
        "reject_margin_insufficient": max(0, to_int(reason_counts.get("reject_margin_insufficient", 0))),
        "reject_strength_fail": max(0, to_int(reason_counts.get("reject_strength_fail", 0))),
        "reject_expected_value_fail": max(0, to_int(reason_counts.get("reject_expected_value_fail", 0))),
        "reject_expected_edge_negative_count": max(
            0, to_int(reason_counts.get("reject_expected_edge_negative_count", 0))
        ),
        "reject_regime_entry_disabled_count": max(
            0, to_int(reason_counts.get("reject_regime_entry_disabled_count", 0))
        ),
        "reject_quality_filter_fail": max(0, to_int(reason_counts.get("reject_quality_filter_fail", 0))),
        "reject_ev_confidence_low": max(0, to_int(reason_counts.get("reject_ev_confidence_low", 0))),
        "reject_cost_tail_fail": max(0, to_int(reason_counts.get("reject_cost_tail_fail", 0))),
    }
    reject_rows = sorted(
        [
            {"reason": key, "count": int(value)}
            for key, value in counters.items()
            if key.startswith("reject_") and int(value) > 0
        ],
        key=lambda item: (-int(item["count"]), str(item["reason"])),
    )
    pass_rate_by_regime = parse_phase3_slice_rows(reason_counts, "pass_rate_by_regime")
    pass_rate_by_vol_bucket = parse_phase3_slice_rows(reason_counts, "pass_rate_by_vol_bucket")
    pass_rate_by_liquidity_bucket = parse_phase3_slice_rows(reason_counts, "pass_rate_by_liquidity_bucket")
    pass_rate_by_edge_source = parse_phase3_slice_rows(
        reason_counts,
        "pass_rate_edge_regressor_present_vs_fallback",
    )
    pass_rate_by_cost_mode = parse_phase3_slice_rows(reason_counts, "pass_rate_by_cost_mode")

    enabled = bool(
        counters["candidate_total"] > 0
        or counters["pass_total"] > 0
        or reject_rows
        or pass_rate_by_regime
        or pass_rate_by_vol_bucket
        or pass_rate_by_liquidity_bucket
        or pass_rate_by_edge_source
        or pass_rate_by_cost_mode
    )
    return {
        "enabled": bool(enabled),
        "counters": counters,
        "funnel_breakdown": {
            "candidate_total": int(counters["candidate_total"]),
            "pass_total": int(counters["pass_total"]),
            "reject_margin_insufficient": int(counters["reject_margin_insufficient"]),
            "reject_strength_fail": int(counters["reject_strength_fail"]),
            "reject_expected_value_fail": int(counters["reject_expected_value_fail"]),
            "reject_expected_edge_negative_count": int(
                counters["reject_expected_edge_negative_count"]
            ),
            "reject_regime_entry_disabled_count": int(
                counters["reject_regime_entry_disabled_count"]
            ),
            "reject_quality_filter_fail": int(counters["reject_quality_filter_fail"]),
            "reject_ev_confidence_low": int(counters["reject_ev_confidence_low"]),
            "reject_cost_tail_fail": int(counters["reject_cost_tail_fail"]),
        },
        "pass_rate_by_regime": pass_rate_by_regime,
        "pass_rate_by_vol_bucket": pass_rate_by_vol_bucket,
        "pass_rate_by_liquidity_bucket": pass_rate_by_liquidity_bucket,
        "pass_rate_edge_regressor_present_vs_fallback": pass_rate_by_edge_source,
        "pass_rate_by_cost_mode": pass_rate_by_cost_mode,
        "top3_bottlenecks": reject_rows[:3],
    }


def build_phase3_pass_ev_distribution(
    pass_ev_samples: Any,
    *,
    sample_cap: int = 5000,
) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    if isinstance(pass_ev_samples, list):
        for row in pass_ev_samples:
            if not isinstance(row, dict):
                continue
            expected_bps = to_float(row.get("expected_edge_after_cost_bps", 0.0))
            expected_pct = to_float(row.get("expected_edge_after_cost_pct", expected_bps / 10000.0))
            if not math.isfinite(expected_bps):
                continue
            rows.append(
                {
                    "ts": max(0, to_int(row.get("ts", 0))),
                    "market": str(row.get("market", "")).strip(),
                    "strategy": str(row.get("strategy", "")).strip(),
                    "selected": bool(row.get("selected", False)),
                    "reason": str(row.get("reason", "")).strip(),
                    "expected_edge_after_cost_bps": round(float(expected_bps), 6),
                    "expected_edge_after_cost_pct": round(float(expected_pct), 10),
                    "expected_edge_calibrated_bps": round(
                        to_float(row.get("expected_edge_calibrated_bps", expected_bps)),
                        6,
                    ),
                    "expected_edge_used_for_gate_bps": round(
                        to_float(row.get("expected_edge_used_for_gate_bps", expected_bps)),
                        6,
                    ),
                    "cost_bps_estimate_used": round(
                        to_float(row.get("cost_bps_estimate_used", 0.0)),
                        6,
                    ),
                    "edge_semantics": str(row.get("edge_semantics", "unknown")).strip().lower() or "unknown",
                    "root_cost_model_enabled_configured": bool(
                        row.get("root_cost_model_enabled_configured", False)
                    ),
                    "phase3_cost_model_enabled_configured": bool(
                        row.get("phase3_cost_model_enabled_configured", False)
                    ),
                    "root_cost_model_enabled_effective": bool(
                        row.get("root_cost_model_enabled_effective", False)
                    ),
                    "phase3_cost_model_enabled_effective": bool(
                        row.get("phase3_cost_model_enabled_effective", False)
                    ),
                    "edge_semantics_guard_violation": bool(
                        row.get("edge_semantics_guard_violation", False)
                    ),
                    "edge_semantics_guard_forced_off": bool(
                        row.get("edge_semantics_guard_forced_off", False)
                    ),
                    "edge_semantics_guard_action": str(
                        row.get("edge_semantics_guard_action", "none")
                    ).strip()
                    or "none",
                }
            )
    total_count = len(rows)
    rows.sort(
        key=lambda item: (
            int(item.get("ts", 0)),
            str(item.get("market", "")),
            str(item.get("strategy", "")),
            -int(bool(item.get("selected", False))),
        )
    )
    cap = max(1, int(sample_cap))
    sampled_rows = rows[:cap]
    sampled_bps = [to_float(x.get("expected_edge_after_cost_bps", 0.0)) for x in sampled_rows]
    selected_bps = [
        to_float(x.get("expected_edge_after_cost_bps", 0.0))
        for x in sampled_rows
        if bool(x.get("selected", False))
    ]

    def _quantiles(values: List[float]) -> Dict[str, float]:
        if not values:
            return {
                "p10": 0.0,
                "p25": 0.0,
                "p50": 0.0,
                "p75": 0.0,
                "p90": 0.0,
            }
        return {
            "p10": round(quantile(values, 0.10), 6),
            "p25": round(quantile(values, 0.25), 6),
            "p50": round(quantile(values, 0.50), 6),
            "p75": round(quantile(values, 0.75), 6),
            "p90": round(quantile(values, 0.90), 6),
        }

    quantiles_bps = _quantiles(sampled_bps)
    quantiles_pct = {
        key: round(float(value) / 10000.0, 10)
        for key, value in quantiles_bps.items()
    }
    selected_quantiles_bps = _quantiles(selected_bps)
    selected_quantiles_pct = {
        key: round(float(value) / 10000.0, 10)
        for key, value in selected_quantiles_bps.items()
    }

    return {
        "enabled": bool(total_count > 0),
        "source": "policy_decisions_backtest_jsonl",
        "sample_cap": int(cap),
        "sample_size_total": int(total_count),
        "sample_size_used": int(len(sampled_rows)),
        "sample_truncated": bool(total_count > len(sampled_rows)),
        "selected_sample_size_used": int(len(selected_bps)),
        "quantiles_bps": quantiles_bps,
        "quantiles_pct": quantiles_pct,
        "selected_quantiles_bps": selected_quantiles_bps,
        "selected_quantiles_pct": selected_quantiles_pct,
        "samples_bps": [round(float(x), 6) for x in sampled_bps],
        "samples": sampled_rows,
    }


def collect_phase3_pass_ev_samples(dataset_diagnostics: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for item in dataset_diagnostics:
        if not isinstance(item, dict):
            continue
        dataset_name = str(item.get("dataset", "")).strip()
        phase3_dist = item.get("phase3_pass_ev_distribution", {})
        if not isinstance(phase3_dist, dict):
            continue
        samples = phase3_dist.get("samples", [])
        if not isinstance(samples, list):
            continue
        for sample in samples:
            if not isinstance(sample, dict):
                continue
            row = dict(sample)
            row["dataset"] = dataset_name
            rows.append(row)
    rows.sort(
        key=lambda item: (
            int(item.get("ts", 0)),
            str(item.get("market", "")),
            str(item.get("strategy", "")),
            -int(bool(item.get("selected", False))),
        )
    )
    return rows


def build_runtime_cost_semantics_debug(
    dataset_diagnostics: List[Dict[str, Any]],
    *,
    sample_cap: int = 50,
) -> Dict[str, Any]:
    rows = collect_phase3_pass_ev_samples(dataset_diagnostics)
    edge_semantics_counts: Dict[str, int] = {"net": 0, "gross": 0, "unknown": 0}
    guard_action_counts: Dict[str, int] = {}
    cost_values: List[float] = []
    root_cfg_true = 0
    phase3_cfg_true = 0
    root_eff_true = 0
    phase3_eff_true = 0
    guard_violation_count = 0
    guard_forced_off_count = 0

    for row in rows:
        if not isinstance(row, dict):
            continue
        edge_semantics = str(row.get("edge_semantics", "unknown")).strip().lower()
        if edge_semantics not in ("net", "gross"):
            edge_semantics = "unknown"
        edge_semantics_counts[edge_semantics] = int(edge_semantics_counts.get(edge_semantics, 0)) + 1
        if bool(row.get("root_cost_model_enabled_configured", False)):
            root_cfg_true += 1
        if bool(row.get("phase3_cost_model_enabled_configured", False)):
            phase3_cfg_true += 1
        if bool(row.get("root_cost_model_enabled_effective", False)):
            root_eff_true += 1
        if bool(row.get("phase3_cost_model_enabled_effective", False)):
            phase3_eff_true += 1
        if bool(row.get("edge_semantics_guard_violation", False)):
            guard_violation_count += 1
        if bool(row.get("edge_semantics_guard_forced_off", False)):
            guard_forced_off_count += 1
        guard_action = str(row.get("edge_semantics_guard_action", "none")).strip().lower() or "none"
        guard_action_counts[guard_action] = int(guard_action_counts.get(guard_action, 0)) + 1
        cost_bps = to_float(row.get("cost_bps_estimate_used", 0.0))
        if math.isfinite(cost_bps):
            cost_values.append(float(cost_bps))

    cap = max(1, int(sample_cap))
    sample_rows: List[Dict[str, Any]] = []
    for row in rows[:cap]:
        if not isinstance(row, dict):
            continue
        sample_rows.append(
            {
                "dataset": str(row.get("dataset", "")).strip(),
                "ts": max(0, to_int(row.get("ts", 0))),
                "market": str(row.get("market", "")).strip(),
                "strategy": str(row.get("strategy", "")).strip(),
                "selected": bool(row.get("selected", False)),
                "reason": str(row.get("reason", "")).strip(),
                "expected_edge_calibrated_bps": round(
                    to_float(row.get("expected_edge_calibrated_bps", 0.0)),
                    6,
                ),
                "expected_edge_used_for_gate_bps": round(
                    to_float(row.get("expected_edge_used_for_gate_bps", 0.0)),
                    6,
                ),
                "cost_bps_estimate_used": round(
                    to_float(row.get("cost_bps_estimate_used", 0.0)),
                    6,
                ),
                "edge_semantics": str(row.get("edge_semantics", "unknown")).strip().lower() or "unknown",
                "root_cost_model_enabled_configured": bool(
                    row.get("root_cost_model_enabled_configured", False)
                ),
                "phase3_cost_model_enabled_configured": bool(
                    row.get("phase3_cost_model_enabled_configured", False)
                ),
                "root_cost_model_enabled_effective": bool(
                    row.get("root_cost_model_enabled_effective", False)
                ),
                "phase3_cost_model_enabled_effective": bool(
                    row.get("phase3_cost_model_enabled_effective", False)
                ),
                "edge_semantics_guard_violation": bool(
                    row.get("edge_semantics_guard_violation", False)
                ),
                "edge_semantics_guard_forced_off": bool(
                    row.get("edge_semantics_guard_forced_off", False)
                ),
                "edge_semantics_guard_action": str(
                    row.get("edge_semantics_guard_action", "none")
                ).strip()
                or "none",
            }
        )

    dominant_semantics = "unknown"
    if rows:
        dominant_semantics = max(
            edge_semantics_counts.items(),
            key=lambda item: (int(item[1]), item[0]),
        )[0]
    mean_cost_bps = safe_avg(cost_values)
    max_cost_bps = max(cost_values) if cost_values else 0.0
    min_cost_bps = min(cost_values) if cost_values else 0.0
    p50_cost_bps = quantile(cost_values, 0.50) if cost_values else 0.0
    p90_cost_bps = quantile(cost_values, 0.90) if cost_values else 0.0

    return {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "enabled": bool(len(rows) > 0),
        "source": "policy_decisions_backtest_jsonl",
        "sample_cap": int(cap),
        "sample_size_total": int(len(rows)),
        "sample_size_used": int(len(sample_rows)),
        "sample_truncated": bool(len(rows) > len(sample_rows)),
        "dominant_edge_semantics": dominant_semantics,
        "edge_semantics_counts": edge_semantics_counts,
        "runtime_cost_flags_summary": {
            "root_cost_model_enabled_configured_true_count": int(root_cfg_true),
            "phase3_cost_model_enabled_configured_true_count": int(phase3_cfg_true),
            "root_cost_model_enabled_effective_true_count": int(root_eff_true),
            "phase3_cost_model_enabled_effective_true_count": int(phase3_eff_true),
            "edge_semantics_guard_violation_count": int(guard_violation_count),
            "edge_semantics_guard_forced_off_count": int(guard_forced_off_count),
            "edge_semantics_guard_action_counts": guard_action_counts,
        },
        "cost_bps_estimate_summary": {
            "sample_count": int(len(cost_values)),
            "mean_bps": round(float(mean_cost_bps), 6),
            "max_bps": round(float(max_cost_bps), 6),
            "min_bps": round(float(min_cost_bps), 6),
            "p50_bps": round(float(p50_cost_bps), 6),
            "p90_bps": round(float(p90_cost_bps), 6),
        },
        "samples": sample_rows,
    }


def build_semantics_lock_report(
    bundle_meta: Dict[str, Any],
    runtime_cost_semantics_debug: Dict[str, Any],
    filled_sample_count: Optional[int] = None,
) -> Dict[str, Any]:
    bundle_edge_semantics = str(bundle_meta.get("edge_semantics", "unknown")).strip().lower()
    if bundle_edge_semantics not in ("net", "gross"):
        bundle_edge_semantics = "unknown"
    bundle_root_cfg = bool(bundle_meta.get("root_cost_model_enabled_configured", False))
    bundle_phase3_cfg = bool(bundle_meta.get("phase3_cost_model_enabled_configured", False))
    runtime_flags = (
        runtime_cost_semantics_debug.get("runtime_cost_flags_summary", {})
        if isinstance(runtime_cost_semantics_debug, dict)
        else {}
    )
    if not isinstance(runtime_flags, dict):
        runtime_flags = {}
    cost_summary = (
        runtime_cost_semantics_debug.get("cost_bps_estimate_summary", {})
        if isinstance(runtime_cost_semantics_debug, dict)
        else {}
    )
    if not isinstance(cost_summary, dict):
        cost_summary = {}
    semantics_counts = (
        runtime_cost_semantics_debug.get("edge_semantics_counts", {})
        if isinstance(runtime_cost_semantics_debug, dict)
        else {}
    )
    if not isinstance(semantics_counts, dict):
        semantics_counts = {}
    sample_count = max(0, to_int(runtime_cost_semantics_debug.get("sample_size_total", 0)))
    filled_samples = (
        max(0, to_int(filled_sample_count))
        if filled_sample_count is not None
        else None
    )
    dominant_runtime_semantics = str(
        runtime_cost_semantics_debug.get("dominant_edge_semantics", "unknown")
    ).strip().lower() or "unknown"
    root_effective_true_count = max(
        0,
        to_int(runtime_flags.get("root_cost_model_enabled_effective_true_count", 0)),
    )
    phase3_effective_true_count = max(
        0,
        to_int(runtime_flags.get("phase3_cost_model_enabled_effective_true_count", 0)),
    )
    root_cfg_true_count = max(
        0,
        to_int(runtime_flags.get("root_cost_model_enabled_configured_true_count", 0)),
    )
    phase3_cfg_true_count = max(
        0,
        to_int(runtime_flags.get("phase3_cost_model_enabled_configured_true_count", 0)),
    )
    guard_violation_count = max(
        0,
        to_int(runtime_flags.get("edge_semantics_guard_violation_count", 0)),
    )
    guard_forced_off_count = max(
        0,
        to_int(runtime_flags.get("edge_semantics_guard_forced_off_count", 0)),
    )
    mean_cost_bps = to_float(cost_summary.get("mean_bps", 0.0))
    max_cost_bps = to_float(cost_summary.get("max_bps", 0.0))
    if not math.isfinite(mean_cost_bps):
        mean_cost_bps = 0.0
    if not math.isfinite(max_cost_bps):
        max_cost_bps = 0.0

    if filled_samples is not None and filled_samples <= 0:
        return {
            "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
            "edge_semantics": (bundle_edge_semantics or "unknown"),
            "status": "SKIPPED_NO_SAMPLES",
            "skip_reason": "no_filled_samples",
            "messages": [
                "Semantics lock skipped: no filled trades/samples were observed in this run."
            ],
            "bundle": {
                "bundle_path": str(bundle_meta.get("bundle_path", "")),
                "bundle_version": str(bundle_meta.get("bundle_version", "")),
                "edge_semantics": bundle_edge_semantics,
                "root_cost_model_enabled_configured": bool(bundle_root_cfg),
                "phase3_cost_model_enabled_configured": bool(bundle_phase3_cfg),
            },
            "runtime": {
                "filled_sample_count": int(filled_samples),
                "sample_count": int(sample_count),
                "dominant_edge_semantics": dominant_runtime_semantics,
                "edge_semantics_counts": semantics_counts,
                "root_cost_model_enabled_configured_true_count": int(root_cfg_true_count),
                "phase3_cost_model_enabled_configured_true_count": int(phase3_cfg_true_count),
                "root_cost_model_enabled_effective_true_count": int(root_effective_true_count),
                "phase3_cost_model_enabled_effective_true_count": int(phase3_effective_true_count),
                "edge_semantics_guard_violation_count": int(guard_violation_count),
                "edge_semantics_guard_forced_off_count": int(guard_forced_off_count),
                "edge_semantics_guard_action_counts": (
                    runtime_flags.get("edge_semantics_guard_action_counts", {})
                    if isinstance(runtime_flags.get("edge_semantics_guard_action_counts", {}), dict)
                    else {}
                ),
                "cost_bps_estimate_mean": float(mean_cost_bps),
                "cost_bps_estimate_max": float(max_cost_bps),
            },
        }

    if sample_count <= 0:
        return {
            "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
            "edge_semantics": (bundle_edge_semantics or "unknown"),
            "status": "SKIPPED_NO_SAMPLES",
            "skip_reason": "no_runtime_semantics_samples",
            "messages": [
                "Semantics lock skipped: no runtime semantics/cost samples were observed in this run."
            ],
            "bundle": {
                "bundle_path": str(bundle_meta.get("bundle_path", "")),
                "bundle_version": str(bundle_meta.get("bundle_version", "")),
                "edge_semantics": bundle_edge_semantics,
                "root_cost_model_enabled_configured": bool(bundle_root_cfg),
                "phase3_cost_model_enabled_configured": bool(bundle_phase3_cfg),
            },
            "runtime": {
                "sample_count": 0,
                "dominant_edge_semantics": dominant_runtime_semantics,
                "edge_semantics_counts": semantics_counts,
                "root_cost_model_enabled_configured_true_count": int(root_cfg_true_count),
                "phase3_cost_model_enabled_configured_true_count": int(phase3_cfg_true_count),
                "root_cost_model_enabled_effective_true_count": int(root_effective_true_count),
                "phase3_cost_model_enabled_effective_true_count": int(phase3_effective_true_count),
                "edge_semantics_guard_violation_count": int(guard_violation_count),
                "edge_semantics_guard_forced_off_count": int(guard_forced_off_count),
                "edge_semantics_guard_action_counts": (
                    runtime_flags.get("edge_semantics_guard_action_counts", {})
                    if isinstance(runtime_flags.get("edge_semantics_guard_action_counts", {}), dict)
                    else {}
                ),
            },
            "cost_bps_estimate_summary": {
                "sample_count": int(max(0, to_int(cost_summary.get("sample_count", 0)))),
                "mean_bps": round(float(mean_cost_bps), 6),
                "max_bps": round(float(max_cost_bps), 6),
                "min_bps": round(float(to_float(cost_summary.get("min_bps", 0.0))), 6),
                "p50_bps": round(float(to_float(cost_summary.get("p50_bps", 0.0))), 6),
                "p90_bps": round(float(to_float(cost_summary.get("p90_bps", 0.0))), 6),
            },
            "violations": [],
            "warnings": [],
        }

    violations: List[str] = []
    warnings: List[str] = []
    target_semantics = bundle_edge_semantics
    if target_semantics == "unknown":
        target_semantics = dominant_runtime_semantics
    if target_semantics != "net":
        violations.append("edge_semantics_not_net")
    if sample_count <= 0:
        violations.append("runtime_semantics_samples_missing")

    gross_count = max(0, to_int(semantics_counts.get("gross", 0)))
    unknown_count = max(0, to_int(semantics_counts.get("unknown", 0)))
    if gross_count > 0:
        violations.append("runtime_contains_gross_semantics")
    if unknown_count > 0:
        warnings.append("runtime_contains_unknown_semantics")

    if root_effective_true_count > 0 or phase3_effective_true_count > 0:
        violations.append("runtime_cost_model_effective_on_under_net")

    cost_tolerance_bps = 1e-6
    if target_semantics == "net" and max_cost_bps > cost_tolerance_bps:
        violations.append("runtime_cost_bps_non_zero_under_net")

    if target_semantics == "net" and (bundle_root_cfg or bundle_phase3_cfg):
        warnings.append("bundle_cost_model_configured_true_under_net")
    if target_semantics == "net" and (root_cfg_true_count > 0 or phase3_cfg_true_count > 0):
        warnings.append("runtime_cost_model_configured_true_under_net")
    if guard_violation_count > 0:
        warnings.append("runtime_guard_violation_detected")
    if guard_forced_off_count > 0:
        warnings.append("runtime_guard_forced_off_applied")

    status = "OK" if not violations else "VIOLATION"
    messages: List[str] = []
    if status == "OK":
        messages.append("NET semantics lock verified: runtime effective cost path is OFF and cost_bps is near zero.")
    else:
        messages.append("Semantics lock violation detected. Review violations and runtime evidence.")

    return {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "edge_semantics": target_semantics or "unknown",
        "status": status,
        "messages": messages,
        "bundle": {
            "bundle_path": str(bundle_meta.get("bundle_path", "")),
            "bundle_version": str(bundle_meta.get("bundle_version", "")),
            "edge_semantics": bundle_edge_semantics,
            "root_cost_model_enabled_configured": bool(bundle_root_cfg),
            "phase3_cost_model_enabled_configured": bool(bundle_phase3_cfg),
        },
        "runtime": {
            "sample_count": int(sample_count),
            "dominant_edge_semantics": dominant_runtime_semantics,
            "edge_semantics_counts": semantics_counts,
            "root_cost_model_enabled_configured_true_count": int(root_cfg_true_count),
            "phase3_cost_model_enabled_configured_true_count": int(phase3_cfg_true_count),
            "root_cost_model_enabled_effective_true_count": int(root_effective_true_count),
            "phase3_cost_model_enabled_effective_true_count": int(phase3_effective_true_count),
            "edge_semantics_guard_violation_count": int(guard_violation_count),
            "edge_semantics_guard_forced_off_count": int(guard_forced_off_count),
            "edge_semantics_guard_action_counts": (
                runtime_flags.get("edge_semantics_guard_action_counts", {})
                if isinstance(runtime_flags.get("edge_semantics_guard_action_counts", {}), dict)
                else {}
            ),
        },
        "cost_bps_estimate_summary": {
            "sample_count": int(max(0, to_int(cost_summary.get("sample_count", 0)))),
            "mean_bps": round(float(mean_cost_bps), 6),
            "max_bps": round(float(max_cost_bps), 6),
            "min_bps": round(float(to_float(cost_summary.get("min_bps", 0.0))), 6),
            "p50_bps": round(float(to_float(cost_summary.get("p50_bps", 0.0))), 6),
            "p90_bps": round(float(to_float(cost_summary.get("p90_bps", 0.0))), 6),
        },
        "violations": violations,
        "warnings": warnings,
    }


def build_ranging_shadow_run_meta(
    runtime_config_path: pathlib.Path,
    bundle_meta: Dict[str, Any],
    split_manifest_path: str,
    data_dir: pathlib.Path,
    execution_prewarm_hours: float,
    split_filter_context: Dict[str, Any],
    verification_report_json: pathlib.Path,
    ranging_shadow_signals_jsonl: pathlib.Path,
    semantics_lock_report: Dict[str, Any],
) -> Dict[str, Any]:
    split_context = split_filter_context if isinstance(split_filter_context, dict) else {}
    semantics = semantics_lock_report if isinstance(semantics_lock_report, dict) else {}
    return {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "hard_lock": {
            "runtime_config_path": str(runtime_config_path),
            "bundle_path": str(bundle_meta.get("bundle_path", "")) if isinstance(bundle_meta, dict) else "",
            "split_manifest_path": str(split_manifest_path or ""),
            "dataset_root": str(data_dir),
            "execution_prewarm_hours": float(max(0.0, execution_prewarm_hours)),
            "execution_range_used": (
                split_context.get("execution_range_used", [])
                if isinstance(split_context.get("execution_range_used", []), list)
                else []
            ),
            "evaluation_range_used": (
                split_context.get("evaluation_range_used", [])
                if isinstance(split_context.get("evaluation_range_used", []), list)
                else []
            ),
            "evaluation_range_effective": (
                split_context.get("evaluation_range_effective", [])
                if isinstance(split_context.get("evaluation_range_effective", []), list)
                else []
            ),
            "split_applied": bool(split_context.get("split_applied", False)),
        },
        "semantics": {
            "edge_semantics": str(semantics.get("edge_semantics", "unknown")).strip().lower()
            or "unknown",
            "status": str(semantics.get("status", "unknown")).strip(),
        },
        "artifacts": {
            "verification_report_json": str(verification_report_json),
            "ranging_shadow_signals_jsonl": str(ranging_shadow_signals_jsonl),
        },
    }


def build_stage_d_v1_effect_summary(
    aggregate_diagnostics: Dict[str, Any],
    dataset_diagnostics: List[Dict[str, Any]],
    split_filter_context: Dict[str, Any],
    split_eval_profiles: List[Dict[str, Any]],
) -> Dict[str, Any]:
    split_context = split_filter_context if isinstance(split_filter_context, dict) else {}
    aggregate = aggregate_diagnostics if isinstance(aggregate_diagnostics, dict) else {}
    aggregate_ranging_shadow = aggregate.get("ranging_shadow", {})
    if not isinstance(aggregate_ranging_shadow, dict):
        aggregate_ranging_shadow = {}

    profile_by_dataset: Dict[str, Dict[str, Any]] = {}
    if isinstance(split_eval_profiles, list):
        for row in split_eval_profiles:
            if not isinstance(row, dict):
                continue
            dataset_name = str(row.get("dataset", "")).strip()
            if not dataset_name:
                continue
            profile_by_dataset[dataset_name] = row

    per_dataset_rows: List[Dict[str, Any]] = []
    for item in dataset_diagnostics:
        if not isinstance(item, dict):
            continue
        dataset_name = str(item.get("dataset", "")).strip()
        ranging_shadow = item.get("ranging_shadow", {})
        if not isinstance(ranging_shadow, dict):
            ranging_shadow = {}
        profile = profile_by_dataset.get(dataset_name, {})
        per_dataset_rows.append(
            {
                "dataset": dataset_name,
                "total_trades_core_effective": int(
                    max(0, to_int(profile.get("total_trades_core_effective", 0)))
                ),
                "orders_filled_core_effective": int(
                    max(0, to_int(profile.get("orders_filled_core_effective", 0)))
                ),
                "shadow_count_total": int(max(0, to_int(ranging_shadow.get("shadow_count_total", 0)))),
                "top_shadow_markets": (
                    ranging_shadow.get("top_shadow_markets", [])
                    if isinstance(ranging_shadow.get("top_shadow_markets", []), list)
                    else []
                ),
                "shadow_count_by_regime": (
                    ranging_shadow.get("shadow_count_by_regime", {})
                    if isinstance(ranging_shadow.get("shadow_count_by_regime", {}), dict)
                    else {}
                ),
            }
        )

    per_dataset_rows.sort(key=lambda row: str(row.get("dataset", "")))
    total_trades_core_effective = int(max(0, to_int(split_context.get("total_trades_core_effective", 0))))
    orders_filled_core_effective = int(max(0, to_int(split_context.get("orders_filled_core_effective", 0))))

    return {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "split_name": str(split_context.get("split_name", "")).strip(),
        "split_applied": bool(split_context.get("split_applied", False)),
        "execution_range_used": (
            split_context.get("execution_range_used", [])
            if isinstance(split_context.get("execution_range_used", []), list)
            else []
        ),
        "evaluation_range_used": (
            split_context.get("evaluation_range_used", [])
            if isinstance(split_context.get("evaluation_range_used", []), list)
            else []
        ),
        "evaluation_range_effective": (
            split_context.get("evaluation_range_effective", [])
            if isinstance(split_context.get("evaluation_range_effective", []), list)
            else []
        ),
        "real_orders_core_effective": {
            "total_trades_core_effective": total_trades_core_effective,
            "orders_filled_core_effective": orders_filled_core_effective,
            "real_order_trades_zero": bool(total_trades_core_effective <= 0),
        },
        "ranging_shadow": {
            "shadow_count_total": int(max(0, to_int(aggregate_ranging_shadow.get("shadow_count_total", 0)))),
            "shadow_count_by_regime": (
                aggregate_ranging_shadow.get("shadow_count_by_regime", {})
                if isinstance(aggregate_ranging_shadow.get("shadow_count_by_regime", {}), dict)
                else {}
            ),
            "shadow_count_by_market": (
                aggregate_ranging_shadow.get("shadow_count_by_market", {})
                if isinstance(aggregate_ranging_shadow.get("shadow_count_by_market", {}), dict)
                else {}
            ),
            "shadow_would_pass_quality_selection_count": int(
                max(0, to_int(aggregate_ranging_shadow.get("shadow_would_pass_quality_selection_count", 0)))
            ),
            "shadow_would_pass_execution_guard_count": int(
                max(
                    0,
                    to_int(aggregate_ranging_shadow.get("shadow_would_pass_execution_guard_count", 0)),
                )
            ),
            "shadow_edge_neg_count": int(
                max(0, to_int(aggregate_ranging_shadow.get("shadow_edge_neg_count", 0)))
            ),
            "shadow_edge_pos_count": int(
                max(0, to_int(aggregate_ranging_shadow.get("shadow_edge_pos_count", 0)))
            ),
            "top_shadow_markets": (
                aggregate_ranging_shadow.get("top_shadow_markets", [])
                if isinstance(aggregate_ranging_shadow.get("top_shadow_markets", []), list)
                else []
            ),
        },
        "per_dataset": per_dataset_rows,
    }


def build_stage_g7_ev_affine_debug(
    *,
    dataset_diagnostics: List[Dict[str, Any]],
    split_filter_context: Dict[str, Any],
    rows: List[Dict[str, Any]],
    aggregate_diagnostics: Dict[str, Any],
    bundle_meta: Dict[str, Any],
) -> Dict[str, Any]:
    per_dataset: List[Dict[str, Any]] = []
    raw_weighted_sum = 0.0
    corrected_weighted_sum = 0.0
    weighted_count = 0
    raw_median_weighted_sum = 0.0
    corrected_median_weighted_sum = 0.0
    raw_p95_weighted_sum = 0.0
    corrected_p95_weighted_sum = 0.0
    affine_applied_weighted_sum = 0.0
    affine_enabled_weighted_sum = 0.0
    sample_rows: List[Dict[str, Any]] = []

    for item in dataset_diagnostics:
        if not isinstance(item, dict):
            continue
        dataset_name = str(item.get("dataset", "")).strip()
        telemetry = item.get("lgbm_ev_affine_telemetry", {})
        if not isinstance(telemetry, dict):
            telemetry = {}
        raw_dist = telemetry.get("raw_edge_bps_distribution", {})
        if not isinstance(raw_dist, dict):
            raw_dist = {}
        corrected_dist = telemetry.get("corrected_edge_bps_distribution", {})
        if not isinstance(corrected_dist, dict):
            corrected_dist = {}
        count = max(0, to_int(raw_dist.get("count", 0)))
        raw_mean = to_float(raw_dist.get("mean", float("nan")))
        raw_median = to_float(raw_dist.get("median", float("nan")))
        raw_p95 = to_float(raw_dist.get("p95", float("nan")))
        corrected_mean = to_float(corrected_dist.get("mean", float("nan")))
        corrected_median = to_float(corrected_dist.get("median", float("nan")))
        corrected_p95 = to_float(corrected_dist.get("p95", float("nan")))
        affine_applied_ratio = to_float(telemetry.get("affine_applied_ratio", float("nan")))
        affine_enabled_ratio = to_float(telemetry.get("affine_enabled_ratio", float("nan")))

        if count > 0:
            if math.isfinite(raw_mean):
                raw_weighted_sum += raw_mean * float(count)
            if math.isfinite(corrected_mean):
                corrected_weighted_sum += corrected_mean * float(count)
            if math.isfinite(raw_median):
                raw_median_weighted_sum += raw_median * float(count)
            if math.isfinite(corrected_median):
                corrected_median_weighted_sum += corrected_median * float(count)
            if math.isfinite(raw_p95):
                raw_p95_weighted_sum += raw_p95 * float(count)
            if math.isfinite(corrected_p95):
                corrected_p95_weighted_sum += corrected_p95 * float(count)
            if math.isfinite(affine_applied_ratio):
                affine_applied_weighted_sum += affine_applied_ratio * float(count)
            if math.isfinite(affine_enabled_ratio):
                affine_enabled_weighted_sum += affine_enabled_ratio * float(count)
            weighted_count += count

        samples = telemetry.get("raw_to_corrected_samples", [])
        if isinstance(samples, list):
            for row in samples[:4]:
                if not isinstance(row, dict):
                    continue
                sample_rows.append(
                    {
                        "dataset": dataset_name,
                        "decision_time": max(0, to_int(row.get("decision_time", 0))),
                        "market": str(row.get("market", "")).strip(),
                        "raw_bps": round(to_float(row.get("raw_bps", 0.0)), 8),
                        "corrected_bps": round(to_float(row.get("corrected_bps", 0.0)), 8),
                        "delta_bps": round(to_float(row.get("delta_bps", 0.0)), 8),
                        "affine_applied": bool(row.get("affine_applied", False)),
                    }
                )

        per_dataset.append(
            {
                "dataset": dataset_name,
                "paired_sample_count": count,
                "raw_edge_bps": {
                    "mean": round(raw_mean, 10) if math.isfinite(raw_mean) else None,
                    "median": round(raw_median, 10) if math.isfinite(raw_median) else None,
                    "p95": round(raw_p95, 10) if math.isfinite(raw_p95) else None,
                },
                "corrected_edge_bps": {
                    "mean": round(corrected_mean, 10) if math.isfinite(corrected_mean) else None,
                    "median": round(corrected_median, 10) if math.isfinite(corrected_median) else None,
                    "p95": round(corrected_p95, 10) if math.isfinite(corrected_p95) else None,
                },
                "affine_applied_ratio": (
                    round(affine_applied_ratio, 6) if math.isfinite(affine_applied_ratio) else None
                ),
                "affine_enabled_ratio": (
                    round(affine_enabled_ratio, 6) if math.isfinite(affine_enabled_ratio) else None
                ),
                "affine_params_observed": (
                    telemetry.get("affine_params_observed", [])
                    if isinstance(telemetry.get("affine_params_observed", []), list)
                    else []
                ),
            }
        )

    per_dataset.sort(key=lambda x: str(x.get("dataset", "")))

    split_context = split_filter_context if isinstance(split_filter_context, dict) else {}
    aggregate = aggregate_diagnostics if isinstance(aggregate_diagnostics, dict) else {}
    post_entry = aggregate.get("post_entry_risk_telemetry", {})
    if not isinstance(post_entry, dict):
        post_entry = {}
    stop_loss_count = max(0, to_int(post_entry.get("stop_loss_trigger_count", 0)))
    partial_tp_count = max(0, to_int(post_entry.get("partial_tp_exit_count", 0)))
    tp_full_count = max(0, to_int(post_entry.get("take_profit_full_count", 0)))
    exit_denom = max(1, stop_loss_count + partial_tp_count + tp_full_count)

    weighted_denom = max(1, weighted_count)
    bundle_affine = (
        bundle_meta.get("lgbm_ev_affine", {})
        if isinstance(bundle_meta, dict)
        else {}
    )
    if not isinstance(bundle_affine, dict):
        bundle_affine = {}

    return {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "backend": str(bundle_meta.get("prob_model_backend", "unknown")).strip().lower()
        if isinstance(bundle_meta, dict)
        else "unknown",
        "bundle_affine": {
            "enabled": bool(bundle_affine.get("enabled", False)),
            "scale": round(to_float(bundle_affine.get("scale", 1.0)), 10),
            "shift": round(to_float(bundle_affine.get("shift", 0.0)), 10),
        },
        "split": {
            "split_name": str(split_context.get("split_name", "")).strip(),
            "split_applied": bool(split_context.get("split_applied", False)),
            "execution_range_used": (
                split_context.get("execution_range_used", [])
                if isinstance(split_context.get("execution_range_used", []), list)
                else []
            ),
            "evaluation_range_effective": (
                split_context.get("evaluation_range_effective", [])
                if isinstance(split_context.get("evaluation_range_effective", []), list)
                else []
            ),
            "total_trades_core_effective": int(
                max(0, to_int(split_context.get("total_trades_core_effective", 0)))
            ),
            "orders_filled_core_effective": int(
                max(0, to_int(split_context.get("orders_filled_core_effective", 0)))
            ),
        },
        "performance": {
            "avg_expectancy_krw": round(
                to_float(
                    (
                        (sum(to_float(x.get("expectancy_krw", 0.0)) for x in rows) / float(max(1, len(rows))))
                        if isinstance(rows, list) and rows
                        else 0.0
                    )
                ),
                6,
            ),
            "avg_profit_factor": round(
                to_float(
                    (
                        (sum(to_float(x.get("profit_factor", 0.0)) for x in rows) / float(max(1, len(rows))))
                        if isinstance(rows, list) and rows
                        else 0.0
                    )
                ),
                6,
            ),
            "stop_loss_ratio": round(float(stop_loss_count) / float(exit_denom), 6),
            "partial_tp_ratio": round(float(partial_tp_count) / float(exit_denom), 6),
            "tp_full_ratio": round(float(tp_full_count) / float(exit_denom), 6),
        },
        "edge_bps_summary_weighted": {
            "paired_sample_count_total": int(weighted_count),
            "raw_mean": round(raw_weighted_sum / float(weighted_denom), 10),
            "raw_median_approx": round(raw_median_weighted_sum / float(weighted_denom), 10),
            "raw_p95_approx": round(raw_p95_weighted_sum / float(weighted_denom), 10),
            "corrected_mean": round(corrected_weighted_sum / float(weighted_denom), 10),
            "corrected_median_approx": round(corrected_median_weighted_sum / float(weighted_denom), 10),
            "corrected_p95_approx": round(corrected_p95_weighted_sum / float(weighted_denom), 10),
            "affine_applied_ratio": round(affine_applied_weighted_sum / float(weighted_denom), 6),
            "affine_enabled_ratio": round(affine_enabled_weighted_sum / float(weighted_denom), 6),
        },
        "per_dataset": per_dataset,
        "sample_raw_to_corrected": sample_rows[:20],
    }


def build_stage_g8_backend_provenance_report(
    *,
    dataset_diagnostics: List[Dict[str, Any]],
    bundle_meta: Dict[str, Any],
    split_filter_context: Dict[str, Any],
) -> Dict[str, Any]:
    per_dataset: List[Dict[str, Any]] = []
    effective_counts: Dict[str, int] = {}
    configured_counts: Dict[str, int] = {}
    p_raw_mean_sum = 0.0
    p_raw_std_sum = 0.0
    p_raw_p95_sum = 0.0
    p_cal_mean_sum = 0.0
    p_cal_std_sum = 0.0
    p_cal_p95_sum = 0.0
    margin_mean_sum = 0.0
    margin_std_sum = 0.0
    margin_p95_sum = 0.0
    weighted_count = 0

    for item in dataset_diagnostics:
        if not isinstance(item, dict):
            continue
        dataset_name = str(item.get("dataset", "")).strip()
        prov = item.get("backend_provenance", {})
        if not isinstance(prov, dict):
            prov = {}
        backend_cfg_dataset = str(
            prov.get("prob_model_backend_configured", "unknown")
        ).strip().lower() or "unknown"
        configured_counts[backend_cfg_dataset] = configured_counts.get(backend_cfg_dataset, 0) + 1
        backend_effective = str(prov.get("prob_model_backend_effective", "unknown")).strip().lower() or "unknown"
        effective_counts[backend_effective] = effective_counts.get(backend_effective, 0) + 1
        prob = prov.get("probability_distribution", {})
        if not isinstance(prob, dict):
            prob = {}
        margin = prov.get("margin_distribution", {})
        if not isinstance(margin, dict):
            margin = {}
        tele = item.get("probability_calibration_telemetry", {})
        if not isinstance(tele, dict):
            tele = {}
        count = max(0, to_int(tele.get("paired_sample_count", 0)))
        if count > 0:
            p_raw_mean_sum += to_float(prob.get("p_raw_mean", 0.0)) * count
            p_raw_std_sum += to_float(prob.get("p_raw_std", 0.0)) * count
            p_raw_p95_sum += to_float(prob.get("p_raw_p95", 0.0)) * count
            p_cal_mean_sum += to_float(prob.get("p_cal_mean", 0.0)) * count
            p_cal_std_sum += to_float(prob.get("p_cal_std", 0.0)) * count
            p_cal_p95_sum += to_float(prob.get("p_cal_p95", 0.0)) * count
            margin_mean_sum += to_float(margin.get("mean", 0.0)) * count
            margin_std_sum += to_float(margin.get("std", 0.0)) * count
            margin_p95_sum += to_float(margin.get("p95", 0.0)) * count
            weighted_count += count

        per_dataset.append(
            {
                "dataset": dataset_name,
                "prob_model_backend_effective": backend_effective,
                "prob_model_backend_counts": (
                    prov.get("prob_model_backend_counts", {})
                    if isinstance(prov.get("prob_model_backend_counts", {}), dict)
                    else {}
                ),
                "lgbm_model_path": str(prov.get("lgbm_model_path", "")).strip(),
                "lgbm_model_sha256": str(prov.get("lgbm_model_sha256", "")).strip().lower(),
                "calibration_mode": str(prov.get("calibration_mode", "off")).strip().lower() or "off",
                "calibration_params": (
                    prov.get("calibration_params", {})
                    if isinstance(prov.get("calibration_params", {}), dict)
                    else {"a": None, "b": None}
                ),
                "ev_affine": (
                    prov.get("ev_affine", {})
                    if isinstance(prov.get("ev_affine", {}), dict)
                    else {"enabled": False, "scale": 1.0, "shift": 0.0}
                ),
                "probability_distribution": prob,
                "margin_distribution": margin,
            }
        )

    per_dataset.sort(key=lambda x: str(x.get("dataset", "")))
    dominant_backend = "unknown"
    if effective_counts:
        dominant_backend = sorted(
            effective_counts.items(),
            key=lambda item: (-int(item[1]), str(item[0])),
        )[0][0]
    w = max(1, weighted_count)

    split_context = split_filter_context if isinstance(split_filter_context, dict) else {}
    bundle = bundle_meta if isinstance(bundle_meta, dict) else {}
    bundle_lgbm_cal = bundle.get("lgbm_h5_calibration", {})
    if not isinstance(bundle_lgbm_cal, dict):
        bundle_lgbm_cal = {}
    bundle_ev_affine = bundle.get("lgbm_ev_affine", {})
    if not isinstance(bundle_ev_affine, dict):
        bundle_ev_affine = {}
    configured_backend_from_bundle = (
        str(bundle.get("prob_model_backend", "unknown")).strip().lower() or "unknown"
    )
    if configured_backend_from_bundle in {"unknown", "n/a", ""} and configured_counts:
        configured_backend_from_bundle = sorted(
            configured_counts.items(),
            key=lambda item: (-int(item[1]), str(item[0])),
        )[0][0]

    lgbm_model_path_cfg = str(bundle.get("lgbm_model_path", "")).strip()
    lgbm_model_sha_cfg = str(bundle.get("lgbm_model_sha256", "")).strip().lower()
    if (not lgbm_model_path_cfg or not lgbm_model_sha_cfg) and per_dataset:
        for row in per_dataset:
            if not isinstance(row, dict):
                continue
            p = str(row.get("lgbm_model_path", "")).strip()
            s = str(row.get("lgbm_model_sha256", "")).strip().lower()
            if not lgbm_model_path_cfg and p:
                lgbm_model_path_cfg = p
            if not lgbm_model_sha_cfg and s:
                lgbm_model_sha_cfg = s
            if lgbm_model_path_cfg and lgbm_model_sha_cfg:
                break

    return {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "runtime_bundle_path": str(bundle.get("bundle_path", "")),
        "prob_model_backend_configured": configured_backend_from_bundle,
        "prob_model_backend_effective_counts": {
            str(k): int(v) for k, v in sorted(effective_counts.items(), key=lambda x: str(x[0]))
        },
        "prob_model_backend_effective_dominant": str(dominant_backend),
        "lgbm_model_path": lgbm_model_path_cfg,
        "lgbm_model_sha256": lgbm_model_sha_cfg,
        "calibration_mode": str(bundle.get("lgbm_calibration_mode", "off")).strip().lower(),
        "calibration_params": {
            "a": bundle_lgbm_cal.get("a", None),
            "b": bundle_lgbm_cal.get("b", None),
        },
        "ev_affine": {
            "enabled": bool(bundle_ev_affine.get("enabled", False)),
            "scale": round(to_float(bundle_ev_affine.get("scale", 1.0)), 10),
            "shift": round(to_float(bundle_ev_affine.get("shift", 0.0)), 10),
        },
        "weighted_probability_distribution": {
            "sample_count": int(weighted_count),
            "p_raw_mean": round(p_raw_mean_sum / float(w), 10),
            "p_raw_std": round(p_raw_std_sum / float(w), 10),
            "p_raw_p95": round(p_raw_p95_sum / float(w), 10),
            "p_cal_mean": round(p_cal_mean_sum / float(w), 10),
            "p_cal_std": round(p_cal_std_sum / float(w), 10),
            "p_cal_p95": round(p_cal_p95_sum / float(w), 10),
        },
        "weighted_margin_distribution": {
            "sample_count": int(weighted_count),
            "mean": round(margin_mean_sum / float(w), 10),
            "std": round(margin_std_sum / float(w), 10),
            "p95": round(margin_p95_sum / float(w), 10),
        },
        "split": {
            "split_name": str(split_context.get("split_name", "")).strip(),
            "split_applied": bool(split_context.get("split_applied", False)),
            "evaluation_range_effective": (
                split_context.get("evaluation_range_effective", [])
                if isinstance(split_context.get("evaluation_range_effective", []), list)
                else []
            ),
            "total_trades_core_effective": int(
                max(0, to_int(split_context.get("total_trades_core_effective", 0)))
            ),
        },
        "per_dataset": per_dataset,
    }


def build_edge_sign_distribution(
    dataset_diagnostics: List[Dict[str, Any]],
    split_filter_context: Dict[str, Any],
) -> Dict[str, Any]:
    eval_range = (
        split_filter_context.get("evaluation_range_effective", [])
        if isinstance(split_filter_context, dict)
        else []
    )
    range_start = (
        int(eval_range[0])
        if isinstance(eval_range, list) and len(eval_range) == 2
        else 0
    )
    range_end = (
        int(eval_range[1])
        if isinstance(eval_range, list) and len(eval_range) == 2
        else 0
    )
    range_enabled = bool(range_start > 0 and range_end > 0 and range_end >= range_start)

    all_values_bps: List[float] = []
    count_edge_neg = 0
    count_edge_pos = 0
    count_edge_zero = 0
    selected_count = 0
    per_dataset_rows: List[Dict[str, Any]] = []

    for item in dataset_diagnostics:
        if not isinstance(item, dict):
            continue
        dataset_name = str(item.get("dataset", "")).strip()
        samples = (
            item.get("phase3_pass_ev_distribution", {}).get("samples", [])
            if isinstance(item.get("phase3_pass_ev_distribution", {}), dict)
            else []
        )
        if not isinstance(samples, list):
            samples = []

        dataset_values_bps: List[float] = []
        dataset_neg = 0
        dataset_pos = 0
        dataset_zero = 0
        dataset_selected = 0
        for row in samples:
            if not isinstance(row, dict):
                continue
            ts_value = max(0, to_int(row.get("ts", 0)))
            if range_enabled and not (range_start <= ts_value <= range_end):
                continue
            bps = to_float(row.get("expected_edge_after_cost_bps", float("nan")))
            if not math.isfinite(bps):
                continue
            value = float(bps)
            dataset_values_bps.append(value)
            all_values_bps.append(value)
            if value < 0.0:
                dataset_neg += 1
            elif value > 0.0:
                dataset_pos += 1
            else:
                dataset_zero += 1
            if bool(row.get("selected", False)):
                dataset_selected += 1

        dataset_total = len(dataset_values_bps)
        per_dataset_rows.append(
            {
                "dataset": dataset_name,
                "count_total": int(dataset_total),
                "count_edge_neg": int(dataset_neg),
                "count_edge_pos": int(dataset_pos),
                "count_edge_zero": int(dataset_zero),
                "selected_count": int(dataset_selected),
                "p10_bps": round(quantile(dataset_values_bps, 0.10), 6)
                if dataset_values_bps
                else None,
                "p50_bps": round(quantile(dataset_values_bps, 0.50), 6)
                if dataset_values_bps
                else None,
                "p90_bps": round(quantile(dataset_values_bps, 0.90), 6)
                if dataset_values_bps
                else None,
            }
        )
        count_edge_neg += dataset_neg
        count_edge_pos += dataset_pos
        count_edge_zero += dataset_zero
        selected_count += dataset_selected

    count_total = len(all_values_bps)
    payload = {
        "enabled": bool(count_total > 0),
        "split_name": (
            str(split_filter_context.get("split_name", "")).strip()
            if isinstance(split_filter_context, dict)
            else ""
        ),
        "range_mode": (
            "evaluation_range_effective" if range_enabled else "full_sample"
        ),
        "evaluation_range_effective": [int(range_start), int(range_end)]
        if range_enabled
        else [],
        "count_total": int(count_total),
        "count_edge_neg": int(count_edge_neg),
        "count_edge_pos": int(count_edge_pos),
        "count_edge_zero": int(count_edge_zero),
        "selected_count": int(selected_count),
        "edge_pos_ratio": round(float(count_edge_pos) / float(max(1, count_total)), 6),
        "edge_neg_ratio": round(float(count_edge_neg) / float(max(1, count_total)), 6),
        "edge_zero_ratio": round(float(count_edge_zero) / float(max(1, count_total)), 6),
        "p10_bps": round(quantile(all_values_bps, 0.10), 6) if all_values_bps else None,
        "p50_bps": round(quantile(all_values_bps, 0.50), 6) if all_values_bps else None,
        "p90_bps": round(quantile(all_values_bps, 0.90), 6) if all_values_bps else None,
        "by_dataset": per_dataset_rows,
    }
    return payload


def build_edge_pos_but_no_trade_breakdown(
    edge_sign_distribution: Dict[str, Any],
    aggregate_diagnostics: Dict[str, Any],
    split_filter_context: Dict[str, Any],
) -> Dict[str, Any]:
    phase3 = (
        aggregate_diagnostics.get("phase3_diagnostics_v2", {})
        if isinstance(aggregate_diagnostics, dict)
        else {}
    )
    phase3_funnel = (
        phase3.get("funnel_breakdown", {})
        if isinstance(phase3, dict)
        else {}
    )
    phase4 = (
        aggregate_diagnostics.get("phase4_portfolio_diagnostics", {})
        if isinstance(aggregate_diagnostics, dict)
        else {}
    )
    phase4_selection = (
        phase4.get("selection_breakdown", {})
        if isinstance(phase4, dict)
        else {}
    )

    count_edge_pos = max(0, to_int(edge_sign_distribution.get("count_edge_pos", 0)))
    total_trades_core = (
        max(0, to_int(split_filter_context.get("total_trades_core_effective", 0)))
        if isinstance(split_filter_context, dict)
        else 0
    )
    edge_pos_but_no_trade = max(0, count_edge_pos - total_trades_core)

    source_breakdown = {
        "filtered_by_manager_strength": max(
            0, to_int(phase3_funnel.get("reject_strength_fail", 0))
        ),
        "filtered_by_quality_filter": max(
            0, to_int(phase3_funnel.get("reject_quality_filter_fail", 0))
        ),
        "filtered_by_budget_phase4": max(
            0,
            to_int(phase4_selection.get("rejected_by_budget", 0))
            + to_int(phase4_selection.get("rejected_by_cluster_cap", 0))
            + to_int(phase4_selection.get("rejected_by_correlation_penalty", 0))
            + to_int(phase4_selection.get("rejected_by_drawdown_governor", 0)),
        ),
        "execution_cap": max(
            0, to_int(phase4_selection.get("rejected_by_execution_cap", 0))
        ),
    }
    source_total = max(0, sum(source_breakdown.values()))

    estimated_breakdown = {
        "filtered_by_manager_strength": 0,
        "filtered_by_quality_filter": 0,
        "filtered_by_budget_phase4": 0,
        "execution_cap": 0,
        "other": int(edge_pos_but_no_trade),
    }
    if edge_pos_but_no_trade > 0 and source_total > 0:
        running = 0
        ordered_keys = [
            "filtered_by_manager_strength",
            "filtered_by_quality_filter",
            "filtered_by_budget_phase4",
            "execution_cap",
        ]
        for key in ordered_keys:
            estimate = int(
                round(
                    float(edge_pos_but_no_trade)
                    * float(source_breakdown.get(key, 0))
                    / float(max(1, source_total))
                )
            )
            estimate = max(0, estimate)
            estimated_breakdown[key] = estimate
            running += estimate
        estimated_breakdown["other"] = max(0, edge_pos_but_no_trade - running)

    recommended_case = "A10-1a" if count_edge_pos <= 0 else "A10-1b"
    recommended_axis = (
        "ev_cost_model_scale_single_step_adjust"
        if recommended_case == "A10-1a"
        else "required_ev_offset_or_base_min_strength_single_step_relax_with_negative_ev_block_kept"
    )

    return {
        "enabled": bool(edge_sign_distribution.get("enabled", False)),
        "split_name": str(
            split_filter_context.get("split_name", "")
            if isinstance(split_filter_context, dict)
            else ""
        ).strip(),
        "count_edge_pos": int(count_edge_pos),
        "total_trades_core_effective": int(total_trades_core),
        "edge_pos_but_no_trade": int(edge_pos_but_no_trade),
        "source_breakdown_raw": source_breakdown,
        "source_breakdown_raw_total": int(source_total),
        "estimated_breakdown": estimated_breakdown,
        "recommended_case": recommended_case,
        "recommended_single_axis": recommended_axis,
        "methodology_note": (
            "stage counters are global across candidates; edge_pos-specific "
            "breakdown is estimated by proportional allocation"
        ),
    }


def build_a10_2_stage_funnel_report(
    aggregate_diagnostics: Dict[str, Any],
    split_filter_context: Dict[str, Any],
) -> Dict[str, Any]:
    ctx = split_filter_context if isinstance(split_filter_context, dict) else {}
    phase4 = (
        aggregate_diagnostics.get("phase4_portfolio_diagnostics", {})
        if isinstance(aggregate_diagnostics, dict)
        else {}
    )
    phase4_selection = (
        phase4.get("selection_breakdown", {})
        if isinstance(phase4, dict)
        else {}
    )
    phase3 = (
        aggregate_diagnostics.get("phase3_diagnostics_v2", {})
        if isinstance(aggregate_diagnostics, dict)
        else {}
    )
    phase3_funnel = (
        phase3.get("funnel_breakdown", {})
        if isinstance(phase3, dict)
        else {}
    )

    s0 = max(
        0,
        to_int(
            ctx.get(
                "stage_candidates_total_core_effective",
                ctx.get("candidate_total_core_effective", 0),
            )
        ),
    )
    s1 = max(
        0,
        to_int(
            ctx.get(
                "stage_candidates_after_quality_topk_core_effective",
                ctx.get("candidate_total_core_effective", 0),
            )
        ),
    )
    s2 = max(
        0,
        to_int(
            ctx.get(
                "stage_candidates_after_manager_core_effective",
                ctx.get("candidate_total_core_effective", 0),
            )
        ),
    )
    s3 = max(
        0,
        to_int(
            ctx.get(
                "stage_candidates_after_portfolio_core_effective",
                ctx.get("pass_total_core_effective", 0),
            )
        ),
    )
    s4 = max(0, to_int(ctx.get("orders_submitted_core_effective", 0)))
    s5 = max(0, to_int(ctx.get("orders_filled_core_effective", 0)))
    ev_samples_core = max(0, to_int(ctx.get("ev_samples_core_effective", 0)))
    ev_manager_sum_core = to_float(ctx.get("ev_at_manager_pass_sum_core_effective", 0.0))
    ev_order_sum_core = to_float(
        ctx.get("ev_at_order_submit_check_sum_core_effective", 0.0)
    )
    ev_mismatch_count_core = max(
        0,
        to_int(ctx.get("ev_mismatch_count_core_effective", 0)),
    )

    order_block_reason_counts = ctx.get("stage_order_block_reason_counts_core_effective", {})
    if not isinstance(order_block_reason_counts, dict):
        order_block_reason_counts = {}
    order_block_reason_rows = sorted(
        [
            {"reason": str(k), "count": int(max(0, to_int(v)))}
            for k, v in order_block_reason_counts.items()
            if int(max(0, to_int(v))) > 0
        ],
        key=lambda item: (-int(item["count"]), str(item["reason"])),
    )

    phase4_candidates_exec = max(
        0, to_int(phase4_selection.get("candidates_total", 0))
    )
    phase4_scale = (
        float(s2) / float(max(1, phase4_candidates_exec))
        if phase4_candidates_exec > 0
        else 0.0
    )
    rejected_by_budget_core_est = int(
        round(float(max(0, to_int(phase4_selection.get("rejected_by_budget", 0)))) * phase4_scale)
    )
    rejected_by_cluster_cap_core_est = int(
        round(
            float(max(0, to_int(phase4_selection.get("rejected_by_cluster_cap", 0))))
            * phase4_scale
        )
    )
    rejected_by_exec_cap_core_est = int(
        round(
            float(max(0, to_int(phase4_selection.get("rejected_by_execution_cap", 0))))
            * phase4_scale
        )
    )

    reject_expected_edge_negative_count_core_actual = max(
        0,
        to_int(order_block_reason_counts.get("reject_expected_edge_negative_count", 0))
        + to_int(order_block_reason_counts.get("reject_expected_edge_negative", 0)),
    )
    reject_expected_edge_negative_count_core_est = (
        int(reject_expected_edge_negative_count_core_actual)
        if reject_expected_edge_negative_count_core_actual > 0
        else int(
            round(
                float(
                    max(
                        0,
                        to_int(phase3_funnel.get("reject_expected_edge_negative_count", 0)),
                    )
                )
                * (
                    float(max(0, to_int(ctx.get("candidate_total_core_effective", 0))))
                    / float(max(1, to_int(ctx.get("candidate_total_execution", 1))))
                )
            )
        )
    )
    reject_regime_entry_disabled_count_core_actual = max(
        0,
        to_int(order_block_reason_counts.get("reject_regime_entry_disabled_count", 0))
        + to_int(order_block_reason_counts.get("reject_regime_entry_disabled", 0)),
    )
    reject_regime_entry_disabled_count_core_est = (
        int(reject_regime_entry_disabled_count_core_actual)
        if reject_regime_entry_disabled_count_core_actual > 0
        else int(
            round(
                float(
                    max(
                        0,
                        to_int(phase3_funnel.get("reject_regime_entry_disabled_count", 0)),
                    )
                )
                * (
                    float(max(0, to_int(ctx.get("candidate_total_core_effective", 0))))
                    / float(max(1, to_int(ctx.get("candidate_total_execution", 1))))
                )
            )
        )
    )

    return {
        "enabled": True,
        "split_name": str(ctx.get("split_name", "")).strip(),
        "evaluation_range_effective": ctx.get("evaluation_range_effective", []),
        "funnel_summary_core": {
            "S0_candidates_total_core": int(s0),
            "S1_candidates_after_quality_topk_core": int(s1),
            "S2_candidates_after_manager_core": int(s2),
            "S3_candidates_after_portfolio_core": int(s3),
            "S4_orders_submitted_core": int(s4),
            "S5_orders_filled_core": int(s5),
        },
        "stage_counters_core": {
            "reject_expected_edge_negative_count_core_est": int(
                max(0, reject_expected_edge_negative_count_core_est)
            ),
            "reject_regime_entry_disabled_count_core_est": int(
                max(0, reject_regime_entry_disabled_count_core_est)
            ),
            "rejected_by_budget_core_est": int(max(0, rejected_by_budget_core_est)),
            "rejected_by_cluster_cap_core_est": int(
                max(0, rejected_by_cluster_cap_core_est)
            ),
            "rejected_by_exec_cap_core_est": int(max(0, rejected_by_exec_cap_core_est)),
            "ev_samples_core": int(ev_samples_core),
            "ev_at_manager_pass_core_avg": round(
                (ev_manager_sum_core / float(ev_samples_core)) if ev_samples_core > 0 else 0.0,
                8,
            ),
            "ev_at_order_submit_check_core_avg": round(
                (ev_order_sum_core / float(ev_samples_core)) if ev_samples_core > 0 else 0.0,
                8,
            ),
            "ev_mismatch_count_core": int(ev_mismatch_count_core),
        },
        "top_order_block_reason_core": order_block_reason_rows[:3],
        "order_block_reason_counts_core": {
            str(item["reason"]): int(item["count"]) for item in order_block_reason_rows
        },
        "a10_3_case_hint": (
            "case1_order_submission_block"
            if s3 > 0 and s4 <= 0
            else (
                "case2_fill_block"
                if s4 > 0 and s5 <= 0
                else (
                    "case3_portfolio_or_budget_block"
                    if s2 <= 0 or s3 <= 0
                    else ("case4_quality_filter_block" if s1 <= 0 else "case0_observe")
                )
            )
        ),
        "methodology_note": (
            "S0~S5 are aggregated from core-effective stage telemetry; "
            "per-reason phase4 and reject_expected_edge_negative counters are estimated "
            "by execution-to-core scaling."
        ),
    }


def build_quality_filter_fail_breakdown(
    quality_filter_dataset_samples: List[Dict[str, Any]],
    split_filter_context: Dict[str, Any],
) -> Dict[str, Any]:
    source_dataset_rows: List[Dict[str, Any]] = []
    samples: List[Dict[str, Any]] = []
    if isinstance(quality_filter_dataset_samples, list):
        for dataset_payload in quality_filter_dataset_samples:
            if not isinstance(dataset_payload, dict):
                continue
            dataset_name = str(dataset_payload.get("dataset", "")).strip()
            row_samples = dataset_payload.get("samples", [])
            if not isinstance(row_samples, list):
                row_samples = []
            source_dataset_rows.append(
                {
                    "dataset": dataset_name,
                    "line_count_in_range": max(0, to_int(dataset_payload.get("line_count_in_range", 0))),
                    "sample_count_in_range": max(0, to_int(dataset_payload.get("sample_count_in_range", 0))),
                    "quality_filter_enabled_count_in_range": max(
                        0,
                        to_int(dataset_payload.get("quality_filter_enabled_count_in_range", 0)),
                    ),
                    "quality_filter_fail_count_in_range": max(
                        0,
                        to_int(dataset_payload.get("quality_filter_fail_count_in_range", 0)),
                    ),
                }
            )
            for sample in row_samples:
                if isinstance(sample, dict):
                    sample_with_dataset = dict(sample)
                    sample_with_dataset["_dataset"] = dataset_name
                    samples.append(sample_with_dataset)

    reason_specs = {
        "required_ev_fail": {
            "observed_key": "expected_value_observed",
            "limit_key": "required_expected_value",
            "slack_key": "expected_value_slack",
            "desc": "expected_value < required_expected_value",
        },
        "required_margin_fail": {
            "observed_key": "margin_observed",
            "limit_key": "margin_floor",
            "slack_key": "margin_slack",
            "desc": "margin < margin_floor",
        },
        "confidence_fail": {
            "observed_key": "ev_confidence_observed",
            "limit_key": "ev_confidence_floor",
            "slack_key": "ev_confidence_slack",
            "desc": "ev_confidence < ev_confidence_floor",
        },
        "cost_tail_fail": {
            "observed_key": "cost_tail_observed",
            "limit_key": "cost_tail_limit",
            "slack_key": "cost_tail_slack",
            "desc": "cost_tail_term > cost_tail_limit",
        },
        "other_quality_filter_fail": {
            "observed_key": "",
            "limit_key": "",
            "slack_key": "",
            "desc": "quality_filter_fail_with_no_subreason_flag",
        },
    }

    reason_counts: Dict[str, int] = {key: 0 for key in reason_specs.keys()}
    observed_values: Dict[str, List[float]] = {key: [] for key in reason_specs.keys()}
    limit_values: Dict[str, List[float]] = {key: [] for key in reason_specs.keys()}
    slack_values: Dict[str, List[float]] = {key: [] for key in reason_specs.keys()}

    quality_filter_enabled_count = 0
    quality_filter_fail_count = 0
    for row in samples:
        if not isinstance(row, dict):
            continue
        if not bool(row.get("quality_filter_enabled", False)):
            continue
        quality_filter_enabled_count += 1
        if bool(row.get("quality_filter_pass", True)):
            continue
        quality_filter_fail_count += 1
        fail_reason_count = 0

        if not bool(row.get("sizing_pass", True)):
            reason_key = "required_ev_fail"
            reason_counts[reason_key] += 1
            fail_reason_count += 1
            observed_values[reason_key].append(to_float(row.get("expected_value_observed", float("nan"))))
            limit_values[reason_key].append(to_float(row.get("required_expected_value", float("nan"))))
            slack_values[reason_key].append(to_float(row.get("expected_value_slack", float("nan"))))

        if not bool(row.get("margin_pass", True)):
            reason_key = "required_margin_fail"
            reason_counts[reason_key] += 1
            fail_reason_count += 1
            observed_values[reason_key].append(to_float(row.get("margin_observed", float("nan"))))
            limit_values[reason_key].append(to_float(row.get("margin_floor", float("nan"))))
            slack_values[reason_key].append(to_float(row.get("margin_slack", float("nan"))))

        if not bool(row.get("ev_confidence_pass", True)):
            reason_key = "confidence_fail"
            reason_counts[reason_key] += 1
            fail_reason_count += 1
            observed_values[reason_key].append(to_float(row.get("ev_confidence_observed", float("nan"))))
            limit_values[reason_key].append(to_float(row.get("ev_confidence_floor", float("nan"))))
            slack_values[reason_key].append(to_float(row.get("ev_confidence_slack", float("nan"))))

        if not bool(row.get("cost_tail_pass", True)):
            reason_key = "cost_tail_fail"
            reason_counts[reason_key] += 1
            fail_reason_count += 1
            observed_values[reason_key].append(to_float(row.get("cost_tail_observed", float("nan"))))
            limit_values[reason_key].append(to_float(row.get("cost_tail_limit", float("nan"))))
            slack_values[reason_key].append(to_float(row.get("cost_tail_slack", float("nan"))))

        if fail_reason_count <= 0:
            reason_counts["other_quality_filter_fail"] += 1

    reason_rows: List[Dict[str, Any]] = []
    for reason_key, spec in reason_specs.items():
        count = max(0, to_int(reason_counts.get(reason_key, 0)))
        if count <= 0:
            continue
        reason_rows.append(
            {
                "reason": str(reason_key),
                "description": str(spec.get("desc", "")).strip(),
                "count": int(count),
                "contribution_share_vs_quality_filter_fail": round(
                    float(count) / float(max(1, quality_filter_fail_count)),
                    6,
                ),
                "observed_distribution": summarize_values_quantiles(observed_values.get(reason_key, [])),
                "limit_distribution": summarize_values_quantiles(limit_values.get(reason_key, [])),
                "slack_distribution": summarize_values_quantiles(slack_values.get(reason_key, [])),
            }
        )
    reason_rows.sort(
        key=lambda item: (
            -int(item.get("count", 0)),
            str(item.get("reason", "")),
        )
    )
    top3 = reason_rows[:3]
    top1_reason = str(top3[0].get("reason", "")) if top3 else ""

    recommended_axis = "A18-1d"
    if top1_reason == "required_ev_fail":
        recommended_axis = "A18-1a"
    elif top1_reason == "required_margin_fail":
        recommended_axis = "A18-1b"
    elif top1_reason == "confidence_fail":
        recommended_axis = "A18-1d"
    elif top1_reason == "cost_tail_fail":
        recommended_axis = "A18-1d"

    return {
        "enabled": bool(quality_filter_fail_count > 0),
        "split_name": str(
            split_filter_context.get("split_name", "")
            if isinstance(split_filter_context, dict)
            else ""
        ).strip(),
        "evaluation_range_effective": (
            split_filter_context.get("evaluation_range_effective", [])
            if isinstance(split_filter_context, dict)
            else []
        ),
        "source": {
            "artifact": "logs/quality_filter_backtest.jsonl (per-dataset run capture)",
            "dataset_count": int(len(source_dataset_rows)),
            "line_count_in_range_total": int(
                sum(max(0, to_int(x.get("line_count_in_range", 0))) for x in source_dataset_rows)
            ),
            "sample_count_in_range_total": int(
                sum(max(0, to_int(x.get("sample_count_in_range", 0))) for x in source_dataset_rows)
            ),
            "per_dataset": source_dataset_rows,
        },
        "quality_filter_enabled_count_in_range": int(quality_filter_enabled_count),
        "quality_filter_fail_count_in_range": int(quality_filter_fail_count),
        "quality_filter_fail_breakdown_top3": top3,
        "quality_filter_fail_breakdown_all": reason_rows,
        "recommended_single_axis_case": recommended_axis,
        "recommended_top1_reason": top1_reason,
    }


def build_phase4_exposure_summary_from_samples(
    backtest_result: Dict[str, Any],
    phase4_market_cluster_map: Dict[str, str],
) -> Dict[str, Any]:
    samples = backtest_result.get("phase4_candidate_snapshot_samples", [])
    if not isinstance(samples, list) or not samples:
        return {
            "source": "none",
            "selected_total": 0,
            "total_selected_notional_exposure": 0.0,
            "max_cluster_share": 0.0,
            "exposure_by_cluster": [],
        }

    market_cluster_map: Dict[str, str] = {}
    if isinstance(phase4_market_cluster_map, dict):
        for key, value in phase4_market_cluster_map.items():
            market = str(key).strip().upper()
            cluster = str(value).strip()
            if market and cluster:
                market_cluster_map[market] = cluster

    selected_total = 0
    selected_by_cluster: Dict[str, int] = {}
    exposure_by_cluster: Dict[str, float] = {}
    for item in samples:
        if not isinstance(item, dict):
            continue
        if not bool(item.get("selected", False)):
            continue
        selected_total += 1
        market = str(item.get("market", item.get("market_id", ""))).strip().upper()
        cluster = market_cluster_map.get(market, "unclustered")
        selected_by_cluster[cluster] = selected_by_cluster.get(cluster, 0) + 1
        # Snapshot samples do not emit notional size yet; selected-count unit is used as proxy.
        exposure_by_cluster[cluster] = exposure_by_cluster.get(cluster, 0.0) + 1.0

    total_exposure = sum(max(0.0, float(v)) for v in exposure_by_cluster.values())
    rows: List[Dict[str, Any]] = []
    max_cluster_share = 0.0
    for cluster in sorted(exposure_by_cluster.keys()):
        exposure = max(0.0, float(exposure_by_cluster.get(cluster, 0.0)))
        share = (exposure / total_exposure) if total_exposure > 0.0 else 0.0
        max_cluster_share = max(max_cluster_share, share)
        rows.append(
            {
                "cluster": str(cluster),
                "selected_count": int(selected_by_cluster.get(cluster, 0)),
                "notional_exposure_sum": round(exposure, 8),
                "share": round(share, 6),
            }
        )

    return {
        "source": "selected_count_unit_proxy",
        "selected_total": int(selected_total),
        "total_selected_notional_exposure": round(total_exposure, 8),
        "max_cluster_share": round(max_cluster_share, 6),
        "exposure_by_cluster": rows,
    }


def build_phase4_portfolio_diagnostics(
    backtest_result: Dict[str, Any],
    phase4_market_cluster_map: Dict[str, str],
) -> Dict[str, Any]:
    raw = backtest_result.get("phase4_portfolio_diagnostics", {})
    if not isinstance(raw, dict):
        raw = {}

    counters = {
        "candidates_total": max(0, to_int(raw.get("candidates_total", 0))),
        "selected_total": max(0, to_int(raw.get("selected_total", 0))),
        "rejected_by_budget": max(0, to_int(raw.get("rejected_by_budget", 0))),
        "rejected_by_cluster_cap": max(0, to_int(raw.get("rejected_by_cluster_cap", 0))),
        "rejected_by_correlation_penalty": max(
            0, to_int(raw.get("rejected_by_correlation_penalty", 0))
        ),
        "cluster_cap_skips_count": max(
            0, to_int(raw.get("cluster_cap_skips_count", 0))
        ),
        "cluster_cap_would_exceed_count": max(
            0, to_int(raw.get("cluster_cap_would_exceed_count", 0))
        ),
        "cluster_exposure_update_count": max(
            0, to_int(raw.get("cluster_exposure_update_count", 0))
        ),
        "rejected_by_execution_cap": max(0, to_int(raw.get("rejected_by_execution_cap", 0))),
        "rejected_by_drawdown_governor": max(
            0, to_int(raw.get("rejected_by_drawdown_governor", 0))
        ),
        "candidate_snapshot_total": max(0, to_int(raw.get("candidate_snapshot_total", 0))),
        "candidate_snapshot_sampled": max(0, to_int(raw.get("candidate_snapshot_sampled", 0))),
    }
    counters["selection_rate"] = round(
        float(counters["selected_total"]) / float(max(1, counters["candidates_total"])),
        6,
    )

    reject_rows = sorted(
        [
            {"reason": key, "count": int(value)}
            for key, value in counters.items()
            if key.startswith("rejected_") and int(value) > 0
        ],
        key=lambda item: (-int(item["count"]), str(item["reason"])),
    )
    enabled = bool(raw.get("enabled", False))
    flags = {
        "phase4_portfolio_allocator_enabled": bool(
            raw.get("phase4_portfolio_allocator_enabled", False)
        ),
        "phase4_correlation_control_enabled": bool(
            raw.get("phase4_correlation_control_enabled", False)
        ),
        "phase4_risk_budget_enabled": bool(raw.get("phase4_risk_budget_enabled", False)),
        "phase4_drawdown_governor_enabled": bool(
            raw.get("phase4_drawdown_governor_enabled", False)
        ),
        "phase4_execution_aware_sizing_enabled": bool(
            raw.get("phase4_execution_aware_sizing_enabled", False)
        ),
        "phase4_portfolio_diagnostics_enabled": bool(
            raw.get("phase4_portfolio_diagnostics_enabled", False)
        ),
    }
    allocator_policy = {
        "top_k": max(1, to_int(raw.get("allocator_top_k", 1))),
        "min_score": round(to_float(raw.get("allocator_min_score", -1.0e6)), 8),
    }

    raw_regime_budget_multipliers = raw.get("risk_budget_regime_multipliers", {})
    regime_budget_multipliers: Dict[str, float] = {}
    if isinstance(raw_regime_budget_multipliers, dict):
        for key, value in raw_regime_budget_multipliers.items():
            regime_key = str(key).strip()
            if not regime_key:
                continue
            regime_budget_multipliers[regime_key] = round(max(0.0, min(1.0, to_float(value))), 8)

    raw_regime_budget_count_map = raw.get("regime_budget_multiplier_count_by_regime", {})
    regime_budget_multiplier_count_by_regime: Dict[str, int] = {}
    if isinstance(raw_regime_budget_count_map, dict):
        for key, value in raw_regime_budget_count_map.items():
            regime_key = str(key).strip()
            if not regime_key:
                continue
            regime_budget_multiplier_count_by_regime[regime_key] = max(0, to_int(value))

    raw_regime_budget_avg_map = raw.get("regime_budget_multiplier_avg_by_regime", {})
    regime_budget_multiplier_avg_by_regime: Dict[str, float] = {}
    if isinstance(raw_regime_budget_avg_map, dict):
        for key, value in raw_regime_budget_avg_map.items():
            regime_key = str(key).strip()
            if not regime_key:
                continue
            regime_budget_multiplier_avg_by_regime[regime_key] = round(max(0.0, min(1.0, to_float(value))), 8)

    risk_budget_policy = {
        "per_market_cap": round(to_float(raw.get("risk_budget_per_market_cap", 0.0)), 8),
        "gross_cap": round(to_float(raw.get("risk_budget_gross_cap", 0.0)), 8),
        "risk_budget_cap": round(to_float(raw.get("risk_budget_cap", 0.0)), 8),
        "regime_budget_multipliers": regime_budget_multipliers,
    }
    drawdown_policy = {
        "drawdown_current": round(to_float(raw.get("drawdown_current", 0.0)), 8),
        "drawdown_budget_multiplier": round(
            to_float(raw.get("drawdown_budget_multiplier", 1.0)), 8
        ),
    }
    correlation_policy = {
        "default_cluster_cap": round(
            to_float(raw.get("correlation_default_cluster_cap", 0.0)), 8
        ),
        "market_cluster_count": max(0, to_int(raw.get("correlation_market_cluster_count", 0))),
    }
    execution_policy = {
        "liquidity_low_threshold": round(
            to_float(raw.get("execution_liquidity_low_threshold", 0.0)), 8
        ),
        "liquidity_mid_threshold": round(
            to_float(raw.get("execution_liquidity_mid_threshold", 0.0)), 8
        ),
        "min_position_size": round(to_float(raw.get("execution_min_position_size", 0.0)), 8),
    }
    correlation_stage = str(raw.get("correlation_constraint_apply_stage", "")).strip()
    correlation_unit = str(raw.get("correlation_constraint_unit", "")).strip()
    correlation_cluster_eval_count = max(
        0,
        to_int(raw.get("correlation_cluster_eval_count", 0)),
    )
    correlation_cluster_near_cap_count = max(
        0,
        to_int(raw.get("correlation_cluster_near_cap_count", 0)),
    )
    correlation_penalty_applied_count = max(
        0,
        to_int(raw.get("correlation_penalty_applied_count", 0)),
    )
    correlation_penalty_avg = round(
        to_float(raw.get("correlation_penalty_avg", 0.0)),
        8,
    )
    correlation_penalty_max = round(
        to_float(raw.get("correlation_penalty_max", 0.0)),
        8,
    )

    def _parse_cluster_float_map(node: Any) -> Dict[str, float]:
        out: Dict[str, float] = {}
        if not isinstance(node, dict):
            return out
        for key, value in node.items():
            cluster = str(key).strip()
            if not cluster:
                continue
            out[cluster] = max(0.0, to_float(value))
        return out

    cluster_exposure_current = _parse_cluster_float_map(
        raw.get("correlation_cluster_exposure_current", {})
    )
    cluster_cap_values = _parse_cluster_float_map(
        raw.get("correlation_cluster_cap_values", {})
    )
    cluster_keys = sorted(set(cluster_exposure_current.keys()) | set(cluster_cap_values.keys()))
    cluster_exposure_rows: List[Dict[str, Any]] = []
    for cluster in cluster_keys:
        exposure_current = max(0.0, to_float(cluster_exposure_current.get(cluster, 0.0)))
        cap_value = max(0.0, to_float(cluster_cap_values.get(cluster, 0.0)))
        headroom = cap_value - exposure_current
        utilization = (exposure_current / cap_value) if cap_value > 1.0e-9 else 0.0
        cluster_exposure_rows.append(
            {
                "cluster": str(cluster),
                "exposure_current": round(exposure_current, 8),
                "cluster_cap_value": round(cap_value, 8),
                "headroom": round(headroom, 8),
                "utilization": round(utilization, 6),
            }
        )

    correlation_near_cap_candidates: List[Dict[str, Any]] = []
    raw_near_cap = raw.get("correlation_near_cap_candidates", [])
    if isinstance(raw_near_cap, list):
        for row in raw_near_cap:
            if not isinstance(row, dict):
                continue
            cluster_cap_value = max(0.0, to_float(row.get("cluster_cap_value", 0.0)))
            projected_exposure = max(0.0, to_float(row.get("projected_exposure", 0.0)))
            exposure_current = max(0.0, to_float(row.get("exposure_current", 0.0)))
            candidate_position_size = max(
                0.0,
                to_float(row.get("candidate_position_size", 0.0)),
            )
            headroom_before = (
                to_float(row.get("headroom_before", cluster_cap_value - exposure_current))
            )
            if cluster_cap_value > 1.0e-9:
                headroom_ratio = headroom_before / cluster_cap_value
            else:
                headroom_ratio = 0.0
            correlation_near_cap_candidates.append(
                {
                    "market": str(row.get("market", "")).strip(),
                    "cluster": str(row.get("cluster", row.get("cluster_id", ""))).strip(),
                    "exposure_current": round(exposure_current, 8),
                    "cluster_cap_value": round(cluster_cap_value, 8),
                    "candidate_position_size": round(candidate_position_size, 8),
                    "projected_exposure": round(projected_exposure, 8),
                    "headroom_before": round(headroom_before, 8),
                    "headroom_ratio": round(headroom_ratio, 6),
                    "rejected_by_cluster_cap": bool(
                        row.get("rejected_by_cluster_cap", False)
                    ),
                }
            )
    correlation_near_cap_candidates.sort(
        key=lambda item: (
            abs(to_float(item.get("headroom_ratio", 0.0))),
            abs(to_float(item.get("headroom_before", 0.0))),
            str(item.get("market", "")),
        )
    )
    correlation_near_cap_candidates = correlation_near_cap_candidates[:12]

    correlation_penalty_score_samples: List[Dict[str, Any]] = []
    raw_penalty_samples = raw.get("correlation_penalty_score_samples", [])
    if isinstance(raw_penalty_samples, list):
        for row in raw_penalty_samples:
            if not isinstance(row, dict):
                continue
            correlation_penalty_score_samples.append(
                {
                    "market": str(row.get("market", "")).strip(),
                    "cluster": str(row.get("cluster", "")).strip(),
                    "score_before_penalty": round(
                        to_float(row.get("score_before_penalty", 0.0)),
                        8,
                    ),
                    "penalty": round(to_float(row.get("penalty", 0.0)), 8),
                    "score_after_penalty": round(
                        to_float(row.get("score_after_penalty", 0.0)),
                        8,
                    ),
                    "rejected_by_penalty": bool(row.get("rejected_by_penalty", False)),
                }
            )
    correlation_penalty_score_samples = correlation_penalty_score_samples[:12]

    cluster_cap_debug_trace_samples: List[Dict[str, Any]] = []
    raw_cluster_debug = raw.get("cluster_cap_debug_trace_samples", [])
    if isinstance(raw_cluster_debug, list):
        for row in raw_cluster_debug:
            if not isinstance(row, dict):
                continue
            cluster_cap_debug_trace_samples.append(
                {
                    "market": str(row.get("market", "")).strip(),
                    "cluster": str(row.get("cluster", "")).strip(),
                    "cluster_exposure_before": round(
                        max(0.0, to_float(row.get("cluster_exposure_before", 0.0))),
                        8,
                    ),
                    "candidate_notional_fraction": round(
                        max(0.0, to_float(row.get("candidate_notional_fraction", 0.0))),
                        8,
                    ),
                    "cluster_cap_value": round(
                        max(0.0, to_float(row.get("cluster_cap_value", 0.0))),
                        8,
                    ),
                    "would_exceed": bool(row.get("would_exceed", False)),
                    "after_accept_cluster_exposure": round(
                        max(0.0, to_float(row.get("after_accept_cluster_exposure", 0.0))),
                        8,
                    ),
                }
            )
    cluster_cap_debug_trace_samples = cluster_cap_debug_trace_samples[:20]

    correlation_applied_telemetry = {
        "constraint_apply_stage": correlation_stage,
        "constraint_unit": correlation_unit,
        "cluster_cap_checks_total": int(correlation_cluster_eval_count),
        "cluster_cap_near_cap_count": int(correlation_cluster_near_cap_count),
        "cluster_cap_skips_count": int(counters["cluster_cap_skips_count"]),
        "cluster_cap_would_exceed_count": int(counters["cluster_cap_would_exceed_count"]),
        "cluster_exposure_update_count": int(counters["cluster_exposure_update_count"]),
        "corr_penalty_applied_count": int(correlation_penalty_applied_count),
        "corr_penalty_avg": float(correlation_penalty_avg),
        "corr_penalty_max": float(correlation_penalty_max),
        "penalty_score_samples": correlation_penalty_score_samples,
        "cluster_exposure_current": cluster_exposure_rows,
        "near_cap_candidates": correlation_near_cap_candidates,
        "cluster_cap_debug_trace_samples": cluster_cap_debug_trace_samples,
        "binding_detected": bool(
            counters["rejected_by_cluster_cap"] > 0 or correlation_penalty_applied_count > 0
        ),
    }

    exposure_summary = build_phase4_exposure_summary_from_samples(
        backtest_result=backtest_result,
        phase4_market_cluster_map=phase4_market_cluster_map,
    )

    return {
        "enabled": bool(enabled or counters["candidates_total"] > 0 or counters["selected_total"] > 0),
        "flags": flags,
        "allocator_policy": allocator_policy,
        "risk_budget_policy": risk_budget_policy,
        "drawdown_policy": drawdown_policy,
        "correlation_policy": correlation_policy,
        "execution_policy": execution_policy,
        "exposure_source": str(exposure_summary.get("source", "none")),
        "max_cluster_share": round(to_float(exposure_summary.get("max_cluster_share", 0.0)), 6),
        "total_selected_notional_exposure": round(
            to_float(exposure_summary.get("total_selected_notional_exposure", 0.0)),
            8,
        ),
        "exposure_by_cluster": exposure_summary.get("exposure_by_cluster", []),
        "correlation_applied_telemetry": correlation_applied_telemetry,
        "counters": counters,
        "selection_breakdown": {
            "candidates_total": int(counters["candidates_total"]),
            "selected_total": int(counters["selected_total"]),
            "selection_rate": float(counters["selection_rate"]),
            "rejected_by_budget": int(counters["rejected_by_budget"]),
            "rejected_by_cluster_cap": int(counters["rejected_by_cluster_cap"]),
            "rejected_by_correlation_penalty": int(counters["rejected_by_correlation_penalty"]),
            "cluster_cap_skips_count": int(counters["cluster_cap_skips_count"]),
            "cluster_cap_would_exceed_count": int(counters["cluster_cap_would_exceed_count"]),
            "cluster_exposure_update_count": int(counters["cluster_exposure_update_count"]),
            "rejected_by_execution_cap": int(counters["rejected_by_execution_cap"]),
            "rejected_by_drawdown_governor": int(counters["rejected_by_drawdown_governor"]),
        },
        "regime_budget_multiplier": {
            "applied_count": max(0, to_int(raw.get("regime_budget_multiplier_applied_count", 0))),
            "count_by_regime": regime_budget_multiplier_count_by_regime,
            "avg_by_regime": regime_budget_multiplier_avg_by_regime,
        },
        "top3_bottlenecks": reject_rows[:3],
    }


def build_candidate_generation_ab_playbook(
    component_shares: Dict[str, float],
    top_no_signal_reasons: List[Dict[str, Any]],
    top_manager_prefilter_reasons: List[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    items: List[Dict[str, Any]] = []
    no_signal_share = to_float(component_shares.get("no_signal_generated_share", 0.0))
    manager_share = to_float(component_shares.get("filtered_out_by_sizing_share", 0.0))
    no_best_share = to_float(component_shares.get("no_best_signal_share", 0.0))

    if no_signal_share >= 0.60:
        items.append(
            {
                "arm": "A_signal_supply",
                "trigger": "no_signal_generated share >= 0.60",
                "focus": "strategy-level candidate generation path",
                "target_metric": "candidate_generation_components.no_signal_generated_share",
                "evidence_reason": top_no_signal_reasons[0]["reason"] if top_no_signal_reasons else "",
                "proposal": (
                    "Add context-gated fallback archetype only for dominant no-signal market patterns "
                    "(keep low frequency; do not globally relax thresholds)."
                ),
            }
        )
    if manager_share >= 0.10:
        items.append(
            {
                "arm": "B_manager_prefilter",
                "trigger": "filtered_out_by_sizing share >= 0.10",
                "focus": "manager prefilter staging",
                "target_metric": "candidate_generation_components.filtered_out_by_sizing_share",
                "evidence_reason": top_manager_prefilter_reasons[0]["reason"] if top_manager_prefilter_reasons else "",
                "proposal": (
                    "Split manager prefilter into hard safety reject vs soft score queue, "
                    "then evaluate supply lift against shadow policy path."
                ),
            }
        )
    if no_best_share >= 0.15:
        items.append(
            {
                "arm": "C_best_signal_selection",
                "trigger": "no_best_signal share >= 0.15",
                "focus": "selection ranking / tie-break quality",
                "target_metric": "candidate_generation_components.no_best_signal_share",
                "evidence_reason": "",
                "proposal": (
                    "Audit ranking collapse cases and add deterministic tie-break diagnostics "
                    "before changing scoring weights."
                ),
            }
        )
    return items


def top_pattern_rows(pattern_counts: Dict[str, int], limit: int = 5) -> List[Dict[str, Any]]:
    if not pattern_counts:
        return []
    ordered = sorted(pattern_counts.items(), key=lambda item: (-int(item[1]), item[0]))
    return [{"pattern": key, "count": int(count)} for key, count in ordered[: max(1, int(limit))]]


def build_pattern_cell_profit_map(pattern_rows: Any) -> Dict[str, Dict[str, float]]:
    out: Dict[str, Dict[str, float]] = {}
    if not isinstance(pattern_rows, list):
        return out
    for row in pattern_rows:
        if not isinstance(row, dict):
            continue
        regime = str(row.get("regime", "")).strip() or "UNKNOWN"
        vol_bucket = str(row.get("volatility_bucket", "")).strip() or "vol_unknown"
        liq_bucket = str(row.get("liquidity_bucket", "")).strip() or "liq_unknown"
        strength_bucket = str(row.get("strength_bucket", "")).strip() or "strength_unknown"
        archetype = str(row.get("entry_archetype", "")).strip() or "UNSPECIFIED"
        pattern_key = (
            f"regime={regime}|vol={vol_bucket}|liq={liq_bucket}|"
            f"strength={strength_bucket}|arch={archetype}"
        )
        trades = max(0, to_int(row.get("total_trades", 0)))
        total_profit = to_float(row.get("total_profit", row.get("total_profit_krw", 0.0)))
        slot = out.setdefault(pattern_key, {"trades": 0.0, "total_profit_krw": 0.0})
        slot["trades"] += float(trades)
        slot["total_profit_krw"] += float(total_profit)
    return out


def top_loss_pattern_cells(
    pattern_profit_map: Dict[str, Dict[str, float]],
    limit: int = 6,
) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for key, item in pattern_profit_map.items():
        if not isinstance(item, dict):
            continue
        trades = max(0, to_int(item.get("trades", 0)))
        total_profit = to_float(item.get("total_profit_krw", 0.0))
        if trades <= 0:
            continue
        rows.append(
            {
                "pattern": str(key),
                "trades": int(trades),
                "total_profit_krw": round(total_profit, 4),
                "avg_profit_krw": round(total_profit / float(trades), 4),
            }
        )
    rows.sort(key=lambda x: (float(x["total_profit_krw"]), -int(x["trades"]), str(x["pattern"])))
    return rows[: max(1, int(limit))]


def bounded(value: float, low: float, high: float) -> float:
    return max(float(low), min(float(high), float(value)))


def compute_risk_adjusted_score_components(
    *,
    expectancy_krw: float,
    profit_factor: float,
    avg_fee_krw: float,
    max_drawdown_pct: float,
    downtrend_loss_per_trade_krw: float,
    downtrend_trade_share: float,
    total_trades: int,
    risk_adjusted_score_policy: Dict[str, Any] = None,
) -> Dict[str, Any]:
    policy = risk_adjusted_score_policy if isinstance(risk_adjusted_score_policy, dict) else {}

    def _float(key: str, default: float) -> float:
        try:
            value = policy.get(key, default)
            return float(value)
        except Exception:
            return float(default)

    def _int(key: str, default: int) -> int:
        try:
            value = policy.get(key, default)
            return int(float(value))
        except Exception:
            return int(default)

    expectancy_scale_krw = max(1.0e-9, _float("ev_expectancy_scale_krw", 10.0))
    expectancy_multiplier = max(0.0, _float("ev_expectancy_term_multiplier", 1.0))
    expectancy_term_cap = max(0.1, _float("ev_expectancy_term_cap", 3.0))

    profit_factor_center = _float("ev_profit_factor_center", 1.0)
    profit_factor_slope = _float("ev_profit_factor_slope", 2.0)
    profit_factor_multiplier = max(0.0, _float("ev_profit_factor_term_multiplier", 1.0))
    profit_factor_term_cap = max(0.1, _float("ev_profit_factor_term_cap", 2.0))

    cost_fee_scale_krw = max(1.0e-9, _float("cost_fee_scale_krw", 500.0))
    cost_term_multiplier = max(0.0, _float("cost_term_multiplier", 0.0))
    cost_term_cap = max(0.0, _float("cost_term_cap", 1.5))

    drawdown_scale_pct = max(1.0e-9, _float("risk_drawdown_scale_pct", 6.0))
    risk_term_multiplier = max(0.0, _float("risk_term_multiplier", 1.0))
    risk_term_cap = max(0.1, _float("risk_term_cap", 3.0))

    hostility_loss_scale_krw = max(1.0e-9, _float("hostility_downtrend_loss_scale_krw", 8.0))
    hostility_loss_multiplier = max(0.0, _float("hostility_downtrend_loss_multiplier", 1.0))
    hostility_loss_cap = max(0.0, _float("hostility_downtrend_loss_cap", 3.0))

    hostility_share_threshold = _float("hostility_downtrend_share_threshold", 0.50)
    hostility_share_slope = _float("hostility_downtrend_share_slope", 4.0)
    hostility_share_multiplier = max(0.0, _float("hostility_downtrend_share_multiplier", 1.0))
    hostility_share_cap = max(0.0, _float("hostility_downtrend_share_cap", 2.0))

    confidence_low_trade_threshold = max(1, _int("confidence_low_trade_threshold", 10))
    confidence_low_trade_penalty = max(0.0, _float("confidence_low_trade_penalty", 0.5))
    confidence_term_multiplier = max(0.0, _float("confidence_term_multiplier", 1.0))
    confidence_term_cap = max(0.0, _float("confidence_term_cap", 2.0))

    expectancy_term = bounded(
        (float(expectancy_krw) / expectancy_scale_krw) * expectancy_multiplier,
        -expectancy_term_cap,
        expectancy_term_cap,
    )
    profit_factor_term = bounded(
        ((float(profit_factor) - profit_factor_center) * profit_factor_slope) *
        profit_factor_multiplier,
        -profit_factor_term_cap,
        profit_factor_term_cap,
    )
    ev_term = expectancy_term + profit_factor_term
    cost_term = bounded(
        (max(0.0, float(avg_fee_krw)) / cost_fee_scale_krw) * cost_term_multiplier,
        0.0,
        cost_term_cap,
    )
    drawdown_penalty = bounded(
        (float(max_drawdown_pct) / drawdown_scale_pct) * risk_term_multiplier,
        0.0,
        risk_term_cap,
    )
    downtrend_loss_penalty = bounded(
        (float(downtrend_loss_per_trade_krw) / hostility_loss_scale_krw) *
        hostility_loss_multiplier,
        0.0,
        hostility_loss_cap,
    )
    downtrend_share_penalty = (
        bounded(
            (float(downtrend_trade_share) - hostility_share_threshold) * hostility_share_slope *
            hostility_share_multiplier,
            0.0,
            hostility_share_cap,
        )
        if float(downtrend_trade_share) > hostility_share_threshold
        else 0.0
    )
    low_trade_penalty = (
        confidence_low_trade_penalty if int(total_trades) < confidence_low_trade_threshold else 0.0
    )
    confidence_term = bounded(
        low_trade_penalty * confidence_term_multiplier,
        0.0,
        confidence_term_cap,
    )
    hostility_term = downtrend_loss_penalty + downtrend_share_penalty
    risk_term = drawdown_penalty
    score = (
        ev_term
        - cost_term
        - risk_term
        - hostility_term
        - confidence_term
    )
    return {
        "expectancy_term": round(expectancy_term, 4),
        "profit_factor_term": round(profit_factor_term, 4),
        "drawdown_penalty": round(drawdown_penalty, 4),
        "downtrend_loss_penalty": round(downtrend_loss_penalty, 4),
        "downtrend_share_penalty": round(downtrend_share_penalty, 4),
        "low_trade_penalty": round(low_trade_penalty, 4),
        "ev_term": round(ev_term, 4),
        "cost_term": round(cost_term, 4),
        "risk_term": round(risk_term, 4),
        "hostility_term": round(hostility_term, 4),
        "confidence_term": round(confidence_term, 4),
        "score_total": round(score, 4),
        "score": round(score, 4),
        "normalization": {
            "ev_expectancy_scale_krw": round(expectancy_scale_krw, 8),
            "ev_expectancy_term_multiplier": round(expectancy_multiplier, 8),
            "ev_expectancy_term_cap": round(expectancy_term_cap, 8),
            "ev_profit_factor_center": round(profit_factor_center, 8),
            "ev_profit_factor_slope": round(profit_factor_slope, 8),
            "ev_profit_factor_term_multiplier": round(profit_factor_multiplier, 8),
            "ev_profit_factor_term_cap": round(profit_factor_term_cap, 8),
            "cost_fee_scale_krw": round(cost_fee_scale_krw, 8),
            "cost_term_multiplier": round(cost_term_multiplier, 8),
            "cost_term_cap": round(cost_term_cap, 8),
            "risk_drawdown_scale_pct": round(drawdown_scale_pct, 8),
            "risk_term_multiplier": round(risk_term_multiplier, 8),
            "risk_term_cap": round(risk_term_cap, 8),
            "hostility_downtrend_loss_scale_krw": round(hostility_loss_scale_krw, 8),
            "hostility_downtrend_loss_multiplier": round(hostility_loss_multiplier, 8),
            "hostility_downtrend_loss_cap": round(hostility_loss_cap, 8),
            "hostility_downtrend_share_threshold": round(hostility_share_threshold, 8),
            "hostility_downtrend_share_slope": round(hostility_share_slope, 8),
            "hostility_downtrend_share_multiplier": round(hostility_share_multiplier, 8),
            "hostility_downtrend_share_cap": round(hostility_share_cap, 8),
            "confidence_low_trade_threshold": int(confidence_low_trade_threshold),
            "confidence_low_trade_penalty": round(confidence_low_trade_penalty, 8),
            "confidence_term_multiplier": round(confidence_term_multiplier, 8),
            "confidence_term_cap": round(confidence_term_cap, 8),
        },
    }


def build_loss_tail_decomposition(
    *,
    pattern_rows: Any,
    dataset_name: str,
    top_cell_limit: int = 8,
) -> Dict[str, Any]:
    negative_profit_total = 0.0
    regime_loss_abs: Dict[str, float] = {}
    archetype_loss_abs: Dict[str, float] = {}
    cell_rows: List[Dict[str, Any]] = []

    if not isinstance(pattern_rows, list):
        pattern_rows = []

    for row in pattern_rows:
        if not isinstance(row, dict):
            continue
        trades = max(0, to_int(row.get("total_trades", 0)))
        total_profit = to_float(row.get("total_profit", row.get("total_profit_krw", 0.0)))
        if trades <= 0 or total_profit >= 0.0:
            continue

        regime = str(row.get("regime", "")).strip() or "UNKNOWN"
        archetype = str(row.get("entry_archetype", "")).strip() or "UNSPECIFIED"
        vol_bucket = str(row.get("volatility_bucket", "")).strip() or "vol_unknown"
        liq_bucket = str(row.get("liquidity_bucket", "")).strip() or "liq_unknown"
        strength_bucket = str(row.get("strength_bucket", "")).strip() or "strength_unknown"
        pattern_key = (
            f"regime={regime}|vol={vol_bucket}|liq={liq_bucket}|"
            f"strength={strength_bucket}|arch={archetype}"
        )

        loss_abs = -float(total_profit)
        negative_profit_total += loss_abs
        regime_loss_abs[regime] = regime_loss_abs.get(regime, 0.0) + loss_abs
        archetype_loss_abs[archetype] = archetype_loss_abs.get(archetype, 0.0) + loss_abs

        cell_rows.append(
            {
                "dataset": str(dataset_name),
                "pattern": pattern_key,
                "regime": regime,
                "entry_archetype": archetype,
                "trades": int(trades),
                "total_profit_krw": round(float(total_profit), 4),
                "avg_profit_krw": round(float(total_profit) / float(trades), 4),
                "loss_abs_krw": round(loss_abs, 4),
            }
        )

    cell_rows.sort(key=lambda x: (float(x["total_profit_krw"]), -int(x["trades"]), str(x["pattern"])))
    top_cells = cell_rows[: max(1, int(top_cell_limit))]

    cumulative = 0.0
    for item in top_cells:
        loss_abs = max(0.0, to_float(item.get("loss_abs_krw", 0.0)))
        share = (loss_abs / negative_profit_total) if negative_profit_total > 0.0 else 0.0
        cumulative += share
        item["loss_share"] = round(share, 4)
        item["loss_share_cumulative"] = round(cumulative, 4)

    def _top_loss_rows(loss_map: Dict[str, float], key_name: str, limit: int) -> List[Dict[str, Any]]:
        rows = []
        for key, loss_abs in loss_map.items():
            share = (loss_abs / negative_profit_total) if negative_profit_total > 0.0 else 0.0
            rows.append(
                {
                    key_name: str(key),
                    "loss_abs_krw": round(float(loss_abs), 4),
                    "loss_share": round(float(share), 4),
                }
            )
        rows.sort(key=lambda x: (-to_float(x["loss_abs_krw"]), str(x[key_name])))
        return rows[: max(1, int(limit))]

    top_loss_regimes = _top_loss_rows(regime_loss_abs, "regime", limit=5) if regime_loss_abs else []
    top_loss_archetypes = (
        _top_loss_rows(archetype_loss_abs, "entry_archetype", limit=5)
        if archetype_loss_abs else []
    )
    top3_concentration = 0.0
    if negative_profit_total > 0.0 and top_cells:
        top3_abs = sum(max(0.0, to_float(x.get("loss_abs_krw", 0.0))) for x in top_cells[:3])
        top3_concentration = top3_abs / negative_profit_total

    return {
        "negative_profit_abs_krw": round(negative_profit_total, 4),
        "negative_cell_count": len(cell_rows),
        "top3_loss_concentration": round(top3_concentration, 4),
        "top_loss_cells": top_cells,
        "top_loss_regimes": top_loss_regimes,
        "top_loss_archetypes": top_loss_archetypes,
        "loss_by_regime_abs_krw": {
            str(k): round(float(v), 4) for k, v in sorted(regime_loss_abs.items(), key=lambda item: item[0])
        },
        "loss_by_archetype_abs_krw": {
            str(k): round(float(v), 4) for k, v in sorted(archetype_loss_abs.items(), key=lambda item: item[0])
        },
    }


def build_strategy_funnel_diagnostics(strategy_signal_funnel: Any) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    if isinstance(strategy_signal_funnel, list):
        for raw in strategy_signal_funnel:
            if not isinstance(raw, dict):
                continue
            strategy_name = str(raw.get("strategy_name", "")).strip() or "unknown"
            generated = max(0, to_int(raw.get("generated_signals", 0)))
            selected = max(0, to_int(raw.get("selected_best", 0)))
            blocked_by_risk_manager = max(0, to_int(raw.get("blocked_by_risk_manager", 0)))
            executed = max(0, to_int(raw.get("entries_executed", 0)))
            selection_rate = (float(selected) / float(generated)) if generated > 0 else 0.0
            execution_rate = (float(executed) / float(generated)) if generated > 0 else 0.0
            rows.append(
                {
                    "strategy_name": strategy_name,
                    "generated_signals": generated,
                    "selected_best": selected,
                    "blocked_by_risk_manager": blocked_by_risk_manager,
                    "entries_executed": executed,
                    "selection_rate": round(selection_rate, 4),
                    "execution_rate": round(execution_rate, 4),
                }
            )

    top_generated = sorted(
        rows,
        key=lambda item: (-int(item["generated_signals"]), item["strategy_name"]),
    )[:3]
    weak_execution = sorted(
        [x for x in rows if int(x["generated_signals"]) >= 10],
        key=lambda item: (float(item["execution_rate"]), -int(item["generated_signals"]), item["strategy_name"]),
    )[:3]
    return {
        "strategy_count": len(rows),
        "top_generated": top_generated,
        "weak_execution_under_load": weak_execution,
    }


def build_strategy_collection_diagnostics(strategy_collection_summaries: Any) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    if isinstance(strategy_collection_summaries, list):
        for raw in strategy_collection_summaries:
            if not isinstance(raw, dict):
                continue
            strategy_name = str(raw.get("strategy_name", "")).strip() or "unknown"
            skipped_disabled = max(0, to_int(raw.get("skipped_disabled", 0)))
            no_signal = max(0, to_int(raw.get("no_signal", 0)))
            generated = max(0, to_int(raw.get("generated", 0)))
            total_attempted = skipped_disabled + no_signal + generated
            no_signal_rate = (float(no_signal) / float(total_attempted)) if total_attempted > 0 else 0.0
            rows.append(
                {
                    "strategy_name": strategy_name,
                    "skipped_disabled": skipped_disabled,
                    "no_signal": no_signal,
                    "generated": generated,
                    "total_attempted": total_attempted,
                    "no_signal_rate": round(no_signal_rate, 4),
                }
            )

    top_no_signal = sorted(
        [x for x in rows if int(x["no_signal"]) > 0],
        key=lambda item: (-int(item["no_signal"]), item["strategy_name"]),
    )[:3]
    top_skipped_disabled = sorted(
        [x for x in rows if int(x["skipped_disabled"]) > 0],
        key=lambda item: (-int(item["skipped_disabled"]), item["strategy_name"]),
    )[:3]
    return {
        "strategy_count": len(rows),
        "top_no_signal": top_no_signal,
        "top_skipped_disabled": top_skipped_disabled,
    }


def build_top_loss_trade_samples(trade_history_samples: Any, limit: int = 8) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    if not isinstance(trade_history_samples, list):
        return rows
    for raw in trade_history_samples:
        if not isinstance(raw, dict):
            continue
        profit_loss_krw = to_float(raw.get("profit_loss_krw", 0.0))
        if profit_loss_krw >= 0.0:
            continue
        row = {
            "market": str(raw.get("market", "")).strip(),
            "strategy_name": str(raw.get("strategy_name", "")).strip(),
            "entry_archetype": str(raw.get("entry_archetype", "")).strip(),
            "regime": str(raw.get("regime", "")).strip(),
            "profit_loss_krw": round(profit_loss_krw, 4),
            "profit_loss_pct": round(to_float(raw.get("profit_loss_pct", 0.0)), 8),
            "holding_minutes": round(to_float(raw.get("holding_minutes", 0.0)), 4),
            "signal_filter": round(to_float(raw.get("signal_filter", 0.0)), 6),
            "signal_strength": round(to_float(raw.get("signal_strength", 0.0)), 6),
            "liquidity_score": round(to_float(raw.get("liquidity_score", 0.0)), 6),
            "volatility": round(to_float(raw.get("volatility", 0.0)), 6),
            "expected_value": round(to_float(raw.get("expected_value", 0.0)), 8),
            "reward_risk_ratio": round(to_float(raw.get("reward_risk_ratio", 0.0)), 6),
            "probabilistic_h5_calibrated": round(
                to_float(raw.get("probabilistic_h5_calibrated", 0.0)),
                8,
            ),
            "probabilistic_h5_margin": round(to_float(raw.get("probabilistic_h5_margin", 0.0)), 8),
            "exit_reason": str(raw.get("exit_reason", "")).strip(),
        }
        rows.append(row)
    rows.sort(key=lambda item: (to_float(item.get("profit_loss_krw", 0.0)), item.get("entry_archetype", "")))
    return rows[: max(0, int(limit))]


def build_probability_calibration_telemetry(
    backtest_result: Dict[str, Any],
    *,
    sample_limit: int = 20,
) -> Dict[str, Any]:
    raw_samples = backtest_result.get("phase4_candidate_snapshot_samples", [])
    if not isinstance(raw_samples, list) or not raw_samples:
        return {
            "enabled": False,
            "source": "phase4_candidate_snapshot_samples",
            "snapshot_sample_total": 0,
            "paired_sample_count": 0,
            "p_raw_distribution": {"count": 0},
            "p_cal_distribution": {"count": 0},
            "implied_win_distribution": {"count": 0},
            "margin_distribution": {"count": 0},
            "ev_bps_distribution": {"count": 0},
            "gt_0_5_ratio": {"raw": None, "cal": None},
            "raw_to_cal_samples": [],
        }

    rows: List[Dict[str, Any]] = []
    backend_counts: Dict[str, int] = {}
    for item in raw_samples:
        if not isinstance(item, dict):
            continue
        backend = str(item.get("prob_model_backend", "")).strip().lower() or "unknown"
        backend_counts[backend] = backend_counts.get(backend, 0) + 1
        p_raw = to_float(item.get("prob_confidence_raw", float("nan")))
        p_cal = to_float(
            item.get(
                "prob_confidence_calibrated",
                item.get("prob_confidence", float("nan")),
            )
        )
        if not (math.isfinite(p_raw) and math.isfinite(p_cal)):
            continue
        p_raw = max(0.0, min(1.0, float(p_raw)))
        p_cal = max(0.0, min(1.0, float(p_cal)))
        implied_win = to_float(item.get("implied_win", float("nan")))
        margin = to_float(item.get("margin", float("nan")))
        ev_pct = to_float(item.get("expected_edge_after_cost_pct", float("nan")))
        rows.append(
            {
                "decision_time": max(0, to_int(item.get("decision_time", 0))),
                "market": str(item.get("market", item.get("market_id", ""))).strip(),
                "strategy_name": str(item.get("strategy_name", "")).strip(),
                "backend": backend,
                "p_raw": p_raw,
                "p_cal": p_cal,
                "delta": p_cal - p_raw,
                "implied_win": implied_win,
                "margin": margin,
                "ev_bps": (ev_pct * 10000.0) if math.isfinite(ev_pct) else float("nan"),
            }
        )

    if not rows:
        return {
            "enabled": False,
            "source": "phase4_candidate_snapshot_samples",
            "snapshot_sample_total": int(len(raw_samples)),
            "paired_sample_count": 0,
            "p_raw_distribution": {"count": 0},
            "p_cal_distribution": {"count": 0},
            "implied_win_distribution": {"count": 0},
            "margin_distribution": {"count": 0},
            "ev_bps_distribution": {"count": 0},
            "gt_0_5_ratio": {"raw": None, "cal": None},
            "raw_to_cal_samples": [],
        }

    raw_values = [float(x["p_raw"]) for x in rows]
    cal_values = [float(x["p_cal"]) for x in rows]
    implied_values = [float(x["implied_win"]) for x in rows if math.isfinite(to_float(x.get("implied_win", float("nan"))))]
    margin_values = [float(x["margin"]) for x in rows if math.isfinite(to_float(x.get("margin", float("nan"))))]
    ev_bps_values = [float(x["ev_bps"]) for x in rows if math.isfinite(to_float(x.get("ev_bps", float("nan"))))]

    def _dist(values: List[float]) -> Dict[str, Any]:
        if not values:
            return {"count": 0}
        n = len(values)
        mean_v = float(sum(values)) / float(n)
        var_v = float(sum((v - mean_v) * (v - mean_v) for v in values)) / float(n)
        return {
            "count": int(n),
            "mean": round(mean_v, 10),
            "std": round(math.sqrt(max(0.0, var_v)), 10),
            "p95": round(quantile(values, 0.95), 10),
            "p99": round(quantile(values, 0.99), 10),
            "min": round(min(values), 10),
            "max": round(max(values), 10),
        }

    rows.sort(
        key=lambda item: (
            int(item.get("decision_time", 0)),
            str(item.get("market", "")),
            str(item.get("strategy_name", "")),
        )
    )
    sample_cap = max(1, int(sample_limit))
    sample_rows = rows[:sample_cap]
    backend_effective = "unknown"
    if backend_counts:
        backend_effective = sorted(
            backend_counts.items(),
            key=lambda item: (-int(item[1]), str(item[0])),
        )[0][0]

    return {
        "enabled": True,
        "source": "phase4_candidate_snapshot_samples",
        "snapshot_sample_total": int(len(raw_samples)),
        "paired_sample_count": int(len(rows)),
        "prob_model_backend_counts": {
            str(k): int(max(0, to_int(v))) for k, v in sorted(backend_counts.items(), key=lambda x: str(x[0]))
        },
        "prob_model_backend_effective": str(backend_effective),
        "p_raw_distribution": _dist(raw_values),
        "p_cal_distribution": _dist(cal_values),
        "implied_win_distribution": _dist(implied_values),
        "margin_distribution": _dist(margin_values),
        "ev_bps_distribution": _dist(ev_bps_values),
        "gt_0_5_ratio": {
            "raw": round(
                float(sum(1 for v in raw_values if v > 0.5)) / float(max(1, len(raw_values))),
                6,
            ),
            "cal": round(
                float(sum(1 for v in cal_values if v > 0.5)) / float(max(1, len(cal_values))),
                6,
            ),
        },
        "raw_to_cal_samples": [
            {
                "decision_time": int(row["decision_time"]),
                "market": row["market"],
                "strategy_name": row["strategy_name"],
                "backend": row["backend"],
                "p_raw": round(float(row["p_raw"]), 8),
                "p_cal": round(float(row["p_cal"]), 8),
                "delta": round(float(row["delta"]), 8),
            }
            for row in sample_rows
        ],
    }


def build_lgbm_ev_affine_telemetry(
    backtest_result: Dict[str, Any],
    *,
    sample_limit: int = 20,
) -> Dict[str, Any]:
    raw_samples = backtest_result.get("phase4_candidate_snapshot_samples", [])
    if not isinstance(raw_samples, list) or not raw_samples:
        return {
            "enabled": False,
            "source": "phase4_candidate_snapshot_samples",
            "snapshot_sample_total": 0,
            "paired_sample_count": 0,
            "raw_edge_bps_distribution": {"count": 0},
            "corrected_edge_bps_distribution": {"count": 0},
            "affine_applied_ratio": None,
            "affine_enabled_ratio": None,
            "affine_params_observed": [],
            "raw_to_corrected_samples": [],
        }

    rows: List[Dict[str, Any]] = []
    for item in raw_samples:
        if not isinstance(item, dict):
            continue
        raw_bps = to_float(item.get("expected_edge_calibrated_raw_bps", float("nan")))
        corrected_bps = to_float(
            item.get(
                "expected_edge_calibrated_corrected_bps",
                item.get("expected_edge_calibrated_bps", float("nan")),
            )
        )
        if not (math.isfinite(raw_bps) and math.isfinite(corrected_bps)):
            continue
        affine_enabled = bool(item.get("lgbm_ev_affine_enabled", False))
        affine_applied = bool(item.get("lgbm_ev_affine_applied", False))
        scale = to_float(item.get("lgbm_ev_affine_scale", float("nan")))
        shift = to_float(item.get("lgbm_ev_affine_shift", float("nan")))
        rows.append(
            {
                "decision_time": max(0, to_int(item.get("decision_time", 0))),
                "market": str(item.get("market", item.get("market_id", ""))).strip(),
                "strategy_name": str(item.get("strategy_name", "")).strip(),
                "raw_bps": float(raw_bps),
                "corrected_bps": float(corrected_bps),
                "delta_bps": float(corrected_bps - raw_bps),
                "affine_enabled": affine_enabled,
                "affine_applied": affine_applied,
                "scale": scale if math.isfinite(scale) else None,
                "shift": shift if math.isfinite(shift) else None,
            }
        )

    if not rows:
        return {
            "enabled": False,
            "source": "phase4_candidate_snapshot_samples",
            "snapshot_sample_total": int(len(raw_samples)),
            "paired_sample_count": 0,
            "raw_edge_bps_distribution": {"count": 0},
            "corrected_edge_bps_distribution": {"count": 0},
            "affine_applied_ratio": None,
            "affine_enabled_ratio": None,
            "affine_params_observed": [],
            "raw_to_corrected_samples": [],
        }

    def _dist(values: List[float]) -> Dict[str, Any]:
        if not values:
            return {"count": 0}
        n = len(values)
        mean_v = float(sum(values)) / float(n)
        var_v = float(sum((v - mean_v) * (v - mean_v) for v in values)) / float(n)
        return {
            "count": int(n),
            "mean": round(mean_v, 10),
            "median": round(quantile(values, 0.50), 10),
            "p95": round(quantile(values, 0.95), 10),
            "std": round(math.sqrt(max(0.0, var_v)), 10),
            "min": round(min(values), 10),
            "max": round(max(values), 10),
        }

    raw_values = [float(x["raw_bps"]) for x in rows]
    corrected_values = [float(x["corrected_bps"]) for x in rows]
    applied_count = sum(1 for x in rows if bool(x.get("affine_applied", False)))
    enabled_count = sum(1 for x in rows if bool(x.get("affine_enabled", False)))
    n_rows = max(1, len(rows))

    param_set: Dict[Tuple[float, float], int] = {}
    for x in rows:
        scale = x.get("scale", None)
        shift = x.get("shift", None)
        if scale is None or shift is None:
            continue
        key = (round(float(scale), 10), round(float(shift), 10))
        param_set[key] = param_set.get(key, 0) + 1
    param_rows = [
        {"scale": k[0], "shift": k[1], "count": int(v)}
        for k, v in sorted(param_set.items(), key=lambda item: (-item[1], item[0][0], item[0][1]))
    ]

    rows.sort(
        key=lambda item: (
            int(item.get("decision_time", 0)),
            str(item.get("market", "")),
            str(item.get("strategy_name", "")),
        )
    )
    sample_rows = rows[: max(1, int(sample_limit))]

    return {
        "enabled": True,
        "source": "phase4_candidate_snapshot_samples",
        "snapshot_sample_total": int(len(raw_samples)),
        "paired_sample_count": int(len(rows)),
        "raw_edge_bps_distribution": _dist(raw_values),
        "corrected_edge_bps_distribution": _dist(corrected_values),
        "affine_applied_ratio": round(float(applied_count) / float(n_rows), 6),
        "affine_enabled_ratio": round(float(enabled_count) / float(n_rows), 6),
        "affine_params_observed": param_rows,
        "raw_to_corrected_samples": [
            {
                "decision_time": int(row["decision_time"]),
                "market": row["market"],
                "strategy_name": row["strategy_name"],
                "raw_bps": round(float(row["raw_bps"]), 8),
                "corrected_bps": round(float(row["corrected_bps"]), 8),
                "delta_bps": round(float(row["delta_bps"]), 8),
                "affine_enabled": bool(row["affine_enabled"]),
                "affine_applied": bool(row["affine_applied"]),
                "scale": row["scale"],
                "shift": row["shift"],
            }
            for row in sample_rows
        ],
    }


def build_dataset_diagnostics(
    dataset_name: str,
    backtest_result: Dict[str, Any],
    phase4_market_cluster_map: Dict[str, str],
    bundle_meta: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    entry_funnel = backtest_result.get("entry_funnel", {})
    if not isinstance(entry_funnel, dict):
        entry_funnel = {}
    shadow_funnel_raw = backtest_result.get("shadow_funnel", {})
    if not isinstance(shadow_funnel_raw, dict):
        shadow_funnel_raw = {}

    entry_rounds = max(0, to_int(entry_funnel.get("entry_rounds", 0)))
    skipped_due_to_open_position = max(0, to_int(entry_funnel.get("skipped_due_to_open_position", 0)))
    no_signal_generated = max(0, to_int(entry_funnel.get("no_signal_generated", 0)))
    filtered_out_by_sizing = max(0, to_int(entry_funnel.get("filtered_out_by_sizing", 0)))
    filtered_out_by_policy = max(0, to_int(entry_funnel.get("filtered_out_by_policy", 0)))
    gate_system_version_effective = str(
        entry_funnel.get("gate_system_version_effective", "vnext")
    ).strip().lower() or "vnext"
    quality_topk_effective = max(0, to_int(entry_funnel.get("quality_topk_effective", 0)))
    stage_funnel_vnext_raw = entry_funnel.get("stage_funnel_vnext", {})
    if not isinstance(stage_funnel_vnext_raw, dict):
        stage_funnel_vnext_raw = {}
    gate_vnext_s0_snapshots_valid = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "s0_snapshots_valid",
                entry_funnel.get("gate_vnext_s0_snapshots_valid", 0),
            )
        ),
    )
    gate_vnext_s1_selected_topk = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "s1_selected_topk",
                entry_funnel.get("gate_vnext_s1_selected_topk", 0),
            )
        ),
    )
    gate_vnext_s2_sized_count = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "s2_sized_count",
                entry_funnel.get("gate_vnext_s2_sized_count", 0),
            )
        ),
    )
    gate_vnext_s3_exec_gate_pass = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "s3_exec_gate_pass",
                entry_funnel.get("gate_vnext_s3_exec_gate_pass", 0),
            )
        ),
    )
    gate_vnext_s4_submitted = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "s4_submitted",
                entry_funnel.get("gate_vnext_s4_submitted", 0),
            )
        ),
    )
    gate_vnext_s5_filled = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "s5_filled",
                entry_funnel.get("gate_vnext_s5_filled", 0),
            )
        ),
    )
    gate_vnext_drop_ev_negative_count = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "drop_ev_negative_count",
                entry_funnel.get("gate_vnext_drop_ev_negative_count", 0),
            )
        ),
    )
    gate_vnext_ev_negative_size_zero_count = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "ev_negative_size_zero_count",
                entry_funnel.get("gate_vnext_ev_negative_size_zero_count", 0),
            )
        ),
    )
    gate_vnext_ev_positive_size_gt_zero_count = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "ev_positive_size_gt_zero_count",
                entry_funnel.get("gate_vnext_ev_positive_size_gt_zero_count", 0),
            )
        ),
    )
    gate_vnext_expected_value_from_prob_min_bps = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "expected_value_from_prob_min_bps",
                entry_funnel.get("gate_vnext_expected_value_from_prob_min_bps", 0.0),
            )
        ),
        10,
    )
    gate_vnext_expected_value_from_prob_median_bps = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "expected_value_from_prob_median_bps",
                entry_funnel.get("gate_vnext_expected_value_from_prob_median_bps", 0.0),
            )
        ),
        10,
    )
    gate_vnext_expected_value_from_prob_max_bps = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "expected_value_from_prob_max_bps",
                entry_funnel.get("gate_vnext_expected_value_from_prob_max_bps", 0.0),
            )
        ),
        10,
    )
    gate_vnext_p_cal_min = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "p_cal_min",
                entry_funnel.get("gate_vnext_p_cal_min", 0.0),
            )
        ),
        10,
    )
    gate_vnext_p_cal_median = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "p_cal_median",
                entry_funnel.get("gate_vnext_p_cal_median", 0.0),
            )
        ),
        10,
    )
    gate_vnext_p_cal_max = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "p_cal_max",
                entry_funnel.get("gate_vnext_p_cal_max", 0.0),
            )
        ),
        10,
    )
    gate_vnext_tp_pct_min = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "tp_pct_min",
                entry_funnel.get("gate_vnext_tp_pct_min", 0.0),
            )
        ),
        10,
    )
    gate_vnext_tp_pct_median = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "tp_pct_median",
                entry_funnel.get("gate_vnext_tp_pct_median", 0.0),
            )
        ),
        10,
    )
    gate_vnext_tp_pct_max = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "tp_pct_max",
                entry_funnel.get("gate_vnext_tp_pct_max", 0.0),
            )
        ),
        10,
    )
    gate_vnext_sl_pct_min = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "sl_pct_min",
                entry_funnel.get("gate_vnext_sl_pct_min", 0.0),
            )
        ),
        10,
    )
    gate_vnext_sl_pct_median = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "sl_pct_median",
                entry_funnel.get("gate_vnext_sl_pct_median", 0.0),
            )
        ),
        10,
    )
    gate_vnext_sl_pct_max = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "sl_pct_max",
                entry_funnel.get("gate_vnext_sl_pct_max", 0.0),
            )
        ),
        10,
    )
    gate_vnext_ev_in_min_bps = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "ev_in_min_bps",
                entry_funnel.get("gate_vnext_ev_in_min_bps", 0.0),
            )
        ),
        10,
    )
    gate_vnext_ev_in_median_bps = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "ev_in_median_bps",
                entry_funnel.get("gate_vnext_ev_in_median_bps", 0.0),
            )
        ),
        10,
    )
    gate_vnext_ev_in_max_bps = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "ev_in_max_bps",
                entry_funnel.get("gate_vnext_ev_in_max_bps", 0.0),
            )
        ),
        10,
    )
    gate_vnext_ev_for_size_min_bps = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "ev_for_size_min_bps",
                entry_funnel.get("gate_vnext_ev_for_size_min_bps", 0.0),
            )
        ),
        10,
    )
    gate_vnext_ev_for_size_median_bps = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "ev_for_size_median_bps",
                entry_funnel.get("gate_vnext_ev_for_size_median_bps", 0.0),
            )
        ),
        10,
    )
    gate_vnext_ev_for_size_max_bps = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "ev_for_size_max_bps",
                entry_funnel.get("gate_vnext_ev_for_size_max_bps", 0.0),
            )
        ),
        10,
    )
    gate_vnext_size_fraction_min = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "size_fraction_min",
                entry_funnel.get("gate_vnext_size_fraction_min", 0.0),
            )
        ),
        10,
    )
    gate_vnext_size_fraction_median = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "size_fraction_median",
                entry_funnel.get("gate_vnext_size_fraction_median", 0.0),
            )
        ),
        10,
    )
    gate_vnext_size_fraction_max = round(
        to_float(
            stage_funnel_vnext_raw.get(
                "size_fraction_max",
                entry_funnel.get("gate_vnext_size_fraction_max", 0.0),
            )
        ),
        10,
    )
    gate_vnext_scan_rounds = max(
        0,
        to_int(
            stage_funnel_vnext_raw.get(
                "scan_rounds",
                entry_funnel.get("gate_vnext_scan_rounds", 0),
            )
        ),
    )
    gate_vnext_topk_bound = int(gate_vnext_scan_rounds * max(1, quality_topk_effective))
    gate_vnext_s1_exceeds_topk_bound = bool(
        gate_vnext_scan_rounds > 0 and gate_vnext_s1_selected_topk > gate_vnext_topk_bound
    )
    reject_regime_entry_disabled_count = max(
        0, to_int(entry_funnel.get("reject_regime_entry_disabled_count", 0))
    )
    regime_entry_disable_enabled = bool(entry_funnel.get("regime_entry_disable_enabled", False))
    raw_regime_entry_disable = entry_funnel.get("regime_entry_disable", {})
    regime_entry_disable: Dict[str, bool] = {}
    if isinstance(raw_regime_entry_disable, dict):
        for key, value in raw_regime_entry_disable.items():
            regime_key = str(key).strip()
            if not regime_key:
                continue
            regime_entry_disable[regime_key] = bool(value)
    disabled_regimes = sorted([key for key, value in regime_entry_disable.items() if bool(value)])
    raw_reject_regime_entry_disabled_by_regime = entry_funnel.get(
        "reject_regime_entry_disabled_by_regime",
        {},
    )
    reject_regime_entry_disabled_by_regime: Dict[str, int] = {}
    if isinstance(raw_reject_regime_entry_disabled_by_regime, dict):
        for key, value in raw_reject_regime_entry_disabled_by_regime.items():
            regime_key = str(key).strip()
            if not regime_key:
                continue
            reject_regime_entry_disabled_by_regime[regime_key] = max(0, to_int(value))
    no_best_signal = max(0, to_int(entry_funnel.get("no_best_signal", 0)))
    blocked_pattern_gate = max(0, to_int(entry_funnel.get("blocked_pattern_gate", 0)))
    blocked_rr_rebalance = max(0, to_int(entry_funnel.get("blocked_rr_rebalance", 0)))
    blocked_risk_gate = max(0, to_int(entry_funnel.get("blocked_risk_gate", 0)))
    blocked_risk_manager = max(0, to_int(entry_funnel.get("blocked_risk_manager", 0)))
    blocked_min_order_or_capital = max(0, to_int(entry_funnel.get("blocked_min_order_or_capital", 0)))
    blocked_order_sizing = max(0, to_int(entry_funnel.get("blocked_order_sizing", 0)))
    entries_executed = max(0, to_int(entry_funnel.get("entries_executed", 0)))
    strategy_exit_mode_effective = str(entry_funnel.get("strategy_exit_mode_effective", "")).strip()
    if not strategy_exit_mode_effective:
        strategy_exit_mode_effective = "enforce"
    strategy_exit_triggered_count = max(
        0, to_int(entry_funnel.get("strategy_exit_triggered_count", 0))
    )
    strategy_exit_observe_only_suppressed_count = max(
        0, to_int(entry_funnel.get("strategy_exit_observe_only_suppressed_count", 0))
    )
    strategy_exit_executed_count = max(
        0, to_int(entry_funnel.get("strategy_exit_executed_count", 0))
    )
    strategy_exit_clamp_applied_count = max(
        0, to_int(entry_funnel.get("strategy_exit_clamp_applied_count", 0))
    )
    raw_strategy_exit_triggered_by_market = entry_funnel.get(
        "strategy_exit_triggered_by_market",
        {},
    )
    strategy_exit_triggered_by_market: Dict[str, int] = {}
    if isinstance(raw_strategy_exit_triggered_by_market, dict):
        for key, value in raw_strategy_exit_triggered_by_market.items():
            market_key = str(key).strip()
            if not market_key:
                continue
            strategy_exit_triggered_by_market[market_key] = max(0, to_int(value))
    raw_strategy_exit_triggered_by_regime = entry_funnel.get(
        "strategy_exit_triggered_by_regime",
        {},
    )
    strategy_exit_triggered_by_regime: Dict[str, int] = {}
    if isinstance(raw_strategy_exit_triggered_by_regime, dict):
        for key, value in raw_strategy_exit_triggered_by_regime.items():
            regime_key = str(key).strip()
            if not regime_key:
                continue
            strategy_exit_triggered_by_regime[regime_key] = max(0, to_int(value))
    raw_strategy_exit_trigger_samples = entry_funnel.get("strategy_exit_trigger_samples", [])
    strategy_exit_trigger_samples: List[Dict[str, Any]] = []
    if isinstance(raw_strategy_exit_trigger_samples, list):
        for raw in raw_strategy_exit_trigger_samples:
            if not isinstance(raw, dict):
                continue
            strategy_exit_trigger_samples.append(
                {
                    "ts_ms": to_int(raw.get("ts_ms", 0)),
                    "market": str(raw.get("market", "")).strip(),
                    "regime": str(raw.get("regime", "")).strip(),
                    "unrealized_pnl_at_trigger": round(
                        to_float(raw.get("unrealized_pnl_at_trigger", 0.0)),
                        6,
                    ),
                    "holding_time_seconds": round(
                        to_float(raw.get("holding_time_seconds", 0.0)),
                        4,
                    ),
                    "reason_code": str(raw.get("reason_code", "")).strip(),
                }
            )
            if len(strategy_exit_trigger_samples) >= 10:
                break
    raw_strategy_exit_clamp_samples = entry_funnel.get("strategy_exit_clamp_samples", [])
    strategy_exit_clamp_samples: List[Dict[str, Any]] = []
    if isinstance(raw_strategy_exit_clamp_samples, list):
        for raw in raw_strategy_exit_clamp_samples:
            if not isinstance(raw, dict):
                continue
            strategy_exit_clamp_samples.append(
                {
                    "ts_ms": to_int(raw.get("ts_ms", 0)),
                    "market": str(raw.get("market", "")).strip(),
                    "regime": str(raw.get("regime", "")).strip(),
                    "stop_loss_price": round(to_float(raw.get("stop_loss_price", 0.0)), 8),
                    "exit_price_before_clamp": round(
                        to_float(raw.get("exit_price_before_clamp", 0.0)),
                        8,
                    ),
                    "exit_price_after_clamp": round(
                        to_float(raw.get("exit_price_after_clamp", 0.0)),
                        8,
                    ),
                    "pnl_before_clamp": round(to_float(raw.get("pnl_before_clamp", 0.0)), 6),
                    "pnl_after_clamp": round(to_float(raw.get("pnl_after_clamp", 0.0)), 6),
                    "reason_code": str(raw.get("reason_code", "")).strip(),
                }
            )
            if len(strategy_exit_clamp_samples) >= 10:
                break
    shadow_rounds = max(0, to_int(shadow_funnel_raw.get("rounds", 0)))
    shadow_primary_generated_signals = max(
        0, to_int(shadow_funnel_raw.get("primary_generated_signals", 0))
    )
    shadow_primary_after_sizing = max(
        0, to_int(shadow_funnel_raw.get("primary_after_sizing", 0))
    )
    shadow_shadow_after_sizing = max(
        0, to_int(shadow_funnel_raw.get("shadow_after_sizing", 0))
    )
    shadow_primary_after_policy_filter = max(
        0, to_int(shadow_funnel_raw.get("primary_after_policy_filter", 0))
    )
    shadow_shadow_after_policy_filter = max(
        0, to_int(shadow_funnel_raw.get("shadow_after_policy_filter", 0))
    )
    shadow_primary_best_signal_available = max(
        0, to_int(shadow_funnel_raw.get("primary_best_signal_available", 0))
    )
    shadow_shadow_best_signal_available = max(
        0, to_int(shadow_funnel_raw.get("shadow_best_signal_available", 0))
    )
    shadow_supply_improved_rounds = max(
        0, to_int(shadow_funnel_raw.get("supply_improved_rounds", 0))
    )
    shadow_avg_manager_supply_lift = to_float(
        shadow_funnel_raw.get("avg_manager_supply_lift", 0.0)
    )
    shadow_avg_policy_supply_lift = to_float(
        shadow_funnel_raw.get("avg_policy_supply_lift", 0.0)
    )
    shadow_generated_denominator = max(1, shadow_primary_generated_signals)
    shadow_policy_supply_lift_per_signal = (
        float(shadow_shadow_after_policy_filter - shadow_primary_after_policy_filter)
        / float(shadow_generated_denominator)
    )
    shadow_contract = {
        "shadow_probe_active": shadow_rounds > 0,
        "candidate_supply_lift_positive": (
            shadow_shadow_after_policy_filter > shadow_primary_after_policy_filter
        ),
        "best_signal_lift_non_negative": (
            shadow_shadow_best_signal_available >= shadow_primary_best_signal_available
        ),
    }
    shadow_contract["all_pass"] = all(bool(v) for v in shadow_contract.values())
    ranging_shadow_raw = backtest_result.get("ranging_shadow", {})
    if not isinstance(ranging_shadow_raw, dict):
        ranging_shadow_raw = {}
    shadow_count_total = max(0, to_int(ranging_shadow_raw.get("shadow_count_total", 0)))
    raw_shadow_count_by_regime = ranging_shadow_raw.get("shadow_count_by_regime", {})
    shadow_count_by_regime: Dict[str, int] = {}
    if isinstance(raw_shadow_count_by_regime, dict):
        for key, value in raw_shadow_count_by_regime.items():
            regime_key = str(key).strip()
            if not regime_key:
                continue
            shadow_count_by_regime[regime_key] = max(0, to_int(value))
    raw_shadow_count_by_market = ranging_shadow_raw.get("shadow_count_by_market", {})
    shadow_count_by_market: Dict[str, int] = {}
    if isinstance(raw_shadow_count_by_market, dict):
        for key, value in raw_shadow_count_by_market.items():
            market_key = str(key).strip()
            if not market_key:
                continue
            shadow_count_by_market[market_key] = max(0, to_int(value))
    shadow_would_pass_quality_selection_count = max(
        0,
        to_int(ranging_shadow_raw.get("shadow_would_pass_quality_selection_count", 0)),
    )
    shadow_would_pass_manager_count = max(
        0,
        to_int(ranging_shadow_raw.get("shadow_would_pass_manager_count", 0)),
    )
    shadow_would_pass_execution_guard_count = max(
        0,
        to_int(ranging_shadow_raw.get("shadow_would_pass_execution_guard_count", 0)),
    )
    shadow_edge_neg_count = max(0, to_int(ranging_shadow_raw.get("shadow_edge_neg_count", 0)))
    shadow_edge_pos_count = max(0, to_int(ranging_shadow_raw.get("shadow_edge_pos_count", 0)))
    top_shadow_markets = [
        {"market": str(market), "count": int(count)}
        for market, count in sorted(
            shadow_count_by_market.items(),
            key=lambda item: (-int(item[1]), str(item[0])),
        )[:5]
    ]

    candidate_generation = (
        no_signal_generated
        + filtered_out_by_sizing
        + filtered_out_by_policy
        + no_best_signal
    )
    quality_and_risk_gate = (
        blocked_pattern_gate
        + blocked_rr_rebalance
        + blocked_risk_gate
    )
    execution_constraints = (
        skipped_due_to_open_position
        + blocked_risk_manager
        + blocked_min_order_or_capital
        + blocked_order_sizing
    )
    non_execution_total = candidate_generation + quality_and_risk_gate + execution_constraints

    bottleneck_groups = {
        "candidate_generation": int(candidate_generation),
        "quality_and_risk_gate": int(quality_and_risk_gate),
        "execution_constraints": int(execution_constraints),
        "entries_executed": int(entries_executed),
    }
    ordered_non_execution_groups = sorted(
        [
            ("candidate_generation", candidate_generation),
            ("quality_and_risk_gate", quality_and_risk_gate),
            ("execution_constraints", execution_constraints),
        ],
        key=lambda item: (-int(item[1]), item[0]),
    )
    top_group_name, top_group_count = ordered_non_execution_groups[0]
    top_group_share = (
        (float(top_group_count) / float(non_execution_total)) if non_execution_total > 0 else 0.0
    )
    round_denominator = float(max(1, entry_rounds))
    bottleneck_group_rates = {
        "candidate_generation_rate": round(candidate_generation / round_denominator, 4),
        "quality_and_risk_gate_rate": round(quality_and_risk_gate / round_denominator, 4),
        "execution_constraints_rate": round(execution_constraints / round_denominator, 4),
        "entry_execution_rate": round(entries_executed / round_denominator, 4),
    }

    reason_counts = normalized_reason_counts(backtest_result.get("entry_rejection_reason_counts", {}))
    split_stage_eval_stats = backtest_result.get("split_stage_funnel_eval_stats", {})
    if not isinstance(split_stage_eval_stats, dict):
        split_stage_eval_stats = {}
    phase3_diag = build_phase3_diagnostics_v2(reason_counts)
    phase4_diag = build_phase4_portfolio_diagnostics(
        backtest_result,
        phase4_market_cluster_map=phase4_market_cluster_map,
    )
    no_signal_pattern_counts = normalized_reason_counts(backtest_result.get("no_signal_pattern_counts", {}))
    edge_gap_bucket_counts = normalized_reason_counts(
        backtest_result.get("entry_quality_edge_gap_buckets", {})
    )
    no_signal_reason_counts = filter_reason_counts_by_prefix(
        reason_counts,
        ["foundation_no_signal_", "no_signal_", "probabilistic_"],
        exclude_exact=["no_signal_generated"],
    )
    no_signal_reason_counts_core_effective = normalized_reason_counts(
        split_stage_eval_stats.get("no_signal_reason_counts_in_range", {})
    )
    manager_prefilter_reason_counts = filter_reason_counts_by_prefix(
        reason_counts,
        ["filtered_out_by_sizing"],
    )
    policy_prefilter_reason_counts = filter_reason_counts_by_prefix(
        reason_counts,
        ["filtered_out_by_policy"],
    )
    candidate_components = {
        "no_signal_generated": int(no_signal_generated),
        "filtered_out_by_sizing": int(filtered_out_by_sizing),
        "filtered_out_by_policy": int(filtered_out_by_policy),
        "no_best_signal": int(no_best_signal),
    }
    candidate_total = max(0, sum(candidate_components.values()))
    candidate_denom = float(max(1, candidate_total))
    candidate_component_shares = {
        "no_signal_generated_share": round(float(no_signal_generated) / candidate_denom, 4),
        "filtered_out_by_sizing_share": round(float(filtered_out_by_sizing) / candidate_denom, 4),
        "filtered_out_by_policy_share": round(float(filtered_out_by_policy) / candidate_denom, 4),
        "no_best_signal_share": round(float(no_best_signal) / candidate_denom, 4),
    }
    ordered_candidate_components = sorted(
        candidate_components.items(),
        key=lambda item: (-int(item[1]), item[0]),
    )
    primary_candidate_component_name, primary_candidate_component_count = ordered_candidate_components[0]
    top_no_signal_reason_rows = top_reason_rows(no_signal_reason_counts, limit=8)
    top_no_signal_reason_rows_core_effective = top_reason_rows(
        no_signal_reason_counts_core_effective,
        limit=8,
    )
    no_signal_reason_timestamp_evidence_core_effective = {
        "source": "entry_stage_funnel_backtest_jsonl",
        "enabled": bool(split_stage_eval_stats.get("enabled", False)),
        "range_start_ts": int(split_stage_eval_stats.get("start_ts", 0)),
        "range_end_ts": int(split_stage_eval_stats.get("end_ts", 0)),
        "event_rows_in_range": int(
            max(0, to_int(split_stage_eval_stats.get("no_signal_reason_event_rows_in_range", 0)))
        ),
        "event_rows_total": int(
            max(0, to_int(split_stage_eval_stats.get("no_signal_reason_event_rows_total", 0)))
        ),
        "event_rows_with_ts_total": int(
            max(
                0,
                to_int(split_stage_eval_stats.get("no_signal_reason_event_rows_with_ts_total", 0)),
            )
        ),
        "event_rows_missing_ts_total": int(
            max(
                0,
                to_int(split_stage_eval_stats.get("no_signal_reason_event_rows_missing_ts_total", 0)),
            )
        ),
        "timestamp_coverage_total": round(
            to_float(split_stage_eval_stats.get("no_signal_reason_timestamp_coverage_total", 0.0)),
            6,
        ),
    }
    top_manager_prefilter_reason_rows = top_reason_rows(manager_prefilter_reason_counts, limit=8)
    top_policy_prefilter_reason_rows = top_reason_rows(policy_prefilter_reason_counts, limit=8)
    ab_playbook_rows = build_candidate_generation_ab_playbook(
        component_shares=candidate_component_shares,
        top_no_signal_reasons=top_no_signal_reason_rows,
        top_manager_prefilter_reasons=top_manager_prefilter_reason_rows,
    )
    pattern_profit_map = build_pattern_cell_profit_map(backtest_result.get("pattern_summaries", []))
    strategy_diag = build_strategy_funnel_diagnostics(backtest_result.get("strategy_signal_funnel", []))
    strategy_collection_diag = build_strategy_collection_diagnostics(
        backtest_result.get("strategy_collection_summaries", [])
    )
    post_entry_telemetry = parse_post_entry_risk_telemetry(backtest_result)
    phase3_pass_ev_distribution = build_phase3_pass_ev_distribution(
        backtest_result.get("phase3_pass_ev_samples", []),
        sample_cap=5000,
    )
    probability_calibration_telemetry = build_probability_calibration_telemetry(
        backtest_result,
        sample_limit=20,
    )
    lgbm_ev_affine_telemetry = build_lgbm_ev_affine_telemetry(
        backtest_result,
        sample_limit=20,
    )
    bundle = bundle_meta if isinstance(bundle_meta, dict) else {}
    run_provenance = backtest_result.get("run_provenance", {})
    if not isinstance(run_provenance, dict):
        run_provenance = {}
    gate_vnext_backend_provenance = entry_funnel.get("gate_vnext_backend_provenance", {})
    if not isinstance(gate_vnext_backend_provenance, dict):
        gate_vnext_backend_provenance = {}
    configured_backend = str(bundle.get("prob_model_backend", "unknown")).strip().lower() or "unknown"
    backend_request = (
        str(gate_vnext_backend_provenance.get("backend_request", "")).strip().lower()
        or str(run_provenance.get("prob_model_backend", "")).strip().lower()
        or configured_backend
    )
    backend_effective_raw = str(
        probability_calibration_telemetry.get("prob_model_backend_effective", "unknown")
    ).strip().lower() or "unknown"
    backend_effective_source = "runtime_samples"
    backend_effective = backend_effective_raw
    if backend_effective in {"", "unknown", "n/a"}:
        gate_effective = str(
            gate_vnext_backend_provenance.get("backend_effective", "")
        ).strip().lower()
        if gate_effective in {"sgd", "lgbm"}:
            backend_effective = gate_effective
            backend_effective_source = "gate_vnext_fallback"
        elif backend_request in {"sgd", "lgbm"}:
            backend_effective = backend_request
            backend_effective_source = "run_provenance_fallback"
        elif configured_backend in {"sgd", "lgbm"}:
            backend_effective = configured_backend
            backend_effective_source = "bundle_config_fallback"
        else:
            backend_effective = "unknown"
            backend_effective_source = "unknown"
    prob_backend_counts = (
        probability_calibration_telemetry.get("prob_model_backend_counts", {})
        if isinstance(probability_calibration_telemetry.get("prob_model_backend_counts", {}), dict)
        else {}
    )
    lgbm_model_path = str(
        run_provenance.get("lgbm_model_path", bundle.get("lgbm_model_path", ""))
    ).strip()
    lgbm_model_sha256 = str(
        gate_vnext_backend_provenance.get(
            "lgbm_model_sha256",
            run_provenance.get("lgbm_model_sha256", bundle.get("lgbm_model_sha256", "")),
        )
    ).strip().lower()
    lgbm_calibration_mode = str(bundle.get("lgbm_calibration_mode", "off")).strip().lower() or "off"
    lgbm_h5_calibration = (
        bundle.get("lgbm_h5_calibration", {})
        if isinstance(bundle.get("lgbm_h5_calibration", {}), dict)
        else {}
    )
    lgbm_calibration_a = lgbm_h5_calibration.get("a", None)
    lgbm_calibration_b = lgbm_h5_calibration.get("b", None)
    lgbm_ev_affine_cfg = (
        bundle.get("lgbm_ev_affine", {})
        if isinstance(bundle.get("lgbm_ev_affine", {}), dict)
        else {}
    )
    p_raw_dist = (
        probability_calibration_telemetry.get("p_raw_distribution", {})
        if isinstance(probability_calibration_telemetry.get("p_raw_distribution", {}), dict)
        else {}
    )
    p_cal_dist = (
        probability_calibration_telemetry.get("p_cal_distribution", {})
        if isinstance(probability_calibration_telemetry.get("p_cal_distribution", {}), dict)
        else {}
    )
    margin_dist = (
        probability_calibration_telemetry.get("margin_distribution", {})
        if isinstance(probability_calibration_telemetry.get("margin_distribution", {}), dict)
        else {}
    )
    backend_provenance = {
        "backend_request": backend_request,
        "prob_model_backend_configured": configured_backend,
        "prob_model_backend_effective": backend_effective,
        "prob_model_backend_effective_source": backend_effective_source,
        "prob_model_backend_effective_raw": backend_effective_raw,
        "prob_model_backend_counts": {
            str(k): int(max(0, to_int(v)))
            for k, v in sorted(prob_backend_counts.items(), key=lambda x: str(x[0]))
        },
        "lgbm_model_path": lgbm_model_path if backend_effective == "lgbm" else "",
        "lgbm_model_sha256": lgbm_model_sha256 if backend_effective == "lgbm" else "",
        "calibration_mode": lgbm_calibration_mode if backend_effective == "lgbm" else "off",
        "calibration_params": {
            "a": (
                round(to_float(lgbm_calibration_a), 12)
                if (backend_effective == "lgbm" and lgbm_calibration_a is not None)
                else None
            ),
            "b": (
                round(to_float(lgbm_calibration_b), 12)
                if (backend_effective == "lgbm" and lgbm_calibration_b is not None)
                else None
            ),
        },
        "ev_affine": {
            "enabled": bool(lgbm_ev_affine_cfg.get("enabled", False))
            if backend_effective == "lgbm"
            else False,
            "scale": (
                round(to_float(lgbm_ev_affine_cfg.get("scale", 1.0)), 10)
                if backend_effective == "lgbm"
                else 1.0
            ),
            "shift": (
                round(to_float(lgbm_ev_affine_cfg.get("shift", 0.0)), 10)
                if backend_effective == "lgbm"
                else 0.0
            ),
        },
        "probability_distribution": {
            "p_raw_mean": round(to_float(p_raw_dist.get("mean", 0.0)), 10),
            "p_raw_std": round(to_float(p_raw_dist.get("std", 0.0)), 10),
            "p_raw_p95": round(to_float(p_raw_dist.get("p95", 0.0)), 10),
            "p_cal_mean": round(to_float(p_cal_dist.get("mean", 0.0)), 10),
            "p_cal_std": round(to_float(p_cal_dist.get("std", 0.0)), 10),
            "p_cal_p95": round(to_float(p_cal_dist.get("p95", 0.0)), 10),
        },
        "margin_distribution": {
            "mean": round(to_float(margin_dist.get("mean", 0.0)), 10),
            "std": round(to_float(margin_dist.get("std", 0.0)), 10),
            "p95": round(to_float(margin_dist.get("p95", 0.0)), 10),
        },
    }
    top_loss_trade_samples = build_top_loss_trade_samples(
        backtest_result.get("trade_history_samples", []),
        limit=10,
    )
    raw_vnext_ev_samples = backtest_result.get("vnext_ev_samples", [])
    vnext_ev_samples: List[Dict[str, Any]] = []
    if isinstance(raw_vnext_ev_samples, list):
        for raw in raw_vnext_ev_samples:
            if not isinstance(raw, dict):
                continue
            vnext_ev_samples.append(
                {
                    "ts_ms": int(max(0, to_int(raw.get("ts_ms", 0)))),
                    "market": str(raw.get("market", "")).strip(),
                    "regime": str(raw.get("regime", "")).strip(),
                    "snapshot_id": str(raw.get("snapshot_id", "")).strip(),
                    "backend_effective": str(raw.get("backend_effective", "")).strip().lower(),
                    "p_calibrated": round(to_float(raw.get("p_calibrated", 0.0)), 10),
                    "margin": round(to_float(raw.get("margin", 0.0)), 10),
                    "tp_pct": round(to_float(raw.get("tp_pct", 0.0)), 10),
                    "sl_pct": round(to_float(raw.get("sl_pct", 0.0)), 10),
                    "label_cost_bps": round(to_float(raw.get("label_cost_bps", 0.0)), 10),
                    "expected_value_from_prob_bps": round(
                        to_float(raw.get("expected_value_from_prob_bps", 0.0)),
                        10,
                    ),
                    "stop_loss_trending_multiplier_effective": round(
                        to_float(raw.get("stop_loss_trending_multiplier_effective", 0.0)),
                        10,
                    ),
                    "tp_distance_trending_multiplier_effective": round(
                        to_float(raw.get("tp_distance_trending_multiplier_effective", 0.0)),
                        10,
                    ),
                    "expected_edge_calibrated_bps": round(
                        to_float(raw.get("expected_edge_calibrated_bps", 0.0)),
                        10,
                    ),
                    "expected_edge_used_for_gate_bps": round(
                        to_float(raw.get("expected_edge_used_for_gate_bps", 0.0)),
                        10,
                    ),
                    "expected_edge_used_for_gate_source": str(
                        raw.get("expected_edge_used_for_gate_source", "")
                    ).strip(),
                    "edge_regressor_available": bool(raw.get("edge_regressor_available", False)),
                    "edge_regressor_used": bool(raw.get("edge_regressor_used", False)),
                    "edge_profile_used": bool(raw.get("edge_profile_used", False)),
                    "signal_expected_value": round(
                        to_float(raw.get("signal_expected_value", 0.0)),
                        10,
                    ),
                    "edge_bps_from_snapshot": round(
                        to_float(raw.get("edge_bps_from_snapshot", 0.0)),
                        10,
                    ),
                    "ev_in_bps": round(to_float(raw.get("ev_in_bps", 0.0)), 10),
                    "ev_for_size_bps": round(to_float(raw.get("ev_for_size_bps", 0.0)), 10),
                    "expected_value_vnext_bps": round(
                        to_float(raw.get("expected_value_vnext_bps", 0.0)),
                        10,
                    ),
                    "size_fraction": round(to_float(raw.get("size_fraction", 0.0)), 10),
                    "execution_gate_pass": bool(raw.get("execution_gate_pass", False)),
                    "execution_reject_reason": str(raw.get("execution_reject_reason", "")).strip(),
                }
            )
            if len(vnext_ev_samples) >= 50:
                break

    return {
        "dataset": dataset_name,
        "entry_rounds": int(entry_rounds),
        "non_execution_total": int(non_execution_total),
        "bottleneck_groups": bottleneck_groups,
        "bottleneck_group_rates": bottleneck_group_rates,
        "top_non_execution_group": {
            "name": top_group_name,
            "count": int(top_group_count),
            "share_of_non_execution": round(top_group_share, 4),
        },
        "top_rejection_reasons": top_reason_rows(reason_counts, limit=5),
        "rejection_reason_counts": reason_counts,
        "no_signal_pattern_counts": no_signal_pattern_counts,
        "top_no_signal_patterns": top_pattern_rows(no_signal_pattern_counts, limit=5),
        "candidate_generation_breakdown": {
            "total": int(candidate_total),
            "components": candidate_components,
            "component_shares": candidate_component_shares,
            "primary_component": {
                "name": str(primary_candidate_component_name),
                "count": int(primary_candidate_component_count),
                "share": round(float(primary_candidate_component_count) / candidate_denom, 4),
            },
            "no_signal_reason_counts_execution_scope": no_signal_reason_counts,
            "no_signal_reason_counts_core_effective": no_signal_reason_counts_core_effective,
            "no_signal_reason_counts": no_signal_reason_counts,
            "manager_prefilter_reason_counts": manager_prefilter_reason_counts,
            "policy_prefilter_reason_counts": policy_prefilter_reason_counts,
            "top_no_signal_reasons_execution_scope": top_no_signal_reason_rows,
            "top_no_signal_reasons_core_effective": top_no_signal_reason_rows_core_effective,
            "top_no_signal_reasons": top_no_signal_reason_rows,
            "top_manager_prefilter_reasons": top_manager_prefilter_reason_rows,
            "top_policy_prefilter_reasons": top_policy_prefilter_reason_rows,
            "execution_no_signal_top3": top_reason_rows(no_signal_reason_counts, limit=3),
            "core_no_signal_top3": top_reason_rows(
                no_signal_reason_counts_core_effective,
                limit=3,
            ),
            "no_signal_reason_timestamp_evidence_core_effective": (
                no_signal_reason_timestamp_evidence_core_effective
            ),
            "shadow_policy_supply_lift_absolute": int(
                shadow_shadow_after_policy_filter - shadow_primary_after_policy_filter
            ),
            "shadow_policy_supply_lift_per_generated_signal": round(
                shadow_policy_supply_lift_per_signal,
                6,
            ),
            "ab_playbook_candidates": ab_playbook_rows,
        },
        "entry_quality_edge_gap_buckets": edge_gap_bucket_counts,
        "top_entry_quality_edge_gap_buckets": top_pattern_rows(edge_gap_bucket_counts, limit=5),
        "pattern_cell_profit_map": pattern_profit_map,
        "top_loss_pattern_cells": top_loss_pattern_cells(pattern_profit_map, limit=6),
        "top_loss_trade_samples": top_loss_trade_samples,
        "strategy_funnel": strategy_diag,
        "strategy_collection": strategy_collection_diag,
        "post_entry_risk_telemetry": post_entry_telemetry,
        "phase3_pass_ev_distribution": phase3_pass_ev_distribution,
        "probability_calibration_telemetry": probability_calibration_telemetry,
        "lgbm_ev_affine_telemetry": lgbm_ev_affine_telemetry,
        "backend_provenance": backend_provenance,
        "phase3_diagnostics_v2": phase3_diag,
        "phase4_portfolio_diagnostics": phase4_diag,
        "gate_vnext": {
            "gate_system_version_effective": gate_system_version_effective,
            "quality_topk_effective": int(quality_topk_effective),
            "backend_provenance": {
                "backend_request": str(backend_request),
                "backend_effective": str(backend_effective),
                "lgbm_model_sha256": str(lgbm_model_sha256 if backend_effective == "lgbm" else ""),
                "source": str(backend_effective_source),
            },
            "scans_count": int(gate_vnext_scan_rounds),
            "scan_rounds": int(gate_vnext_scan_rounds),
            "s1_topk_bound": int(gate_vnext_topk_bound),
            "s1_exceeds_topk_bound": bool(gate_vnext_s1_exceeds_topk_bound),
            "stage_funnel_vnext": {
                "s0_snapshots_valid": int(gate_vnext_s0_snapshots_valid),
                "s1_selected_topk": int(gate_vnext_s1_selected_topk),
                "s2_sized_count": int(gate_vnext_s2_sized_count),
                "s3_exec_gate_pass": int(gate_vnext_s3_exec_gate_pass),
                "s4_submitted": int(gate_vnext_s4_submitted),
                "s5_filled": int(gate_vnext_s5_filled),
                "drop_ev_negative_count": int(gate_vnext_drop_ev_negative_count),
                "ev_negative_size_zero_count": int(gate_vnext_ev_negative_size_zero_count),
                "ev_positive_size_gt_zero_count": int(gate_vnext_ev_positive_size_gt_zero_count),
                "expected_value_from_prob_min_bps": gate_vnext_expected_value_from_prob_min_bps,
                "expected_value_from_prob_median_bps": gate_vnext_expected_value_from_prob_median_bps,
                "expected_value_from_prob_max_bps": gate_vnext_expected_value_from_prob_max_bps,
                "p_cal_min": gate_vnext_p_cal_min,
                "p_cal_median": gate_vnext_p_cal_median,
                "p_cal_max": gate_vnext_p_cal_max,
                "tp_pct_min": gate_vnext_tp_pct_min,
                "tp_pct_median": gate_vnext_tp_pct_median,
                "tp_pct_max": gate_vnext_tp_pct_max,
                "sl_pct_min": gate_vnext_sl_pct_min,
                "sl_pct_median": gate_vnext_sl_pct_median,
                "sl_pct_max": gate_vnext_sl_pct_max,
                "ev_in_min_bps": gate_vnext_ev_in_min_bps,
                "ev_in_median_bps": gate_vnext_ev_in_median_bps,
                "ev_in_max_bps": gate_vnext_ev_in_max_bps,
                "ev_for_size_min_bps": gate_vnext_ev_for_size_min_bps,
                "ev_for_size_median_bps": gate_vnext_ev_for_size_median_bps,
                "ev_for_size_max_bps": gate_vnext_ev_for_size_max_bps,
                "size_fraction_min": gate_vnext_size_fraction_min,
                "size_fraction_median": gate_vnext_size_fraction_median,
                "size_fraction_max": gate_vnext_size_fraction_max,
                "scan_rounds": int(gate_vnext_scan_rounds),
            },
            "ev_samples": vnext_ev_samples,
        },
        "strategy_exit": {
            "mode_effective": strategy_exit_mode_effective,
            "strategy_exit_triggered_count": int(strategy_exit_triggered_count),
            "strategy_exit_would_trigger_count": int(strategy_exit_triggered_count),
            "strategy_exit_observe_only_suppressed_count": int(
                strategy_exit_observe_only_suppressed_count
            ),
            "strategy_exit_executed_count": int(strategy_exit_executed_count),
            "strategy_exit_clamp_applied_count": int(strategy_exit_clamp_applied_count),
            "strategy_exit_triggered_by_market": strategy_exit_triggered_by_market,
            "strategy_exit_triggered_by_regime": strategy_exit_triggered_by_regime,
            "strategy_exit_would_trigger_samples": strategy_exit_trigger_samples,
            "strategy_exit_clamp_samples": strategy_exit_clamp_samples,
        },
        "regime_entry_disable": {
            "enabled": regime_entry_disable_enabled,
            "disabled_regimes": disabled_regimes,
            "reject_regime_entry_disabled_count": int(reject_regime_entry_disabled_count),
            "reject_regime_entry_disabled_by_regime": reject_regime_entry_disabled_by_regime,
        },
        "ranging_shadow": {
            "shadow_count_total": int(shadow_count_total),
            "shadow_count_by_regime": {
                str(k): int(max(0, to_int(v)))
                for k, v in sorted(shadow_count_by_regime.items(), key=lambda item: str(item[0]))
            },
            "shadow_count_by_market": {
                str(k): int(max(0, to_int(v)))
                for k, v in sorted(shadow_count_by_market.items(), key=lambda item: str(item[0]))
            },
            "shadow_would_pass_quality_selection_count": int(shadow_would_pass_quality_selection_count),
            "shadow_would_pass_manager_count": int(shadow_would_pass_manager_count),
            "shadow_would_pass_execution_guard_count": int(
                shadow_would_pass_execution_guard_count
            ),
            "shadow_edge_neg_count": int(shadow_edge_neg_count),
            "shadow_edge_pos_count": int(shadow_edge_pos_count),
            "top_shadow_markets": top_shadow_markets,
        },
        "shadow_funnel": {
            "rounds": int(shadow_rounds),
            "primary_generated_signals": int(shadow_primary_generated_signals),
            "primary_after_sizing": int(shadow_primary_after_sizing),
            "shadow_after_sizing": int(shadow_shadow_after_sizing),
            "primary_after_policy_filter": int(shadow_primary_after_policy_filter),
            "shadow_after_policy_filter": int(shadow_shadow_after_policy_filter),
            "primary_best_signal_available": int(shadow_primary_best_signal_available),
            "shadow_best_signal_available": int(shadow_shadow_best_signal_available),
            "supply_improved_rounds": int(shadow_supply_improved_rounds),
            "avg_manager_supply_lift": round(shadow_avg_manager_supply_lift, 6),
            "avg_policy_supply_lift": round(shadow_avg_policy_supply_lift, 6),
            "policy_supply_lift_per_generated_signal": round(
                shadow_policy_supply_lift_per_signal,
                6,
            ),
        },
        "shadow_contract": shadow_contract,
        "strategy_collect_exception_count": max(
            0, to_int(backtest_result.get("strategy_collect_exception_count", 0))
        ),
    }


def aggregate_dataset_diagnostics(dataset_diagnostics: List[Dict[str, Any]]) -> Dict[str, Any]:
    aggregate_groups = {
        "candidate_generation": 0,
        "quality_and_risk_gate": 0,
        "execution_constraints": 0,
        "entries_executed": 0,
    }
    aggregate_reasons: Dict[str, int] = {}
    aggregate_no_signal_patterns: Dict[str, int] = {}
    aggregate_edge_gap_buckets: Dict[str, int] = {}
    aggregate_pattern_profit_map: Dict[str, Dict[str, float]] = {}
    top_group_votes: Dict[str, int] = {}
    aggregate_candidate_components: Dict[str, int] = {
        "no_signal_generated": 0,
        "filtered_out_by_sizing": 0,
        "filtered_out_by_policy": 0,
        "no_best_signal": 0,
    }
    aggregate_no_signal_reasons: Dict[str, int] = {}
    aggregate_no_signal_reasons_core_effective: Dict[str, int] = {}
    aggregate_manager_prefilter_reasons: Dict[str, int] = {}
    aggregate_policy_prefilter_reasons: Dict[str, int] = {}
    aggregate_no_signal_timestamp_evidence_core_effective: Dict[str, Any] = {
        "source": "entry_stage_funnel_backtest_jsonl",
        "dataset_count_with_evidence": 0,
        "event_rows_in_range": 0,
        "event_rows_total": 0,
        "event_rows_with_ts_total": 0,
        "event_rows_missing_ts_total": 0,
    }
    aggregate_post_entry_telemetry: Dict[str, Any] = {
        "adaptive_stop_updates": 0,
        "adaptive_tp_recalibration_updates": 0,
        "adaptive_partial_ratio_samples": 0,
        "stop_loss_trigger_count": 0,
        "stop_loss_pnl_sum_krw": 0.0,
        "partial_tp_exit_count": 0,
        "take_profit_full_count": 0,
        "take_profit_full_pnl_sum_krw": 0.0,
        "trending_trade_count": 0,
        "trending_holding_minutes_sum": 0.0,
        "stop_loss_distance_samples_trending": 0,
        "stop_loss_distance_sum_pct_trending": 0.0,
        "take_profit_distance_samples_trending": 0,
        "take_profit_distance_sum_pct_trending": 0.0,
        "adaptive_partial_ratio_histogram": {
            "0.35_0.44": 0,
            "0.45_0.54": 0,
            "0.55_0.64": 0,
            "0.65_0.74": 0,
            "0.75_0.80": 0,
        },
    }
    aggregate_phase3_pass_ev_samples_bps: List[float] = []
    aggregate_phase3_pass_ev_selected_samples_bps: List[float] = []
    aggregate_phase3_pass_ev_source_counts: Dict[str, int] = {}
    aggregate_shadow: Dict[str, Any] = {
        "rounds": 0,
        "primary_generated_signals": 0,
        "primary_after_sizing": 0,
        "shadow_after_sizing": 0,
        "primary_after_policy_filter": 0,
        "shadow_after_policy_filter": 0,
        "primary_best_signal_available": 0,
        "shadow_best_signal_available": 0,
        "supply_improved_rounds": 0,
        "avg_manager_supply_lift_acc": 0.0,
        "avg_policy_supply_lift_acc": 0.0,
        "dataset_with_shadow_probe": 0,
        "contract_all_pass_count": 0,
    }
    phase3_counter_keys = [
        "candidate_total",
        "pass_total",
        "reject_margin_insufficient",
        "reject_strength_fail",
        "reject_expected_value_fail",
        "reject_expected_edge_negative_count",
        "reject_regime_entry_disabled_count",
        "reject_quality_filter_fail",
        "reject_ev_confidence_low",
        "reject_cost_tail_fail",
    ]
    aggregate_phase3_counters: Dict[str, int] = {k: 0 for k in phase3_counter_keys}
    aggregate_phase3_slices: Dict[str, Dict[str, Dict[str, int]]] = {
        "pass_rate_by_regime": {},
        "pass_rate_by_vol_bucket": {},
        "pass_rate_by_liquidity_bucket": {},
        "pass_rate_edge_regressor_present_vs_fallback": {},
        "pass_rate_by_cost_mode": {},
    }
    phase4_counter_keys = [
        "candidates_total",
        "selected_total",
        "rejected_by_budget",
        "rejected_by_cluster_cap",
        "rejected_by_correlation_penalty",
        "cluster_cap_skips_count",
        "cluster_cap_would_exceed_count",
        "cluster_exposure_update_count",
        "rejected_by_execution_cap",
        "rejected_by_drawdown_governor",
        "candidate_snapshot_total",
        "candidate_snapshot_sampled",
    ]
    aggregate_phase4_counters: Dict[str, int] = {k: 0 for k in phase4_counter_keys}
    aggregate_phase4_flags: Dict[str, bool] = {
        "phase4_portfolio_allocator_enabled": False,
        "phase4_correlation_control_enabled": False,
        "phase4_risk_budget_enabled": False,
        "phase4_drawdown_governor_enabled": False,
        "phase4_execution_aware_sizing_enabled": False,
        "phase4_portfolio_diagnostics_enabled": False,
    }
    aggregate_phase4_selected_by_cluster: Dict[str, int] = {}
    aggregate_phase4_exposure_by_cluster: Dict[str, float] = {}
    aggregate_phase4_corr_stage_votes: Dict[str, int] = {}
    aggregate_phase4_corr_unit_votes: Dict[str, int] = {}
    aggregate_phase4_corr_checks_total = 0
    aggregate_phase4_corr_near_cap_count = 0
    aggregate_phase4_corr_penalty_applied_count = 0
    aggregate_phase4_corr_penalty_weighted_sum = 0.0
    aggregate_phase4_corr_penalty_max = 0.0
    aggregate_phase4_corr_cluster_exposure_max: Dict[str, float] = {}
    aggregate_phase4_corr_cluster_cap_max: Dict[str, float] = {}
    aggregate_phase4_corr_near_cap_rows: List[Dict[str, Any]] = []
    aggregate_phase4_corr_penalty_score_rows: List[Dict[str, Any]] = []
    aggregate_phase4_corr_debug_trace_rows: List[Dict[str, Any]] = []
    aggregate_regime_entry_disable_enabled = False
    aggregate_disabled_regimes = set()
    aggregate_reject_regime_entry_disabled_count = 0
    aggregate_reject_regime_entry_disabled_by_regime: Dict[str, int] = {}
    aggregate_strategy_exit: Dict[str, Any] = {
        "mode_votes": {},
        "strategy_exit_triggered_count": 0,
        "strategy_exit_observe_only_suppressed_count": 0,
        "strategy_exit_executed_count": 0,
        "strategy_exit_clamp_applied_count": 0,
        "strategy_exit_triggered_by_market": {},
        "strategy_exit_triggered_by_regime": {},
        "strategy_exit_would_trigger_samples": [],
        "strategy_exit_clamp_samples": [],
    }
    aggregate_ranging_shadow: Dict[str, Any] = {
        "shadow_count_total": 0,
        "shadow_count_by_regime": {},
        "shadow_count_by_market": {},
        "shadow_would_pass_quality_selection_count": 0,
        "shadow_would_pass_manager_count": 0,
        "shadow_would_pass_execution_guard_count": 0,
        "shadow_edge_neg_count": 0,
        "shadow_edge_pos_count": 0,
    }

    for item in dataset_diagnostics:
        groups = item.get("bottleneck_groups", {})
        if isinstance(groups, dict):
            for key in aggregate_groups:
                aggregate_groups[key] += max(0, to_int(groups.get(key, 0)))

        reasons = item.get("rejection_reason_counts", {})
        if isinstance(reasons, dict):
            for k, v in reasons.items():
                reason_key = str(k).strip()
                if not reason_key:
                    continue
                aggregate_reasons[reason_key] = aggregate_reasons.get(reason_key, 0) + max(0, to_int(v))

        regime_entry_disable = item.get("regime_entry_disable", {})
        if isinstance(regime_entry_disable, dict):
            aggregate_regime_entry_disable_enabled = bool(
                aggregate_regime_entry_disable_enabled
                or bool(regime_entry_disable.get("enabled", False))
            )
            disabled_rows = regime_entry_disable.get("disabled_regimes", [])
            if isinstance(disabled_rows, list):
                for value in disabled_rows:
                    regime_key = str(value).strip()
                    if regime_key:
                        aggregate_disabled_regimes.add(regime_key)
            aggregate_reject_regime_entry_disabled_count += max(
                0,
                to_int(regime_entry_disable.get("reject_regime_entry_disabled_count", 0)),
            )
            by_regime = regime_entry_disable.get("reject_regime_entry_disabled_by_regime", {})
            if isinstance(by_regime, dict):
                for k, v in by_regime.items():
                    regime_key = str(k).strip()
                    if not regime_key:
                        continue
                    aggregate_reject_regime_entry_disabled_by_regime[regime_key] = (
                        aggregate_reject_regime_entry_disabled_by_regime.get(regime_key, 0)
                        + max(0, to_int(v))
                    )
        strategy_exit = item.get("strategy_exit", {})
        if isinstance(strategy_exit, dict):
            mode_effective = str(strategy_exit.get("mode_effective", "")).strip() or "enforce"
            aggregate_strategy_exit["mode_votes"][mode_effective] = (
                aggregate_strategy_exit["mode_votes"].get(mode_effective, 0) + 1
            )
            aggregate_strategy_exit["strategy_exit_triggered_count"] += max(
                0,
                to_int(strategy_exit.get("strategy_exit_triggered_count", 0)),
            )
            aggregate_strategy_exit["strategy_exit_observe_only_suppressed_count"] += max(
                0,
                to_int(strategy_exit.get("strategy_exit_observe_only_suppressed_count", 0)),
            )
            aggregate_strategy_exit["strategy_exit_executed_count"] += max(
                0,
                to_int(strategy_exit.get("strategy_exit_executed_count", 0)),
            )
            aggregate_strategy_exit["strategy_exit_clamp_applied_count"] += max(
                0,
                to_int(strategy_exit.get("strategy_exit_clamp_applied_count", 0)),
            )
            by_market = strategy_exit.get("strategy_exit_triggered_by_market", {})
            if isinstance(by_market, dict):
                for k, v in by_market.items():
                    market_key = str(k).strip()
                    if not market_key:
                        continue
                    aggregate_strategy_exit["strategy_exit_triggered_by_market"][market_key] = (
                        aggregate_strategy_exit["strategy_exit_triggered_by_market"].get(
                            market_key,
                            0,
                        )
                        + max(0, to_int(v))
                    )
            by_regime = strategy_exit.get("strategy_exit_triggered_by_regime", {})
            if isinstance(by_regime, dict):
                for k, v in by_regime.items():
                    regime_key = str(k).strip()
                    if not regime_key:
                        continue
                    aggregate_strategy_exit["strategy_exit_triggered_by_regime"][regime_key] = (
                        aggregate_strategy_exit["strategy_exit_triggered_by_regime"].get(
                            regime_key,
                            0,
                        )
                        + max(0, to_int(v))
                    )
            trigger_samples = strategy_exit.get("strategy_exit_would_trigger_samples", [])
            if isinstance(trigger_samples, list):
                for sample in trigger_samples:
                    if not isinstance(sample, dict):
                        continue
                    if len(aggregate_strategy_exit["strategy_exit_would_trigger_samples"]) >= 10:
                        break
                    aggregate_strategy_exit["strategy_exit_would_trigger_samples"].append(
                        {
                            "dataset": str(item.get("dataset", "")).strip(),
                            "ts_ms": to_int(sample.get("ts_ms", 0)),
                            "market": str(sample.get("market", "")).strip(),
                            "regime": str(sample.get("regime", "")).strip(),
                            "unrealized_pnl_at_trigger": round(
                                to_float(sample.get("unrealized_pnl_at_trigger", 0.0)),
                                6,
                            ),
                            "holding_time_seconds": round(
                                to_float(sample.get("holding_time_seconds", 0.0)),
                                4,
                            ),
                            "reason_code": str(sample.get("reason_code", "")).strip(),
                        }
                    )
            samples = strategy_exit.get("strategy_exit_clamp_samples", [])
            if isinstance(samples, list):
                for sample in samples:
                    if not isinstance(sample, dict):
                        continue
                    if len(aggregate_strategy_exit["strategy_exit_clamp_samples"]) >= 10:
                        break
                    aggregate_strategy_exit["strategy_exit_clamp_samples"].append(
                        {
                            "dataset": str(item.get("dataset", "")).strip(),
                            "ts_ms": to_int(sample.get("ts_ms", 0)),
                            "market": str(sample.get("market", "")).strip(),
                            "regime": str(sample.get("regime", "")).strip(),
                            "stop_loss_price": round(to_float(sample.get("stop_loss_price", 0.0)), 8),
                            "exit_price_before_clamp": round(
                                to_float(sample.get("exit_price_before_clamp", 0.0)),
                                8,
                            ),
                            "exit_price_after_clamp": round(
                                to_float(sample.get("exit_price_after_clamp", 0.0)),
                                8,
                            ),
                            "pnl_before_clamp": round(
                                to_float(sample.get("pnl_before_clamp", 0.0)),
                                6,
                            ),
                            "pnl_after_clamp": round(
                                to_float(sample.get("pnl_after_clamp", 0.0)),
                                6,
                            ),
                            "reason_code": str(sample.get("reason_code", "")).strip(),
                        }
                    )
        ranging_shadow = item.get("ranging_shadow", {})
        if isinstance(ranging_shadow, dict):
            aggregate_ranging_shadow["shadow_count_total"] += max(
                0,
                to_int(ranging_shadow.get("shadow_count_total", 0)),
            )
            aggregate_ranging_shadow["shadow_would_pass_quality_selection_count"] += max(
                0,
                to_int(ranging_shadow.get("shadow_would_pass_quality_selection_count", 0)),
            )
            aggregate_ranging_shadow["shadow_would_pass_manager_count"] += max(
                0,
                to_int(ranging_shadow.get("shadow_would_pass_manager_count", 0)),
            )
            aggregate_ranging_shadow["shadow_would_pass_execution_guard_count"] += max(
                0,
                to_int(ranging_shadow.get("shadow_would_pass_execution_guard_count", 0)),
            )
            aggregate_ranging_shadow["shadow_edge_neg_count"] += max(
                0,
                to_int(ranging_shadow.get("shadow_edge_neg_count", 0)),
            )
            aggregate_ranging_shadow["shadow_edge_pos_count"] += max(
                0,
                to_int(ranging_shadow.get("shadow_edge_pos_count", 0)),
            )
            by_regime = ranging_shadow.get("shadow_count_by_regime", {})
            if isinstance(by_regime, dict):
                for k, v in by_regime.items():
                    regime_key = str(k).strip()
                    if not regime_key:
                        continue
                    aggregate_ranging_shadow["shadow_count_by_regime"][regime_key] = (
                        aggregate_ranging_shadow["shadow_count_by_regime"].get(regime_key, 0)
                        + max(0, to_int(v))
                    )
            by_market = ranging_shadow.get("shadow_count_by_market", {})
            if isinstance(by_market, dict):
                for k, v in by_market.items():
                    market_key = str(k).strip()
                    if not market_key:
                        continue
                    aggregate_ranging_shadow["shadow_count_by_market"][market_key] = (
                        aggregate_ranging_shadow["shadow_count_by_market"].get(market_key, 0)
                        + max(0, to_int(v))
                    )

        phase3_diag = item.get("phase3_diagnostics_v2", {})
        if isinstance(phase3_diag, dict) and bool(phase3_diag.get("enabled", False)):
            counters = phase3_diag.get("counters", {})
            if isinstance(counters, dict):
                for key in phase3_counter_keys:
                    aggregate_phase3_counters[key] += max(0, to_int(counters.get(key, 0)))
            for slice_key in aggregate_phase3_slices:
                rows = phase3_diag.get(slice_key, [])
                if not isinstance(rows, list):
                    continue
                for row in rows:
                    if not isinstance(row, dict):
                        continue
                    label = str(row.get("label", "")).strip()
                    if not label:
                        continue
                    slot = aggregate_phase3_slices[slice_key].setdefault(
                        label,
                        {"candidate_total": 0, "pass_total": 0},
                    )
                    slot["candidate_total"] += max(0, to_int(row.get("candidate_total", 0)))
                    slot["pass_total"] += max(0, to_int(row.get("pass_total", 0)))

        phase3_pass_ev = item.get("phase3_pass_ev_distribution", {})
        if isinstance(phase3_pass_ev, dict) and bool(phase3_pass_ev.get("enabled", False)):
            source_name = str(phase3_pass_ev.get("source", "")).strip() or "unknown"
            aggregate_phase3_pass_ev_source_counts[source_name] = (
                aggregate_phase3_pass_ev_source_counts.get(source_name, 0) + 1
            )
            sample_rows = phase3_pass_ev.get("samples", [])
            if isinstance(sample_rows, list):
                for row in sample_rows:
                    if not isinstance(row, dict):
                        continue
                    v_bps = to_float(row.get("expected_edge_after_cost_bps", 0.0))
                    if not math.isfinite(v_bps):
                        continue
                    aggregate_phase3_pass_ev_samples_bps.append(float(v_bps))
                    if bool(row.get("selected", False)):
                        aggregate_phase3_pass_ev_selected_samples_bps.append(float(v_bps))
            elif isinstance(phase3_pass_ev.get("samples_bps", []), list):
                for v in phase3_pass_ev.get("samples_bps", []):
                    v_bps = to_float(v)
                    if not math.isfinite(v_bps):
                        continue
                    aggregate_phase3_pass_ev_samples_bps.append(float(v_bps))

        phase4_diag = item.get("phase4_portfolio_diagnostics", {})
        if isinstance(phase4_diag, dict) and bool(phase4_diag.get("enabled", False)):
            counters = phase4_diag.get("counters", {})
            if isinstance(counters, dict):
                for key in phase4_counter_keys:
                    aggregate_phase4_counters[key] += max(0, to_int(counters.get(key, 0)))
            flags = phase4_diag.get("flags", {})
            if isinstance(flags, dict):
                for key in aggregate_phase4_flags:
                    aggregate_phase4_flags[key] = bool(
                        aggregate_phase4_flags[key] or bool(flags.get(key, False))
                    )
            exposure_rows = phase4_diag.get("exposure_by_cluster", [])
            if isinstance(exposure_rows, list):
                for row in exposure_rows:
                    if not isinstance(row, dict):
                        continue
                    cluster = str(row.get("cluster", "")).strip() or "unclustered"
                    aggregate_phase4_selected_by_cluster[cluster] = (
                        aggregate_phase4_selected_by_cluster.get(cluster, 0)
                        + max(0, to_int(row.get("selected_count", 0)))
                    )
                    aggregate_phase4_exposure_by_cluster[cluster] = (
                        aggregate_phase4_exposure_by_cluster.get(cluster, 0.0)
                        + max(0.0, to_float(row.get("notional_exposure_sum", 0.0)))
                    )
            corr_telemetry = phase4_diag.get("correlation_applied_telemetry", {})
            if isinstance(corr_telemetry, dict):
                stage = str(corr_telemetry.get("constraint_apply_stage", "")).strip()
                if stage:
                    aggregate_phase4_corr_stage_votes[stage] = (
                        aggregate_phase4_corr_stage_votes.get(stage, 0) + 1
                    )
                unit = str(corr_telemetry.get("constraint_unit", "")).strip()
                if unit:
                    aggregate_phase4_corr_unit_votes[unit] = (
                        aggregate_phase4_corr_unit_votes.get(unit, 0) + 1
                    )
                checks_total = max(
                    0,
                    to_int(corr_telemetry.get("cluster_cap_checks_total", 0)),
                )
                near_cap_count = max(
                    0,
                    to_int(corr_telemetry.get("cluster_cap_near_cap_count", 0)),
                )
                penalty_count = max(
                    0,
                    to_int(corr_telemetry.get("corr_penalty_applied_count", 0)),
                )
                penalty_avg = to_float(corr_telemetry.get("corr_penalty_avg", 0.0))
                penalty_max = max(0.0, to_float(corr_telemetry.get("corr_penalty_max", 0.0)))
                aggregate_phase4_corr_checks_total += checks_total
                aggregate_phase4_corr_near_cap_count += near_cap_count
                aggregate_phase4_corr_penalty_applied_count += penalty_count
                aggregate_phase4_corr_penalty_weighted_sum += penalty_avg * float(penalty_count)
                aggregate_phase4_corr_penalty_max = max(
                    aggregate_phase4_corr_penalty_max,
                    penalty_max,
                )

                cluster_exposure_rows = corr_telemetry.get("cluster_exposure_current", [])
                if isinstance(cluster_exposure_rows, list):
                    for row in cluster_exposure_rows:
                        if not isinstance(row, dict):
                            continue
                        cluster = str(row.get("cluster", "")).strip() or "unclustered"
                        exposure_current = max(0.0, to_float(row.get("exposure_current", 0.0)))
                        cluster_cap_value = max(0.0, to_float(row.get("cluster_cap_value", 0.0)))
                        aggregate_phase4_corr_cluster_exposure_max[cluster] = max(
                            aggregate_phase4_corr_cluster_exposure_max.get(cluster, 0.0),
                            exposure_current,
                        )
                        aggregate_phase4_corr_cluster_cap_max[cluster] = max(
                            aggregate_phase4_corr_cluster_cap_max.get(cluster, 0.0),
                            cluster_cap_value,
                        )

                near_cap_rows = corr_telemetry.get("near_cap_candidates", [])
                if isinstance(near_cap_rows, list):
                    for row in near_cap_rows:
                        if not isinstance(row, dict):
                            continue
                        market = str(row.get("market", "")).strip()
                        cluster = str(row.get("cluster", "")).strip() or "unclustered"
                        cluster_cap_value = max(
                            0.0,
                            to_float(row.get("cluster_cap_value", 0.0)),
                        )
                        headroom_before = to_float(row.get("headroom_before", 0.0))
                        headroom_ratio = to_float(row.get("headroom_ratio", 0.0))
                        if cluster_cap_value > 1.0e-9 and headroom_ratio == 0.0:
                            headroom_ratio = headroom_before / cluster_cap_value
                        aggregate_phase4_corr_near_cap_rows.append(
                            {
                                "dataset": str(item.get("dataset", "")).strip(),
                                "market": market,
                                "cluster": cluster,
                                "exposure_current": round(
                                    max(0.0, to_float(row.get("exposure_current", 0.0))),
                                    8,
                                ),
                                "cluster_cap_value": round(cluster_cap_value, 8),
                                "candidate_position_size": round(
                                    max(0.0, to_float(row.get("candidate_position_size", 0.0))),
                                    8,
                                ),
                                "projected_exposure": round(
                                    max(0.0, to_float(row.get("projected_exposure", 0.0))),
                                    8,
                                ),
                                "headroom_before": round(headroom_before, 8),
                                "headroom_ratio": round(headroom_ratio, 6),
                                "rejected_by_cluster_cap": bool(
                                    row.get("rejected_by_cluster_cap", False)
                                ),
                            }
                        )
                penalty_score_rows = corr_telemetry.get("penalty_score_samples", [])
                if isinstance(penalty_score_rows, list):
                    for row in penalty_score_rows:
                        if not isinstance(row, dict):
                            continue
                        aggregate_phase4_corr_penalty_score_rows.append(
                            {
                                "dataset": str(item.get("dataset", "")).strip(),
                                "market": str(row.get("market", "")).strip(),
                                "cluster": str(row.get("cluster", "")).strip() or "unclustered",
                                "score_before_penalty": round(
                                    to_float(row.get("score_before_penalty", 0.0)),
                                    8,
                                ),
                                "penalty": round(to_float(row.get("penalty", 0.0)), 8),
                                "score_after_penalty": round(
                                    to_float(row.get("score_after_penalty", 0.0)),
                                    8,
                                ),
                                "rejected_by_penalty": bool(
                                    row.get("rejected_by_penalty", False)
                                ),
                            }
                        )
                debug_trace_rows = corr_telemetry.get("cluster_cap_debug_trace_samples", [])
                if isinstance(debug_trace_rows, list):
                    for row in debug_trace_rows:
                        if not isinstance(row, dict):
                            continue
                        aggregate_phase4_corr_debug_trace_rows.append(
                            {
                                "dataset": str(item.get("dataset", "")).strip(),
                                "market": str(row.get("market", "")).strip(),
                                "cluster": str(row.get("cluster", "")).strip() or "unclustered",
                                "cluster_exposure_before": round(
                                    max(0.0, to_float(row.get("cluster_exposure_before", 0.0))),
                                    8,
                                ),
                                "candidate_notional_fraction": round(
                                    max(
                                        0.0,
                                        to_float(row.get("candidate_notional_fraction", 0.0)),
                                    ),
                                    8,
                                ),
                                "cluster_cap_value": round(
                                    max(0.0, to_float(row.get("cluster_cap_value", 0.0))),
                                    8,
                                ),
                                "would_exceed": bool(row.get("would_exceed", False)),
                                "after_accept_cluster_exposure": round(
                                    max(
                                        0.0,
                                        to_float(
                                            row.get("after_accept_cluster_exposure", 0.0)
                                        ),
                                    ),
                                    8,
                                ),
                            }
                        )

        no_signal_patterns = item.get("no_signal_pattern_counts", {})
        if isinstance(no_signal_patterns, dict):
            for k, v in no_signal_patterns.items():
                pattern_key = str(k).strip()
                if not pattern_key:
                    continue
                aggregate_no_signal_patterns[pattern_key] = (
                    aggregate_no_signal_patterns.get(pattern_key, 0) + max(0, to_int(v))
                )

        edge_gap_buckets = item.get("entry_quality_edge_gap_buckets", {})
        if isinstance(edge_gap_buckets, dict):
            for k, v in edge_gap_buckets.items():
                bucket_key = str(k).strip()
                if not bucket_key:
                    continue
                aggregate_edge_gap_buckets[bucket_key] = (
                    aggregate_edge_gap_buckets.get(bucket_key, 0) + max(0, to_int(v))
                )

        pattern_profit_map = item.get("pattern_cell_profit_map", {})
        if isinstance(pattern_profit_map, dict):
            for k, v in pattern_profit_map.items():
                pattern_key = str(k).strip()
                if not pattern_key or not isinstance(v, dict):
                    continue
                slot = aggregate_pattern_profit_map.setdefault(
                    pattern_key,
                    {"trades": 0.0, "total_profit_krw": 0.0},
                )
                slot["trades"] += float(max(0, to_int(v.get("trades", 0))))
                slot["total_profit_krw"] += float(to_float(v.get("total_profit_krw", 0.0)))

        top_group = item.get("top_non_execution_group", {})
        if isinstance(top_group, dict):
            name = str(top_group.get("name", "")).strip()
            if name:
                top_group_votes[name] = top_group_votes.get(name, 0) + 1

        candidate_breakdown = item.get("candidate_generation_breakdown", {})
        if isinstance(candidate_breakdown, dict):
            components = candidate_breakdown.get("components", {})
            if isinstance(components, dict):
                for key in aggregate_candidate_components:
                    aggregate_candidate_components[key] += max(0, to_int(components.get(key, 0)))

            no_signal_reasons = candidate_breakdown.get("no_signal_reason_counts", {})
            if isinstance(no_signal_reasons, dict):
                for k, v in no_signal_reasons.items():
                    rk = str(k).strip()
                    if not rk:
                        continue
                    aggregate_no_signal_reasons[rk] = aggregate_no_signal_reasons.get(rk, 0) + max(0, to_int(v))

            core_no_signal_reasons = candidate_breakdown.get(
                "no_signal_reason_counts_core_effective",
                {},
            )
            if isinstance(core_no_signal_reasons, dict):
                for k, v in core_no_signal_reasons.items():
                    rk = str(k).strip()
                    if not rk:
                        continue
                    aggregate_no_signal_reasons_core_effective[rk] = (
                        aggregate_no_signal_reasons_core_effective.get(rk, 0)
                        + max(0, to_int(v))
                    )

            core_no_signal_ts_evidence = candidate_breakdown.get(
                "no_signal_reason_timestamp_evidence_core_effective",
                {},
            )
            if isinstance(core_no_signal_ts_evidence, dict):
                aggregate_no_signal_timestamp_evidence_core_effective[
                    "dataset_count_with_evidence"
                ] += 1
                aggregate_no_signal_timestamp_evidence_core_effective[
                    "event_rows_in_range"
                ] += max(0, to_int(core_no_signal_ts_evidence.get("event_rows_in_range", 0)))
                aggregate_no_signal_timestamp_evidence_core_effective[
                    "event_rows_total"
                ] += max(0, to_int(core_no_signal_ts_evidence.get("event_rows_total", 0)))
                aggregate_no_signal_timestamp_evidence_core_effective[
                    "event_rows_with_ts_total"
                ] += max(
                    0,
                    to_int(core_no_signal_ts_evidence.get("event_rows_with_ts_total", 0)),
                )
                aggregate_no_signal_timestamp_evidence_core_effective[
                    "event_rows_missing_ts_total"
                ] += max(
                    0,
                    to_int(core_no_signal_ts_evidence.get("event_rows_missing_ts_total", 0)),
                )

            manager_reasons = candidate_breakdown.get("manager_prefilter_reason_counts", {})
            if isinstance(manager_reasons, dict):
                for k, v in manager_reasons.items():
                    rk = str(k).strip()
                    if not rk:
                        continue
                    aggregate_manager_prefilter_reasons[rk] = (
                        aggregate_manager_prefilter_reasons.get(rk, 0) + max(0, to_int(v))
                    )

            policy_reasons = candidate_breakdown.get("policy_prefilter_reason_counts", {})
            if isinstance(policy_reasons, dict):
                for k, v in policy_reasons.items():
                    rk = str(k).strip()
                    if not rk:
                        continue
                    aggregate_policy_prefilter_reasons[rk] = (
                        aggregate_policy_prefilter_reasons.get(rk, 0) + max(0, to_int(v))
                    )

        post_entry_telemetry = item.get("post_entry_risk_telemetry", {})
        if isinstance(post_entry_telemetry, dict):
            aggregate_post_entry_telemetry["adaptive_stop_updates"] += max(
                0, to_int(post_entry_telemetry.get("adaptive_stop_updates", 0))
            )
            aggregate_post_entry_telemetry["adaptive_tp_recalibration_updates"] += max(
                0,
                to_int(post_entry_telemetry.get("adaptive_tp_recalibration_updates", 0)),
            )
            aggregate_post_entry_telemetry["adaptive_partial_ratio_samples"] += max(
                0,
                to_int(post_entry_telemetry.get("adaptive_partial_ratio_samples", 0)),
            )
            aggregate_post_entry_telemetry["stop_loss_trigger_count"] += max(
                0,
                to_int(post_entry_telemetry.get("stop_loss_trigger_count", 0)),
            )
            aggregate_post_entry_telemetry["stop_loss_pnl_sum_krw"] += to_float(
                post_entry_telemetry.get("stop_loss_pnl_sum_krw", 0.0)
            )
            aggregate_post_entry_telemetry["partial_tp_exit_count"] += max(
                0,
                to_int(post_entry_telemetry.get("partial_tp_exit_count", 0)),
            )
            aggregate_post_entry_telemetry["take_profit_full_count"] += max(
                0,
                to_int(post_entry_telemetry.get("take_profit_full_count", 0)),
            )
            aggregate_post_entry_telemetry["take_profit_full_pnl_sum_krw"] += to_float(
                post_entry_telemetry.get("take_profit_full_pnl_sum_krw", 0.0)
            )
            aggregate_post_entry_telemetry["trending_trade_count"] += max(
                0,
                to_int(post_entry_telemetry.get("trending_trade_count", 0)),
            )
            aggregate_post_entry_telemetry["trending_holding_minutes_sum"] += max(
                0.0,
                to_float(post_entry_telemetry.get("trending_holding_minutes_sum", 0.0)),
            )
            aggregate_post_entry_telemetry["stop_loss_distance_samples_trending"] += max(
                0,
                to_int(post_entry_telemetry.get("stop_loss_distance_samples_trending", 0)),
            )
            aggregate_post_entry_telemetry["stop_loss_distance_sum_pct_trending"] += max(
                0.0,
                to_float(post_entry_telemetry.get("stop_loss_distance_sum_pct_trending", 0.0)),
            )
            aggregate_post_entry_telemetry["take_profit_distance_samples_trending"] += max(
                0,
                to_int(post_entry_telemetry.get("take_profit_distance_samples_trending", 0)),
            )
            aggregate_post_entry_telemetry["take_profit_distance_sum_pct_trending"] += max(
                0.0,
                to_float(
                    post_entry_telemetry.get("take_profit_distance_sum_pct_trending", 0.0)
                ),
            )
            histogram = post_entry_telemetry.get("adaptive_partial_ratio_histogram", {})
            if isinstance(histogram, dict):
                for key in aggregate_post_entry_telemetry["adaptive_partial_ratio_histogram"]:
                    aggregate_post_entry_telemetry["adaptive_partial_ratio_histogram"][key] += max(
                        0,
                        to_int(histogram.get(key, 0)),
                    )

        shadow_funnel = item.get("shadow_funnel", {})
        if isinstance(shadow_funnel, dict):
            aggregate_shadow["rounds"] += max(0, to_int(shadow_funnel.get("rounds", 0)))
            aggregate_shadow["primary_generated_signals"] += max(
                0,
                to_int(shadow_funnel.get("primary_generated_signals", 0)),
            )
            aggregate_shadow["primary_after_sizing"] += max(
                0,
                to_int(shadow_funnel.get("primary_after_sizing", 0)),
            )
            aggregate_shadow["shadow_after_sizing"] += max(
                0,
                to_int(shadow_funnel.get("shadow_after_sizing", 0)),
            )
            aggregate_shadow["primary_after_policy_filter"] += max(
                0,
                to_int(shadow_funnel.get("primary_after_policy_filter", 0)),
            )
            aggregate_shadow["shadow_after_policy_filter"] += max(
                0,
                to_int(shadow_funnel.get("shadow_after_policy_filter", 0)),
            )
            aggregate_shadow["primary_best_signal_available"] += max(
                0,
                to_int(shadow_funnel.get("primary_best_signal_available", 0)),
            )
            aggregate_shadow["shadow_best_signal_available"] += max(
                0,
                to_int(shadow_funnel.get("shadow_best_signal_available", 0)),
            )
            aggregate_shadow["supply_improved_rounds"] += max(
                0,
                to_int(shadow_funnel.get("supply_improved_rounds", 0)),
            )
            aggregate_shadow["avg_manager_supply_lift_acc"] += to_float(
                shadow_funnel.get("avg_manager_supply_lift", 0.0)
            )
            aggregate_shadow["avg_policy_supply_lift_acc"] += to_float(
                shadow_funnel.get("avg_policy_supply_lift", 0.0)
            )
            aggregate_shadow["dataset_with_shadow_probe"] += 1

        shadow_contract = item.get("shadow_contract", {})
        if isinstance(shadow_contract, dict) and bool(shadow_contract.get("all_pass", False)):
            aggregate_shadow["contract_all_pass_count"] += 1

    non_execution_total = (
        aggregate_groups["candidate_generation"]
        + aggregate_groups["quality_and_risk_gate"]
        + aggregate_groups["execution_constraints"]
    )
    ordered_non_execution_groups = sorted(
        [
            ("candidate_generation", aggregate_groups["candidate_generation"]),
            ("quality_and_risk_gate", aggregate_groups["quality_and_risk_gate"]),
            ("execution_constraints", aggregate_groups["execution_constraints"]),
        ],
        key=lambda item: (-int(item[1]), item[0]),
    )
    primary_group_name, primary_group_count = ordered_non_execution_groups[0]
    primary_group_share = (
        (float(primary_group_count) / float(non_execution_total)) if non_execution_total > 0 else 0.0
    )
    shadow_dataset_count = max(0, to_int(aggregate_shadow.get("dataset_with_shadow_probe", 0)))
    aggregate_shadow_summary = {
        "rounds": int(aggregate_shadow["rounds"]),
        "primary_generated_signals": int(aggregate_shadow["primary_generated_signals"]),
        "primary_after_sizing": int(aggregate_shadow["primary_after_sizing"]),
        "shadow_after_sizing": int(aggregate_shadow["shadow_after_sizing"]),
        "primary_after_policy_filter": int(aggregate_shadow["primary_after_policy_filter"]),
        "shadow_after_policy_filter": int(aggregate_shadow["shadow_after_policy_filter"]),
        "primary_best_signal_available": int(aggregate_shadow["primary_best_signal_available"]),
        "shadow_best_signal_available": int(aggregate_shadow["shadow_best_signal_available"]),
        "supply_improved_rounds": int(aggregate_shadow["supply_improved_rounds"]),
        "avg_manager_supply_lift": round(
            (aggregate_shadow["avg_manager_supply_lift_acc"] / float(shadow_dataset_count))
            if shadow_dataset_count > 0 else 0.0,
            6,
        ),
        "avg_policy_supply_lift": round(
            (aggregate_shadow["avg_policy_supply_lift_acc"] / float(shadow_dataset_count))
            if shadow_dataset_count > 0 else 0.0,
            6,
        ),
        "contract_all_pass_count": int(aggregate_shadow["contract_all_pass_count"]),
        "contract_all_pass_rate": round(
            (float(aggregate_shadow["contract_all_pass_count"]) / float(shadow_dataset_count))
            if shadow_dataset_count > 0 else 0.0,
            4,
        ),
    }
    aggregate_top_shadow_markets = [
        {"market": str(market), "count": int(count)}
        for market, count in sorted(
            aggregate_ranging_shadow["shadow_count_by_market"].items(),
            key=lambda item: (-int(item[1]), str(item[0])),
        )[:5]
    ]
    aggregate_ranging_shadow_summary = {
        "shadow_count_total": int(aggregate_ranging_shadow["shadow_count_total"]),
        "shadow_count_by_regime": {
            str(k): int(max(0, to_int(v)))
            for k, v in sorted(
                aggregate_ranging_shadow["shadow_count_by_regime"].items(),
                key=lambda item: str(item[0]),
            )
        },
        "shadow_count_by_market": {
            str(k): int(max(0, to_int(v)))
            for k, v in sorted(
                aggregate_ranging_shadow["shadow_count_by_market"].items(),
                key=lambda item: str(item[0]),
            )
        },
        "shadow_would_pass_quality_selection_count": int(
            aggregate_ranging_shadow["shadow_would_pass_quality_selection_count"]
        ),
        "shadow_would_pass_manager_count": int(
            aggregate_ranging_shadow["shadow_would_pass_manager_count"]
        ),
        "shadow_would_pass_execution_guard_count": int(
            aggregate_ranging_shadow["shadow_would_pass_execution_guard_count"]
        ),
        "shadow_edge_neg_count": int(aggregate_ranging_shadow["shadow_edge_neg_count"]),
        "shadow_edge_pos_count": int(aggregate_ranging_shadow["shadow_edge_pos_count"]),
        "top_shadow_markets": aggregate_top_shadow_markets,
    }
    candidate_component_total = max(0, sum(aggregate_candidate_components.values()))
    candidate_component_denom = float(max(1, candidate_component_total))
    ordered_candidate_components = sorted(
        aggregate_candidate_components.items(),
        key=lambda item: (-int(item[1]), item[0]),
    )
    primary_candidate_component_name, primary_candidate_component_count = ordered_candidate_components[0]
    aggregate_candidate_component_shares = {
        "no_signal_generated_share": round(
            float(aggregate_candidate_components["no_signal_generated"]) / candidate_component_denom,
            4,
        ),
        "filtered_out_by_sizing_share": round(
            float(aggregate_candidate_components["filtered_out_by_sizing"]) / candidate_component_denom,
            4,
        ),
        "filtered_out_by_policy_share": round(
            float(aggregate_candidate_components["filtered_out_by_policy"]) / candidate_component_denom,
            4,
        ),
        "no_best_signal_share": round(
            float(aggregate_candidate_components["no_best_signal"]) / candidate_component_denom,
            4,
        ),
    }
    aggregate_top_no_signal_reasons = top_reason_rows(aggregate_no_signal_reasons, limit=10)
    aggregate_top_no_signal_reasons_core_effective = top_reason_rows(
        aggregate_no_signal_reasons_core_effective,
        limit=10,
    )
    aggregate_top_manager_prefilter_reasons = top_reason_rows(
        aggregate_manager_prefilter_reasons,
        limit=10,
    )
    aggregate_top_policy_prefilter_reasons = top_reason_rows(
        aggregate_policy_prefilter_reasons,
        limit=10,
    )
    aggregate_ab_playbook = build_candidate_generation_ab_playbook(
        component_shares=aggregate_candidate_component_shares,
        top_no_signal_reasons=aggregate_top_no_signal_reasons,
        top_manager_prefilter_reasons=aggregate_top_manager_prefilter_reasons,
    )
    _core_ts_total = max(
        0,
        to_int(
            aggregate_no_signal_timestamp_evidence_core_effective.get(
                "event_rows_total",
                0,
            )
        ),
    )
    aggregate_no_signal_timestamp_evidence_core_effective[
        "timestamp_coverage_total"
    ] = (
        round(
            float(
                max(
                    0,
                    to_int(
                        aggregate_no_signal_timestamp_evidence_core_effective.get(
                            "event_rows_with_ts_total",
                            0,
                        )
                    ),
                )
            )
            / float(max(1, _core_ts_total)),
            6,
        )
        if _core_ts_total > 0
        else 0.0
    )

    def build_phase3_aggregate_rows(
        payload: Dict[str, Dict[str, int]],
    ) -> List[Dict[str, Any]]:
        rows: List[Dict[str, Any]] = []
        for label, counts in payload.items():
            candidate_total = max(0, to_int(counts.get("candidate_total", 0)))
            pass_total = max(0, to_int(counts.get("pass_total", 0)))
            rows.append(
                {
                    "label": str(label),
                    "candidate_total": int(candidate_total),
                    "pass_total": int(pass_total),
                    "pass_rate": round(float(pass_total) / float(max(1, candidate_total)), 4),
                }
            )
        rows.sort(key=lambda item: (-int(item["candidate_total"]), str(item["label"])))
        return rows

    phase3_reject_rows = sorted(
        [
            {"reason": key, "count": int(value)}
            for key, value in aggregate_phase3_counters.items()
            if key.startswith("reject_") and int(value) > 0
        ],
        key=lambda item: (-int(item["count"]), str(item["reason"])),
    )
    aggregate_phase3_diag = {
        "enabled": bool(
            aggregate_phase3_counters.get("candidate_total", 0) > 0
            or aggregate_phase3_counters.get("pass_total", 0) > 0
            or phase3_reject_rows
        ),
        "counters": {key: int(value) for key, value in aggregate_phase3_counters.items()},
        "funnel_breakdown": {
            "candidate_total": int(aggregate_phase3_counters.get("candidate_total", 0)),
            "pass_total": int(aggregate_phase3_counters.get("pass_total", 0)),
            "reject_margin_insufficient": int(
                aggregate_phase3_counters.get("reject_margin_insufficient", 0)
            ),
            "reject_strength_fail": int(aggregate_phase3_counters.get("reject_strength_fail", 0)),
            "reject_expected_value_fail": int(
                aggregate_phase3_counters.get("reject_expected_value_fail", 0)
            ),
            "reject_expected_edge_negative_count": int(
                aggregate_phase3_counters.get("reject_expected_edge_negative_count", 0)
            ),
            "reject_regime_entry_disabled_count": int(
                aggregate_phase3_counters.get("reject_regime_entry_disabled_count", 0)
            ),
            "reject_quality_filter_fail": int(aggregate_phase3_counters.get("reject_quality_filter_fail", 0)),
            "reject_ev_confidence_low": int(
                aggregate_phase3_counters.get("reject_ev_confidence_low", 0)
            ),
            "reject_cost_tail_fail": int(aggregate_phase3_counters.get("reject_cost_tail_fail", 0)),
        },
        "pass_rate_by_regime": build_phase3_aggregate_rows(
            aggregate_phase3_slices["pass_rate_by_regime"]
        ),
        "pass_rate_by_vol_bucket": build_phase3_aggregate_rows(
            aggregate_phase3_slices["pass_rate_by_vol_bucket"]
        ),
        "pass_rate_by_liquidity_bucket": build_phase3_aggregate_rows(
            aggregate_phase3_slices["pass_rate_by_liquidity_bucket"]
        ),
        "pass_rate_edge_regressor_present_vs_fallback": build_phase3_aggregate_rows(
            aggregate_phase3_slices["pass_rate_edge_regressor_present_vs_fallback"]
        ),
        "pass_rate_by_cost_mode": build_phase3_aggregate_rows(
            aggregate_phase3_slices["pass_rate_by_cost_mode"]
        ),
        "top3_bottlenecks": phase3_reject_rows[:3],
    }
    aggregate_phase3_pass_ev_samples_bps.sort()
    aggregate_phase3_pass_ev_selected_samples_bps.sort()

    def _ev_quantiles(values: List[float]) -> Dict[str, float]:
        if not values:
            return {
                "p10": 0.0,
                "p25": 0.0,
                "p50": 0.0,
                "p75": 0.0,
                "p90": 0.0,
            }
        return {
            "p10": round(quantile(values, 0.10), 6),
            "p25": round(quantile(values, 0.25), 6),
            "p50": round(quantile(values, 0.50), 6),
            "p75": round(quantile(values, 0.75), 6),
            "p90": round(quantile(values, 0.90), 6),
        }

    aggregate_phase3_pass_ev_quantiles_bps = _ev_quantiles(aggregate_phase3_pass_ev_samples_bps)
    aggregate_phase3_pass_ev_quantiles_pct = {
        key: round(float(value) / 10000.0, 10)
        for key, value in aggregate_phase3_pass_ev_quantiles_bps.items()
    }
    aggregate_phase3_pass_ev_selected_quantiles_bps = _ev_quantiles(
        aggregate_phase3_pass_ev_selected_samples_bps
    )
    aggregate_phase3_pass_ev_selected_quantiles_pct = {
        key: round(float(value) / 10000.0, 10)
        for key, value in aggregate_phase3_pass_ev_selected_quantiles_bps.items()
    }
    phase4_reject_rows = sorted(
        [
            {"reason": key, "count": int(value)}
            for key, value in aggregate_phase4_counters.items()
            if key.startswith("rejected_") and int(value) > 0
        ],
        key=lambda item: (-int(item["count"]), str(item["reason"])),
    )
    phase4_selection_rate = round(
        float(aggregate_phase4_counters.get("selected_total", 0))
        / float(max(1, aggregate_phase4_counters.get("candidates_total", 0))),
        6,
    )
    aggregate_phase4_total_exposure = sum(
        max(0.0, to_float(v)) for v in aggregate_phase4_exposure_by_cluster.values()
    )
    aggregate_phase4_cluster_rows: List[Dict[str, Any]] = []
    aggregate_phase4_max_cluster_share = 0.0
    for cluster in sorted(aggregate_phase4_exposure_by_cluster.keys()):
        exposure = max(0.0, to_float(aggregate_phase4_exposure_by_cluster.get(cluster, 0.0)))
        share = (
            (exposure / aggregate_phase4_total_exposure)
            if aggregate_phase4_total_exposure > 0.0
            else 0.0
        )
        aggregate_phase4_max_cluster_share = max(aggregate_phase4_max_cluster_share, share)
        aggregate_phase4_cluster_rows.append(
            {
                "cluster": str(cluster),
                "selected_count": int(aggregate_phase4_selected_by_cluster.get(cluster, 0)),
                "notional_exposure_sum": round(exposure, 8),
                "share": round(share, 6),
            }
        )

    aggregate_corr_stage = ""
    if aggregate_phase4_corr_stage_votes:
        aggregate_corr_stage = sorted(
            aggregate_phase4_corr_stage_votes.items(),
            key=lambda item: (-int(item[1]), str(item[0])),
        )[0][0]
    aggregate_corr_unit = ""
    if aggregate_phase4_corr_unit_votes:
        aggregate_corr_unit = sorted(
            aggregate_phase4_corr_unit_votes.items(),
            key=lambda item: (-int(item[1]), str(item[0])),
        )[0][0]
    strategy_exit_mode_effective = "enforce"
    if aggregate_strategy_exit["mode_votes"]:
        strategy_exit_mode_effective = sorted(
            aggregate_strategy_exit["mode_votes"].items(),
            key=lambda item: (-int(item[1]), str(item[0])),
        )[0][0]
    aggregate_corr_penalty_avg = (
        aggregate_phase4_corr_penalty_weighted_sum
        / float(aggregate_phase4_corr_penalty_applied_count)
        if aggregate_phase4_corr_penalty_applied_count > 0
        else 0.0
    )
    aggregate_corr_cluster_rows: List[Dict[str, Any]] = []
    for cluster in sorted(
        set(aggregate_phase4_corr_cluster_exposure_max.keys())
        | set(aggregate_phase4_corr_cluster_cap_max.keys())
    ):
        exposure_current = max(
            0.0,
            to_float(aggregate_phase4_corr_cluster_exposure_max.get(cluster, 0.0)),
        )
        cluster_cap_value = max(
            0.0,
            to_float(aggregate_phase4_corr_cluster_cap_max.get(cluster, 0.0)),
        )
        headroom = cluster_cap_value - exposure_current
        utilization = (
            (exposure_current / cluster_cap_value) if cluster_cap_value > 1.0e-9 else 0.0
        )
        aggregate_corr_cluster_rows.append(
            {
                "cluster": str(cluster),
                "exposure_current": round(exposure_current, 8),
                "cluster_cap_value": round(cluster_cap_value, 8),
                "headroom": round(headroom, 8),
                "utilization": round(utilization, 6),
            }
        )
    aggregate_phase4_corr_near_cap_rows.sort(
        key=lambda item: (
            abs(to_float(item.get("headroom_ratio", 0.0))),
            abs(to_float(item.get("headroom_before", 0.0))),
            str(item.get("market", "")),
        )
    )
    aggregate_phase4_corr_near_cap_rows = aggregate_phase4_corr_near_cap_rows[:16]
    aggregate_phase4_corr_penalty_score_rows.sort(
        key=lambda item: (
            -abs(to_float(item.get("penalty", 0.0))),
            str(item.get("market", "")),
            str(item.get("dataset", "")),
        )
    )
    aggregate_phase4_corr_penalty_score_rows = aggregate_phase4_corr_penalty_score_rows[:16]
    aggregate_phase4_corr_debug_trace_rows.sort(
        key=lambda item: (
            -int(bool(item.get("would_exceed", False))),
            -to_float(item.get("candidate_notional_fraction", 0.0)),
            str(item.get("market", "")),
            str(item.get("dataset", "")),
        )
    )
    aggregate_phase4_corr_debug_trace_rows = aggregate_phase4_corr_debug_trace_rows[:20]
    aggregate_corr_applied_telemetry = {
        "constraint_apply_stage": aggregate_corr_stage,
        "constraint_apply_stage_votes": aggregate_phase4_corr_stage_votes,
        "constraint_unit": aggregate_corr_unit,
        "constraint_unit_votes": aggregate_phase4_corr_unit_votes,
        "cluster_cap_checks_total": int(aggregate_phase4_corr_checks_total),
        "cluster_cap_near_cap_count": int(aggregate_phase4_corr_near_cap_count),
        "cluster_cap_skips_count": int(
            aggregate_phase4_counters.get("cluster_cap_skips_count", 0)
        ),
        "cluster_cap_would_exceed_count": int(
            aggregate_phase4_counters.get("cluster_cap_would_exceed_count", 0)
        ),
        "cluster_exposure_update_count": int(
            aggregate_phase4_counters.get("cluster_exposure_update_count", 0)
        ),
        "corr_penalty_applied_count": int(aggregate_phase4_corr_penalty_applied_count),
        "corr_penalty_avg": round(float(aggregate_corr_penalty_avg), 8),
        "corr_penalty_max": round(float(aggregate_phase4_corr_penalty_max), 8),
        "penalty_score_samples": aggregate_phase4_corr_penalty_score_rows,
        "cluster_exposure_current": aggregate_corr_cluster_rows,
        "near_cap_candidates": aggregate_phase4_corr_near_cap_rows,
        "cluster_cap_debug_trace_samples": aggregate_phase4_corr_debug_trace_rows,
        "binding_detected": bool(
            aggregate_phase4_counters.get("rejected_by_cluster_cap", 0) > 0
            or aggregate_phase4_corr_penalty_applied_count > 0
        ),
    }

    aggregate_phase4_diag = {
        "enabled": bool(
            aggregate_phase4_counters.get("candidates_total", 0) > 0
            or aggregate_phase4_counters.get("selected_total", 0) > 0
        ),
        "flags": aggregate_phase4_flags,
        "exposure_source": "selected_count_unit_proxy",
        "max_cluster_share": round(aggregate_phase4_max_cluster_share, 6),
        "total_selected_notional_exposure": round(aggregate_phase4_total_exposure, 8),
        "exposure_by_cluster": aggregate_phase4_cluster_rows,
        "correlation_applied_telemetry": aggregate_corr_applied_telemetry,
        "counters": {
            **{key: int(value) for key, value in aggregate_phase4_counters.items()},
            "selection_rate": float(phase4_selection_rate),
        },
        "selection_breakdown": {
            "candidates_total": int(aggregate_phase4_counters.get("candidates_total", 0)),
            "selected_total": int(aggregate_phase4_counters.get("selected_total", 0)),
            "selection_rate": float(phase4_selection_rate),
            "rejected_by_budget": int(aggregate_phase4_counters.get("rejected_by_budget", 0)),
            "rejected_by_cluster_cap": int(
                aggregate_phase4_counters.get("rejected_by_cluster_cap", 0)
            ),
            "rejected_by_correlation_penalty": int(
                aggregate_phase4_counters.get("rejected_by_correlation_penalty", 0)
            ),
            "cluster_cap_skips_count": int(
                aggregate_phase4_counters.get("cluster_cap_skips_count", 0)
            ),
            "cluster_cap_would_exceed_count": int(
                aggregate_phase4_counters.get("cluster_cap_would_exceed_count", 0)
            ),
            "cluster_exposure_update_count": int(
                aggregate_phase4_counters.get("cluster_exposure_update_count", 0)
            ),
            "rejected_by_execution_cap": int(
                aggregate_phase4_counters.get("rejected_by_execution_cap", 0)
            ),
            "rejected_by_drawdown_governor": int(
                aggregate_phase4_counters.get("rejected_by_drawdown_governor", 0)
            ),
            "candidate_snapshot_total": int(
                aggregate_phase4_counters.get("candidate_snapshot_total", 0)
            ),
            "candidate_snapshot_sampled": int(
                aggregate_phase4_counters.get("candidate_snapshot_sampled", 0)
            ),
        },
        "top3_bottlenecks": phase4_reject_rows[:3],
    }
    aggregate_stop_loss_trigger_count = max(
        0,
        to_int(aggregate_post_entry_telemetry.get("stop_loss_trigger_count", 0)),
    )
    aggregate_post_entry_telemetry["stop_loss_avg_pnl_krw"] = (
        round(
            to_float(aggregate_post_entry_telemetry.get("stop_loss_pnl_sum_krw", 0.0))
            / float(aggregate_stop_loss_trigger_count),
            6,
        )
        if aggregate_stop_loss_trigger_count > 0
        else 0.0
    )
    aggregate_take_profit_full_count = max(
        0,
        to_int(aggregate_post_entry_telemetry.get("take_profit_full_count", 0)),
    )
    aggregate_post_entry_telemetry["take_profit_full_avg_pnl_krw"] = (
        round(
            to_float(aggregate_post_entry_telemetry.get("take_profit_full_pnl_sum_krw", 0.0))
            / float(aggregate_take_profit_full_count),
            6,
        )
        if aggregate_take_profit_full_count > 0
        else 0.0
    )
    aggregate_trending_trade_count = max(
        0,
        to_int(aggregate_post_entry_telemetry.get("trending_trade_count", 0)),
    )
    aggregate_post_entry_telemetry["avg_holding_minutes_trending"] = (
        round(
            to_float(aggregate_post_entry_telemetry.get("trending_holding_minutes_sum", 0.0))
            / float(aggregate_trending_trade_count),
            6,
        )
        if aggregate_trending_trade_count > 0
        else 0.0
    )
    aggregate_stop_loss_distance_samples_trending = max(
        0,
        to_int(aggregate_post_entry_telemetry.get("stop_loss_distance_samples_trending", 0)),
    )
    aggregate_post_entry_telemetry["avg_stop_loss_distance_pct_trending"] = (
        round(
            to_float(aggregate_post_entry_telemetry.get("stop_loss_distance_sum_pct_trending", 0.0))
            / float(aggregate_stop_loss_distance_samples_trending),
            8,
        )
        if aggregate_stop_loss_distance_samples_trending > 0
        else 0.0
    )
    aggregate_take_profit_distance_samples_trending = max(
        0,
        to_int(aggregate_post_entry_telemetry.get("take_profit_distance_samples_trending", 0)),
    )
    aggregate_post_entry_telemetry["avg_take_profit_distance_pct_trending"] = (
        round(
            to_float(
                aggregate_post_entry_telemetry.get("take_profit_distance_sum_pct_trending", 0.0)
            )
            / float(aggregate_take_profit_distance_samples_trending),
            8,
        )
        if aggregate_take_profit_distance_samples_trending > 0
        else 0.0
    )
    aggregate_total_exit_count = (
        aggregate_stop_loss_trigger_count
        + max(0, to_int(aggregate_post_entry_telemetry.get("partial_tp_exit_count", 0)))
        + aggregate_take_profit_full_count
    )
    aggregate_post_entry_telemetry["tp_hit_rate"] = (
        round(float(aggregate_take_profit_full_count) / float(aggregate_total_exit_count), 6)
        if aggregate_total_exit_count > 0
        else 0.0
    )

    return {
        "dataset_count": len(dataset_diagnostics),
        "aggregate_bottleneck_groups": aggregate_groups,
        "primary_non_execution_group": {
            "name": primary_group_name,
            "count": int(primary_group_count),
            "share_of_non_execution": round(primary_group_share, 4),
        },
        "top_rejection_reasons": top_reason_rows(aggregate_reasons, limit=8),
        "top_no_signal_patterns": top_pattern_rows(aggregate_no_signal_patterns, limit=8),
        "top_entry_quality_edge_gap_buckets": top_pattern_rows(aggregate_edge_gap_buckets, limit=8),
        "top_loss_pattern_cells": top_loss_pattern_cells(aggregate_pattern_profit_map, limit=10),
        "candidate_generation_analysis": {
            "components": aggregate_candidate_components,
            "component_shares": aggregate_candidate_component_shares,
            "primary_component": {
                "name": str(primary_candidate_component_name),
                "count": int(primary_candidate_component_count),
                "share": round(
                    float(primary_candidate_component_count) / candidate_component_denom,
                    4,
                ),
            },
            "no_signal_reason_counts_execution_scope": aggregate_no_signal_reasons,
            "no_signal_reason_counts_core_effective": aggregate_no_signal_reasons_core_effective,
            "top_no_signal_reasons_execution_scope": aggregate_top_no_signal_reasons,
            "top_no_signal_reasons_core_effective": aggregate_top_no_signal_reasons_core_effective,
            "top_no_signal_reasons": aggregate_top_no_signal_reasons,
            "execution_no_signal_top3": top_reason_rows(aggregate_no_signal_reasons, limit=3),
            "core_no_signal_top3": top_reason_rows(
                aggregate_no_signal_reasons_core_effective,
                limit=3,
            ),
            "no_signal_reason_timestamp_evidence_core_effective": (
                aggregate_no_signal_timestamp_evidence_core_effective
            ),
            "top_manager_prefilter_reasons": aggregate_top_manager_prefilter_reasons,
            "top_policy_prefilter_reasons": aggregate_top_policy_prefilter_reasons,
            "ab_playbook_candidates": aggregate_ab_playbook,
        },
        "top_non_execution_group_vote_counts": top_group_votes,
        "post_entry_risk_telemetry": aggregate_post_entry_telemetry,
        "phase3_pass_ev_distribution": {
            "enabled": bool(len(aggregate_phase3_pass_ev_samples_bps) > 0),
            "source": "policy_decisions_backtest_jsonl",
            "source_counts": aggregate_phase3_pass_ev_source_counts,
            "sample_size_total": int(len(aggregate_phase3_pass_ev_samples_bps)),
            "selected_sample_size_total": int(len(aggregate_phase3_pass_ev_selected_samples_bps)),
            "quantiles_bps": aggregate_phase3_pass_ev_quantiles_bps,
            "quantiles_pct": aggregate_phase3_pass_ev_quantiles_pct,
            "selected_quantiles_bps": aggregate_phase3_pass_ev_selected_quantiles_bps,
            "selected_quantiles_pct": aggregate_phase3_pass_ev_selected_quantiles_pct,
            "samples_bps": [round(float(x), 6) for x in aggregate_phase3_pass_ev_samples_bps[:5000]],
            "sample_cap": 5000,
            "sample_truncated": bool(len(aggregate_phase3_pass_ev_samples_bps) > 5000),
        },
        "phase3_diagnostics_v2": aggregate_phase3_diag,
        "phase4_portfolio_diagnostics": aggregate_phase4_diag,
        "strategy_exit": {
            "mode_effective": strategy_exit_mode_effective,
            "mode_votes": aggregate_strategy_exit["mode_votes"],
            "strategy_exit_triggered_count": int(
                aggregate_strategy_exit["strategy_exit_triggered_count"]
            ),
            "strategy_exit_would_trigger_count": int(
                aggregate_strategy_exit["strategy_exit_triggered_count"]
            ),
            "strategy_exit_observe_only_suppressed_count": int(
                aggregate_strategy_exit["strategy_exit_observe_only_suppressed_count"]
            ),
            "strategy_exit_executed_count": int(
                aggregate_strategy_exit["strategy_exit_executed_count"]
            ),
            "strategy_exit_clamp_applied_count": int(
                aggregate_strategy_exit["strategy_exit_clamp_applied_count"]
            ),
            "strategy_exit_triggered_by_market": {
                str(k): int(max(0, to_int(v)))
                for k, v in sorted(
                    aggregate_strategy_exit["strategy_exit_triggered_by_market"].items(),
                    key=lambda item: str(item[0]),
                )
            },
            "strategy_exit_triggered_by_regime": {
                str(k): int(max(0, to_int(v)))
                for k, v in sorted(
                    aggregate_strategy_exit["strategy_exit_triggered_by_regime"].items(),
                    key=lambda item: str(item[0]),
                )
            },
            "strategy_exit_would_trigger_samples": aggregate_strategy_exit[
                "strategy_exit_would_trigger_samples"
            ],
            "strategy_exit_clamp_samples": aggregate_strategy_exit["strategy_exit_clamp_samples"],
        },
        "regime_entry_disable": {
            "enabled": bool(aggregate_regime_entry_disable_enabled),
            "disabled_regimes": sorted(list(aggregate_disabled_regimes)),
            "reject_regime_entry_disabled_count": int(
                max(0, aggregate_reject_regime_entry_disabled_count)
            ),
            "reject_regime_entry_disabled_by_regime": {
                str(k): int(max(0, to_int(v)))
                for k, v in sorted(
                    aggregate_reject_regime_entry_disabled_by_regime.items(),
                    key=lambda item: (item[0]),
                )
            },
        },
        "ranging_shadow": aggregate_ranging_shadow_summary,
        "shadow_funnel": aggregate_shadow_summary,
    }


def build_core_no_signal_summary(
    aggregate_diagnostics: Dict[str, Any],
    dataset_diagnostics: List[Dict[str, Any]],
    split_filter_context: Dict[str, Any],
    split_name: str,
) -> Dict[str, Any]:
    agg_candidate = aggregate_diagnostics.get("candidate_generation_analysis", {})
    if not isinstance(agg_candidate, dict):
        agg_candidate = {}

    execution_no_signal_top3 = agg_candidate.get("execution_no_signal_top3", [])
    if not isinstance(execution_no_signal_top3, list):
        execution_no_signal_top3 = []
    core_no_signal_top3 = agg_candidate.get("core_no_signal_top3", [])
    if not isinstance(core_no_signal_top3, list):
        core_no_signal_top3 = []

    execution_no_signal_counts = normalized_reason_counts(
        agg_candidate.get("no_signal_reason_counts_execution_scope", {})
    )
    core_no_signal_counts = normalized_reason_counts(
        agg_candidate.get("no_signal_reason_counts_core_effective", {})
    )
    ts_evidence = agg_candidate.get("no_signal_reason_timestamp_evidence_core_effective", {})
    if not isinstance(ts_evidence, dict):
        ts_evidence = {}

    per_dataset_rows: List[Dict[str, Any]] = []
    for row in dataset_diagnostics:
        if not isinstance(row, dict):
            continue
        candidate_breakdown = row.get("candidate_generation_breakdown", {})
        if not isinstance(candidate_breakdown, dict):
            candidate_breakdown = {}
        execution_counts_ds = normalized_reason_counts(
            candidate_breakdown.get("no_signal_reason_counts_execution_scope", {})
            or candidate_breakdown.get("no_signal_reason_counts", {})
        )
        core_counts_ds = normalized_reason_counts(
            candidate_breakdown.get("no_signal_reason_counts_core_effective", {})
        )
        per_dataset_rows.append(
            {
                "dataset": str(row.get("dataset", "")).strip(),
                "execution_no_signal_top3": top_reason_rows(execution_counts_ds, limit=3),
                "core_no_signal_top3": top_reason_rows(core_counts_ds, limit=3),
                "execution_probabilistic_feature_build_failed": int(
                    execution_counts_ds.get("probabilistic_feature_build_failed", 0)
                ),
                "core_probabilistic_feature_build_failed": int(
                    core_counts_ds.get("probabilistic_feature_build_failed", 0)
                ),
            }
        )

    core_top1_reason = ""
    if core_no_signal_top3 and isinstance(core_no_signal_top3[0], dict):
        core_top1_reason = str(core_no_signal_top3[0].get("reason", "")).strip()

    return {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "split_name": str(split_name).strip(),
        "evaluation_range_effective": (
            split_filter_context.get("evaluation_range_effective", [])
            if isinstance(split_filter_context, dict)
            else []
        ),
        "execution_range_used": (
            split_filter_context.get("execution_range_used", [])
            if isinstance(split_filter_context, dict)
            else []
        ),
        "aggregate": {
            "execution_no_signal_top3": execution_no_signal_top3[:3],
            "core_no_signal_top3": core_no_signal_top3[:3],
            "execution_no_signal_reason_counts": execution_no_signal_counts,
            "core_no_signal_reason_counts": core_no_signal_counts,
            "execution_probabilistic_feature_build_failed": int(
                execution_no_signal_counts.get("probabilistic_feature_build_failed", 0)
            ),
            "core_probabilistic_feature_build_failed": int(
                core_no_signal_counts.get("probabilistic_feature_build_failed", 0)
            ),
            "timestamp_evidence_core_effective": ts_evidence,
            "candidate_total_core_effective": int(
                split_filter_context.get("candidate_total_core_effective", 0)
                if isinstance(split_filter_context, dict)
                else 0
            ),
            "total_trades_core_effective": int(
                split_filter_context.get("total_trades_core_effective", 0)
                if isinstance(split_filter_context, dict)
                else 0
            ),
        },
        "per_dataset": per_dataset_rows,
        "summary": {
            "core_feature_build_failed_dominant": bool(
                str(core_top1_reason).strip().lower() == "probabilistic_feature_build_failed"
            ),
            "core_has_candidates_or_trades": bool(
                int(
                    split_filter_context.get("candidate_total_core_effective", 0)
                    if isinstance(split_filter_context, dict)
                    else 0
                )
                > 0
                or int(
                    split_filter_context.get("total_trades_core_effective", 0)
                    if isinstance(split_filter_context, dict)
                    else 0
                )
                > 0
            ),
        },
    }


def build_failure_attribution(
    gate: Dict[str, bool],
    avg_total_trades: float,
    min_avg_trades: float,
    aggregate_diagnostics: Dict[str, Any],
) -> Dict[str, Any]:
    failed_gates = [k for k, v in gate.items() if not bool(v)]
    primary_group_name = "candidate_generation"
    primary_group = aggregate_diagnostics.get("primary_non_execution_group", {})
    if isinstance(primary_group, dict):
        name = str(primary_group.get("name", "")).strip()
        if name:
            primary_group_name = name
    primary_no_signal_pattern = ""
    top_no_signal_patterns = aggregate_diagnostics.get("top_no_signal_patterns", [])
    if isinstance(top_no_signal_patterns, list) and top_no_signal_patterns:
        first = top_no_signal_patterns[0]
        if isinstance(first, dict):
            primary_no_signal_pattern = str(first.get("pattern", "")).strip()
    primary_edge_gap_bucket = ""
    top_edge_gap_buckets = aggregate_diagnostics.get("top_entry_quality_edge_gap_buckets", [])
    if isinstance(top_edge_gap_buckets, list) and top_edge_gap_buckets:
        first_gap = top_edge_gap_buckets[0]
        if isinstance(first_gap, dict):
            primary_edge_gap_bucket = str(first_gap.get("pattern", "")).strip()
    primary_loss_pattern_cell = ""
    top_loss_pattern_cells_rows = aggregate_diagnostics.get("top_loss_pattern_cells", [])
    if isinstance(top_loss_pattern_cells_rows, list) and top_loss_pattern_cells_rows:
        first_loss = top_loss_pattern_cells_rows[0]
        if isinstance(first_loss, dict):
            primary_loss_pattern_cell = str(first_loss.get("pattern", "")).strip()
    candidate_generation_analysis = aggregate_diagnostics.get("candidate_generation_analysis", {})
    if not isinstance(candidate_generation_analysis, dict):
        candidate_generation_analysis = {}
    primary_candidate_component = ""
    primary_candidate_component_share = 0.0
    primary_component = candidate_generation_analysis.get("primary_component", {})
    if isinstance(primary_component, dict):
        primary_candidate_component = str(primary_component.get("name", "")).strip()
        primary_candidate_component_share = to_float(primary_component.get("share", 0.0))
    primary_no_signal_reason = ""
    top_no_signal_reasons = candidate_generation_analysis.get("top_no_signal_reasons", [])
    if isinstance(top_no_signal_reasons, list) and top_no_signal_reasons:
        first_reason = top_no_signal_reasons[0]
        if isinstance(first_reason, dict):
            primary_no_signal_reason = str(first_reason.get("reason", "")).strip()
    primary_manager_prefilter_reason = ""
    top_manager_prefilter_reasons = candidate_generation_analysis.get("top_manager_prefilter_reasons", [])
    if isinstance(top_manager_prefilter_reasons, list) and top_manager_prefilter_reasons:
        first_reason = top_manager_prefilter_reasons[0]
        if isinstance(first_reason, dict):
            primary_manager_prefilter_reason = str(first_reason.get("reason", "")).strip()

    hypothesis = "balanced_or_data_limited"
    next_focus: List[str] = []
    if primary_group_name == "candidate_generation":
        hypothesis = "signal_supply_or_prefilter_bottleneck"
        next_focus = [
            "Inspect strategy-level generated->selected->executed conversion first.",
            "Check whether manager prefilter and expected-value floor are too strict.",
            "Extract shared market-pattern signatures where no_signal_generated dominates.",
        ]
        if primary_candidate_component:
            next_focus.append(
                "Primary candidate component: "
                f"{primary_candidate_component} (share={round(primary_candidate_component_share, 4)})."
            )
        if primary_no_signal_reason:
            next_focus.append(f"Top no-signal reason: {primary_no_signal_reason}")
        if primary_manager_prefilter_reason:
            next_focus.append(f"Top manager prefilter reason: {primary_manager_prefilter_reason}")
    elif primary_group_name == "quality_and_risk_gate":
        hypothesis = "risk_gate_overconstraint_or_quality_mismatch"
        next_focus = [
            "Inspect blocked_risk_gate composition and risk-gate reason mix.",
            "Check if risk-gate thresholds are overly strict for current regimes.",
            "Prefer regime-specific gate design over blanket threshold relaxation.",
        ]
    elif primary_group_name == "execution_constraints":
        hypothesis = "execution_path_or_position_constraints"
        next_focus = [
            "Inspect blocked_min_order_or_capital and blocked_order_sizing share.",
            "Check opportunity-loss structure from open-position occupancy.",
            "Review engine sizing and concurrent-position limit logic.",
        ]

    low_trade_condition = float(avg_total_trades) < float(min_avg_trades)
    return {
        "failed_gates": failed_gates,
        "primary_non_execution_group": primary_group_name,
        "primary_no_signal_pattern": primary_no_signal_pattern,
        "primary_entry_quality_edge_gap_bucket": primary_edge_gap_bucket,
        "primary_loss_pattern_cell": primary_loss_pattern_cell,
        "primary_candidate_generation_component": primary_candidate_component,
        "primary_candidate_generation_component_share": round(primary_candidate_component_share, 4),
        "primary_no_signal_reason": primary_no_signal_reason,
        "primary_manager_prefilter_reason": primary_manager_prefilter_reason,
        "low_trade_condition": low_trade_condition,
        "hypothesis": hypothesis,
        "next_focus": next_focus,
    }


def build_regime_metrics_from_patterns(backtest_result: Dict[str, Any]) -> Dict[str, Any]:
    pattern_rows = backtest_result.get("pattern_summaries", [])
    regime_bucket: Dict[str, Dict[str, float]] = {}
    if isinstance(pattern_rows, list):
        for row in pattern_rows:
            if not isinstance(row, dict):
                continue
            regime = str(row.get("regime", "")).strip() or "UNKNOWN"
            trades = max(0, to_int(row.get("total_trades", 0)))
            total_profit = to_float(row.get("total_profit", 0.0))
            if regime not in regime_bucket:
                regime_bucket[regime] = {"trades": 0.0, "total_profit": 0.0}
            regime_bucket[regime]["trades"] += float(trades)
            regime_bucket[regime]["total_profit"] += float(total_profit)

    total_pattern_trades = 0.0
    for value in regime_bucket.values():
        total_pattern_trades += float(value.get("trades", 0.0))

    output: Dict[str, Any] = {"regimes": {}, "total_pattern_trades": int(total_pattern_trades)}
    for regime, value in regime_bucket.items():
        trades = max(0.0, float(value.get("trades", 0.0)))
        total_profit = float(value.get("total_profit", 0.0))
        output["regimes"][regime] = {
            "trades": int(trades),
            "total_profit_krw": round(total_profit, 4),
            "profit_per_trade_krw": round(total_profit / trades, 4) if trades > 0 else 0.0,
            "trade_share": round(trades / total_pattern_trades, 4) if total_pattern_trades > 0 else 0.0,
        }
    return output


def parse_post_entry_risk_telemetry(backtest_result: Dict[str, Any]) -> Dict[str, Any]:
    telemetry = backtest_result.get("post_entry_risk_telemetry", {})
    if not isinstance(telemetry, dict):
        telemetry = {}

    histogram_raw = telemetry.get("adaptive_partial_ratio_histogram", {})
    histogram: Dict[str, int] = {}
    if isinstance(histogram_raw, dict):
        for key in ("0.35_0.44", "0.45_0.54", "0.55_0.64", "0.65_0.74", "0.75_0.80"):
            histogram[key] = max(0, to_int(histogram_raw.get(key, 0)))

    return {
        "adaptive_stop_updates": max(0, to_int(telemetry.get("adaptive_stop_updates", 0))),
        "adaptive_tp_recalibration_updates": max(
            0,
            to_int(telemetry.get("adaptive_tp_recalibration_updates", 0)),
        ),
        "adaptive_partial_ratio_samples": max(
            0,
            to_int(telemetry.get("adaptive_partial_ratio_samples", 0)),
        ),
        "adaptive_partial_ratio_avg": max(
            0.0,
            to_float(telemetry.get("adaptive_partial_ratio_avg", 0.0)),
        ),
        "be_move_attempt_count": max(
            0,
            to_int(telemetry.get("be_move_attempt_count", 0)),
        ),
        "be_move_applied_count": max(
            0,
            to_int(telemetry.get("be_move_applied_count", 0)),
        ),
        "be_move_skipped_due_to_delay_count": max(
            0,
            to_int(telemetry.get("be_move_skipped_due_to_delay_count", 0)),
        ),
        "stop_loss_after_partial_tp_count": max(
            0,
            to_int(telemetry.get("stop_loss_after_partial_tp_count", 0)),
        ),
        "stop_loss_before_partial_tp_count": max(
            0,
            to_int(telemetry.get("stop_loss_before_partial_tp_count", 0)),
        ),
        "be_after_partial_tp_delay_sec": max(
            0,
            to_int(telemetry.get("be_after_partial_tp_delay_sec", 0)),
        ),
        "stop_loss_trigger_count": max(
            0,
            to_int(telemetry.get("stop_loss_trigger_count", 0)),
        ),
        "stop_loss_pnl_sum_krw": to_float(telemetry.get("stop_loss_pnl_sum_krw", 0.0)),
        "stop_loss_avg_pnl_krw": to_float(telemetry.get("stop_loss_avg_pnl_krw", 0.0)),
        "partial_tp_exit_count": max(
            0,
            to_int(telemetry.get("partial_tp_exit_count", 0)),
        ),
        "take_profit_full_count": max(
            0,
            to_int(telemetry.get("take_profit_full_count", 0)),
        ),
        "take_profit_full_pnl_sum_krw": to_float(
            telemetry.get("take_profit_full_pnl_sum_krw", 0.0)
        ),
        "take_profit_full_avg_pnl_krw": to_float(
            telemetry.get("take_profit_full_avg_pnl_krw", 0.0)
        ),
        "tp_hit_rate": max(
            0.0,
            to_float(telemetry.get("tp_hit_rate", 0.0)),
        ),
        "trending_trade_count": max(
            0,
            to_int(telemetry.get("trending_trade_count", 0)),
        ),
        "trending_holding_minutes_sum": max(
            0.0,
            to_float(telemetry.get("trending_holding_minutes_sum", 0.0)),
        ),
        "avg_holding_minutes_trending": max(
            0.0,
            to_float(telemetry.get("avg_holding_minutes_trending", 0.0)),
        ),
        "stop_loss_distance_samples_trending": max(
            0,
            to_int(telemetry.get("stop_loss_distance_samples_trending", 0)),
        ),
        "stop_loss_distance_sum_pct_trending": max(
            0.0,
            to_float(telemetry.get("stop_loss_distance_sum_pct_trending", 0.0)),
        ),
        "avg_stop_loss_distance_pct_trending": max(
            0.0,
            to_float(telemetry.get("avg_stop_loss_distance_pct_trending", 0.0)),
        ),
        "take_profit_distance_samples_trending": max(
            0,
            to_int(telemetry.get("take_profit_distance_samples_trending", 0)),
        ),
        "take_profit_distance_sum_pct_trending": max(
            0.0,
            to_float(telemetry.get("take_profit_distance_sum_pct_trending", 0.0)),
        ),
        "avg_take_profit_distance_pct_trending": max(
            0.0,
            to_float(telemetry.get("avg_take_profit_distance_pct_trending", 0.0)),
        ),
        "adaptive_partial_ratio_histogram": histogram,
    }


def build_adaptive_dataset_profile(
    dataset_name: str,
    backtest_result: Dict[str, Any],
    risk_adjusted_score_policy: Dict[str, Any] = None,
) -> Dict[str, Any]:
    regime_metrics = build_regime_metrics_from_patterns(backtest_result)
    post_entry_telemetry = parse_post_entry_risk_telemetry(backtest_result)
    loss_tail_decomposition = build_loss_tail_decomposition(
        pattern_rows=backtest_result.get("pattern_summaries", []),
        dataset_name=dataset_name,
        top_cell_limit=8,
    )
    regimes = regime_metrics.get("regimes", {})
    downtrend = regimes.get("TRENDING_DOWN", {})
    uptrend = regimes.get("TRENDING_UP", {})

    downtrend_trade_share = to_float(downtrend.get("trade_share", 0.0))
    downtrend_profit_per_trade = to_float(downtrend.get("profit_per_trade_krw", 0.0))
    downtrend_loss_per_trade = max(0.0, -downtrend_profit_per_trade)
    uptrend_expectancy = to_float(uptrend.get("profit_per_trade_krw", 0.0))
    total_trades = max(0, to_int(backtest_result.get("total_trades", 0)))
    profit_factor = max(0.0, to_float(backtest_result.get("profit_factor", 0.0)))
    expectancy_krw = to_float(backtest_result.get("expectancy_krw", 0.0))
    avg_fee_krw = max(0.0, to_float(backtest_result.get("avg_fee_krw", 0.0)))
    max_drawdown_pct = max(0.0, to_float(backtest_result.get("max_drawdown", 0.0)) * 100.0)

    risk_score_components = compute_risk_adjusted_score_components(
        expectancy_krw=expectancy_krw,
        profit_factor=profit_factor,
        avg_fee_krw=avg_fee_krw,
        max_drawdown_pct=max_drawdown_pct,
        downtrend_loss_per_trade_krw=downtrend_loss_per_trade,
        downtrend_trade_share=downtrend_trade_share,
        total_trades=total_trades,
        risk_adjusted_score_policy=risk_adjusted_score_policy,
    )
    score = to_float(risk_score_components.get("score", 0.0))

    generated_signals = 0
    entries_executed = 0
    entry_funnel = backtest_result.get("entry_funnel", {})
    if not isinstance(entry_funnel, dict):
        entry_funnel = {}
    shadow_funnel_raw = backtest_result.get("shadow_funnel", {})
    if not isinstance(shadow_funnel_raw, dict):
        shadow_funnel_raw = {}
    strategy_signal_funnel = backtest_result.get("strategy_signal_funnel", [])
    if isinstance(strategy_signal_funnel, list):
        for row in strategy_signal_funnel:
            if not isinstance(row, dict):
                continue
            generated_signals += max(0, to_int(row.get("generated_signals", 0)))
            entries_executed += max(0, to_int(row.get("entries_executed", 0)))
    if generated_signals <= 0:
        generated_signals = max(0, to_int(entry_funnel.get("entry_rounds", 0)))
    if entries_executed <= 0:
        entries_executed = max(0, to_int(entry_funnel.get("entries_executed", 0)))
    opportunity_conversion = (
        float(entries_executed) / float(generated_signals)
        if generated_signals > 0 else 0.0
    )
    shadow_generated_signals = max(
        generated_signals,
        max(0, to_int(shadow_funnel_raw.get("primary_generated_signals", 0))),
    )
    shadow_policy_candidates = max(
        0,
        to_int(shadow_funnel_raw.get("shadow_after_policy_filter", 0)),
    )
    primary_policy_candidates = max(
        0,
        to_int(shadow_funnel_raw.get("primary_after_policy_filter", 0)),
    )
    shadow_opportunity_conversion = (
        float(shadow_policy_candidates) / float(shadow_generated_signals)
        if shadow_generated_signals > 0
        else 0.0
    )
    primary_candidate_conversion = (
        float(primary_policy_candidates) / float(shadow_generated_signals)
        if shadow_generated_signals > 0
        else 0.0
    )

    return {
        "dataset": dataset_name,
        "total_trades": int(total_trades),
        "profit_factor": round(profit_factor, 4),
        "expectancy_krw": round(expectancy_krw, 4),
        "max_drawdown_pct": round(max_drawdown_pct, 4),
        "downtrend_trade_share": round(downtrend_trade_share, 4),
        "downtrend_loss_per_trade_krw": round(downtrend_loss_per_trade, 4),
        "uptrend_expectancy_krw": round(uptrend_expectancy, 4),
        "risk_adjusted_score": round(score, 4),
        "risk_adjusted_score_components": risk_score_components,
        "generated_signals": int(generated_signals),
        "entries_executed": int(entries_executed),
        "opportunity_conversion": round(opportunity_conversion, 4),
        "primary_candidate_conversion": round(primary_candidate_conversion, 4),
        "shadow_candidate_conversion": round(shadow_opportunity_conversion, 4),
        "shadow_candidate_supply_lift": round(
            shadow_opportunity_conversion - primary_candidate_conversion,
            4,
        ),
        "post_entry_risk_telemetry": post_entry_telemetry,
        "regime_metrics": regime_metrics,
        "loss_tail_decomposition": loss_tail_decomposition,
    }


def aggregate_adaptive_validation(dataset_profiles: List[Dict[str, Any]]) -> Dict[str, Any]:
    if not dataset_profiles:
        return {
            "dataset_count": 0,
            "avg_risk_adjusted_score": 0.0,
            "avg_downtrend_trade_share": 0.0,
            "avg_downtrend_loss_per_trade_krw": 0.0,
            "avg_uptrend_expectancy_krw": 0.0,
            "avg_primary_candidate_conversion": 0.0,
            "avg_shadow_candidate_conversion": 0.0,
            "avg_shadow_candidate_supply_lift": 0.0,
            "avg_adaptive_stop_updates": 0.0,
            "avg_adaptive_tp_recalibration_updates": 0.0,
            "avg_adaptive_partial_ratio_samples": 0.0,
            "avg_adaptive_partial_ratio": 0.0,
            "avg_be_move_attempt_count": 0.0,
            "avg_be_move_applied_count": 0.0,
            "avg_be_move_skipped_due_to_delay_count": 0.0,
            "avg_stop_loss_after_partial_tp_count": 0.0,
            "avg_stop_loss_before_partial_tp_count": 0.0,
            "be_after_partial_tp_delay_sec_mode": 0,
            "adaptive_partial_ratio_histogram": {
                "0.35_0.44": 0,
                "0.45_0.54": 0,
                "0.55_0.64": 0,
                "0.65_0.74": 0,
                "0.75_0.80": 0,
            },
            "avg_risk_adjusted_score_components": {
                "expectancy_term": 0.0,
                "profit_factor_term": 0.0,
                "ev_term": 0.0,
                "cost_term": 0.0,
                "risk_term": 0.0,
                "hostility_term": 0.0,
                "confidence_term": 0.0,
                "drawdown_penalty": 0.0,
                "downtrend_loss_penalty": 0.0,
                "downtrend_share_penalty": 0.0,
                "low_trade_penalty": 0.0,
            },
            "loss_tail_aggregate": {
                "negative_profit_abs_krw": 0.0,
                "avg_top3_loss_concentration": 0.0,
                "tail_metric_sample_size": 0,
                "tail_metric_effective_trades_count": 0.0,
                "tail_metric_ess": 0.0,
                "tail_metric_weighting": "entries_executed",
                "top_loss_regimes": [],
                "top_loss_archetypes": [],
                "top_loss_cells": [],
            },
        }

    scores = [to_float(x.get("risk_adjusted_score", 0.0)) for x in dataset_profiles]
    downtrend_share = [to_float(x.get("downtrend_trade_share", 0.0)) for x in dataset_profiles]
    downtrend_loss = [to_float(x.get("downtrend_loss_per_trade_krw", 0.0)) for x in dataset_profiles]
    uptrend_exp = [to_float(x.get("uptrend_expectancy_krw", 0.0)) for x in dataset_profiles]
    generated = [max(0.0, to_float(x.get("generated_signals", 0.0))) for x in dataset_profiles]
    executed = [max(0.0, to_float(x.get("entries_executed", 0.0))) for x in dataset_profiles]
    primary_candidate_conversion = [
        max(0.0, to_float(x.get("primary_candidate_conversion", 0.0))) for x in dataset_profiles
    ]
    shadow_candidate_conversion = [
        max(0.0, to_float(x.get("shadow_candidate_conversion", 0.0))) for x in dataset_profiles
    ]
    shadow_candidate_supply_lift = [
        to_float(x.get("shadow_candidate_supply_lift", 0.0)) for x in dataset_profiles
    ]
    stop_updates = []
    tp_recalibration_updates = []
    partial_ratio_samples = []
    partial_ratio_weighted_sum = 0.0
    be_move_attempt_counts = []
    be_move_applied_counts = []
    be_move_skipped_counts = []
    stop_loss_after_partial_tp_counts = []
    stop_loss_before_partial_tp_counts = []
    stop_loss_trigger_counts = []
    stop_loss_pnl_sum_values = []
    stop_loss_avg_pnl_values = []
    partial_tp_exit_counts = []
    take_profit_full_counts = []
    take_profit_full_pnl_sum_values = []
    take_profit_full_avg_pnl_values = []
    tp_hit_rate_values = []
    trending_holding_avg_values = []
    avg_stop_loss_distance_pct_trending_values = []
    avg_take_profit_distance_pct_trending_values = []
    be_delay_secs = []
    partial_ratio_histogram = {
        "0.35_0.44": 0,
        "0.45_0.54": 0,
        "0.55_0.64": 0,
        "0.65_0.74": 0,
        "0.75_0.80": 0,
    }
    score_expectancy_terms: List[float] = []
    score_profit_factor_terms: List[float] = []
    score_ev_terms: List[float] = []
    score_cost_terms: List[float] = []
    score_risk_terms: List[float] = []
    score_hostility_terms: List[float] = []
    score_confidence_terms: List[float] = []
    score_drawdown_penalties: List[float] = []
    score_downtrend_loss_penalties: List[float] = []
    score_downtrend_share_penalties: List[float] = []
    score_low_trade_penalties: List[float] = []
    top3_concentrations: List[float] = []
    tail_metric_weights: List[float] = []
    aggregate_negative_profit_abs = 0.0
    aggregate_regime_loss_abs: Dict[str, float] = {}
    aggregate_archetype_loss_abs: Dict[str, float] = {}
    aggregate_loss_cells: List[Dict[str, Any]] = []

    for profile in dataset_profiles:
        telemetry = profile.get("post_entry_risk_telemetry", {})
        if not isinstance(telemetry, dict):
            telemetry = {}
        stop_update_count = max(0.0, to_float(telemetry.get("adaptive_stop_updates", 0.0)))
        tp_recalibration_count = max(
            0.0,
            to_float(telemetry.get("adaptive_tp_recalibration_updates", 0.0)),
        )
        ratio_samples = max(0.0, to_float(telemetry.get("adaptive_partial_ratio_samples", 0.0)))
        ratio_avg = max(0.0, to_float(telemetry.get("adaptive_partial_ratio_avg", 0.0)))
        stop_updates.append(stop_update_count)
        tp_recalibration_updates.append(tp_recalibration_count)
        partial_ratio_samples.append(ratio_samples)
        partial_ratio_weighted_sum += ratio_avg * ratio_samples
        be_move_attempt_counts.append(max(0.0, to_float(telemetry.get("be_move_attempt_count", 0.0))))
        be_move_applied_counts.append(max(0.0, to_float(telemetry.get("be_move_applied_count", 0.0))))
        be_move_skipped_counts.append(
            max(0.0, to_float(telemetry.get("be_move_skipped_due_to_delay_count", 0.0)))
        )
        stop_loss_after_partial_tp_counts.append(
            max(0.0, to_float(telemetry.get("stop_loss_after_partial_tp_count", 0.0)))
        )
        stop_loss_before_partial_tp_counts.append(
            max(0.0, to_float(telemetry.get("stop_loss_before_partial_tp_count", 0.0)))
        )
        stop_loss_trigger_counts.append(
            max(0.0, to_float(telemetry.get("stop_loss_trigger_count", 0.0)))
        )
        stop_loss_pnl_sum_values.append(
            to_float(telemetry.get("stop_loss_pnl_sum_krw", 0.0))
        )
        stop_loss_avg_pnl_values.append(
            to_float(telemetry.get("stop_loss_avg_pnl_krw", 0.0))
        )
        partial_tp_exit_counts.append(
            max(0.0, to_float(telemetry.get("partial_tp_exit_count", 0.0)))
        )
        take_profit_full_counts.append(
            max(0.0, to_float(telemetry.get("take_profit_full_count", 0.0)))
        )
        take_profit_full_pnl_sum_values.append(
            to_float(telemetry.get("take_profit_full_pnl_sum_krw", 0.0))
        )
        take_profit_full_avg_pnl_values.append(
            to_float(telemetry.get("take_profit_full_avg_pnl_krw", 0.0))
        )
        tp_hit_rate_values.append(
            max(0.0, to_float(telemetry.get("tp_hit_rate", 0.0)))
        )
        trending_holding_avg_values.append(
            max(0.0, to_float(telemetry.get("avg_holding_minutes_trending", 0.0)))
        )
        avg_stop_loss_distance_pct_trending_values.append(
            max(0.0, to_float(telemetry.get("avg_stop_loss_distance_pct_trending", 0.0)))
        )
        avg_take_profit_distance_pct_trending_values.append(
            max(0.0, to_float(telemetry.get("avg_take_profit_distance_pct_trending", 0.0)))
        )
        be_delay_secs.append(max(0, to_int(telemetry.get("be_after_partial_tp_delay_sec", 0))))

        hist = telemetry.get("adaptive_partial_ratio_histogram", {})
        if isinstance(hist, dict):
            for key in partial_ratio_histogram:
                partial_ratio_histogram[key] += max(0, to_int(hist.get(key, 0)))

        components = profile.get("risk_adjusted_score_components", {})
        if isinstance(components, dict):
            score_expectancy_terms.append(to_float(components.get("expectancy_term", 0.0)))
            score_profit_factor_terms.append(to_float(components.get("profit_factor_term", 0.0)))
            score_ev_terms.append(to_float(components.get("ev_term", 0.0)))
            score_cost_terms.append(to_float(components.get("cost_term", 0.0)))
            score_risk_terms.append(to_float(components.get("risk_term", 0.0)))
            score_hostility_terms.append(to_float(components.get("hostility_term", 0.0)))
            score_confidence_terms.append(to_float(components.get("confidence_term", 0.0)))
            score_drawdown_penalties.append(to_float(components.get("drawdown_penalty", 0.0)))
            score_downtrend_loss_penalties.append(to_float(components.get("downtrend_loss_penalty", 0.0)))
            score_downtrend_share_penalties.append(to_float(components.get("downtrend_share_penalty", 0.0)))
            score_low_trade_penalties.append(to_float(components.get("low_trade_penalty", 0.0)))

        tail = profile.get("loss_tail_decomposition", {})
        if isinstance(tail, dict):
            aggregate_negative_profit_abs += max(0.0, to_float(tail.get("negative_profit_abs_krw", 0.0)))
            top3_concentration = max(0.0, to_float(tail.get("top3_loss_concentration", 0.0)))
            top3_concentrations.append(top3_concentration)
            tail_weight = max(0.0, to_float(profile.get("entries_executed", 0.0)))
            if tail_weight > 0.0:
                tail_metric_weights.append(tail_weight)
            regime_map = tail.get("loss_by_regime_abs_krw", {})
            if isinstance(regime_map, dict):
                for k, v in regime_map.items():
                    key = str(k).strip() or "UNKNOWN"
                    aggregate_regime_loss_abs[key] = (
                        aggregate_regime_loss_abs.get(key, 0.0) + max(0.0, to_float(v))
                    )
            archetype_map = tail.get("loss_by_archetype_abs_krw", {})
            if isinstance(archetype_map, dict):
                for k, v in archetype_map.items():
                    key = str(k).strip() or "UNSPECIFIED"
                    aggregate_archetype_loss_abs[key] = (
                        aggregate_archetype_loss_abs.get(key, 0.0) + max(0.0, to_float(v))
                    )
            cells = tail.get("top_loss_cells", [])
            if isinstance(cells, list):
                for item in cells:
                    if not isinstance(item, dict):
                        continue
                    aggregate_loss_cells.append(
                        {
                            "dataset": str(item.get("dataset", profile.get("dataset", ""))).strip(),
                            "pattern": str(item.get("pattern", "")).strip(),
                            "regime": str(item.get("regime", "")).strip(),
                            "entry_archetype": str(item.get("entry_archetype", "")).strip(),
                            "trades": max(0, to_int(item.get("trades", 0))),
                            "total_profit_krw": round(to_float(item.get("total_profit_krw", 0.0)), 4),
                            "loss_abs_krw": round(max(0.0, to_float(item.get("loss_abs_krw", 0.0))), 4),
                        }
                    )

    total_generated = sum(generated)
    total_executed = sum(executed)
    total_partial_ratio_samples = sum(partial_ratio_samples)
    avg_opportunity_conversion = (
        (total_executed / total_generated) if total_generated > 0.0 else 0.0
    )
    avg_adaptive_partial_ratio = (
        partial_ratio_weighted_sum / total_partial_ratio_samples
        if total_partial_ratio_samples > 0.0
        else 0.0
    )

    def _top_loss_rows(loss_map: Dict[str, float], key_name: str, limit: int) -> List[Dict[str, Any]]:
        rows = []
        for key, loss_abs in loss_map.items():
            share = (float(loss_abs) / aggregate_negative_profit_abs) if aggregate_negative_profit_abs > 0.0 else 0.0
            rows.append(
                {
                    key_name: str(key),
                    "loss_abs_krw": round(float(loss_abs), 4),
                    "loss_share": round(float(share), 4),
                }
            )
        rows.sort(key=lambda x: (-to_float(x["loss_abs_krw"]), str(x[key_name])))
        return rows[: max(1, int(limit))]

    aggregate_loss_cells_sorted = sorted(
        aggregate_loss_cells,
        key=lambda x: (to_float(x["total_profit_krw"]), -to_int(x["trades"]), str(x["pattern"])),
    )
    for item in aggregate_loss_cells_sorted:
        loss_abs = max(0.0, to_float(item.get("loss_abs_krw", 0.0)))
        item["loss_share"] = round((loss_abs / aggregate_negative_profit_abs), 4) if aggregate_negative_profit_abs > 0.0 else 0.0

    tail_metric_sample_size = len(top3_concentrations)
    tail_metric_effective_trades_count = sum(tail_metric_weights)
    tail_metric_ess = 0.0
    if tail_metric_weights:
        weight_sq_sum = sum((w * w) for w in tail_metric_weights)
        if weight_sq_sum > 0.0:
            tail_metric_ess = (tail_metric_effective_trades_count ** 2) / weight_sq_sum
    if tail_metric_ess <= 0.0:
        tail_metric_ess = float(tail_metric_sample_size)

    be_delay_mode = 0
    if be_delay_secs:
        freq: Dict[int, int] = {}
        for value in be_delay_secs:
            key = max(0, int(value))
            freq[key] = freq.get(key, 0) + 1
        be_delay_mode = sorted(freq.items(), key=lambda item: (-item[1], item[0]))[0][0]

    return {
        "dataset_count": len(dataset_profiles),
        "avg_risk_adjusted_score": round(safe_avg(scores), 4),
        "avg_downtrend_trade_share": round(safe_avg(downtrend_share), 4),
        "avg_downtrend_loss_per_trade_krw": round(safe_avg(downtrend_loss), 4),
        "avg_uptrend_expectancy_krw": round(safe_avg(uptrend_exp), 4),
        "avg_generated_signals": round(safe_avg(generated), 4),
        "avg_entries_executed": round(safe_avg(executed), 4),
        "avg_opportunity_conversion": round(avg_opportunity_conversion, 4),
        "avg_primary_candidate_conversion": round(
            safe_avg(primary_candidate_conversion),
            4,
        ),
        "avg_shadow_candidate_conversion": round(
            safe_avg(shadow_candidate_conversion),
            4,
        ),
        "avg_shadow_candidate_supply_lift": round(
            safe_avg(shadow_candidate_supply_lift),
            4,
        ),
        "avg_adaptive_stop_updates": round(safe_avg(stop_updates), 4),
        "avg_adaptive_tp_recalibration_updates": round(safe_avg(tp_recalibration_updates), 4),
        "avg_adaptive_partial_ratio_samples": round(safe_avg(partial_ratio_samples), 4),
        "avg_adaptive_partial_ratio": round(avg_adaptive_partial_ratio, 4),
        "avg_be_move_attempt_count": round(safe_avg(be_move_attempt_counts), 4),
        "avg_be_move_applied_count": round(safe_avg(be_move_applied_counts), 4),
        "avg_be_move_skipped_due_to_delay_count": round(safe_avg(be_move_skipped_counts), 4),
        "avg_stop_loss_after_partial_tp_count": round(
            safe_avg(stop_loss_after_partial_tp_counts), 4
        ),
        "avg_stop_loss_before_partial_tp_count": round(
            safe_avg(stop_loss_before_partial_tp_counts), 4
        ),
        "avg_stop_loss_trigger_count": round(safe_avg(stop_loss_trigger_counts), 4),
        "avg_stop_loss_pnl_sum_krw": round(safe_avg(stop_loss_pnl_sum_values), 4),
        "avg_stop_loss_avg_pnl_krw": round(safe_avg(stop_loss_avg_pnl_values), 4),
        "avg_partial_tp_exit_count": round(safe_avg(partial_tp_exit_counts), 4),
        "avg_take_profit_full_count": round(safe_avg(take_profit_full_counts), 4),
        "avg_take_profit_full_pnl_sum_krw": round(safe_avg(take_profit_full_pnl_sum_values), 4),
        "avg_take_profit_full_avg_pnl_krw": round(safe_avg(take_profit_full_avg_pnl_values), 4),
        "avg_tp_hit_rate": round(safe_avg(tp_hit_rate_values), 6),
        "avg_holding_minutes_trending": round(safe_avg(trending_holding_avg_values), 4),
        "avg_stop_loss_distance_pct_trending": round(
            safe_avg(avg_stop_loss_distance_pct_trending_values),
            8,
        ),
        "avg_take_profit_distance_pct_trending": round(
            safe_avg(avg_take_profit_distance_pct_trending_values),
            8,
        ),
        "be_after_partial_tp_delay_sec_mode": int(be_delay_mode),
        "adaptive_partial_ratio_histogram": partial_ratio_histogram,
        "avg_risk_adjusted_score_components": {
            "expectancy_term": round(safe_avg(score_expectancy_terms), 4),
            "profit_factor_term": round(safe_avg(score_profit_factor_terms), 4),
            "ev_term": round(safe_avg(score_ev_terms), 4),
            "cost_term": round(safe_avg(score_cost_terms), 4),
            "risk_term": round(safe_avg(score_risk_terms), 4),
            "hostility_term": round(safe_avg(score_hostility_terms), 4),
            "confidence_term": round(safe_avg(score_confidence_terms), 4),
            "drawdown_penalty": round(safe_avg(score_drawdown_penalties), 4),
            "downtrend_loss_penalty": round(safe_avg(score_downtrend_loss_penalties), 4),
            "downtrend_share_penalty": round(safe_avg(score_downtrend_share_penalties), 4),
            "low_trade_penalty": round(safe_avg(score_low_trade_penalties), 4),
        },
        "loss_tail_aggregate": {
            "negative_profit_abs_krw": round(aggregate_negative_profit_abs, 4),
            "avg_top3_loss_concentration": round(safe_avg(top3_concentrations), 4),
            "tail_metric_sample_size": int(tail_metric_sample_size),
            "tail_metric_effective_trades_count": round(
                float(tail_metric_effective_trades_count),
                4,
            ),
            "tail_metric_ess": round(float(tail_metric_ess), 4),
            "tail_metric_weighting": "entries_executed",
            "top_loss_regimes": _top_loss_rows(aggregate_regime_loss_abs, "regime", limit=6)
            if aggregate_regime_loss_abs else [],
            "top_loss_archetypes": _top_loss_rows(aggregate_archetype_loss_abs, "entry_archetype", limit=6)
            if aggregate_archetype_loss_abs else [],
            "top_loss_cells": aggregate_loss_cells_sorted[:8],
        },
    }


def build_adaptive_verdict(
    adaptive_aggregates: Dict[str, Any],
    max_downtrend_loss_per_trade_krw: float,
    max_downtrend_trade_share: float,
    min_uptrend_expectancy_krw: float,
    min_risk_adjusted_score: float,
    avg_total_trades: float,
    min_avg_trades: float,
    peak_max_drawdown_pct: float,
    max_drawdown_pct_limit: float,
    risk_adjusted_score_guard_policy: Dict[str, Any] = None,
    risk_adjusted_score_guard_context: Dict[str, Any] = None,
    risk_adjusted_score_guard_history: Dict[str, Any] = None,
) -> Dict[str, Any]:
    avg_downtrend_trade_share = to_float(adaptive_aggregates.get("avg_downtrend_trade_share", 0.0))
    avg_generated_signals = to_float(adaptive_aggregates.get("avg_generated_signals", 0.0))
    avg_opportunity_conversion = to_float(adaptive_aggregates.get("avg_opportunity_conversion", 0.0))
    avg_primary_candidate_conversion = to_float(
        adaptive_aggregates.get("avg_primary_candidate_conversion", 0.0)
    )
    avg_shadow_candidate_conversion = to_float(
        adaptive_aggregates.get("avg_shadow_candidate_conversion", 0.0)
    )
    avg_shadow_candidate_supply_lift = to_float(
        adaptive_aggregates.get("avg_shadow_candidate_supply_lift", 0.0)
    )

    hostile_sample_relax_factor = 1.0
    if avg_downtrend_trade_share >= 0.45:
        hostile_sample_relax_factor = 0.60
    elif avg_downtrend_trade_share >= 0.30:
        hostile_sample_relax_factor = 0.75
    elif avg_downtrend_trade_share >= 0.15:
        hostile_sample_relax_factor = 0.90
    effective_min_avg_trades = max(3.0, float(min_avg_trades) * hostile_sample_relax_factor)

    min_opportunity_conversion = 0.025
    if avg_downtrend_trade_share >= 0.45:
        min_opportunity_conversion = 0.012
    elif avg_downtrend_trade_share >= 0.30:
        min_opportunity_conversion = 0.018
    low_opportunity_observed = avg_generated_signals < 5.0

    loss_tail = adaptive_aggregates.get("loss_tail_aggregate", {})
    if not isinstance(loss_tail, dict):
        loss_tail = {}
    observed_tail_ess = max(0.0, to_float(loss_tail.get("tail_metric_ess", 0.0)))
    guard_context = (
        risk_adjusted_score_guard_context
        if isinstance(risk_adjusted_score_guard_context, dict)
        else {}
    )
    guard_history = (
        risk_adjusted_score_guard_history
        if isinstance(risk_adjusted_score_guard_history, dict)
        else {}
    )
    risk_guard = evaluate_risk_adjusted_score_guard(
        avg_score=to_float(adaptive_aggregates.get("avg_risk_adjusted_score", 0.0)),
        min_risk_adjusted_score=float(min_risk_adjusted_score),
        policy=(
            risk_adjusted_score_guard_policy
            if isinstance(risk_adjusted_score_guard_policy, dict)
            else {}
        ),
        context=guard_context,
        observed_ess=observed_tail_ess,
        history_scores=(
            guard_history.get("scores", [])
            if isinstance(guard_history.get("scores", []), list)
            else []
        ),
        history_ess_values=(
            guard_history.get("ess_values", [])
            if isinstance(guard_history.get("ess_values", []), list)
            else []
        ),
    )

    checks = {
        "sample_size_guard_pass": float(avg_total_trades) >= float(effective_min_avg_trades),
        "drawdown_guard_pass": float(peak_max_drawdown_pct) <= float(max_drawdown_pct_limit),
        "downtrend_loss_guard_pass": to_float(adaptive_aggregates.get("avg_downtrend_loss_per_trade_krw", 0.0))
        <= float(max_downtrend_loss_per_trade_krw),
        "downtrend_trade_share_guard_pass": to_float(adaptive_aggregates.get("avg_downtrend_trade_share", 0.0))
        <= float(max_downtrend_trade_share),
        "uptrend_expectancy_guard_pass": to_float(adaptive_aggregates.get("avg_uptrend_expectancy_krw", 0.0))
        >= float(min_uptrend_expectancy_krw),
        "risk_adjusted_score_guard_pass": bool(risk_guard.get("guard_pass", False)),
        "risk_adjusted_score_guard_hard_fail": bool(risk_guard.get("hard_fail", False)),
        "risk_adjusted_score_guard_soft_fail": bool(risk_guard.get("soft_fail", False)),
        "risk_adjusted_score_guard_promotion_hold": bool(risk_guard.get("promotion_hold", False)),
        "opportunity_conversion_guard_pass": (
            True if low_opportunity_observed
            else (avg_opportunity_conversion >= float(min_opportunity_conversion))
        ),
    }
    failed = [k for k, v in checks.items() if not bool(v)]
    hard_fail_keys = [
        "drawdown_guard_pass",
        "downtrend_loss_guard_pass",
        "downtrend_trade_share_guard_pass",
        "uptrend_expectancy_guard_pass",
    ]
    hard_fail = any(not bool(checks.get(key, True)) for key in hard_fail_keys) or bool(
        checks.get("risk_adjusted_score_guard_hard_fail", False)
    )

    if hard_fail:
        verdict = "fail"
    elif bool(checks.get("risk_adjusted_score_guard_promotion_hold", False)):
        verdict = "inconclusive"
    elif bool(checks.get("risk_adjusted_score_guard_soft_fail", False)):
        verdict = "inconclusive"
    elif (
        checks["sample_size_guard_pass"] and
        checks["opportunity_conversion_guard_pass"]
    ):
        verdict = "pass"
    else:
        verdict = "inconclusive"
    return {
        "verdict": verdict,
        "checks": checks,
        "failed_checks": failed,
        "effective_thresholds": {
            "effective_min_avg_trades": round(effective_min_avg_trades, 4),
            "hostile_sample_relax_factor": round(hostile_sample_relax_factor, 4),
            "min_opportunity_conversion": round(min_opportunity_conversion, 4),
            "risk_adjusted_score_guard": risk_guard,
        },
        "context": {
            "avg_generated_signals": round(avg_generated_signals, 4),
            "avg_opportunity_conversion": round(avg_opportunity_conversion, 4),
            "avg_primary_candidate_conversion": round(avg_primary_candidate_conversion, 4),
            "avg_shadow_candidate_conversion": round(avg_shadow_candidate_conversion, 4),
            "avg_shadow_candidate_supply_lift": round(avg_shadow_candidate_supply_lift, 4),
            "low_opportunity_observed": bool(low_opportunity_observed),
            "risk_adjusted_score_guard_context": guard_context,
            "risk_adjusted_score_guard_history_sources": (
                guard_history.get("sources", [])
                if isinstance(guard_history.get("sources", []), list)
                else []
            ),
        },
    }


def build_risk_adjusted_failure_decomposition(
    *,
    adaptive_aggregates: Dict[str, Any],
    adaptive_verdict: Dict[str, Any],
    min_risk_adjusted_score: float,
) -> Dict[str, Any]:
    checks = adaptive_verdict.get("checks", {})
    if not isinstance(checks, dict):
        checks = {}
    risk_guard_pass = bool(checks.get("risk_adjusted_score_guard_pass", False))
    risk_guard_hard_fail = bool(checks.get("risk_adjusted_score_guard_hard_fail", False))
    risk_guard_soft_fail = bool(checks.get("risk_adjusted_score_guard_soft_fail", False))
    risk_guard_promotion_hold = bool(checks.get("risk_adjusted_score_guard_promotion_hold", False))
    avg_score = to_float(adaptive_aggregates.get("avg_risk_adjusted_score", 0.0))
    risk_guard_detail = nested_get(
        adaptive_verdict,
        ["effective_thresholds", "risk_adjusted_score_guard"],
        {},
    )
    if not isinstance(risk_guard_detail, dict):
        risk_guard_detail = {}
    dynamic_enabled = bool(risk_guard_detail.get("enabled", False))
    dynamic_final_limit = to_float(
        risk_guard_detail.get("final_limit", min_risk_adjusted_score)
    )
    score_gap = float(dynamic_final_limit) - float(avg_score)
    components = adaptive_aggregates.get("avg_risk_adjusted_score_components", {})
    if not isinstance(components, dict):
        components = {}

    expectancy_term = to_float(components.get("expectancy_term", 0.0))
    profit_factor_term = to_float(components.get("profit_factor_term", 0.0))
    drawdown_penalty = to_float(components.get("drawdown_penalty", 0.0))
    downtrend_loss_penalty = to_float(components.get("downtrend_loss_penalty", 0.0))
    downtrend_share_penalty = to_float(components.get("downtrend_share_penalty", 0.0))
    low_trade_penalty = to_float(components.get("low_trade_penalty", 0.0))
    ev_term = to_float(components.get("ev_term", expectancy_term + profit_factor_term))
    cost_term = to_float(components.get("cost_term", 0.0))
    risk_term = to_float(components.get("risk_term", drawdown_penalty))
    hostility_term = to_float(
        components.get(
            "hostility_term",
            downtrend_loss_penalty + downtrend_share_penalty,
        )
    )
    confidence_term = to_float(components.get("confidence_term", low_trade_penalty))
    reconstructed_score = (
        ev_term
        - cost_term
        - risk_term
        - hostility_term
        - confidence_term
    )

    penalty_rows = [
        {"name": "cost_term", "value": round(cost_term, 4)},
        {"name": "risk_term", "value": round(risk_term, 4)},
        {"name": "hostility_term", "value": round(hostility_term, 4)},
        {"name": "confidence_term", "value": round(confidence_term, 4)},
        {"name": "drawdown_penalty", "value": round(drawdown_penalty, 4)},
        {"name": "downtrend_loss_penalty", "value": round(downtrend_loss_penalty, 4)},
        {"name": "downtrend_share_penalty", "value": round(downtrend_share_penalty, 4)},
        {"name": "low_trade_penalty", "value": round(low_trade_penalty, 4)},
    ]
    penalty_rows = [x for x in penalty_rows if to_float(x.get("value", 0.0)) > 0.0]
    penalty_rows.sort(key=lambda x: (-to_float(x["value"]), str(x["name"])))

    positive_rows = [
        {"name": "ev_term", "value": round(ev_term, 4)},
        {"name": "expectancy_term", "value": round(expectancy_term, 4)},
        {"name": "profit_factor_term", "value": round(profit_factor_term, 4)},
    ]
    positive_rows.sort(key=lambda x: (-to_float(x["value"]), str(x["name"])))

    dominant_fail_term = ""
    dominant_fail_magnitude = 0.0
    dominant_candidates = [
        ("ev_term", max(0.0, -ev_term)),
        ("cost_term", max(0.0, cost_term)),
        ("risk_term", max(0.0, risk_term)),
        ("hostility_term", max(0.0, hostility_term)),
        ("confidence_term", max(0.0, confidence_term)),
    ]
    dominant_candidates.sort(key=lambda x: (-float(x[1]), str(x[0])))
    if dominant_candidates:
        dominant_fail_term = str(dominant_candidates[0][0])
        dominant_fail_magnitude = float(dominant_candidates[0][1])

    tail = adaptive_aggregates.get("loss_tail_aggregate", {})
    if not isinstance(tail, dict):
        tail = {}
    top_loss_regimes = tail.get("top_loss_regimes", [])
    if not isinstance(top_loss_regimes, list):
        top_loss_regimes = []
    top_loss_archetypes = tail.get("top_loss_archetypes", [])
    if not isinstance(top_loss_archetypes, list):
        top_loss_archetypes = []
    top_loss_cells = tail.get("top_loss_cells", [])
    if not isinstance(top_loss_cells, list):
        top_loss_cells = []

    recommended_focus: List[str] = []
    if not risk_guard_pass:
        recommended_focus.append(
            "Prioritize heavy-loss tail reduction by regime/archetype before broad threshold relaxation."
        )
        if top_loss_regimes:
            first = top_loss_regimes[0]
            recommended_focus.append(
                "Primary loss regime: "
                f"{str(first.get('regime', '')).strip() or 'UNKNOWN'} "
                f"(share={to_float(first.get('loss_share', 0.0)):.4f})"
            )
        if top_loss_archetypes:
            first = top_loss_archetypes[0]
            recommended_focus.append(
                "Primary loss archetype: "
                f"{str(first.get('entry_archetype', '')).strip() or 'UNSPECIFIED'} "
                f"(share={to_float(first.get('loss_share', 0.0)):.4f})"
            )
        if penalty_rows:
            recommended_focus.append(
                "Largest score penalty term: "
                f"{penalty_rows[0]['name']}={to_float(penalty_rows[0]['value']):.4f}"
            )

    return {
        "active": bool(risk_guard_hard_fail),
        "risk_adjusted_score_guard_pass": bool(risk_guard_pass),
        "risk_adjusted_score_guard_hard_fail": bool(risk_guard_hard_fail),
        "risk_adjusted_score_guard_soft_fail": bool(risk_guard_soft_fail),
        "risk_adjusted_score_guard_promotion_hold": bool(risk_guard_promotion_hold),
        "risk_adjusted_score_guard_status": str(
            risk_guard_detail.get(
                "status",
                (
                    "hard_fail"
                    if risk_guard_hard_fail
                    else ("promotion_hold" if risk_guard_promotion_hold else ("soft_fail" if risk_guard_soft_fail else "pass"))
                ),
            )
        ).strip(),
        "risk_adjusted_score_limit_mode": str(
            risk_guard_detail.get("limit_mode", "absolute_threshold_static")
        ).strip(),
        "min_risk_adjusted_score": round(float(min_risk_adjusted_score), 4),
        "dynamic_final_limit": round(float(dynamic_final_limit), 4),
        "dynamic_lookup_limit": round(to_float(risk_guard_detail.get("lookup_limit", 0.0)), 4),
        "dynamic_dist_limit": round(to_float(risk_guard_detail.get("dist_limit", 0.0)), 4),
        "dynamic_uncertainty_component": (
            risk_guard_detail.get("uncertainty_component", {})
            if isinstance(risk_guard_detail.get("uncertainty_component", {}), dict)
            else {}
        ),
        "fallback_core_zero_mode": bool(risk_guard_detail.get("fallback_core_zero_mode", False)),
        "avg_risk_adjusted_score": round(float(avg_score), 4),
        "score_gap_to_threshold": round(float(score_gap), 4),
        "score_components": {
            "expectancy_term": round(expectancy_term, 4),
            "profit_factor_term": round(profit_factor_term, 4),
            "ev_term": round(ev_term, 4),
            "cost_term": round(cost_term, 4),
            "risk_term": round(risk_term, 4),
            "hostility_term": round(hostility_term, 4),
            "confidence_term": round(confidence_term, 4),
            "drawdown_penalty": round(drawdown_penalty, 4),
            "downtrend_loss_penalty": round(downtrend_loss_penalty, 4),
            "downtrend_share_penalty": round(downtrend_share_penalty, 4),
            "low_trade_penalty": round(low_trade_penalty, 4),
            "reconstructed_score": round(reconstructed_score, 4),
        },
        "dominant_fail_term": dominant_fail_term,
        "dominant_fail_magnitude": round(dominant_fail_magnitude, 4),
        "dominant_penalties": penalty_rows[:4],
        "positive_terms": positive_rows,
        "loss_tail": {
            "negative_profit_abs_krw": round(to_float(tail.get("negative_profit_abs_krw", 0.0)), 4),
            "avg_top3_loss_concentration": round(to_float(tail.get("avg_top3_loss_concentration", 0.0)), 4),
            "top_loss_regimes": top_loss_regimes[:5],
            "top_loss_archetypes": top_loss_archetypes[:5],
            "top_loss_cells": top_loss_cells[:5],
        },
        "recommended_focus": recommended_focus,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Minimal sequential verification pipeline (reset baseline)."
    )
    parser.add_argument("--exe-path", default=r".\build\AutoLifeTrading.exe")
    parser.add_argument("--config-path", default=r".\build\Release\config\config.json")
    parser.add_argument(
        "--run-dir",
        default="",
        help="Optional runtime output root passed to AutoLifeTrading.exe (--run-dir).",
    )
    parser.add_argument("--source-config-path", default=r".\config\config.json")
    parser.add_argument("--data-dir", default=r".\data\backtest_real")
    parser.add_argument("--dataset-names", nargs="*", default=[])
    parser.add_argument(
        "--data-mode",
        choices=["fixed", "refresh_if_missing", "refresh_force"],
        default="fixed",
    )
    parser.add_argument("--output-csv", default=r".\build\Release\logs\verification_matrix.csv")
    parser.add_argument("--output-json", default=r".\build\Release\logs\verification_report.json")
    parser.add_argument(
        "--baseline-report-path",
        default=r".\build\Release\logs\verification_report_baseline_current.json",
    )
    parser.add_argument(
        "--validation-profile",
        choices=["adaptive"],
        default="adaptive",
        help="adaptive: regime-aware dynamic validation (primary mode)",
    )
    parser.add_argument("--min-profit-factor", type=float, default=1.00)
    parser.add_argument("--min-expectancy-krw", type=float, default=0.0)
    parser.add_argument("--max-drawdown-pct", type=float, default=12.0)
    parser.add_argument("--min-profitable-ratio", type=float, default=0.55)
    parser.add_argument("--min-avg-win-rate-pct", type=float, default=0.0)
    parser.add_argument("--min-avg-trades", type=float, default=10.0)
    parser.add_argument("--max-downtrend-loss-per-trade-krw", type=float, default=12.0)
    parser.add_argument("--max-downtrend-trade-share", type=float, default=0.55)
    parser.add_argument("--min-uptrend-expectancy-krw", type=float, default=0.0)
    parser.add_argument("--min-risk-adjusted-score", type=float, default=-0.10)
    parser.add_argument("--require-higher-tf-companions", action="store_true")
    parser.add_argument("--enable-adaptive-state-io", action="store_true")
    parser.add_argument("--verification-lock-path", default=r".\build\Release\logs\verification_run.lock")
    parser.add_argument("--verification-lock-timeout-sec", type=int, default=1800)
    parser.add_argument("--verification-lock-stale-sec", type=int, default=14400)
    parser.add_argument("--skip-probabilistic-coverage-check", action="store_true")
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("auto", "v2"),
        default="auto",
        help="Gate profile selector. auto infers from runtime bundle when available (v2 only).",
    )
    parser.add_argument(
        "--phase4-off-on-compare",
        action="store_true",
        help=(
            "Run additional OFF/ON comparison by forcing phase4 enabled flags in a temporary runtime bundle. "
            "Comparison output is embedded into verification_report.json."
        ),
    )
    parser.add_argument(
        "--split-manifest-json",
        default="",
        help="Optional time-split/purge manifest JSON path to attach protocol evidence into report.",
    )
    parser.add_argument(
        "--split-name",
        default="",
        help="Optional split label (e.g., development/validation/quarantine/all_purged).",
    )
    parser.add_argument(
        "--split-time-based",
        action="store_true",
        help="Mark report as time-split protocol run (for explicit protocol evidence).",
    )
    parser.add_argument(
        "--split-purge-gap-minutes",
        type=int,
        default=-1,
        help="Optional purge gap minutes applied to split boundaries.",
    )
    parser.add_argument(
        "--split-note",
        default="",
        help="Optional operator note attached to split protocol evidence.",
    )
    parser.add_argument(
        "--split-execution-prewarm-hours",
        type=float,
        default=168.0,
        help=(
            "When split manifest is used with core ranges, extend execution input "
            "backwards by this many hours (prewarm) while keeping evaluation on core."
        ),
    )
    parser.add_argument(
        "--disable-split-core-zero-fallback-cumulative",
        action="store_true",
        help=(
            "Disable automatic evaluation fallback to cumulative range when core evaluation "
            "has zero proxy coverage (candidate/trade/entry rounds all zero)."
        ),
    )
    parser.add_argument(
        "--core-zero-diagnosis-json",
        default=r".\build\Release\logs\core_zero_diagnosis.json",
        help="Output JSON path for split core-zero diagnosis and fallback decision.",
    )
    parser.add_argument(
        "--disable-core-window-direct-eval",
        action="store_true",
        help=(
            "Disable core-window direct evaluation even when split manifest provides "
            "development_core/validation_core/quarantine_core datasets."
        ),
    )
    parser.add_argument(
        "--vol-bucket-pct-window-size",
        type=int,
        default=240,
        help="Rolling window size for percentile-based volatility buckets (verification-only).",
    )
    parser.add_argument(
        "--vol-bucket-pct-min-points",
        type=int,
        default=0,
        help="Minimum rolling sample size for percentile bucket assignment; 0 means ceil(window_size*0.7).",
    )
    parser.add_argument(
        "--vol-bucket-pct-lower-q",
        type=float,
        default=0.33,
        help="Lower quantile for rolling percentile volatility bucket (LOW <= q).",
    )
    parser.add_argument(
        "--vol-bucket-pct-upper-q",
        type=float,
        default=0.67,
        help="Upper quantile for rolling percentile volatility bucket (MID <= q).",
    )
    parser.add_argument(
        "--vol-bucket-fixed-low-cut",
        type=float,
        default=1.8,
        help="Fixed volatility LOW cutoff (atr_pct).",
    )
    parser.add_argument(
        "--vol-bucket-fixed-high-cut",
        type=float,
        default=3.5,
        help="Fixed volatility MID/HIGH cutoff (atr_pct).",
    )
    parser.add_argument(
        "--quarantine-vol-bucket-pct-distribution-json",
        default=r".\build\Release\logs\quarantine_vol_bucket_pct_distribution.json",
        help="Output JSON path for percentile bucket distribution diagnostics.",
    )
    parser.add_argument(
        "--quarantine-pnl-by-regime-x-vol-fixed-json",
        default=r".\build\Release\logs\quarantine_pnl_by_regime_x_vol_fixed.json",
        help="Output JSON path for (regime x fixed vol bucket) PnL cross table.",
    )
    parser.add_argument(
        "--quarantine-pnl-by-regime-x-vol-pct-json",
        default=r".\build\Release\logs\quarantine_pnl_by_regime_x_vol_pct.json",
        help="Output JSON path for (regime x percentile vol bucket) PnL cross table.",
    )
    parser.add_argument(
        "--top-loss-cells-regime-vol-pct-json",
        default=r".\build\Release\logs\top_loss_cells_regime_vol_pct.json",
        help="Output JSON path for top-loss market x regime x percentile-vol-bucket cells.",
    )
    parser.add_argument(
        "--a8-quantile-exec-coverage-debug-json",
        default=r".\build\Release\logs\a8_0_quantile_exec_coverage_debug.json",
        help=(
            "Output JSON path for A8-0 execution-quantile/evaluation-coverage debug "
            "(exec points, valid quantile points, mapping coverage, none ratio)."
        ),
    )
    parser.add_argument(
        "--cell-root-cause-report-json",
        default=r".\build\Release\logs\cell_root_cause_report_quarantine.json",
        help="Output JSON path for A9-0 cell root cause report (regime x vol_bucket_pct x market).",
    )
    parser.add_argument(
        "--cell-root-cause-top-n",
        type=int,
        default=3,
        help="Top-N worst-loss cells to include in cell root cause report.",
    )
    parser.add_argument(
        "--edge-sign-distribution-json",
        default=r".\build\Release\logs\edge_sign_distribution_quarantine.json",
        help="Output JSON path for A10-0 expected-edge sign distribution diagnostics.",
    )
    parser.add_argument(
        "--edge-pos-no-trade-breakdown-json",
        default=r".\build\Release\logs\edge_pos_but_no_trade_breakdown.json",
        help="Output JSON path for A10-0 edge_pos-but-no-trade stage breakdown.",
    )
    parser.add_argument(
        "--a10-2-stage-funnel-json",
        default=r".\build\Release\logs\a10_2_stage_funnel_quarantine.json",
        help="Output JSON path for A10-2 core-effective stage funnel diagnostics.",
    )
    parser.add_argument(
        "--quality-filter-breakdown-json",
        default=r".\build\Release\logs\quality_filter_fail_breakdown_quarantine.json",
        help="Output JSON path for A18-0 quality filter breakdown diagnostics.",
    )
    parser.add_argument(
        "--a20-consistency-check-json",
        default=r".\build\Release\logs\a20_consistency_check.json",
        help=(
            "Output JSON path for A20 consistency check "
            "(total_trades_core_effective vs orders_filled_core_effective)."
        ),
    )
    parser.add_argument(
        "--runtime-cost-semantics-debug-json",
        default=r".\build\Release\logs\runtime_cost_semantics_debug.json",
        help=(
            "Output JSON path for runtime cost/semantics debug evidence "
            "(edge_semantics, runtime cost flags, cost_bps summary, sampled rows)."
        ),
    )
    parser.add_argument(
        "--semantics-lock-report-json",
        default=r".\build\Release\logs\semantics_lock_report.json",
        help="Output JSON path for B1/B2-1 semantics lock report.",
    )
    parser.add_argument(
        "--ranging-shadow-signals-jsonl",
        default=r".\build\Release\logs\ranging_shadow_signals.jsonl",
        help="Artifact JSONL path for Stage D v1 ranging shadow signal events.",
    )
    parser.add_argument(
        "--ranging-shadow-run-meta-json",
        default=r".\build\Release\logs\ranging_shadow_run_meta.json",
        help="Output JSON path for Stage D v1 ranging shadow run metadata.",
    )
    parser.add_argument(
        "--stage-d-v1-effect-summary-json",
        default=r".\build\Release\logs\stageD_v1_effect_summary.json",
        help="Output JSON path for Stage D v1 effect summary (real trades + shadow).",
    )
    parser.add_argument(
        "--core-no-signal-summary-json",
        default=r".\build\Release\logs\fresh14_core_no_signal_summary.json",
        help=(
            "Output JSON path for execution-vs-core no-signal reason dual-scope summary "
            "(Fresh14 follow-up diagnostic)."
        ),
    )
    parser.add_argument(
        "--stage-g7-ev-affine-debug-json",
        default=r".\build\Release\logs\stageG7_ev_affine_debug.json",
        help="Output JSON path for Stage G7 LGBM EV affine telemetry/debug summary.",
    )
    parser.add_argument(
        "--stage-g8-backend-provenance-json",
        default=r".\build\Release\logs\stageG8_backend_provenance_report.json",
        help="Output JSON path for Stage G8 backend/model provenance summary.",
    )
    args = parser.parse_args(argv)

    exe_path = resolve_path(args.exe_path, "Executable")
    config_path = resolve_path(args.config_path, "Build config", must_exist=False)
    runtime_run_dir: Optional[pathlib.Path] = None
    if str(args.run_dir).strip():
        runtime_run_dir = resolve_path(args.run_dir, "Runtime run dir", must_exist=False)
    source_config_path = resolve_path(args.source_config_path, "Source config")
    data_dir = resolve_path(args.data_dir, "Data directory")
    output_csv = resolve_path(args.output_csv, "Output csv", must_exist=False)
    output_json = resolve_path(args.output_json, "Output json", must_exist=False)
    vol_bucket_pct_distribution_json = resolve_path(
        args.quarantine_vol_bucket_pct_distribution_json,
        "Vol bucket percentile distribution json",
        must_exist=False,
    )
    pnl_by_regime_x_vol_fixed_json = resolve_path(
        args.quarantine_pnl_by_regime_x_vol_fixed_json,
        "PnL by regime x fixed vol bucket json",
        must_exist=False,
    )
    pnl_by_regime_x_vol_pct_json = resolve_path(
        args.quarantine_pnl_by_regime_x_vol_pct_json,
        "PnL by regime x percentile vol bucket json",
        must_exist=False,
    )
    top_loss_cells_regime_vol_pct_json = resolve_path(
        args.top_loss_cells_regime_vol_pct_json,
        "Top loss cells regime vol pct json",
        must_exist=False,
    )
    a8_quantile_exec_coverage_debug_json = resolve_path(
        args.a8_quantile_exec_coverage_debug_json,
        "A8 quantile exec coverage debug json",
        must_exist=False,
    )
    cell_root_cause_report_json = resolve_path(
        args.cell_root_cause_report_json,
        "Cell root cause report json",
        must_exist=False,
    )
    edge_sign_distribution_json = resolve_path(
        args.edge_sign_distribution_json,
        "Edge sign distribution json",
        must_exist=False,
    )
    edge_pos_no_trade_breakdown_json = resolve_path(
        args.edge_pos_no_trade_breakdown_json,
        "Edge pos but no trade breakdown json",
        must_exist=False,
    )
    a10_2_stage_funnel_json = resolve_path(
        args.a10_2_stage_funnel_json,
        "A10-2 stage funnel json",
        must_exist=False,
    )
    quality_filter_fail_breakdown_arg = getattr(
        args,
        "quality_filter_fail_breakdown_json",
        getattr(args, "quality_filter_breakdown_json", None),
    )
    quality_filter_fail_breakdown_json = resolve_path(
        quality_filter_fail_breakdown_arg,
        "Frontier fail breakdown json",
        must_exist=False,
    )
    a20_consistency_check_json = resolve_path(
        args.a20_consistency_check_json,
        "A20 consistency check json",
        must_exist=False,
    )
    runtime_cost_semantics_debug_json = resolve_path(
        args.runtime_cost_semantics_debug_json,
        "Runtime cost semantics debug json",
        must_exist=False,
    )
    semantics_lock_report_json = resolve_path(
        args.semantics_lock_report_json,
        "Semantics lock report json",
        must_exist=False,
    )
    ranging_shadow_signals_jsonl = resolve_path(
        args.ranging_shadow_signals_jsonl,
        "Ranging shadow signals jsonl",
        must_exist=False,
    )
    ranging_shadow_run_meta_json = resolve_path(
        args.ranging_shadow_run_meta_json,
        "Ranging shadow run meta json",
        must_exist=False,
    )
    stage_d_v1_effect_summary_json = resolve_path(
        args.stage_d_v1_effect_summary_json,
        "Stage D v1 effect summary json",
        must_exist=False,
    )
    core_no_signal_summary_json = resolve_path(
        args.core_no_signal_summary_json,
        "Core no-signal summary json",
        must_exist=False,
    )
    stage_g7_ev_affine_debug_json = resolve_path(
        args.stage_g7_ev_affine_debug_json,
        "Stage G7 EV affine debug json",
        must_exist=False,
    )
    stage_g8_backend_provenance_json = resolve_path(
        args.stage_g8_backend_provenance_json,
        "Stage G8 backend provenance json",
        must_exist=False,
    )
    core_zero_diagnosis_json = resolve_path(
        args.core_zero_diagnosis_json,
        "Core zero diagnosis json",
        must_exist=False,
    )
    baseline_report_path = resolve_path(
        args.baseline_report_path,
        "Baseline report",
        must_exist=False,
    )
    lock_path = resolve_path(args.verification_lock_path, "Verification lock", must_exist=False)

    ensure_parent(config_path)
    if not config_path.exists():
        config_path.write_text(source_config_path.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")

    dataset_paths: List[pathlib.Path] = []
    for name in args.dataset_names:
        token = str(name).strip()
        if not token:
            continue
        candidate = pathlib.Path(token)
        if not candidate.is_absolute():
            candidate = (data_dir / candidate).resolve()
        if not candidate.exists():
            raise FileNotFoundError(f"Dataset not found: {candidate}")
        dataset_paths.append(candidate)
    if not dataset_paths:
        raise RuntimeError("No datasets configured.")
    dataset_key_counts: Dict[str, int] = {}
    for path_value in dataset_paths:
        key = str(path_value.resolve()).lower()
        dataset_key_counts[key] = dataset_key_counts.get(key, 0) + 1
    duplicated = sorted([k for k, c in dataset_key_counts.items() if int(c) > 1])
    if duplicated:
        duplicate_names = [pathlib.Path(x).name for x in duplicated]
        raise RuntimeError(
            "Duplicate datasets configured (remove duplicates to keep verification weighting stable): "
            + ",".join(duplicate_names)
        )

    config_payload: Dict[str, Any] = {}
    try:
        with config_path.open("r", encoding="utf-8") as fp:
            loaded = json.load(fp)
            if isinstance(loaded, dict):
                config_payload = loaded
    except Exception:
        config_payload = {}
    bundle_path = resolve_probabilistic_bundle_path(exe_path, config_payload)

    bundle_meta: Dict[str, Any] = {}
    phase4_market_cluster_map: Dict[str, str] = {}
    risk_adjusted_score_policy: Dict[str, Any] = {}
    risk_adjusted_score_guard_policy: Dict[str, Any] = {}
    if not bool(args.skip_probabilistic_coverage_check):
        if str(bundle_path).strip() not in ("", ".", ".."):
            if not bundle_path.exists():
                raise FileNotFoundError(
                    "Probabilistic runtime bundle configured but missing: "
                    f"{bundle_path}"
                )
            bundle_meta = load_supported_markets_from_runtime_bundle(bundle_path)
            loaded_cluster_map = bundle_meta.get("phase4_market_cluster_map", {})
            if isinstance(loaded_cluster_map, dict):
                phase4_market_cluster_map = {
                    str(k).strip().upper(): str(v).strip()
                    for k, v in loaded_cluster_map.items()
                    if str(k).strip() and str(v).strip()
                }
            loaded_risk_score_policy = bundle_meta.get("risk_adjusted_score_policy", {})
            if isinstance(loaded_risk_score_policy, dict):
                risk_adjusted_score_policy = loaded_risk_score_policy
            loaded_risk_guard_policy = bundle_meta.get("risk_adjusted_score_guard_policy", {})
            if isinstance(loaded_risk_guard_policy, dict):
                risk_adjusted_score_guard_policy = loaded_risk_guard_policy
            supported_markets = {
                str(x).strip().upper()
                for x in bundle_meta.get("supported_markets", [])
                if str(x).strip()
            }
            if not bool(bundle_meta.get("global_fallback_enabled", False)):
                unsupported_pairs: List[str] = []
                for dataset_path in dataset_paths:
                    market = extract_upbit_market_from_dataset_name(dataset_path.name)
                    if not market:
                        continue
                    if market not in supported_markets:
                        unsupported_pairs.append(f"{dataset_path.name}:{market}")
                if unsupported_pairs:
                    raise RuntimeError(
                        "Probabilistic market coverage check failed. "
                        "Dataset markets are missing in runtime bundle: "
                        + ", ".join(sorted(unsupported_pairs))
                        + f" | bundle={bundle_meta.get('bundle_path', '')}"
                    )
    pipeline_version = resolve_verification_pipeline_version(args.pipeline_version, bundle_meta)
    vol_bucket_pct_policy = resolve_vol_bucket_pct_policy(
        {
            "window_size": int(args.vol_bucket_pct_window_size),
            "min_points": int(args.vol_bucket_pct_min_points),
            "lower_q": float(args.vol_bucket_pct_lower_q),
            "upper_q": float(args.vol_bucket_pct_upper_q),
            "fixed_low_cut": float(args.vol_bucket_fixed_low_cut),
            "fixed_high_cut": float(args.vol_bucket_fixed_high_cut),
            "source": "cli_policy",
        }
    )

    rows: List[Dict[str, Any]] = []
    dataset_diagnostics: List[Dict[str, Any]] = []
    adaptive_dataset_profiles: List[Dict[str, Any]] = []
    vol_bucket_pct_dataset_profiles: List[Dict[str, Any]] = []
    quality_filter_dataset_samples: List[Dict[str, Any]] = []
    ranging_shadow_events_all: List[Dict[str, Any]] = []
    split_eval_profiles: List[Dict[str, Any]] = []
    phase4_off_on_comparison: Dict[str, Any] = {}
    core_window_direct_report: Dict[str, Any] = {}
    split_manifest_payload: Dict[str, Any] = {}
    split_filter_context: Dict[str, Any] = {
        "split_applied": False,
        "reason": "manifest_not_provided",
        "split_name": canonical_split_name(args.split_name),
    }
    split_manifest_path_value = str(args.split_manifest_json).strip()
    if split_manifest_path_value:
        split_manifest_path = resolve_path(split_manifest_path_value, "Split manifest", must_exist=True)
        loaded_manifest = load_report_json(split_manifest_path)
        if isinstance(loaded_manifest, dict):
            split_manifest_payload = loaded_manifest
        if not str(args.split_name).strip():
            raise RuntimeError(
                "--split-manifest-json is set but --split-name is empty; "
                "split_name must be one of development/validation/quarantine/all_purged."
            )
        split_filter_context = apply_manifest_split_filter_to_dataset_paths(
            dataset_paths=dataset_paths,
            split_manifest_payload=split_manifest_payload,
            split_name=str(args.split_name),
            logs_dir=resolve_runtime_logs_dir(exe_path.parent, runtime_run_dir),
            execution_prewarm_hours=float(max(0.0, args.split_execution_prewarm_hours)),
        )
        filtered_paths = split_filter_context.get("filtered_dataset_paths", [])
        if isinstance(filtered_paths, list) and filtered_paths:
            dataset_paths = [pathlib.Path(x).resolve() for x in filtered_paths]
    elif bool(args.split_time_based):
        raise RuntimeError(
            "--split-time-based is set without --split-manifest-json. "
            "Time split mode requires an explicit manifest."
        )

    if bool(args.require_higher_tf_companions):
        for dataset_path in dataset_paths:
            if not is_upbit_primary_1m_dataset(dataset_path):
                raise RuntimeError(
                    "When --require-higher-tf-companions is enabled, "
                    "only upbit_*_1m_*.csv datasets are allowed: "
                    f"{dataset_path.name}"
                )
            if not has_higher_tf_companions(dataset_path.parent, dataset_path):
                raise RuntimeError(
                    "Missing companion timeframe csv (5m/15m/60m/240m) for dataset: "
                    f"{dataset_path.name} (checked_dir={dataset_path.parent})"
                )

    with verification_lock(
        lock_path,
        timeout_sec=int(args.verification_lock_timeout_sec),
        stale_sec=int(args.verification_lock_stale_sec),
    ):
        for dataset_path in dataset_paths:
            execution_range = split_filter_context.get("execution_range_used", [])
            eval_range = split_filter_context.get("evaluation_range_used", [])
            cumulative_range = split_filter_context.get("cumulative_range_used", [])
            eval_start_ts = int(eval_range[0]) if isinstance(eval_range, list) and len(eval_range) == 2 else None
            eval_end_ts = int(eval_range[1]) if isinstance(eval_range, list) and len(eval_range) == 2 else None
            cumulative_start_ts = (
                int(cumulative_range[0]) if isinstance(cumulative_range, list) and len(cumulative_range) == 2 else None
            )
            cumulative_end_ts = (
                int(cumulative_range[1]) if isinstance(cumulative_range, list) and len(cumulative_range) == 2 else None
            )
            result = run_backtest(
                exe_path=exe_path,
                dataset_path=dataset_path,
                config_path=config_path,
                require_higher_tf_companions=bool(args.require_higher_tf_companions),
                disable_adaptive_state_io=not bool(args.enable_adaptive_state_io),
                run_dir=runtime_run_dir,
                evaluation_start_ts=eval_start_ts,
                evaluation_end_ts=eval_end_ts,
                cumulative_start_ts=cumulative_start_ts,
                cumulative_end_ts=cumulative_end_ts,
            )
            ranging_shadow_events_all.extend(
                load_ranging_shadow_events(
                    exe_dir=exe_path.parent,
                    dataset_name=dataset_path.name,
                    run_dir=runtime_run_dir,
                )
            )
            quality_payload = result.get("quality_filter_samples", {})
            if not isinstance(quality_payload, dict):
                quality_payload = {}
            quality_payload_row = {
                "dataset": str(dataset_path.name),
                "line_count_in_range": max(0, to_int(quality_payload.get("line_count_in_range", 0))),
                "sample_count_in_range": max(0, to_int(quality_payload.get("sample_count_in_range", 0))),
                "quality_filter_enabled_count_in_range": max(
                    0, to_int(quality_payload.get("quality_filter_enabled_count_in_range", 0))
                ),
                "quality_filter_fail_count_in_range": max(
                    0, to_int(quality_payload.get("quality_filter_fail_count_in_range", 0))
                ),
                "samples": quality_payload.get("samples", [])
                if isinstance(quality_payload.get("samples", []), list)
                else [],
            }
            quality_filter_dataset_samples.append(quality_payload_row)
            rows.append(build_verification_row(dataset_path.name, result))
            split_eval_stats = result.get("split_eval_stats", {})
            split_trade_eval_stats = result.get("split_trade_eval_stats", {})
            split_cumulative_stats = result.get("split_cumulative_stats", {})
            split_trade_cumulative_stats = result.get("split_trade_cumulative_stats", {})
            split_stage_eval_stats = result.get("split_stage_funnel_eval_stats", {})
            split_stage_cumulative_stats = result.get("split_stage_funnel_cumulative_stats", {})
            entry_funnel = result.get("entry_funnel", {})
            split_eval_profiles.append(
                {
                    "dataset": str(dataset_path.name),
                    "entry_rounds_execution": max(
                        0,
                        to_int(entry_funnel.get("entry_rounds", 0))
                        if isinstance(entry_funnel, dict)
                        else 0,
                    ),
                    "entry_rounds_core_proxy": max(
                        0,
                        to_int(split_eval_stats.get("line_count_in_range", 0))
                        if isinstance(split_eval_stats, dict)
                        else 0,
                    ),
                    "candidate_total_execution": max(
                        0,
                        to_int(result.get("phase3_diagnostics_v2", {}).get("funnel_breakdown", {}).get("candidate_total", 0))
                        if isinstance(result.get("phase3_diagnostics_v2", {}), dict)
                        else 0,
                    ),
                    "candidate_total_core_proxy": max(
                        0,
                        to_int(split_eval_stats.get("decision_count_in_range", 0))
                        if isinstance(split_eval_stats, dict)
                        else 0,
                    ),
                    "pass_total_core_proxy": max(
                        0,
                        to_int(split_eval_stats.get("selected_decision_count_in_range", 0))
                        if isinstance(split_eval_stats, dict)
                        else 0,
                    ),
                    "total_round_trips_core_proxy": max(
                        0,
                        to_int(split_trade_eval_stats.get("terminal_sell_fills_in_range", 0))
                        if isinstance(split_trade_eval_stats, dict)
                        else 0,
                    ),
                    "total_trades_core_proxy": max(
                        0,
                        to_int(split_stage_eval_stats.get("orders_filled_in_range", 0))
                        if isinstance(split_stage_eval_stats, dict)
                        else max(
                            0,
                            to_int(split_trade_eval_stats.get("buy_filled_in_range", 0))
                            if isinstance(split_trade_eval_stats, dict)
                            else 0,
                        ),
                    ),
                    "orders_submitted_core_proxy": max(
                        0,
                        to_int(split_stage_eval_stats.get("orders_submitted_in_range", 0))
                        if isinstance(split_stage_eval_stats, dict)
                        else max(
                            0,
                            to_int(split_trade_eval_stats.get("buy_submitted_in_range", 0))
                            if isinstance(split_trade_eval_stats, dict)
                            else 0,
                        ),
                    ),
                    "orders_filled_core_proxy": max(
                        0,
                        to_int(split_stage_eval_stats.get("orders_filled_in_range", 0))
                        if isinstance(split_stage_eval_stats, dict)
                        else max(
                            0,
                            to_int(split_trade_eval_stats.get("buy_filled_in_range", 0))
                            if isinstance(split_trade_eval_stats, dict)
                            else 0,
                        ),
                    ),
                    "stage_candidates_total_core_proxy": max(
                        0,
                        to_int(split_stage_eval_stats.get("candidates_total_in_range", 0))
                        if isinstance(split_stage_eval_stats, dict)
                        else 0,
                    ),
                    "stage_candidates_after_quality_topk_core_proxy": max(
                        0,
                        to_int(split_stage_eval_stats.get("candidates_after_quality_topk_in_range", 0))
                        if isinstance(split_stage_eval_stats, dict)
                        else 0,
                    ),
                    "stage_candidates_after_manager_core_proxy": max(
                        0,
                        to_int(split_stage_eval_stats.get("candidates_after_manager_in_range", 0))
                        if isinstance(split_stage_eval_stats, dict)
                        else 0,
                    ),
                    "stage_candidates_after_portfolio_core_proxy": max(
                        0,
                        to_int(split_stage_eval_stats.get("candidates_after_portfolio_in_range", 0))
                        if isinstance(split_stage_eval_stats, dict)
                        else 0,
                    ),
                    "stage_order_block_reason_counts_core_proxy": (
                        split_stage_eval_stats.get("order_block_reason_counts_in_range", {})
                        if isinstance(split_stage_eval_stats, dict)
                        else {}
                    ),
                    "ev_samples_core_proxy": max(
                        0,
                        to_int(split_stage_eval_stats.get("ev_samples_in_range", 0))
                        if isinstance(split_stage_eval_stats, dict)
                        else 0,
                    ),
                    "ev_at_manager_pass_sum_core_proxy": (
                        to_float(split_stage_eval_stats.get("ev_at_manager_pass_sum_in_range", 0.0))
                        if isinstance(split_stage_eval_stats, dict)
                        else 0.0
                    ),
                    "ev_at_order_submit_check_sum_core_proxy": (
                        to_float(
                            split_stage_eval_stats.get(
                                "ev_at_order_submit_check_sum_in_range", 0.0
                            )
                        )
                        if isinstance(split_stage_eval_stats, dict)
                        else 0.0
                    ),
                    "ev_mismatch_count_core_proxy": max(
                        0,
                        to_int(split_stage_eval_stats.get("ev_mismatch_count_in_range", 0))
                        if isinstance(split_stage_eval_stats, dict)
                        else 0,
                    ),
                    "candidate_total_cumulative_proxy": max(
                        0,
                        to_int(split_cumulative_stats.get("decision_count_in_range", 0))
                        if isinstance(split_cumulative_stats, dict)
                        else 0,
                    ),
                    "pass_total_cumulative_proxy": max(
                        0,
                        to_int(split_cumulative_stats.get("selected_decision_count_in_range", 0))
                        if isinstance(split_cumulative_stats, dict)
                        else 0,
                    ),
                    "total_round_trips_cumulative_proxy": max(
                        0,
                        to_int(split_trade_cumulative_stats.get("terminal_sell_fills_in_range", 0))
                        if isinstance(split_trade_cumulative_stats, dict)
                        else 0,
                    ),
                    "total_trades_cumulative_proxy": max(
                        0,
                        to_int(split_stage_cumulative_stats.get("orders_filled_in_range", 0))
                        if isinstance(split_stage_cumulative_stats, dict)
                        else max(
                            0,
                            to_int(split_trade_cumulative_stats.get("buy_filled_in_range", 0))
                            if isinstance(split_trade_cumulative_stats, dict)
                            else 0,
                        ),
                    ),
                    "orders_submitted_cumulative_proxy": max(
                        0,
                        to_int(split_stage_cumulative_stats.get("orders_submitted_in_range", 0))
                        if isinstance(split_stage_cumulative_stats, dict)
                        else max(
                            0,
                            to_int(split_trade_cumulative_stats.get("buy_submitted_in_range", 0))
                            if isinstance(split_trade_cumulative_stats, dict)
                            else 0,
                        ),
                    ),
                    "orders_filled_cumulative_proxy": max(
                        0,
                        to_int(split_stage_cumulative_stats.get("orders_filled_in_range", 0))
                        if isinstance(split_stage_cumulative_stats, dict)
                        else max(
                            0,
                            to_int(split_trade_cumulative_stats.get("buy_filled_in_range", 0))
                            if isinstance(split_trade_cumulative_stats, dict)
                            else 0,
                        ),
                    ),
                    "stage_candidates_total_cumulative_proxy": max(
                        0,
                        to_int(split_stage_cumulative_stats.get("candidates_total_in_range", 0))
                        if isinstance(split_stage_cumulative_stats, dict)
                        else 0,
                    ),
                    "stage_candidates_after_quality_topk_cumulative_proxy": max(
                        0,
                        to_int(split_stage_cumulative_stats.get("candidates_after_quality_topk_in_range", 0))
                        if isinstance(split_stage_cumulative_stats, dict)
                        else 0,
                    ),
                    "stage_candidates_after_manager_cumulative_proxy": max(
                        0,
                        to_int(split_stage_cumulative_stats.get("candidates_after_manager_in_range", 0))
                        if isinstance(split_stage_cumulative_stats, dict)
                        else 0,
                    ),
                    "stage_candidates_after_portfolio_cumulative_proxy": max(
                        0,
                        to_int(split_stage_cumulative_stats.get("candidates_after_portfolio_in_range", 0))
                        if isinstance(split_stage_cumulative_stats, dict)
                        else 0,
                    ),
                    "stage_order_block_reason_counts_cumulative_proxy": (
                        split_stage_cumulative_stats.get(
                            "order_block_reason_counts_in_range", {}
                        )
                        if isinstance(split_stage_cumulative_stats, dict)
                        else {}
                    ),
                    "ev_samples_cumulative_proxy": max(
                        0,
                        to_int(split_stage_cumulative_stats.get("ev_samples_in_range", 0))
                        if isinstance(split_stage_cumulative_stats, dict)
                        else 0,
                    ),
                    "ev_at_manager_pass_sum_cumulative_proxy": (
                        to_float(
                            split_stage_cumulative_stats.get(
                                "ev_at_manager_pass_sum_in_range", 0.0
                            )
                        )
                        if isinstance(split_stage_cumulative_stats, dict)
                        else 0.0
                    ),
                    "ev_at_order_submit_check_sum_cumulative_proxy": (
                        to_float(
                            split_stage_cumulative_stats.get(
                                "ev_at_order_submit_check_sum_in_range", 0.0
                            )
                        )
                        if isinstance(split_stage_cumulative_stats, dict)
                        else 0.0
                    ),
                    "ev_mismatch_count_cumulative_proxy": max(
                        0,
                        to_int(split_stage_cumulative_stats.get("ev_mismatch_count_in_range", 0))
                        if isinstance(split_stage_cumulative_stats, dict)
                        else 0,
                    ),
                    "split_eval_stats": split_eval_stats if isinstance(split_eval_stats, dict) else {},
                    "split_trade_eval_stats": (
                        split_trade_eval_stats if isinstance(split_trade_eval_stats, dict) else {}
                    ),
                    "split_stage_funnel_eval_stats": (
                        split_stage_eval_stats if isinstance(split_stage_eval_stats, dict) else {}
                    ),
                    "split_cumulative_stats": (
                        split_cumulative_stats if isinstance(split_cumulative_stats, dict) else {}
                    ),
                    "split_trade_cumulative_stats": (
                        split_trade_cumulative_stats if isinstance(split_trade_cumulative_stats, dict) else {}
                    ),
                    "split_stage_funnel_cumulative_stats": (
                        split_stage_cumulative_stats
                        if isinstance(split_stage_cumulative_stats, dict)
                        else {}
                    ),
                }
            )
            dataset_diagnostics.append(
                build_dataset_diagnostics(
                    dataset_name=dataset_path.name,
                    backtest_result=result,
                    phase4_market_cluster_map=phase4_market_cluster_map,
                    bundle_meta=bundle_meta if isinstance(bundle_meta, dict) else {},
                )
            )
            adaptive_dataset_profiles.append(
                build_adaptive_dataset_profile(
                    dataset_name=dataset_path.name,
                    backtest_result=result,
                    risk_adjusted_score_policy=risk_adjusted_score_policy,
                )
            )
            vol_bucket_pct_dataset_profiles.append(
                build_vol_bucket_pct_dataset_profile(
                    dataset_name=dataset_path.name,
                    backtest_result=result,
                    policy=vol_bucket_pct_policy,
                    dataset_path=dataset_path,
                    execution_range_used=execution_range,
                    evaluation_range_used=eval_range,
                )
            )
        if bool(args.phase4_off_on_compare):
            phase4_off_on_comparison = build_phase4_off_on_comparison(
                exe_path=exe_path,
                dataset_paths=dataset_paths,
                require_higher_tf_companions=bool(args.require_higher_tf_companions),
                disable_adaptive_state_io=not bool(args.enable_adaptive_state_io),
                config_path=config_path,
                config_payload=config_payload,
                bundle_path=bundle_path,
            )
        if split_manifest_payload and not bool(args.disable_core_window_direct_eval):
            split_name_for_core = str(args.split_name).strip().lower()
            core_paths_info = resolve_core_dataset_paths(
                split_manifest_payload=split_manifest_payload,
                split_name=split_name_for_core,
                dataset_paths=dataset_paths,
            )
            if bool(core_paths_info.get("available", False)):
                core_window_direct_report = run_core_window_direct_report(
                    exe_path=exe_path,
                    config_path=config_path,
                    dataset_paths=list(core_paths_info.get("dataset_paths", [])),
                    require_higher_tf_companions=bool(args.require_higher_tf_companions),
                    disable_adaptive_state_io=not bool(args.enable_adaptive_state_io),
                    phase4_market_cluster_map=phase4_market_cluster_map,
                    risk_adjusted_score_policy=risk_adjusted_score_policy,
                    bundle_meta=bundle_meta if isinstance(bundle_meta, dict) else {},
                )
                core_window_direct_report["split_name"] = split_name_for_core
                core_window_direct_report["core_dir"] = str(core_paths_info.get("core_dir", ""))
            else:
                core_window_direct_report = {
                    "available": False,
                    "source": "core_dataset_direct",
                    "split_name": split_name_for_core,
                    "core_dir": str(core_paths_info.get("core_dir", "")),
                    "reason": str(core_paths_info.get("reason", "core_direct_not_available")),
                    "missing_datasets": list(core_paths_info.get("missing_datasets", [])),
                    "dataset_count": 0,
                    "datasets": [],
                }

    ensure_parent(output_csv)
    with output_csv.open("w", encoding="utf-8", newline="\n") as fp:
        fieldnames = [
            "dataset",
            "total_profit_krw",
            "profit_factor",
            "expectancy_krw",
            "max_drawdown_pct",
            "total_trades",
            "win_rate_pct",
            "avg_fee_krw",
            "profitable",
        ]
        writer = csv.DictWriter(fp, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    summary = summarize_verification_rows(rows)
    avg_profit_factor = round(to_float(summary.get("avg_profit_factor", 0.0)), 4)
    avg_expectancy_krw = round(to_float(summary.get("avg_expectancy_krw", 0.0)), 4)
    avg_win_rate_pct = round(to_float(summary.get("avg_win_rate_pct", 0.0)), 4)
    avg_total_trades = round(to_float(summary.get("avg_total_trades", 0.0)), 4)
    peak_max_drawdown_pct = round(to_float(summary.get("peak_max_drawdown_pct", 0.0)), 4)
    total_profit_sum_krw = round(to_float(summary.get("total_profit_sum_krw", 0.0)), 4)
    profitable_ratio = round(to_float(summary.get("profitable_ratio", 0.0)), 4)
    avg_fee_krw = round(to_float(summary.get("avg_fee_krw", 0.0)), 4)
    split_trades_after_filter = int(
        sum(max(0, to_int(item.get("total_trades", 0))) for item in rows if isinstance(item, dict))
    )
    if isinstance(split_filter_context, dict):
        split_filter_context["trades_after_filter"] = int(split_trades_after_filter)
        split_filter_context["entry_rounds_execution"] = int(
            sum(
                max(0, to_int(item.get("entry_rounds_execution", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["entry_rounds_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("entry_rounds_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["candidate_total_execution"] = int(
            sum(
                max(0, to_int(item.get("candidate_total_execution", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["candidate_total_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("candidate_total_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["pass_total_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("pass_total_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["total_trades_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("total_trades_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["total_round_trips_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("total_round_trips_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["candidate_total_cumulative_proxy"] = int(
            sum(
                max(0, to_int(item.get("candidate_total_cumulative_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["pass_total_cumulative_proxy"] = int(
            sum(
                max(0, to_int(item.get("pass_total_cumulative_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["total_trades_cumulative_proxy"] = int(
            sum(
                max(0, to_int(item.get("total_trades_cumulative_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["total_round_trips_cumulative_proxy"] = int(
            sum(
                max(0, to_int(item.get("total_round_trips_cumulative_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["orders_submitted_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("orders_submitted_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["orders_filled_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("orders_filled_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["orders_submitted_cumulative_proxy"] = int(
            sum(
                max(0, to_int(item.get("orders_submitted_cumulative_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["orders_filled_cumulative_proxy"] = int(
            sum(
                max(0, to_int(item.get("orders_filled_cumulative_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["stage_candidates_total_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("stage_candidates_total_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["stage_candidates_after_quality_topk_core_proxy"] = int(
            sum(
                max(
                    0,
                    to_int(item.get("stage_candidates_after_quality_topk_core_proxy", 0)),
                )
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["stage_candidates_after_manager_core_proxy"] = int(
            sum(
                max(
                    0,
                    to_int(item.get("stage_candidates_after_manager_core_proxy", 0)),
                )
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["stage_candidates_after_portfolio_core_proxy"] = int(
            sum(
                max(
                    0,
                    to_int(item.get("stage_candidates_after_portfolio_core_proxy", 0)),
                )
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["stage_candidates_total_cumulative_proxy"] = int(
            sum(
                max(0, to_int(item.get("stage_candidates_total_cumulative_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["stage_candidates_after_quality_topk_cumulative_proxy"] = int(
            sum(
                max(
                    0,
                    to_int(item.get("stage_candidates_after_quality_topk_cumulative_proxy", 0)),
                )
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["stage_candidates_after_manager_cumulative_proxy"] = int(
            sum(
                max(
                    0,
                    to_int(item.get("stage_candidates_after_manager_cumulative_proxy", 0)),
                )
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["stage_candidates_after_portfolio_cumulative_proxy"] = int(
            sum(
                max(
                    0,
                    to_int(item.get("stage_candidates_after_portfolio_cumulative_proxy", 0)),
                )
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        stage_order_block_reason_counts_core_proxy: Dict[str, int] = {}
        stage_order_block_reason_counts_cumulative_proxy: Dict[str, int] = {}
        for item in split_eval_profiles:
            if not isinstance(item, dict):
                continue
            core_reasons = item.get("stage_order_block_reason_counts_core_proxy", {})
            if isinstance(core_reasons, dict):
                for k, v in core_reasons.items():
                    rk = str(k).strip()
                    if not rk:
                        continue
                    stage_order_block_reason_counts_core_proxy[rk] = (
                        stage_order_block_reason_counts_core_proxy.get(rk, 0)
                        + max(0, to_int(v))
                    )
            cumulative_reasons = item.get(
                "stage_order_block_reason_counts_cumulative_proxy", {}
            )
            if isinstance(cumulative_reasons, dict):
                for k, v in cumulative_reasons.items():
                    rk = str(k).strip()
                    if not rk:
                        continue
                    stage_order_block_reason_counts_cumulative_proxy[rk] = (
                        stage_order_block_reason_counts_cumulative_proxy.get(rk, 0)
                        + max(0, to_int(v))
                    )
        split_filter_context["stage_order_block_reason_counts_core_proxy"] = {
            str(k): int(v)
            for k, v in stage_order_block_reason_counts_core_proxy.items()
            if int(v) > 0
        }
        split_filter_context["stage_order_block_reason_counts_cumulative_proxy"] = {
            str(k): int(v)
            for k, v in stage_order_block_reason_counts_cumulative_proxy.items()
            if int(v) > 0
        }
        split_filter_context["ev_samples_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("ev_samples_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["ev_at_manager_pass_sum_core_proxy"] = float(
            sum(
                to_float(item.get("ev_at_manager_pass_sum_core_proxy", 0.0))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["ev_at_order_submit_check_sum_core_proxy"] = float(
            sum(
                to_float(item.get("ev_at_order_submit_check_sum_core_proxy", 0.0))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["ev_mismatch_count_core_proxy"] = int(
            sum(
                max(0, to_int(item.get("ev_mismatch_count_core_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["ev_samples_cumulative_proxy"] = int(
            sum(
                max(0, to_int(item.get("ev_samples_cumulative_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["ev_at_manager_pass_sum_cumulative_proxy"] = float(
            sum(
                to_float(item.get("ev_at_manager_pass_sum_cumulative_proxy", 0.0))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["ev_at_order_submit_check_sum_cumulative_proxy"] = float(
            sum(
                to_float(
                    item.get("ev_at_order_submit_check_sum_cumulative_proxy", 0.0)
                )
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        split_filter_context["ev_mismatch_count_cumulative_proxy"] = int(
            sum(
                max(0, to_int(item.get("ev_mismatch_count_cumulative_proxy", 0)))
                for item in split_eval_profiles
                if isinstance(item, dict)
            )
        )
        core_zero = (
            int(split_filter_context.get("entry_rounds_core_proxy", 0)) <= 0
            and int(split_filter_context.get("candidate_total_core_proxy", 0)) <= 0
            and int(split_filter_context.get("total_trades_core_proxy", 0)) <= 0
        )
        fallback_enabled = not bool(args.disable_split_core_zero_fallback_cumulative)
        fallback_applied = bool(
            bool(split_filter_context.get("split_applied", False))
            and core_zero
            and fallback_enabled
            and (
                int(split_filter_context.get("candidate_total_cumulative_proxy", 0)) > 0
                or int(split_filter_context.get("total_trades_cumulative_proxy", 0)) > 0
                or int(split_filter_context.get("entry_rounds_execution", 0)) > 0
            )
        )
        split_filter_context["core_zero_detected"] = bool(core_zero)
        split_filter_context["core_zero_fallback_enabled"] = bool(fallback_enabled)
        split_filter_context["core_zero_fallback_applied"] = bool(fallback_applied)
        if fallback_applied:
            split_filter_context["evaluation_range_effective"] = split_filter_context.get(
                "cumulative_range_used",
                split_filter_context.get("evaluation_range_used", []),
            )
            split_filter_context["entry_rounds_core_effective"] = int(
                split_filter_context.get("entry_rounds_execution", 0)
            )
            split_filter_context["candidate_total_core_effective"] = int(
                split_filter_context.get("candidate_total_cumulative_proxy", 0)
            )
            split_filter_context["pass_total_core_effective"] = int(
                split_filter_context.get("pass_total_cumulative_proxy", 0)
            )
            split_filter_context["total_trades_core_effective"] = int(
                split_filter_context.get("total_trades_cumulative_proxy", 0)
            )
            split_filter_context["total_round_trips_core_effective"] = int(
                split_filter_context.get("total_round_trips_cumulative_proxy", 0)
            )
            split_filter_context["orders_submitted_core_effective"] = int(
                split_filter_context.get("orders_submitted_cumulative_proxy", 0)
            )
            split_filter_context["orders_filled_core_effective"] = int(
                split_filter_context.get("orders_filled_cumulative_proxy", 0)
            )
            split_filter_context["stage_candidates_total_core_effective"] = int(
                split_filter_context.get("stage_candidates_total_cumulative_proxy", 0)
            )
            split_filter_context["stage_candidates_after_quality_topk_core_effective"] = int(
                split_filter_context.get(
                    "stage_candidates_after_quality_topk_cumulative_proxy", 0
                )
            )
            split_filter_context["stage_candidates_after_manager_core_effective"] = int(
                split_filter_context.get(
                    "stage_candidates_after_manager_cumulative_proxy", 0
                )
            )
            split_filter_context["stage_candidates_after_portfolio_core_effective"] = int(
                split_filter_context.get(
                    "stage_candidates_after_portfolio_cumulative_proxy", 0
                )
            )
            split_filter_context["stage_order_block_reason_counts_core_effective"] = (
                split_filter_context.get(
                    "stage_order_block_reason_counts_cumulative_proxy", {}
                )
                if isinstance(
                    split_filter_context.get(
                        "stage_order_block_reason_counts_cumulative_proxy", {}
                    ),
                    dict,
                )
                else {}
            )
            split_filter_context["ev_samples_core_effective"] = int(
                split_filter_context.get("ev_samples_cumulative_proxy", 0)
            )
            split_filter_context["ev_at_manager_pass_sum_core_effective"] = float(
                split_filter_context.get("ev_at_manager_pass_sum_cumulative_proxy", 0.0)
            )
            split_filter_context["ev_at_order_submit_check_sum_core_effective"] = float(
                split_filter_context.get(
                    "ev_at_order_submit_check_sum_cumulative_proxy", 0.0
                )
            )
            split_filter_context["ev_mismatch_count_core_effective"] = int(
                split_filter_context.get("ev_mismatch_count_cumulative_proxy", 0)
            )
        else:
            split_filter_context["evaluation_range_effective"] = split_filter_context.get(
                "evaluation_range_used",
                [],
            )
            split_filter_context["entry_rounds_core_effective"] = int(
                split_filter_context.get("entry_rounds_core_proxy", 0)
            )
            split_filter_context["candidate_total_core_effective"] = int(
                split_filter_context.get("candidate_total_core_proxy", 0)
            )
            split_filter_context["pass_total_core_effective"] = int(
                split_filter_context.get("pass_total_core_proxy", 0)
            )
            split_filter_context["total_trades_core_effective"] = int(
                split_filter_context.get("total_trades_core_proxy", 0)
            )
            split_filter_context["total_round_trips_core_effective"] = int(
                split_filter_context.get("total_round_trips_core_proxy", 0)
            )
            split_filter_context["orders_submitted_core_effective"] = int(
                split_filter_context.get("orders_submitted_core_proxy", 0)
            )
            split_filter_context["orders_filled_core_effective"] = int(
                split_filter_context.get("orders_filled_core_proxy", 0)
            )
            split_filter_context["stage_candidates_total_core_effective"] = int(
                split_filter_context.get("stage_candidates_total_core_proxy", 0)
            )
            split_filter_context["stage_candidates_after_quality_topk_core_effective"] = int(
                split_filter_context.get("stage_candidates_after_quality_topk_core_proxy", 0)
            )
            split_filter_context["stage_candidates_after_manager_core_effective"] = int(
                split_filter_context.get("stage_candidates_after_manager_core_proxy", 0)
            )
            split_filter_context["stage_candidates_after_portfolio_core_effective"] = int(
                split_filter_context.get("stage_candidates_after_portfolio_core_proxy", 0)
            )
            split_filter_context["stage_order_block_reason_counts_core_effective"] = (
                split_filter_context.get("stage_order_block_reason_counts_core_proxy", {})
                if isinstance(
                    split_filter_context.get("stage_order_block_reason_counts_core_proxy", {}),
                    dict,
                )
                else {}
            )
            split_filter_context["ev_samples_core_effective"] = int(
                split_filter_context.get("ev_samples_core_proxy", 0)
            )
            split_filter_context["ev_at_manager_pass_sum_core_effective"] = float(
                split_filter_context.get("ev_at_manager_pass_sum_core_proxy", 0.0)
            )
            split_filter_context["ev_at_order_submit_check_sum_core_effective"] = float(
                split_filter_context.get("ev_at_order_submit_check_sum_core_proxy", 0.0)
            )
            split_filter_context["ev_mismatch_count_core_effective"] = int(
                split_filter_context.get("ev_mismatch_count_core_proxy", 0)
            )

    gate = {
        "gate_profit_factor_pass": avg_profit_factor >= float(args.min_profit_factor),
        "gate_expectancy_pass": avg_expectancy_krw >= float(args.min_expectancy_krw),
        "gate_drawdown_pass": peak_max_drawdown_pct <= float(args.max_drawdown_pct),
        "gate_profitable_ratio_pass": profitable_ratio >= float(args.min_profitable_ratio),
        "gate_avg_win_rate_pass": avg_win_rate_pct >= float(args.min_avg_win_rate_pct),
        "gate_avg_trades_pass": avg_total_trades >= float(args.min_avg_trades),
    }
    threshold_gate_pass = all(bool(x) for x in gate.values())
    aggregate_diagnostics = aggregate_dataset_diagnostics(dataset_diagnostics)
    core_no_signal_summary = build_core_no_signal_summary(
        aggregate_diagnostics=aggregate_diagnostics,
        dataset_diagnostics=dataset_diagnostics,
        split_filter_context=split_filter_context if isinstance(split_filter_context, dict) else {},
        split_name=str(args.split_name).strip(),
    )
    ensure_parent(core_no_signal_summary_json)
    with core_no_signal_summary_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(core_no_signal_summary, fp, ensure_ascii=False, indent=4)
    if isinstance(split_filter_context, dict):
        split_filter_context["candidate_total_execution"] = int(
            nested_get(
                aggregate_diagnostics,
                ["phase3_diagnostics_v2", "funnel_breakdown", "candidate_total"],
                split_filter_context.get("candidate_total_execution", 0),
            )
        )
    failure_attribution = build_failure_attribution(
        gate=gate,
        avg_total_trades=avg_total_trades,
        min_avg_trades=float(args.min_avg_trades),
        aggregate_diagnostics=aggregate_diagnostics,
    )
    adaptive_aggregates = aggregate_adaptive_validation(adaptive_dataset_profiles)
    risk_score_guard_context = build_risk_score_guard_context(
        aggregate_diagnostics=aggregate_diagnostics,
        core_window_direct_report=core_window_direct_report,
    )
    risk_score_guard_history = load_risk_score_guard_history(
        risk_adjusted_score_guard_policy
    )
    adaptive_verdict = build_adaptive_verdict(
        adaptive_aggregates=adaptive_aggregates,
        max_downtrend_loss_per_trade_krw=float(args.max_downtrend_loss_per_trade_krw),
        max_downtrend_trade_share=float(args.max_downtrend_trade_share),
        min_uptrend_expectancy_krw=float(args.min_uptrend_expectancy_krw),
        min_risk_adjusted_score=float(args.min_risk_adjusted_score),
        avg_total_trades=avg_total_trades,
        min_avg_trades=float(args.min_avg_trades),
        peak_max_drawdown_pct=peak_max_drawdown_pct,
        max_drawdown_pct_limit=float(args.max_drawdown_pct),
        risk_adjusted_score_guard_policy=risk_adjusted_score_guard_policy,
        risk_adjusted_score_guard_context=risk_score_guard_context,
        risk_adjusted_score_guard_history=risk_score_guard_history,
    )
    risk_adjusted_failure_decomposition = build_risk_adjusted_failure_decomposition(
        adaptive_aggregates=adaptive_aggregates,
        adaptive_verdict=adaptive_verdict,
        min_risk_adjusted_score=float(args.min_risk_adjusted_score),
    )
    adaptive_pass = str(adaptive_verdict.get("verdict", "fail")) == "pass"
    overall_gate_pass = adaptive_pass
    vol_bucket_pct_artifacts = build_quarantine_vol_bucket_pct_artifacts(
        vol_bucket_pct_dataset_profiles,
        split_name=str(args.split_name).strip(),
    )

    ensure_parent(vol_bucket_pct_distribution_json)
    with vol_bucket_pct_distribution_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            vol_bucket_pct_artifacts.get("distribution_payload", {}),
            fp,
            ensure_ascii=False,
            indent=4,
        )
    ensure_parent(pnl_by_regime_x_vol_fixed_json)
    with pnl_by_regime_x_vol_fixed_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            vol_bucket_pct_artifacts.get("pnl_fixed_payload", {}),
            fp,
            ensure_ascii=False,
            indent=4,
        )
    ensure_parent(pnl_by_regime_x_vol_pct_json)
    with pnl_by_regime_x_vol_pct_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            vol_bucket_pct_artifacts.get("pnl_pct_payload", {}),
            fp,
            ensure_ascii=False,
            indent=4,
        )
    ensure_parent(top_loss_cells_regime_vol_pct_json)
    with top_loss_cells_regime_vol_pct_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            vol_bucket_pct_artifacts.get("top_loss_payload", {}),
            fp,
            ensure_ascii=False,
            indent=4,
        )
    ensure_parent(a8_quantile_exec_coverage_debug_json)
    with a8_quantile_exec_coverage_debug_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            vol_bucket_pct_artifacts.get("quantile_exec_coverage_payload", {}),
            fp,
            ensure_ascii=False,
            indent=4,
        )
    cell_root_cause_report = build_cell_root_cause_report(
        vol_bucket_pct_dataset_profiles,
        split_name=str(args.split_name).strip(),
        top_n_cells=max(1, int(args.cell_root_cause_top_n)),
    )
    ensure_parent(cell_root_cause_report_json)
    with cell_root_cause_report_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            cell_root_cause_report,
            fp,
            ensure_ascii=False,
            indent=4,
        )
    edge_sign_distribution = build_edge_sign_distribution(
        dataset_diagnostics=dataset_diagnostics,
        split_filter_context=split_filter_context,
    )
    ensure_parent(edge_sign_distribution_json)
    with edge_sign_distribution_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            edge_sign_distribution,
            fp,
            ensure_ascii=False,
            indent=4,
        )
    edge_pos_no_trade_breakdown = build_edge_pos_but_no_trade_breakdown(
        edge_sign_distribution=edge_sign_distribution,
        aggregate_diagnostics=aggregate_diagnostics,
        split_filter_context=split_filter_context,
    )
    ensure_parent(edge_pos_no_trade_breakdown_json)
    with edge_pos_no_trade_breakdown_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            edge_pos_no_trade_breakdown,
            fp,
            ensure_ascii=False,
            indent=4,
        )
    a10_2_stage_funnel_report = build_a10_2_stage_funnel_report(
        aggregate_diagnostics=aggregate_diagnostics,
        split_filter_context=split_filter_context,
    )
    ensure_parent(a10_2_stage_funnel_json)
    with a10_2_stage_funnel_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            a10_2_stage_funnel_report,
            fp,
            ensure_ascii=False,
            indent=4,
        )
    quality_filter_fail_breakdown = build_quality_filter_fail_breakdown(
        quality_filter_dataset_samples=quality_filter_dataset_samples,
        split_filter_context=split_filter_context,
    )
    ensure_parent(quality_filter_fail_breakdown_json)
    with quality_filter_fail_breakdown_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            quality_filter_fail_breakdown,
            fp,
            ensure_ascii=False,
            indent=4,
        )
    runtime_cost_semantics_debug = build_runtime_cost_semantics_debug(
        dataset_diagnostics=dataset_diagnostics,
        sample_cap=50,
    )
    semantics_lock_report = build_semantics_lock_report(
        bundle_meta=bundle_meta if isinstance(bundle_meta, dict) else {},
        runtime_cost_semantics_debug=runtime_cost_semantics_debug,
        filled_sample_count=max(
            int(
                split_filter_context.get(
                    "orders_filled_core_effective",
                    split_filter_context.get("total_trades_core_effective", 0),
                )
            )
            if isinstance(split_filter_context, dict)
            else 0,
            int(
                (a10_2_stage_funnel_report.get("funnel_summary_core", {}) or {}).get(
                    "S5_orders_filled_core",
                    0,
                )
            )
            if isinstance(a10_2_stage_funnel_report, dict)
            else 0,
        ),
    )
    ensure_parent(runtime_cost_semantics_debug_json)
    with runtime_cost_semantics_debug_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            runtime_cost_semantics_debug,
            fp,
            ensure_ascii=False,
            indent=4,
        )
    ensure_parent(semantics_lock_report_json)
    with semantics_lock_report_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            semantics_lock_report,
            fp,
            ensure_ascii=False,
            indent=4,
        )

    a20_total_trades_core_effective = int(
        split_filter_context.get("total_trades_core_effective", 0)
        if isinstance(split_filter_context, dict)
        else 0
    )
    a20_orders_filled_core_effective = int(
        split_filter_context.get("orders_filled_core_effective", 0)
        if isinstance(split_filter_context, dict)
        else 0
    )
    a20_total_round_trips_core_effective = int(
        split_filter_context.get("total_round_trips_core_effective", 0)
        if isinstance(split_filter_context, dict)
        else 0
    )
    a20_funnel_s5_orders_filled_core = int(
        (a10_2_stage_funnel_report.get("funnel_summary_core", {}) or {}).get(
            "S5_orders_filled_core",
            0,
        )
        if isinstance(a10_2_stage_funnel_report, dict)
        else 0
    )
    a20_consistency_check_payload = {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "split_name": str(args.split_name).strip(),
        "evaluation_range_effective": (
            split_filter_context.get("evaluation_range_effective", [])
            if isinstance(split_filter_context, dict)
            else []
        ),
        "definitions": {
            "total_trades_core_effective_basis": "orders_filled_core_effective",
            "total_round_trips_core_effective_basis": "terminal_sell_fills_in_range",
        },
        "values": {
            "total_trades_core_effective": int(a20_total_trades_core_effective),
            "orders_filled_core_effective": int(a20_orders_filled_core_effective),
            "total_round_trips_core_effective": int(a20_total_round_trips_core_effective),
            "funnel_s5_orders_filled_core": int(a20_funnel_s5_orders_filled_core),
        },
        "checks": {
            "trades_equals_filled": bool(
                int(a20_total_trades_core_effective)
                == int(a20_orders_filled_core_effective)
            ),
            "trades_equals_funnel_s5": bool(
                int(a20_total_trades_core_effective)
                == int(a20_funnel_s5_orders_filled_core)
            ),
        },
    }
    a20_consistency_check_payload["mismatch"] = int(
        not (
            bool(a20_consistency_check_payload["checks"]["trades_equals_filled"])
            and bool(a20_consistency_check_payload["checks"]["trades_equals_funnel_s5"])
        )
    )
    ensure_parent(a20_consistency_check_json)
    with a20_consistency_check_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(
            a20_consistency_check_payload,
            fp,
            ensure_ascii=False,
            indent=4,
        )

    core_zero_diagnosis_payload = {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "split_name": str(args.split_name).strip(),
        "split_applied": bool(split_filter_context.get("split_applied", False))
        if isinstance(split_filter_context, dict)
        else False,
        "execution_range_used": (
            split_filter_context.get("execution_range_used", [])
            if isinstance(split_filter_context, dict)
            else []
        ),
        "evaluation_range_used": (
            split_filter_context.get("evaluation_range_used", [])
            if isinstance(split_filter_context, dict)
            else []
        ),
        "evaluation_range_effective": (
            split_filter_context.get("evaluation_range_effective", [])
            if isinstance(split_filter_context, dict)
            else []
        ),
        "rows_after_filter": int(
            split_filter_context.get("rows_after_filter", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "rows_in_core": int(
            split_filter_context.get("rows_in_core", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "entry_rounds_execution": int(
            split_filter_context.get("entry_rounds_execution", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "entry_rounds_core_proxy": int(
            split_filter_context.get("entry_rounds_core_proxy", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "entry_rounds_core_effective": int(
            split_filter_context.get("entry_rounds_core_effective", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "candidate_total_execution": int(
            split_filter_context.get("candidate_total_execution", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "candidate_total_core_proxy": int(
            split_filter_context.get("candidate_total_core_proxy", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "candidate_total_core_effective": int(
            split_filter_context.get("candidate_total_core_effective", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "total_trades_core_proxy": int(
            split_filter_context.get("total_trades_core_proxy", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "total_round_trips_core_proxy": int(
            split_filter_context.get("total_round_trips_core_proxy", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "total_trades_core_effective": int(
            split_filter_context.get("total_trades_core_effective", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "total_round_trips_core_effective": int(
            split_filter_context.get("total_round_trips_core_effective", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "orders_filled_core_effective": int(
            split_filter_context.get("orders_filled_core_effective", 0)
            if isinstance(split_filter_context, dict)
            else 0
        ),
        "core_zero_detected": bool(
            split_filter_context.get("core_zero_detected", False)
            if isinstance(split_filter_context, dict)
            else False
        ),
        "core_zero_fallback_enabled": bool(
            split_filter_context.get("core_zero_fallback_enabled", False)
            if isinstance(split_filter_context, dict)
            else False
        ),
        "core_zero_fallback_applied": bool(
            split_filter_context.get("core_zero_fallback_applied", False)
            if isinstance(split_filter_context, dict)
            else False
        ),
        "reason_hints": [],
    }
    reason_hints = core_zero_diagnosis_payload.get("reason_hints", [])
    if int(core_zero_diagnosis_payload.get("rows_after_filter", 0)) <= 0:
        reason_hints.append("execution_range_rows_zero")
    if int(core_zero_diagnosis_payload.get("rows_in_core", 0)) <= 0:
        reason_hints.append("evaluation_core_rows_zero")
    if int(core_zero_diagnosis_payload.get("entry_rounds_execution", 0)) <= 0:
        reason_hints.append("entry_loop_not_started_execution")
    if int(core_zero_diagnosis_payload.get("entry_rounds_core_proxy", 0)) <= 0:
        reason_hints.append("entry_loop_not_observed_in_core_proxy")
    if int(core_zero_diagnosis_payload.get("candidate_total_core_proxy", 0)) <= 0:
        reason_hints.append("candidate_zero_in_core_proxy")
    if int(core_zero_diagnosis_payload.get("total_trades_core_proxy", 0)) <= 0:
        reason_hints.append("trade_zero_in_core_proxy")
    core_zero_diagnosis_payload["per_dataset"] = split_eval_profiles
    ensure_parent(core_zero_diagnosis_json)
    with core_zero_diagnosis_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(core_zero_diagnosis_payload, fp, ensure_ascii=False, indent=4)

    ranging_shadow_events_all.sort(
        key=lambda row: (
            max(0, to_int(row.get("ts_ms", 0))) if isinstance(row, dict) else 0,
            str(row.get("dataset", "")) if isinstance(row, dict) else "",
            str(row.get("market", "")) if isinstance(row, dict) else "",
        )
    )
    write_ranging_shadow_events_jsonl(
        output_path=ranging_shadow_signals_jsonl,
        events=ranging_shadow_events_all,
    )

    ranging_shadow_run_meta = build_ranging_shadow_run_meta(
        runtime_config_path=config_path,
        bundle_meta=bundle_meta if isinstance(bundle_meta, dict) else {},
        split_manifest_path=split_manifest_path_value,
        data_dir=data_dir,
        execution_prewarm_hours=float(max(0.0, to_float(args.split_execution_prewarm_hours))),
        split_filter_context=split_filter_context if isinstance(split_filter_context, dict) else {},
        verification_report_json=output_json,
        ranging_shadow_signals_jsonl=ranging_shadow_signals_jsonl,
        semantics_lock_report=semantics_lock_report if isinstance(semantics_lock_report, dict) else {},
    )
    ensure_parent(ranging_shadow_run_meta_json)
    with ranging_shadow_run_meta_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(ranging_shadow_run_meta, fp, ensure_ascii=False, indent=4)

    stage_d_v1_effect_summary = build_stage_d_v1_effect_summary(
        aggregate_diagnostics=aggregate_diagnostics,
        dataset_diagnostics=dataset_diagnostics,
        split_filter_context=split_filter_context if isinstance(split_filter_context, dict) else {},
        split_eval_profiles=split_eval_profiles if isinstance(split_eval_profiles, list) else [],
    )
    ensure_parent(stage_d_v1_effect_summary_json)
    with stage_d_v1_effect_summary_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(stage_d_v1_effect_summary, fp, ensure_ascii=False, indent=4)

    stage_g7_ev_affine_debug = build_stage_g7_ev_affine_debug(
        dataset_diagnostics=dataset_diagnostics,
        split_filter_context=split_filter_context if isinstance(split_filter_context, dict) else {},
        rows=rows,
        aggregate_diagnostics=aggregate_diagnostics,
        bundle_meta=bundle_meta if isinstance(bundle_meta, dict) else {},
    )
    ensure_parent(stage_g7_ev_affine_debug_json)
    with stage_g7_ev_affine_debug_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(stage_g7_ev_affine_debug, fp, ensure_ascii=False, indent=4)
    stage_g8_backend_provenance = build_stage_g8_backend_provenance_report(
        dataset_diagnostics=dataset_diagnostics,
        bundle_meta=bundle_meta if isinstance(bundle_meta, dict) else {},
        split_filter_context=split_filter_context if isinstance(split_filter_context, dict) else {},
    )
    ensure_parent(stage_g8_backend_provenance_json)
    with stage_g8_backend_provenance_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(stage_g8_backend_provenance, fp, ensure_ascii=False, indent=4)

    report = {
        "mode": "verification_adaptive",
        "validation_profile": str(args.validation_profile),
        "pipeline_version": str(pipeline_version),
        "data_mode": str(args.data_mode),
        "sequential_only": True,
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "dataset_count": len(rows),
        "datasets": [x["dataset"] for x in rows],
        "thresholds": {
            "min_profit_factor": float(args.min_profit_factor),
            "min_expectancy_krw": float(args.min_expectancy_krw),
            "max_drawdown_pct": float(args.max_drawdown_pct),
            "min_profitable_ratio": float(args.min_profitable_ratio),
            "min_avg_win_rate_pct": float(args.min_avg_win_rate_pct),
            "min_avg_trades": float(args.min_avg_trades),
        },
        "aggregates": {
            "avg_profit_factor": avg_profit_factor,
            "avg_expectancy_krw": avg_expectancy_krw,
            "avg_win_rate_pct": avg_win_rate_pct,
            "avg_total_trades": avg_total_trades,
            "peak_max_drawdown_pct": peak_max_drawdown_pct,
            "profitable_ratio": profitable_ratio,
            "total_profit_sum_krw": total_profit_sum_krw,
            "avg_fee_krw": avg_fee_krw,
        },
        "threshold_gate": {
            "gate": gate,
            "overall_gate_pass": threshold_gate_pass,
        },
        "adaptive_validation": {
            "thresholds": {
                "min_avg_trades": float(args.min_avg_trades),
                "max_downtrend_loss_per_trade_krw": float(args.max_downtrend_loss_per_trade_krw),
                "max_downtrend_trade_share": float(args.max_downtrend_trade_share),
                "min_uptrend_expectancy_krw": float(args.min_uptrend_expectancy_krw),
                "min_risk_adjusted_score": float(args.min_risk_adjusted_score),
                "max_drawdown_pct": float(args.max_drawdown_pct),
            },
            "risk_adjusted_score_policy": risk_adjusted_score_policy,
            "risk_adjusted_score_guard_policy": risk_adjusted_score_guard_policy,
            "vol_bucket_pct_policy": vol_bucket_pct_policy,
            "risk_adjusted_score_guard_context": risk_score_guard_context,
            "risk_adjusted_score_guard_history": {
                "count": int(
                    len(
                        risk_score_guard_history.get("scores", [])
                        if isinstance(risk_score_guard_history.get("scores", []), list)
                        else []
                    )
                ),
                "sources": (
                    risk_score_guard_history.get("sources", [])
                    if isinstance(risk_score_guard_history.get("sources", []), list)
                    else []
                ),
            },
            "aggregates": adaptive_aggregates,
            "verdict": adaptive_verdict,
            "risk_adjusted_failure_decomposition": risk_adjusted_failure_decomposition,
            "per_dataset": adaptive_dataset_profiles,
        },
        "overall_gate_pass": overall_gate_pass,
        "rows": rows,
        "diagnostics": {
            "aggregate": aggregate_diagnostics,
            "per_dataset": dataset_diagnostics,
            "failure_attribution": failure_attribution,
            "vol_bucket_pct_summary": vol_bucket_pct_artifacts.get("summary", {}),
            "a8_quantile_exec_coverage_debug": vol_bucket_pct_artifacts.get(
                "quantile_exec_coverage_payload",
                {},
            ),
            "cell_root_cause_report": cell_root_cause_report,
            "edge_sign_distribution": edge_sign_distribution,
            "edge_pos_but_no_trade_breakdown": edge_pos_no_trade_breakdown,
            "a10_2_stage_funnel": a10_2_stage_funnel_report,
            "quality_filter_fail_breakdown": quality_filter_fail_breakdown,
            "a20_consistency_check": a20_consistency_check_payload,
            "runtime_cost_semantics_debug": runtime_cost_semantics_debug,
            "semantics_lock_report": semantics_lock_report,
            "ranging_shadow_run_meta": ranging_shadow_run_meta,
            "stage_d_v1_effect_summary": stage_d_v1_effect_summary,
            "stage_g7_ev_affine_debug": stage_g7_ev_affine_debug,
            "stage_g8_backend_provenance": stage_g8_backend_provenance,
            "core_no_signal_summary": core_no_signal_summary,
        },
        "split_filter": split_filter_context,
        "split_eval_profiles": split_eval_profiles,
        "artifacts": {
            "output_csv": str(output_csv),
            "output_json": str(output_json),
            "baseline_report_path": str(baseline_report_path),
            "config_path": str(config_path),
            "runtime_run_dir": str(resolve_runtime_logs_dir(exe_path.parent, runtime_run_dir)),
            "source_config_path": str(source_config_path),
            "runtime_bundle_path": str(bundle_meta.get("bundle_path", "")) if bundle_meta else "",
            "runtime_bundle_version": str(bundle_meta.get("bundle_version", "")) if bundle_meta else "",
            "split_manifest_json": split_manifest_path_value,
            "split_filtered_input_root_dir": str(
                split_filter_context.get("filtered_input_root_dir", "")
                if isinstance(split_filter_context, dict)
                else ""
            ),
            "quarantine_vol_bucket_pct_distribution_json": str(vol_bucket_pct_distribution_json),
            "quarantine_pnl_by_regime_x_vol_fixed_json": str(pnl_by_regime_x_vol_fixed_json),
            "quarantine_pnl_by_regime_x_vol_pct_json": str(pnl_by_regime_x_vol_pct_json),
            "top_loss_cells_regime_vol_pct_json": str(top_loss_cells_regime_vol_pct_json),
            "a8_quantile_exec_coverage_debug_json": str(a8_quantile_exec_coverage_debug_json),
            "cell_root_cause_report_json": str(cell_root_cause_report_json),
            "edge_sign_distribution_json": str(edge_sign_distribution_json),
            "edge_pos_but_no_trade_breakdown_json": str(edge_pos_no_trade_breakdown_json),
            "a10_2_stage_funnel_json": str(a10_2_stage_funnel_json),
            "quality_filter_fail_breakdown_json": str(quality_filter_fail_breakdown_json),
            "a20_consistency_check_json": str(a20_consistency_check_json),
            "runtime_cost_semantics_debug_json": str(runtime_cost_semantics_debug_json),
            "semantics_lock_report_json": str(semantics_lock_report_json),
            "ranging_shadow_signals_jsonl": str(ranging_shadow_signals_jsonl),
            "ranging_shadow_run_meta_json": str(ranging_shadow_run_meta_json),
            "stage_d_v1_effect_summary_json": str(stage_d_v1_effect_summary_json),
            "stage_g7_ev_affine_debug_json": str(stage_g7_ev_affine_debug_json),
            "stage_g8_backend_provenance_json": str(stage_g8_backend_provenance_json),
            "core_no_signal_summary_json": str(core_no_signal_summary_json),
            "core_zero_diagnosis_json": str(core_zero_diagnosis_json),
        },
    }
    split_protocol: Dict[str, Any] = {
        "time_split_applied": bool(args.split_time_based or split_manifest_payload),
        "split_applied": bool(
            split_filter_context.get("split_applied", False)
            if isinstance(split_filter_context, dict)
            else False
        ),
        "split_name": str(args.split_name).strip(),
        "purge_gap_applied": bool(int(args.split_purge_gap_minutes) >= 0),
        "purge_gap_minutes": max(-1, int(args.split_purge_gap_minutes)),
        "note": str(args.split_note).strip(),
    }
    if split_manifest_payload:
        split_protocol["manifest_path"] = split_manifest_path_value
        split_protocol["manifest_protocol"] = split_manifest_payload.get("protocol", {})
        split_protocol["manifest_checks"] = split_manifest_payload.get("checks", {})
        split_protocol["manifest_totals"] = split_manifest_payload.get("totals", {})
        split_protocol["manifest_time_bounds"] = split_manifest_payload.get("time_bounds", {})
        if int(split_protocol.get("purge_gap_minutes", -1)) < 0:
            proto = split_manifest_payload.get("protocol", {})
            if isinstance(proto, dict):
                split_protocol["purge_gap_minutes"] = int(proto.get("purge_gap_minutes", -1))
                split_protocol["purge_gap_applied"] = bool(int(split_protocol.get("purge_gap_minutes", -1)) >= 0)
    if isinstance(split_filter_context, dict):
        if split_filter_context.get("split_range_used"):
            split_protocol["split_range_used"] = split_filter_context.get("split_range_used")
        if split_filter_context.get("execution_range_used"):
            split_protocol["execution_range_used"] = split_filter_context.get("execution_range_used")
        if split_filter_context.get("evaluation_range_used"):
            split_protocol["evaluation_range_used"] = split_filter_context.get("evaluation_range_used")
        if split_filter_context.get("evaluation_range_effective"):
            split_protocol["evaluation_range_effective"] = split_filter_context.get("evaluation_range_effective")
        if split_filter_context.get("cumulative_range_used"):
            split_protocol["cumulative_range_used"] = split_filter_context.get("cumulative_range_used")
        if split_filter_context.get("split_range_mode"):
            split_protocol["split_range_mode"] = split_filter_context.get("split_range_mode")
        if split_filter_context.get("execution_range_mode"):
            split_protocol["execution_range_mode"] = split_filter_context.get("execution_range_mode")
        if split_filter_context.get("execution_prewarm_hours") is not None:
            split_protocol["execution_prewarm_hours"] = float(
                split_filter_context.get("execution_prewarm_hours", 0.0)
            )
        split_protocol["rows_before_filter"] = int(split_filter_context.get("rows_before_filter", 0))
        split_protocol["rows_after_filter"] = int(split_filter_context.get("rows_after_filter", 0))
        split_protocol["rows_in_core"] = int(split_filter_context.get("rows_in_core", 0))
        split_protocol["rows_before_filter_primary_1m"] = int(
            split_filter_context.get("rows_before_filter_primary_1m", 0)
        )
        split_protocol["rows_after_filter_primary_1m"] = int(
            split_filter_context.get("rows_after_filter_primary_1m", 0)
        )
        split_protocol["rows_in_core_primary_1m"] = int(
            split_filter_context.get("rows_in_core_primary_1m", 0)
        )
        split_protocol["trades_before_filter"] = split_filter_context.get("trades_before_filter", None)
        split_protocol["trades_after_filter"] = split_filter_context.get("trades_after_filter", None)
        split_protocol["entry_rounds_execution"] = int(split_filter_context.get("entry_rounds_execution", 0))
        split_protocol["entry_rounds_core_proxy"] = int(split_filter_context.get("entry_rounds_core_proxy", 0))
        split_protocol["entry_rounds_core_effective"] = int(
            split_filter_context.get("entry_rounds_core_effective", 0)
        )
        split_protocol["candidate_total_execution"] = int(
            split_filter_context.get("candidate_total_execution", 0)
        )
        split_protocol["candidate_total_core_proxy"] = int(
            split_filter_context.get("candidate_total_core_proxy", 0)
        )
        split_protocol["candidate_total_core_effective"] = int(
            split_filter_context.get("candidate_total_core_effective", 0)
        )
        split_protocol["pass_total_core_effective"] = int(
            split_filter_context.get("pass_total_core_effective", 0)
        )
        split_protocol["total_trades_core_effective"] = int(
            split_filter_context.get("total_trades_core_effective", 0)
        )
        split_protocol["total_round_trips_core_effective"] = int(
            split_filter_context.get("total_round_trips_core_effective", 0)
        )
        split_protocol["orders_submitted_core_effective"] = int(
            split_filter_context.get("orders_submitted_core_effective", 0)
        )
        split_protocol["orders_filled_core_effective"] = int(
            split_filter_context.get("orders_filled_core_effective", 0)
        )
        split_protocol["stage_candidates_total_core_effective"] = int(
            split_filter_context.get("stage_candidates_total_core_effective", 0)
        )
        split_protocol["stage_candidates_after_quality_topk_core_effective"] = int(
            split_filter_context.get("stage_candidates_after_quality_topk_core_effective", 0)
        )
        split_protocol["stage_candidates_after_manager_core_effective"] = int(
            split_filter_context.get("stage_candidates_after_manager_core_effective", 0)
        )
        split_protocol["stage_candidates_after_portfolio_core_effective"] = int(
            split_filter_context.get("stage_candidates_after_portfolio_core_effective", 0)
        )
        split_protocol["stage_order_block_reason_counts_core_effective"] = (
            split_filter_context.get("stage_order_block_reason_counts_core_effective", {})
            if isinstance(
                split_filter_context.get("stage_order_block_reason_counts_core_effective", {}),
                dict,
            )
            else {}
        )
        split_protocol["core_zero_detected"] = bool(
            split_filter_context.get("core_zero_detected", False)
        )
        split_protocol["core_zero_fallback_applied"] = bool(
            split_filter_context.get("core_zero_fallback_applied", False)
        )
        split_protocol["filtered_input_root_dir"] = str(
            split_filter_context.get("filtered_input_root_dir", "")
        )
        if split_filter_context.get("reason"):
            split_protocol["split_apply_reason"] = str(split_filter_context.get("reason"))
    if core_window_direct_report:
        split_protocol["core_window_direct_available"] = bool(
            core_window_direct_report.get("available", False)
        )
        split_protocol["core_window_direct_reason"] = str(
            core_window_direct_report.get("reason", "")
        ).strip()
    report["protocol_split"] = split_protocol
    if core_window_direct_report:
        report["core_window_direct"] = core_window_direct_report

    if bool(args.phase4_off_on_compare):
        report["phase4_off_on_comparison"] = phase4_off_on_comparison
    baseline_comparison_for_gate = build_baseline_comparison(
        current_report=report,
        baseline_report_path=baseline_report_path,
    )
    contract = baseline_comparison_for_gate.get("non_degradation_contract", {})
    if not isinstance(contract, dict):
        contract = {}
    v2_checks = {
        "threshold_gate_pass": bool(threshold_gate_pass),
        "adaptive_verdict_pass": bool(adaptive_pass),
        "baseline_available": bool(baseline_comparison_for_gate.get("available", False)),
        "dataset_set_comparable": bool(baseline_comparison_for_gate.get("comparable_dataset_set", False)),
        "non_degradation_contract_applied": bool(contract.get("applied", False)),
        "non_degradation_contract_pass": bool(contract.get("all_pass", False)),
    }
    overall_gate_pass = all(bool(x) for x in v2_checks.values())
    report["gate_profile"] = {
        "name": "v2_strict",
        "checks": v2_checks,
        "all_pass": bool(overall_gate_pass),
    }
    report["overall_gate_pass"] = bool(overall_gate_pass)
    report["baseline_comparison"] = build_baseline_comparison(
        current_report=report,
        baseline_report_path=baseline_report_path,
    )

    ensure_parent(output_json)
    with output_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(report, fp, ensure_ascii=False, indent=4)

    print(f"[Verification] dataset_count={len(rows)}")
    print(f"[Verification] data_mode={args.data_mode}")
    print(f"[Verification] validation_profile={args.validation_profile}")
    print(f"[Verification] pipeline_version={pipeline_version}")
    print(f"[Verification] avg_profit_factor={avg_profit_factor}")
    print(f"[Verification] avg_expectancy_krw={avg_expectancy_krw}")
    print(f"[Verification] overall_gate_pass={overall_gate_pass}")
    print(
        "[Verification] adaptive_verdict="
        f"{adaptive_verdict.get('verdict', 'fail')}"
    )
    print(
        "[Verification] primary_non_execution_group="
        f"{failure_attribution.get('primary_non_execution_group', 'unknown')}"
    )
    primary_candidate_component = str(
        failure_attribution.get("primary_candidate_generation_component", "")
    ).strip()
    if primary_candidate_component:
        print(
            "[Verification] primary_candidate_generation_component="
            f"{primary_candidate_component} "
            f"share={failure_attribution.get('primary_candidate_generation_component_share', 0.0)}"
        )
    baseline_comparison = report.get("baseline_comparison", {})
    if isinstance(baseline_comparison, dict) and bool(baseline_comparison.get("available", False)):
        deltas = baseline_comparison.get("deltas", {})
        if not isinstance(deltas, dict):
            deltas = {}
        contract = baseline_comparison.get("non_degradation_contract", {})
        contract_tag = "skip"
        if isinstance(contract, dict) and bool(contract.get("applied", False)):
            contract_tag = "pass" if bool(contract.get("all_pass", False)) else "fail"
        print(
            "[Verification] baseline_delta_pf="
            f"{deltas.get('avg_profit_factor', 0.0)} "
            f"baseline_delta_exp={deltas.get('avg_expectancy_krw', 0.0)} "
            f"baseline_contract={contract_tag}"
        )
    else:
        reason = ""
        if isinstance(baseline_comparison, dict):
            reason = str(baseline_comparison.get("reason", "")).strip()
        print(
            "[Verification] baseline_comparison=unavailable "
            f"reason={reason or 'baseline_report_missing_or_invalid'}"
        )
    vol_bucket_summary = vol_bucket_pct_artifacts.get("summary", {})
    if isinstance(vol_bucket_summary, dict):
        print(
            "[Verification] vol_bucket_pct "
            f"coverage={vol_bucket_summary.get('coverage', 0.0)} "
            f"none_ratio={vol_bucket_summary.get('none_ratio', 0.0)} "
            f"ranging_low_loss_pct={vol_bucket_summary.get('ranging_vol_low_loss_contribution_pct', 0.0)}"
        )
    print(
        "[Verification] edge_sign "
        f"count_total={edge_sign_distribution.get('count_total', 0)} "
        f"edge_neg={edge_sign_distribution.get('count_edge_neg', 0)} "
        f"edge_pos={edge_sign_distribution.get('count_edge_pos', 0)} "
        f"p50_bps={edge_sign_distribution.get('p50_bps', None)} "
        f"edge_pos_but_no_trade={edge_pos_no_trade_breakdown.get('edge_pos_but_no_trade', 0)} "
        f"recommended_case={edge_pos_no_trade_breakdown.get('recommended_case', '')}"
    )
    semantics_runtime = (
        semantics_lock_report.get("runtime", {})
        if isinstance(semantics_lock_report, dict)
        else {}
    )
    if not isinstance(semantics_runtime, dict):
        semantics_runtime = {}
    semantics_cost = (
        semantics_lock_report.get("cost_bps_estimate_summary", {})
        if isinstance(semantics_lock_report, dict)
        else {}
    )
    if not isinstance(semantics_cost, dict):
        semantics_cost = {}
    print(
        "[Verification] semantics_lock "
        f"status={semantics_lock_report.get('status', 'VIOLATION')} "
        f"edge_semantics={semantics_lock_report.get('edge_semantics', 'unknown')} "
        f"root_cost_cfg_true={semantics_runtime.get('root_cost_model_enabled_configured_true_count', 0)} "
        f"phase3_cost_cfg_true={semantics_runtime.get('phase3_cost_model_enabled_configured_true_count', 0)} "
        f"root_cost_eff_true={semantics_runtime.get('root_cost_model_enabled_effective_true_count', 0)} "
        f"phase3_cost_eff_true={semantics_runtime.get('phase3_cost_model_enabled_effective_true_count', 0)} "
        f"cost_bps_mean={semantics_cost.get('mean_bps', 0.0)} "
        f"cost_bps_max={semantics_cost.get('max_bps', 0.0)}"
    )
    print(
        "[Verification] backend_provenance "
        f"backend_cfg={stage_g8_backend_provenance.get('prob_model_backend_configured', 'unknown')} "
        f"backend_effective={stage_g8_backend_provenance.get('prob_model_backend_effective_dominant', 'unknown')} "
        f"lgbm_sha={stage_g8_backend_provenance.get('lgbm_model_sha256', '')} "
        f"cal_mode={stage_g8_backend_provenance.get('calibration_mode', 'off')} "
        f"ev_affine={stage_g8_backend_provenance.get('ev_affine', {}).get('enabled', False)} "
        f"scale={stage_g8_backend_provenance.get('ev_affine', {}).get('scale', 1.0)} "
        f"shift={stage_g8_backend_provenance.get('ev_affine', {}).get('shift', 0.0)}"
    )
    stage_d_shadow = (
        stage_d_v1_effect_summary.get("ranging_shadow", {})
        if isinstance(stage_d_v1_effect_summary, dict)
        else {}
    )
    if not isinstance(stage_d_shadow, dict):
        stage_d_shadow = {}
    top_shadow_market = ""
    top_shadow_market_count = 0
    top_shadow_rows = stage_d_shadow.get("top_shadow_markets", [])
    if isinstance(top_shadow_rows, list) and top_shadow_rows:
        first = top_shadow_rows[0]
        if isinstance(first, dict):
            top_shadow_market = str(first.get("market", "")).strip()
            top_shadow_market_count = max(0, to_int(first.get("count", 0)))
    print(
        "[Verification] ranging_shadow "
        f"shadow_count_total={stage_d_shadow.get('shadow_count_total', 0)} "
        f"would_pass_quality_selection={stage_d_shadow.get('shadow_would_pass_quality_selection_count', 0)} "
        f"would_pass_exec_guard={stage_d_shadow.get('shadow_would_pass_execution_guard_count', 0)} "
        f"top_shadow_market={top_shadow_market or 'none'} "
        f"top_shadow_market_count={top_shadow_market_count}"
    )
    a10_funnel_core = (
        a10_2_stage_funnel_report.get("funnel_summary_core", {})
        if isinstance(a10_2_stage_funnel_report, dict)
        else {}
    )
    a10_order_block_top = (
        a10_2_stage_funnel_report.get("top_order_block_reason_core", [])
        if isinstance(a10_2_stage_funnel_report, dict)
        else []
    )
    a10_stage_counters = (
        a10_2_stage_funnel_report.get("stage_counters_core", {})
        if isinstance(a10_2_stage_funnel_report, dict)
        else {}
    )
    top_reason = ""
    if isinstance(a10_order_block_top, list) and a10_order_block_top:
        first = a10_order_block_top[0]
        if isinstance(first, dict):
            top_reason = str(first.get("reason", "")).strip()
    print(
        "[Verification] a10_2_stage_funnel "
        f"S0={a10_funnel_core.get('S0_candidates_total_core', 0)} "
        f"S1={a10_funnel_core.get('S1_candidates_after_quality_topk_core', 0)} "
        f"S2={a10_funnel_core.get('S2_candidates_after_manager_core', 0)} "
        f"S3={a10_funnel_core.get('S3_candidates_after_portfolio_core', 0)} "
        f"S4={a10_funnel_core.get('S4_orders_submitted_core', 0)} "
        f"S5={a10_funnel_core.get('S5_orders_filled_core', 0)} "
        f"ev_mgr_avg={a10_stage_counters.get('ev_at_manager_pass_core_avg', 0.0)} "
        f"ev_order_avg={a10_stage_counters.get('ev_at_order_submit_check_core_avg', 0.0)} "
        f"ev_mismatch={a10_stage_counters.get('ev_mismatch_count_core', 0)} "
        f"top_order_block={top_reason or 'none'} "
        f"case_hint={a10_2_stage_funnel_report.get('a10_3_case_hint', '')}"
    )
    quality_top3 = (
        quality_filter_fail_breakdown.get("quality_filter_fail_breakdown_top3", [])
        if isinstance(quality_filter_fail_breakdown, dict)
        else []
    )
    quality_top1 = ""
    quality_top1_count = 0
    if isinstance(quality_top3, list) and quality_top3:
        first = quality_top3[0]
        if isinstance(first, dict):
            quality_top1 = str(first.get("reason", "")).strip()
            quality_top1_count = max(0, to_int(first.get("count", 0)))
    print(
        "[Verification] quality_filter_fail_breakdown "
        f"quality_filter_fail_count={quality_filter_fail_breakdown.get('quality_filter_fail_count_in_range', 0)} "
        f"top1={quality_top1 or 'none'} "
        f"top1_count={quality_top1_count} "
        f"recommended_case={quality_filter_fail_breakdown.get('recommended_single_axis_case', '')}"
    )
    if bool(args.phase4_off_on_compare):
        phase4_compare = report.get("phase4_off_on_comparison", {})
        if isinstance(phase4_compare, dict) and bool(phase4_compare.get("available", False)):
            deltas = phase4_compare.get("deltas", {})
            if not isinstance(deltas, dict):
                deltas = {}
            print(
                "[Verification] phase4_off_on_delta "
                f"trades={deltas.get('avg_total_trades', 0.0)} "
                f"pf={deltas.get('avg_profit_factor', 0.0)} "
                f"exp={deltas.get('avg_expectancy_krw', 0.0)} "
                f"mdd={deltas.get('peak_max_drawdown_pct', 0.0)} "
                f"sel_rate={deltas.get('phase4_selection_rate', 0.0)}"
            )
        else:
            reason = ""
            if isinstance(phase4_compare, dict):
                reason = str(phase4_compare.get("reason", "")).strip()
            print(
                "[Verification] phase4_off_on_comparison=unavailable "
                f"reason={reason or 'phase4_off_on_compare_failed'}"
            )
    protocol_split = report.get("protocol_split", {})
    if isinstance(protocol_split, dict) and bool(protocol_split.get("time_split_applied", False)):
        print(
            "[Verification] protocol_split "
            f"name={protocol_split.get('split_name', '') or 'unspecified'} "
            f"purge_gap_minutes={protocol_split.get('purge_gap_minutes', -1)} "
            f"manifest={protocol_split.get('manifest_path', '')}"
        )
        print(
            "[Verification] split_filter "
            f"applied={protocol_split.get('split_applied', False)} "
            f"execution_range={protocol_split.get('execution_range_used', [])} "
            f"evaluation_range={protocol_split.get('evaluation_range_used', [])} "
            f"evaluation_effective={protocol_split.get('evaluation_range_effective', [])} "
            f"rows_before={protocol_split.get('rows_before_filter', 0)} "
            f"rows_after={protocol_split.get('rows_after_filter', 0)} "
            f"rows_in_core={protocol_split.get('rows_in_core', 0)} "
            f"entry_rounds_exec={protocol_split.get('entry_rounds_execution', 0)} "
            f"entry_rounds_core={protocol_split.get('entry_rounds_core_effective', 0)} "
            f"candidate_core={protocol_split.get('candidate_total_core_effective', 0)} "
            f"trades_core={protocol_split.get('total_trades_core_effective', 0)} "
            f"core_fallback={protocol_split.get('core_zero_fallback_applied', False)} "
            f"trades_after={protocol_split.get('trades_after_filter', None)}"
        )
    core_window_direct = report.get("core_window_direct", {})
    if isinstance(core_window_direct, dict):
        if bool(core_window_direct.get("available", False)):
            core_agg = core_window_direct.get("aggregates", {})
            if not isinstance(core_agg, dict):
                core_agg = {}
            print(
                "[Verification] core_window_direct "
                f"split={core_window_direct.get('split_name', '')} "
                f"dataset_count={core_window_direct.get('dataset_count', 0)} "
                f"avg_pf={core_agg.get('avg_profit_factor', 0.0)} "
                f"avg_exp={core_agg.get('avg_expectancy_krw', 0.0)}"
            )
        elif str(core_window_direct.get("reason", "")).strip():
            print(
                "[Verification] core_window_direct=unavailable "
                f"reason={core_window_direct.get('reason', '')}"
            )
    print(f"[Verification] report_json={output_json}")
    return 0 if bool(overall_gate_pass) else 2


if __name__ == "__main__":
    sys.exit(main())
