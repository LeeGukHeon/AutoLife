#!/usr/bin/env python3
import argparse
import bisect
from collections import Counter, deque
import copy
import csv
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import uuid
from typing import Any, Dict, List

from _script_common import verification_lock


VOL_BUCKET_LOW = "LOW"
VOL_BUCKET_MID = "MID"
VOL_BUCKET_HIGH = "HIGH"
VOL_BUCKET_NONE = "NONE"
VOL_BUCKET_ORDER = [VOL_BUCKET_LOW, VOL_BUCKET_MID, VOL_BUCKET_HIGH, VOL_BUCKET_NONE]


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
    return {
        "bundle_path": str(bundle_path),
        "bundle_version": bundle_version,
        "pipeline_version": expected_pipeline,
        "supported_markets": sorted(list(supported_markets)),
        "global_fallback_enabled": bool(global_fallback_enabled),
        "export_mode": str(payload.get("export_mode", "")).strip(),
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
    parsed = parse_backtest_json(proc)
    parsed["phase3_pass_ev_samples"] = load_policy_decision_ev_samples(exe_path.parent)
    parsed["policy_decision_volatility_samples"] = load_policy_decision_volatility_samples(exe_path.parent)
    return parsed


def load_policy_decision_ev_samples(exe_dir: pathlib.Path) -> List[Dict[str, Any]]:
    artifact_path = (exe_dir / "logs" / "policy_decisions_backtest.jsonl").resolve()
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
                    samples.append(
                        {
                            "ts": int(ts_value),
                            "market": str(row.get("market", "")).strip(),
                            "strategy": str(row.get("strategy", "")).strip(),
                            "selected": bool(row.get("selected", False)),
                            "reason": str(row.get("reason", "")).strip(),
                            "expected_edge_after_cost_pct": round(float(expected_pct), 10),
                            "expected_edge_after_cost_bps": round(float(expected_bps), 6),
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
    sample_cap: int = 120000,
) -> List[Dict[str, Any]]:
    artifact_path = (exe_dir / "logs" / "policy_decisions_backtest.jsonl").resolve()
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
            p33 = raw.get("p33", None)
            p67 = raw.get("p67", None)
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
        rows.append(
            {
                "market": market,
                "ts": int(ts_value),
                "entry_time": int(entry_time),
                "exit_time": int(exit_time),
                "regime": regime,
                "volatility": float(volatility) if math.isfinite(volatility) else float("nan"),
                "profit_loss_krw": round(to_float(raw.get("profit_loss_krw", 0.0)), 8),
                "profit_loss_pct": round(to_float(raw.get("profit_loss_pct", 0.0)), 10),
                "vol_bucket_fixed": fixed_bucket,
                "vol_bucket_pct": VOL_BUCKET_NONE,
                "vol_bucket_pct_ref_ts": 0,
                "vol_bucket_pct_ref_p33": None,
                "vol_bucket_pct_ref_p67": None,
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
) -> Dict[str, Any]:
    resolved_policy = resolve_vol_bucket_pct_policy(policy if isinstance(policy, dict) else {})
    rolling_samples = build_rolling_percentile_vol_bucket_samples(
        backtest_result.get("policy_decision_volatility_samples", []),
        resolved_policy,
    )
    trade_rows = build_trade_rows_for_vol_bucket_diagnostics(
        backtest_result.get("trade_history_samples", []),
        fixed_low_cut=float(resolved_policy.get("fixed_low_cut", 1.8)),
        fixed_high_cut=float(resolved_policy.get("fixed_high_cut", 3.5)),
    )
    trade_rows = attach_percentile_bucket_to_trade_rows(trade_rows, rolling_samples)
    distribution = build_vol_bucket_pct_distribution_summary(rolling_samples)
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
    return {
        "dataset": str(dataset_name),
        "enabled": bool(rolling_samples or trade_rows),
        "policy": resolved_policy,
        "distribution": distribution,
        "pnl_fixed": pnl_fixed,
        "pnl_pct": pnl_pct,
        "top_loss_cells_regime_vol_pct": top_loss_cells,
        "rolling_samples": rolling_samples,
        "trade_rows": trade_rows,
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
    combined_rolling_samples: List[Dict[str, Any]] = []
    combined_trade_rows: List[Dict[str, Any]] = []
    per_dataset_distribution: List[Dict[str, Any]] = []
    per_dataset_fixed: List[Dict[str, Any]] = []
    per_dataset_pct: List[Dict[str, Any]] = []

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
        rolling_samples = profile.get("rolling_samples", [])
        if isinstance(rolling_samples, list):
            for row in rolling_samples:
                if not isinstance(row, dict):
                    continue
                combined_rolling_samples.append({"dataset": dataset, **row})
        trade_rows = profile.get("trade_rows", [])
        if isinstance(trade_rows, list):
            for row in trade_rows:
                if not isinstance(row, dict):
                    continue
                combined_trade_rows.append({"dataset": dataset, **row})

    aggregate_distribution = build_vol_bucket_pct_distribution_summary(combined_rolling_samples)
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
    dataset_paths: List[pathlib.Path],
    require_higher_tf_companions: bool,
    disable_adaptive_state_io: bool,
    phase4_market_cluster_map: Dict[str, str],
    risk_adjusted_score_policy: Dict[str, Any],
) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    dataset_diagnostics: List[Dict[str, Any]] = []
    adaptive_dataset_profiles: List[Dict[str, Any]] = []
    for dataset_path in dataset_paths:
        result = run_backtest(
            exe_path=exe_path,
            dataset_path=dataset_path,
            require_higher_tf_companions=bool(require_higher_tf_companions),
            disable_adaptive_state_io=bool(disable_adaptive_state_io),
        )
        rows.append(build_verification_row(dataset_path.name, result))
        dataset_diagnostics.append(
            build_dataset_diagnostics(
                dataset_name=dataset_path.name,
                backtest_result=result,
                phase4_market_cluster_map=phase4_market_cluster_map,
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
            "limit_mode": "absolute_threshold_legacy",
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


def build_dataset_diagnostics(
    dataset_name: str,
    backtest_result: Dict[str, Any],
    phase4_market_cluster_map: Dict[str, str],
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
    phase3_pass_ev_distribution = build_phase3_pass_ev_distribution(
        backtest_result.get("phase3_pass_ev_samples", []),
        sample_cap=5000,
    )
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
        "phase3_pass_ev_distribution": phase3_pass_ev_distribution,
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
    aggregate_phase3_pass_ev_samples_bps: List[float] = []
    aggregate_phase3_pass_ev_selected_samples_bps: List[float] = []
    aggregate_phase3_pass_ev_source_counts: Dict[str, int] = {}
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
            risk_guard_detail.get("limit_mode", "absolute_threshold_legacy")
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
    args = parser.parse_args(argv)

    exe_path = resolve_path(args.exe_path, "Executable")
    config_path = resolve_path(args.config_path, "Build config", must_exist=False)
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
    vol_bucket_pct_dataset_profiles: List[Dict[str, Any]] = []
    phase4_off_on_comparison: Dict[str, Any] = {}
    core_window_direct_report: Dict[str, Any] = {}
    split_manifest_payload: Dict[str, Any] = {}
    split_manifest_path_value = str(args.split_manifest_json).strip()
    if split_manifest_path_value:
        split_manifest_path = resolve_path(split_manifest_path_value, "Split manifest", must_exist=True)
        loaded_manifest = load_report_json(split_manifest_path)
        if isinstance(loaded_manifest, dict):
            split_manifest_payload = loaded_manifest

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
                build_dataset_diagnostics(
                    dataset_name=dataset_path.name,
                    backtest_result=result,
                    phase4_market_cluster_map=phase4_market_cluster_map,
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
                    dataset_paths=list(core_paths_info.get("dataset_paths", [])),
                    require_higher_tf_companions=bool(args.require_higher_tf_companions),
                    disable_adaptive_state_io=not bool(args.enable_adaptive_state_io),
                    phase4_market_cluster_map=phase4_market_cluster_map,
                    risk_adjusted_score_policy=risk_adjusted_score_policy,
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
        },
        "artifacts": {
            "output_csv": str(output_csv),
            "output_json": str(output_json),
            "baseline_report_path": str(baseline_report_path),
            "config_path": str(config_path),
            "source_config_path": str(source_config_path),
            "runtime_bundle_path": str(bundle_meta.get("bundle_path", "")) if bundle_meta else "",
            "runtime_bundle_version": str(bundle_meta.get("bundle_version", "")) if bundle_meta else "",
            "split_manifest_json": split_manifest_path_value,
            "quarantine_vol_bucket_pct_distribution_json": str(vol_bucket_pct_distribution_json),
            "quarantine_pnl_by_regime_x_vol_fixed_json": str(pnl_by_regime_x_vol_fixed_json),
            "quarantine_pnl_by_regime_x_vol_pct_json": str(pnl_by_regime_x_vol_pct_json),
            "top_loss_cells_regime_vol_pct_json": str(top_loss_cells_regime_vol_pct_json),
        },
    }
    split_protocol: Dict[str, Any] = {
        "time_split_applied": bool(args.split_time_based or split_manifest_payload),
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
