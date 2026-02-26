#!/usr/bin/env python3
import argparse
from collections import Counter
import copy
import csv
import json
import os
import pathlib
import re
import subprocess
import sys
import uuid
from typing import Any, Dict, List

from _script_common import verification_lock


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
    return {
        "bundle_path": str(bundle_path),
        "bundle_version": bundle_version,
        "pipeline_version": expected_pipeline,
        "supported_markets": sorted(list(supported_markets)),
        "global_fallback_enabled": bool(global_fallback_enabled),
        "export_mode": str(payload.get("export_mode", "")).strip(),
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
            for dataset_path in dataset_paths:
                result = run_backtest(
                    exe_path=exe_path,
                    dataset_path=dataset_path,
                    require_higher_tf_companions=bool(require_higher_tf_companions),
                    disable_adaptive_state_io=bool(disable_adaptive_state_io),
                )
                mode_rows.append(build_verification_row(dataset_path.name, result))
                mode_phase4_diags.append(build_phase4_portfolio_diagnostics(result))

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


def run_backtest(
    exe_path: pathlib.Path,
    dataset_path: pathlib.Path,
    require_higher_tf_companions: bool,
    disable_adaptive_state_io: bool,
) -> Dict[str, Any]:
    cmd = [str(exe_path), "--backtest", str(dataset_path), "--json"]
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
    return parse_backtest_json(proc)


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
        "reject_frontier_fail": max(0, to_int(reason_counts.get("reject_frontier_fail", 0))),
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
            "reject_frontier_fail": int(counters["reject_frontier_fail"]),
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


def build_phase4_portfolio_diagnostics(backtest_result: Dict[str, Any]) -> Dict[str, Any]:
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
    risk_budget_policy = {
        "per_market_cap": round(to_float(raw.get("risk_budget_per_market_cap", 0.0)), 8),
        "gross_cap": round(to_float(raw.get("risk_budget_gross_cap", 0.0)), 8),
        "risk_budget_cap": round(to_float(raw.get("risk_budget_cap", 0.0)), 8),
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

    return {
        "enabled": bool(enabled or counters["candidates_total"] > 0 or counters["selected_total"] > 0),
        "flags": flags,
        "allocator_policy": allocator_policy,
        "risk_budget_policy": risk_budget_policy,
        "drawdown_policy": drawdown_policy,
        "correlation_policy": correlation_policy,
        "execution_policy": execution_policy,
        "counters": counters,
        "selection_breakdown": {
            "candidates_total": int(counters["candidates_total"]),
            "selected_total": int(counters["selected_total"]),
            "selection_rate": float(counters["selection_rate"]),
            "rejected_by_budget": int(counters["rejected_by_budget"]),
            "rejected_by_cluster_cap": int(counters["rejected_by_cluster_cap"]),
            "rejected_by_correlation_penalty": int(counters["rejected_by_correlation_penalty"]),
            "rejected_by_execution_cap": int(counters["rejected_by_execution_cap"]),
            "rejected_by_drawdown_governor": int(counters["rejected_by_drawdown_governor"]),
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
    manager_share = to_float(component_shares.get("filtered_out_by_manager_share", 0.0))
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
                "trigger": "filtered_out_by_manager share >= 0.10",
                "focus": "manager prefilter staging",
                "target_metric": "candidate_generation_components.filtered_out_by_manager_share",
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
    max_drawdown_pct: float,
    downtrend_loss_per_trade_krw: float,
    downtrend_trade_share: float,
    total_trades: int,
) -> Dict[str, float]:
    expectancy_term = bounded(float(expectancy_krw) / 10.0, -3.0, 3.0)
    profit_factor_term = bounded((float(profit_factor) - 1.0) * 2.0, -2.0, 2.0)
    drawdown_penalty = bounded(float(max_drawdown_pct) / 6.0, 0.0, 3.0)
    downtrend_loss_penalty = bounded(float(downtrend_loss_per_trade_krw) / 8.0, 0.0, 3.0)
    downtrend_share_penalty = (
        bounded((float(downtrend_trade_share) - 0.50) * 4.0, 0.0, 2.0)
        if float(downtrend_trade_share) > 0.50
        else 0.0
    )
    low_trade_penalty = 0.5 if int(total_trades) < 10 else 0.0
    score = (
        expectancy_term
        + profit_factor_term
        - drawdown_penalty
        - downtrend_loss_penalty
        - downtrend_share_penalty
        - low_trade_penalty
    )
    return {
        "expectancy_term": round(expectancy_term, 4),
        "profit_factor_term": round(profit_factor_term, 4),
        "drawdown_penalty": round(drawdown_penalty, 4),
        "downtrend_loss_penalty": round(downtrend_loss_penalty, 4),
        "downtrend_share_penalty": round(downtrend_share_penalty, 4),
        "low_trade_penalty": round(low_trade_penalty, 4),
        "score": round(score, 4),
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


def build_dataset_diagnostics(dataset_name: str, backtest_result: Dict[str, Any]) -> Dict[str, Any]:
    entry_funnel = backtest_result.get("entry_funnel", {})
    if not isinstance(entry_funnel, dict):
        entry_funnel = {}
    shadow_funnel_raw = backtest_result.get("shadow_funnel", {})
    if not isinstance(shadow_funnel_raw, dict):
        shadow_funnel_raw = {}

    entry_rounds = max(0, to_int(entry_funnel.get("entry_rounds", 0)))
    skipped_due_to_open_position = max(0, to_int(entry_funnel.get("skipped_due_to_open_position", 0)))
    no_signal_generated = max(0, to_int(entry_funnel.get("no_signal_generated", 0)))
    filtered_out_by_manager = max(0, to_int(entry_funnel.get("filtered_out_by_manager", 0)))
    filtered_out_by_policy = max(0, to_int(entry_funnel.get("filtered_out_by_policy", 0)))
    no_best_signal = max(0, to_int(entry_funnel.get("no_best_signal", 0)))
    blocked_pattern_gate = max(0, to_int(entry_funnel.get("blocked_pattern_gate", 0)))
    blocked_rr_rebalance = max(0, to_int(entry_funnel.get("blocked_rr_rebalance", 0)))
    blocked_risk_gate = max(0, to_int(entry_funnel.get("blocked_risk_gate", 0)))
    blocked_risk_manager = max(0, to_int(entry_funnel.get("blocked_risk_manager", 0)))
    blocked_min_order_or_capital = max(0, to_int(entry_funnel.get("blocked_min_order_or_capital", 0)))
    blocked_order_sizing = max(0, to_int(entry_funnel.get("blocked_order_sizing", 0)))
    entries_executed = max(0, to_int(entry_funnel.get("entries_executed", 0)))
    shadow_rounds = max(0, to_int(shadow_funnel_raw.get("rounds", 0)))
    shadow_primary_generated_signals = max(
        0, to_int(shadow_funnel_raw.get("primary_generated_signals", 0))
    )
    shadow_primary_after_manager_filter = max(
        0, to_int(shadow_funnel_raw.get("primary_after_manager_filter", 0))
    )
    shadow_shadow_after_manager_filter = max(
        0, to_int(shadow_funnel_raw.get("shadow_after_manager_filter", 0))
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

    candidate_generation = (
        no_signal_generated
        + filtered_out_by_manager
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
    phase3_diag = build_phase3_diagnostics_v2(reason_counts)
    phase4_diag = build_phase4_portfolio_diagnostics(backtest_result)
    no_signal_pattern_counts = normalized_reason_counts(backtest_result.get("no_signal_pattern_counts", {}))
    edge_gap_bucket_counts = normalized_reason_counts(
        backtest_result.get("entry_quality_edge_gap_buckets", {})
    )
    no_signal_reason_counts = filter_reason_counts_by_prefix(
        reason_counts,
        ["foundation_no_signal_", "no_signal_", "probabilistic_"],
        exclude_exact=["no_signal_generated"],
    )
    manager_prefilter_reason_counts = filter_reason_counts_by_prefix(
        reason_counts,
        ["filtered_out_by_manager"],
    )
    policy_prefilter_reason_counts = filter_reason_counts_by_prefix(
        reason_counts,
        ["filtered_out_by_policy"],
    )
    candidate_components = {
        "no_signal_generated": int(no_signal_generated),
        "filtered_out_by_manager": int(filtered_out_by_manager),
        "filtered_out_by_policy": int(filtered_out_by_policy),
        "no_best_signal": int(no_best_signal),
    }
    candidate_total = max(0, sum(candidate_components.values()))
    candidate_denom = float(max(1, candidate_total))
    candidate_component_shares = {
        "no_signal_generated_share": round(float(no_signal_generated) / candidate_denom, 4),
        "filtered_out_by_manager_share": round(float(filtered_out_by_manager) / candidate_denom, 4),
        "filtered_out_by_policy_share": round(float(filtered_out_by_policy) / candidate_denom, 4),
        "no_best_signal_share": round(float(no_best_signal) / candidate_denom, 4),
    }
    ordered_candidate_components = sorted(
        candidate_components.items(),
        key=lambda item: (-int(item[1]), item[0]),
    )
    primary_candidate_component_name, primary_candidate_component_count = ordered_candidate_components[0]
    top_no_signal_reason_rows = top_reason_rows(no_signal_reason_counts, limit=8)
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
    top_loss_trade_samples = build_top_loss_trade_samples(
        backtest_result.get("trade_history_samples", []),
        limit=10,
    )

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
            "no_signal_reason_counts": no_signal_reason_counts,
            "manager_prefilter_reason_counts": manager_prefilter_reason_counts,
            "policy_prefilter_reason_counts": policy_prefilter_reason_counts,
            "top_no_signal_reasons": top_no_signal_reason_rows,
            "top_manager_prefilter_reasons": top_manager_prefilter_reason_rows,
            "top_policy_prefilter_reasons": top_policy_prefilter_reason_rows,
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
        "phase3_diagnostics_v2": phase3_diag,
        "phase4_portfolio_diagnostics": phase4_diag,
        "shadow_funnel": {
            "rounds": int(shadow_rounds),
            "primary_generated_signals": int(shadow_primary_generated_signals),
            "primary_after_manager_filter": int(shadow_primary_after_manager_filter),
            "shadow_after_manager_filter": int(shadow_shadow_after_manager_filter),
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
        "filtered_out_by_manager": 0,
        "filtered_out_by_policy": 0,
        "no_best_signal": 0,
    }
    aggregate_no_signal_reasons: Dict[str, int] = {}
    aggregate_manager_prefilter_reasons: Dict[str, int] = {}
    aggregate_policy_prefilter_reasons: Dict[str, int] = {}
    aggregate_post_entry_telemetry: Dict[str, Any] = {
        "adaptive_stop_updates": 0,
        "adaptive_tp_recalibration_updates": 0,
        "adaptive_partial_ratio_samples": 0,
        "adaptive_partial_ratio_histogram": {
            "0.35_0.44": 0,
            "0.45_0.54": 0,
            "0.55_0.64": 0,
            "0.65_0.74": 0,
            "0.75_0.80": 0,
        },
    }
    aggregate_shadow: Dict[str, Any] = {
        "rounds": 0,
        "primary_generated_signals": 0,
        "primary_after_manager_filter": 0,
        "shadow_after_manager_filter": 0,
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
        "reject_frontier_fail",
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
            aggregate_shadow["primary_after_manager_filter"] += max(
                0,
                to_int(shadow_funnel.get("primary_after_manager_filter", 0)),
            )
            aggregate_shadow["shadow_after_manager_filter"] += max(
                0,
                to_int(shadow_funnel.get("shadow_after_manager_filter", 0)),
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
        "primary_after_manager_filter": int(aggregate_shadow["primary_after_manager_filter"]),
        "shadow_after_manager_filter": int(aggregate_shadow["shadow_after_manager_filter"]),
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
        "filtered_out_by_manager_share": round(
            float(aggregate_candidate_components["filtered_out_by_manager"]) / candidate_component_denom,
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
            "reject_frontier_fail": int(aggregate_phase3_counters.get("reject_frontier_fail", 0)),
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
    aggregate_phase4_diag = {
        "enabled": bool(
            aggregate_phase4_counters.get("candidates_total", 0) > 0
            or aggregate_phase4_counters.get("selected_total", 0) > 0
        ),
        "flags": aggregate_phase4_flags,
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
            "top_no_signal_reasons": aggregate_top_no_signal_reasons,
            "top_manager_prefilter_reasons": aggregate_top_manager_prefilter_reasons,
            "top_policy_prefilter_reasons": aggregate_top_policy_prefilter_reasons,
            "ab_playbook_candidates": aggregate_ab_playbook,
        },
        "top_non_execution_group_vote_counts": top_group_votes,
        "post_entry_risk_telemetry": aggregate_post_entry_telemetry,
        "phase3_diagnostics_v2": aggregate_phase3_diag,
        "phase4_portfolio_diagnostics": aggregate_phase4_diag,
        "shadow_funnel": aggregate_shadow_summary,
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
        "adaptive_partial_ratio_histogram": histogram,
    }


def build_adaptive_dataset_profile(dataset_name: str, backtest_result: Dict[str, Any]) -> Dict[str, Any]:
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
    max_drawdown_pct = max(0.0, to_float(backtest_result.get("max_drawdown", 0.0)) * 100.0)

    risk_score_components = compute_risk_adjusted_score_components(
        expectancy_krw=expectancy_krw,
        profit_factor=profit_factor,
        max_drawdown_pct=max_drawdown_pct,
        downtrend_loss_per_trade_krw=downtrend_loss_per_trade,
        downtrend_trade_share=downtrend_trade_share,
        total_trades=total_trades,
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
                "drawdown_penalty": 0.0,
                "downtrend_loss_penalty": 0.0,
                "downtrend_share_penalty": 0.0,
                "low_trade_penalty": 0.0,
            },
            "loss_tail_aggregate": {
                "negative_profit_abs_krw": 0.0,
                "avg_top3_loss_concentration": 0.0,
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
    partial_ratio_histogram = {
        "0.35_0.44": 0,
        "0.45_0.54": 0,
        "0.55_0.64": 0,
        "0.65_0.74": 0,
        "0.75_0.80": 0,
    }
    score_expectancy_terms: List[float] = []
    score_profit_factor_terms: List[float] = []
    score_drawdown_penalties: List[float] = []
    score_downtrend_loss_penalties: List[float] = []
    score_downtrend_share_penalties: List[float] = []
    score_low_trade_penalties: List[float] = []
    top3_concentrations: List[float] = []
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

        hist = telemetry.get("adaptive_partial_ratio_histogram", {})
        if isinstance(hist, dict):
            for key in partial_ratio_histogram:
                partial_ratio_histogram[key] += max(0, to_int(hist.get(key, 0)))

        components = profile.get("risk_adjusted_score_components", {})
        if isinstance(components, dict):
            score_expectancy_terms.append(to_float(components.get("expectancy_term", 0.0)))
            score_profit_factor_terms.append(to_float(components.get("profit_factor_term", 0.0)))
            score_drawdown_penalties.append(to_float(components.get("drawdown_penalty", 0.0)))
            score_downtrend_loss_penalties.append(to_float(components.get("downtrend_loss_penalty", 0.0)))
            score_downtrend_share_penalties.append(to_float(components.get("downtrend_share_penalty", 0.0)))
            score_low_trade_penalties.append(to_float(components.get("low_trade_penalty", 0.0)))

        tail = profile.get("loss_tail_decomposition", {})
        if isinstance(tail, dict):
            aggregate_negative_profit_abs += max(0.0, to_float(tail.get("negative_profit_abs_krw", 0.0)))
            top3_concentrations.append(max(0.0, to_float(tail.get("top3_loss_concentration", 0.0))))
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
        "adaptive_partial_ratio_histogram": partial_ratio_histogram,
        "avg_risk_adjusted_score_components": {
            "expectancy_term": round(safe_avg(score_expectancy_terms), 4),
            "profit_factor_term": round(safe_avg(score_profit_factor_terms), 4),
            "drawdown_penalty": round(safe_avg(score_drawdown_penalties), 4),
            "downtrend_loss_penalty": round(safe_avg(score_downtrend_loss_penalties), 4),
            "downtrend_share_penalty": round(safe_avg(score_downtrend_share_penalties), 4),
            "low_trade_penalty": round(safe_avg(score_low_trade_penalties), 4),
        },
        "loss_tail_aggregate": {
            "negative_profit_abs_krw": round(aggregate_negative_profit_abs, 4),
            "avg_top3_loss_concentration": round(safe_avg(top3_concentrations), 4),
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

    checks = {
        "sample_size_guard_pass": float(avg_total_trades) >= float(effective_min_avg_trades),
        "drawdown_guard_pass": float(peak_max_drawdown_pct) <= float(max_drawdown_pct_limit),
        "downtrend_loss_guard_pass": to_float(adaptive_aggregates.get("avg_downtrend_loss_per_trade_krw", 0.0))
        <= float(max_downtrend_loss_per_trade_krw),
        "downtrend_trade_share_guard_pass": to_float(adaptive_aggregates.get("avg_downtrend_trade_share", 0.0))
        <= float(max_downtrend_trade_share),
        "uptrend_expectancy_guard_pass": to_float(adaptive_aggregates.get("avg_uptrend_expectancy_krw", 0.0))
        >= float(min_uptrend_expectancy_krw),
        "risk_adjusted_score_guard_pass": to_float(adaptive_aggregates.get("avg_risk_adjusted_score", 0.0))
        >= float(min_risk_adjusted_score),
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
        "risk_adjusted_score_guard_pass",
    ]
    hard_fail = any(not bool(checks.get(key, True)) for key in hard_fail_keys)

    if hard_fail:
        verdict = "fail"
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
        },
        "context": {
            "avg_generated_signals": round(avg_generated_signals, 4),
            "avg_opportunity_conversion": round(avg_opportunity_conversion, 4),
            "avg_primary_candidate_conversion": round(avg_primary_candidate_conversion, 4),
            "avg_shadow_candidate_conversion": round(avg_shadow_candidate_conversion, 4),
            "avg_shadow_candidate_supply_lift": round(avg_shadow_candidate_supply_lift, 4),
            "low_opportunity_observed": bool(low_opportunity_observed),
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
    avg_score = to_float(adaptive_aggregates.get("avg_risk_adjusted_score", 0.0))
    components = adaptive_aggregates.get("avg_risk_adjusted_score_components", {})
    if not isinstance(components, dict):
        components = {}

    expectancy_term = to_float(components.get("expectancy_term", 0.0))
    profit_factor_term = to_float(components.get("profit_factor_term", 0.0))
    drawdown_penalty = to_float(components.get("drawdown_penalty", 0.0))
    downtrend_loss_penalty = to_float(components.get("downtrend_loss_penalty", 0.0))
    downtrend_share_penalty = to_float(components.get("downtrend_share_penalty", 0.0))
    low_trade_penalty = to_float(components.get("low_trade_penalty", 0.0))
    reconstructed_score = (
        expectancy_term
        + profit_factor_term
        - drawdown_penalty
        - downtrend_loss_penalty
        - downtrend_share_penalty
        - low_trade_penalty
    )

    penalty_rows = [
        {"name": "drawdown_penalty", "value": round(drawdown_penalty, 4)},
        {"name": "downtrend_loss_penalty", "value": round(downtrend_loss_penalty, 4)},
        {"name": "downtrend_share_penalty", "value": round(downtrend_share_penalty, 4)},
        {"name": "low_trade_penalty", "value": round(low_trade_penalty, 4)},
    ]
    penalty_rows = [x for x in penalty_rows if to_float(x.get("value", 0.0)) > 0.0]
    penalty_rows.sort(key=lambda x: (-to_float(x["value"]), str(x["name"])))

    positive_rows = [
        {"name": "expectancy_term", "value": round(expectancy_term, 4)},
        {"name": "profit_factor_term", "value": round(profit_factor_term, 4)},
    ]
    positive_rows.sort(key=lambda x: (-to_float(x["value"]), str(x["name"])))

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

    score_gap = float(min_risk_adjusted_score) - float(avg_score)
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
        "active": not risk_guard_pass,
        "risk_adjusted_score_guard_pass": bool(risk_guard_pass),
        "min_risk_adjusted_score": round(float(min_risk_adjusted_score), 4),
        "avg_risk_adjusted_score": round(float(avg_score), 4),
        "score_gap_to_threshold": round(float(score_gap), 4),
        "score_components": {
            "expectancy_term": round(expectancy_term, 4),
            "profit_factor_term": round(profit_factor_term, 4),
            "drawdown_penalty": round(drawdown_penalty, 4),
            "downtrend_loss_penalty": round(downtrend_loss_penalty, 4),
            "downtrend_share_penalty": round(downtrend_share_penalty, 4),
            "low_trade_penalty": round(low_trade_penalty, 4),
            "reconstructed_score": round(reconstructed_score, 4),
        },
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
    parser.add_argument("--exe-path", default=r".\build\Release\AutoLifeTrading.exe")
    parser.add_argument("--config-path", default=r".\build\Release\config\config.json")
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
    args = parser.parse_args(argv)

    exe_path = resolve_path(args.exe_path, "Executable")
    config_path = resolve_path(args.config_path, "Build config", must_exist=False)
    source_config_path = resolve_path(args.source_config_path, "Source config")
    data_dir = resolve_path(args.data_dir, "Data directory")
    output_csv = resolve_path(args.output_csv, "Output csv", must_exist=False)
    output_json = resolve_path(args.output_json, "Output json", must_exist=False)
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
    if not bool(args.skip_probabilistic_coverage_check):
        if str(bundle_path).strip() not in ("", ".", ".."):
            if not bundle_path.exists():
                raise FileNotFoundError(
                    "Probabilistic runtime bundle configured but missing: "
                    f"{bundle_path}"
                )
            bundle_meta = load_supported_markets_from_runtime_bundle(bundle_path)
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

    if bool(args.require_higher_tf_companions):
        for dataset_path in dataset_paths:
            if not is_upbit_primary_1m_dataset(dataset_path):
                raise RuntimeError(
                    "When --require-higher-tf-companions is enabled, "
                    "only upbit_*_1m_*.csv datasets are allowed: "
                    f"{dataset_path.name}"
                )
            if not has_higher_tf_companions(data_dir, dataset_path):
                raise RuntimeError(
                    "Missing companion timeframe csv (5m/15m/60m/240m) for dataset: "
                    f"{dataset_path.name}"
                )

    rows: List[Dict[str, Any]] = []
    dataset_diagnostics: List[Dict[str, Any]] = []
    adaptive_dataset_profiles: List[Dict[str, Any]] = []
    phase4_off_on_comparison: Dict[str, Any] = {}
    with verification_lock(
        lock_path,
        timeout_sec=int(args.verification_lock_timeout_sec),
        stale_sec=int(args.verification_lock_stale_sec),
    ):
        for dataset_path in dataset_paths:
            result = run_backtest(
                exe_path=exe_path,
                dataset_path=dataset_path,
                require_higher_tf_companions=bool(args.require_higher_tf_companions),
                disable_adaptive_state_io=not bool(args.enable_adaptive_state_io),
            )
            rows.append(build_verification_row(dataset_path.name, result))
            dataset_diagnostics.append(
                build_dataset_diagnostics(dataset_name=dataset_path.name, backtest_result=result)
            )
            adaptive_dataset_profiles.append(
                build_adaptive_dataset_profile(dataset_name=dataset_path.name, backtest_result=result)
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
    failure_attribution = build_failure_attribution(
        gate=gate,
        avg_total_trades=avg_total_trades,
        min_avg_trades=float(args.min_avg_trades),
        aggregate_diagnostics=aggregate_diagnostics,
    )
    adaptive_aggregates = aggregate_adaptive_validation(adaptive_dataset_profiles)
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
    )
    risk_adjusted_failure_decomposition = build_risk_adjusted_failure_decomposition(
        adaptive_aggregates=adaptive_aggregates,
        adaptive_verdict=adaptive_verdict,
        min_risk_adjusted_score=float(args.min_risk_adjusted_score),
    )
    adaptive_pass = str(adaptive_verdict.get("verdict", "fail")) == "pass"
    overall_gate_pass = adaptive_pass

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
        },
        "artifacts": {
            "output_csv": str(output_csv),
            "output_json": str(output_json),
            "baseline_report_path": str(baseline_report_path),
            "config_path": str(config_path),
            "source_config_path": str(source_config_path),
            "runtime_bundle_path": str(bundle_meta.get("bundle_path", "")) if bundle_meta else "",
            "runtime_bundle_version": str(bundle_meta.get("bundle_version", "")) if bundle_meta else "",
        },
    }
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
    print(f"[Verification] report_json={output_json}")
    return 0 if bool(overall_gate_pass) else 2


if __name__ == "__main__":
    sys.exit(main())
