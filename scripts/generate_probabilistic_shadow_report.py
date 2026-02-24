#!/usr/bin/env python3
import argparse
import hashlib
import json
from collections import Counter
from datetime import datetime, timezone
from typing import Any, Dict, List, Tuple

from _script_common import dump_json, read_nonempty_lines, resolve_repo_path


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate probabilistic shadow report by comparing live/backtest policy "
            "decision logs (fail-closed evidence format)."
        )
    )
    parser.add_argument(
        "--live-decision-log-jsonl",
        default=r".\build\Release\logs\policy_decisions.jsonl",
    )
    parser.add_argument(
        "--backtest-decision-log-jsonl",
        default=r".\build\Release\logs\policy_decisions_backtest.jsonl",
    )
    parser.add_argument(
        "--runtime-bundle-json",
        default="",
        help="Optional shared runtime bundle path used by both live/backtest traces.",
    )
    parser.add_argument(
        "--live-runtime-bundle-json",
        default="",
        help="Optional live runtime bundle path (overrides shared bundle path for live side).",
    )
    parser.add_argument(
        "--backtest-runtime-bundle-json",
        default="",
        help="Optional backtest runtime bundle path (overrides shared bundle path for backtest side).",
    )
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("auto", "v1", "v2"),
        default="auto",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\probabilistic_shadow_report_latest.json",
    )
    parser.add_argument("--strict", action="store_true")
    return parser.parse_args(argv)


def to_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    return str(value).strip().lower() in ("1", "true", "yes", "y", "on")


def to_int(value: Any) -> int:
    try:
        return int(float(value))
    except Exception:
        return 0


def sha256_file(path_value) -> str:
    h = hashlib.sha256()
    with path_value.open("rb") as fp:
        while True:
            chunk = fp.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def infer_pipeline_from_bundle_version(version: str) -> str:
    v = str(version or "").strip().lower()
    if "v2" in v:
        return "v2"
    return "v1"


def load_bundle_meta(path_value) -> Dict[str, Any]:
    out: Dict[str, Any] = {
        "path": str(path_value) if path_value else "",
        "exists": False,
        "sha256": "",
        "version": "",
        "pipeline_version": "",
        "json_valid": False,
    }
    if path_value is None:
        return out
    if not path_value.exists():
        return out
    out["exists"] = True
    out["sha256"] = sha256_file(path_value)
    try:
        payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
    except Exception:
        return out
    if not isinstance(payload, dict):
        return out
    out["json_valid"] = True
    version = str(payload.get("version", "")).strip()
    out["version"] = version
    declared_pipeline = str(payload.get("pipeline_version", "")).strip().lower()
    if declared_pipeline in ("v1", "v2"):
        out["pipeline_version"] = declared_pipeline
    else:
        out["pipeline_version"] = infer_pipeline_from_bundle_version(version)
    return out


def normalize_decision_item(item: Dict[str, Any]) -> Tuple[str, str, bool, str]:
    market = str(item.get("market", "")).strip().upper()
    strategy = str(item.get("strategy", item.get("strategy_name", ""))).strip().lower()
    selected = bool(to_bool(item.get("selected", False)))
    reason = str(item.get("reason", "")).strip().lower()
    return (market, strategy, selected, reason)


def load_policy_decision_log(path_value) -> Dict[str, Any]:
    out: Dict[str, Any] = {
        "path": str(path_value),
        "exists": bool(path_value.exists()),
        "total_lines": 0,
        "parsed_lines": 0,
        "parse_errors": 0,
        "scan_count": 0,
        "decision_count": 0,
        "scans": [],
        "timestamps": [],
    }
    if not out["exists"]:
        return out

    scans: List[Dict[str, Any]] = []
    timestamps: List[int] = []
    for line in read_nonempty_lines(path_value):
        out["total_lines"] += 1
        try:
            payload = json.loads(line)
        except Exception:
            out["parse_errors"] += 1
            continue
        if not isinstance(payload, dict):
            out["parse_errors"] += 1
            continue
        decisions_raw = payload.get("decisions", [])
        if not isinstance(decisions_raw, list):
            out["parse_errors"] += 1
            continue
        decision_counter: Counter[Tuple[str, str, bool, str]] = Counter()
        for item in decisions_raw:
            if not isinstance(item, dict):
                continue
            decision_counter[normalize_decision_item(item)] += 1
        ts = to_int(payload.get("ts", 0))
        scan = {
            "ts": int(ts),
            "dominant_regime": str(payload.get("dominant_regime", "")).strip(),
            "small_seed_mode": bool(to_bool(payload.get("small_seed_mode", False))),
            "max_new_orders_per_scan": int(to_int(payload.get("max_new_orders_per_scan", 0))),
            "decision_counter": decision_counter,
            "decision_count": int(sum(decision_counter.values())),
        }
        scans.append(scan)
        timestamps.append(int(ts))
        out["parsed_lines"] += 1
        out["decision_count"] += int(scan["decision_count"])

    out["scan_count"] = int(len(scans))
    out["scans"] = scans
    out["timestamps"] = timestamps
    return out


def resolve_pipeline_version(
    requested: str,
    live_bundle: Dict[str, Any],
    backtest_bundle: Dict[str, Any],
) -> Tuple[str, List[str]]:
    errors: List[str] = []
    requested_norm = str(requested).strip().lower()
    candidates: List[str] = []
    for meta in (live_bundle, backtest_bundle):
        token = str(meta.get("pipeline_version", "")).strip().lower()
        if token in ("v1", "v2"):
            candidates.append(token)
    unique_candidates = sorted(set(candidates))

    if requested_norm in ("v1", "v2"):
        if any(x != requested_norm for x in unique_candidates):
            errors.append("shadow_bundle_pipeline_mismatch")
        return requested_norm, errors
    if len(unique_candidates) > 1:
        errors.append("shadow_bundle_pipeline_conflict")
        return "v1", errors
    if len(unique_candidates) == 1:
        return unique_candidates[0], errors
    return "v1", errors


def compare_logs(live_log: Dict[str, Any], backtest_log: Dict[str, Any]) -> Dict[str, Any]:
    live_scans = list(live_log.get("scans", []))
    backtest_scans = list(backtest_log.get("scans", []))
    live_timestamps = list(live_log.get("timestamps", []))
    backtest_timestamps = list(backtest_log.get("timestamps", []))
    same_candles = bool(live_timestamps == backtest_timestamps and len(live_timestamps) > 0)

    compared_scan_count = min(len(live_scans), len(backtest_scans))
    timestamp_mismatch_count = 0
    header_mismatch_count = 0
    decision_mismatch_count = 0
    compared_decision_count = 0

    for idx in range(compared_scan_count):
        lhs = live_scans[idx]
        rhs = backtest_scans[idx]
        if int(lhs.get("ts", 0)) != int(rhs.get("ts", 0)):
            timestamp_mismatch_count += 1
            continue
        if (
            str(lhs.get("dominant_regime", "")) != str(rhs.get("dominant_regime", ""))
            or bool(lhs.get("small_seed_mode", False)) != bool(rhs.get("small_seed_mode", False))
            or int(lhs.get("max_new_orders_per_scan", 0)) != int(rhs.get("max_new_orders_per_scan", 0))
        ):
            header_mismatch_count += 1

        lhs_counter: Counter = lhs.get("decision_counter", Counter())
        rhs_counter: Counter = rhs.get("decision_counter", Counter())
        keys = set(lhs_counter.keys()) | set(rhs_counter.keys())
        scan_mismatch = 0
        for key in keys:
            scan_mismatch += abs(int(lhs_counter.get(key, 0)) - int(rhs_counter.get(key, 0)))
        decision_mismatch_count += int(scan_mismatch)
        compared_decision_count += int(max(sum(lhs_counter.values()), sum(rhs_counter.values())))

    unmatched_scan_count = abs(len(live_scans) - len(backtest_scans))
    mismatch_count = int(
        timestamp_mismatch_count
        + header_mismatch_count
        + decision_mismatch_count
        + unmatched_scan_count
    )
    decision_log_comparison_pass = bool(
        same_candles and
        compared_decision_count > 0 and
        mismatch_count == 0
    )

    return {
        "same_candles": same_candles,
        "compared_scan_count": int(compared_scan_count),
        "compared_decision_count": int(compared_decision_count),
        "mismatch_count": int(mismatch_count),
        "timestamp_mismatch_count": int(timestamp_mismatch_count),
        "header_mismatch_count": int(header_mismatch_count),
        "decision_mismatch_count": int(decision_mismatch_count),
        "unmatched_scan_count": int(unmatched_scan_count),
        "decision_log_comparison_pass": decision_log_comparison_pass,
    }


def evaluate(args: argparse.Namespace) -> Dict[str, Any]:
    output_path = resolve_repo_path(args.output_json)
    live_log_path = resolve_repo_path(args.live_decision_log_jsonl)
    backtest_log_path = resolve_repo_path(args.backtest_decision_log_jsonl)
    same_log_path = bool(live_log_path.resolve() == backtest_log_path.resolve())

    shared_bundle = str(args.runtime_bundle_json).strip()
    live_bundle_raw = str(args.live_runtime_bundle_json).strip()
    backtest_bundle_raw = str(args.backtest_runtime_bundle_json).strip()

    live_bundle_path = resolve_repo_path(live_bundle_raw or shared_bundle) if (live_bundle_raw or shared_bundle) else None
    backtest_bundle_path = resolve_repo_path(backtest_bundle_raw or shared_bundle) if (backtest_bundle_raw or shared_bundle) else None

    live_log = load_policy_decision_log(live_log_path)
    backtest_log = load_policy_decision_log(backtest_log_path)
    live_bundle = load_bundle_meta(live_bundle_path)
    backtest_bundle = load_bundle_meta(backtest_bundle_path)

    resolved_pipeline, errors = resolve_pipeline_version(
        str(args.pipeline_version),
        live_bundle,
        backtest_bundle,
    )

    live_exists = bool(live_log.get("exists", False))
    backtest_exists = bool(backtest_log.get("exists", False))
    live_parse_ok = int(live_log.get("parse_errors", 0)) == 0 and int(live_log.get("parsed_lines", 0)) > 0
    backtest_parse_ok = int(backtest_log.get("parse_errors", 0)) == 0 and int(backtest_log.get("parsed_lines", 0)) > 0

    if not live_exists:
        errors.append("shadow_live_decision_log_missing")
    if not backtest_exists:
        errors.append("shadow_backtest_decision_log_missing")
    if live_exists and not live_parse_ok:
        errors.append("shadow_live_decision_log_parse_or_empty")
    if backtest_exists and not backtest_parse_ok:
        errors.append("shadow_backtest_decision_log_parse_or_empty")
    if same_log_path:
        errors.append("shadow_live_backtest_log_path_identical")

    comparison = compare_logs(live_log, backtest_log) if (live_parse_ok and backtest_parse_ok) else {
        "same_candles": False,
        "compared_scan_count": 0,
        "compared_decision_count": 0,
        "mismatch_count": 1,
        "timestamp_mismatch_count": 0,
        "header_mismatch_count": 0,
        "decision_mismatch_count": 0,
        "unmatched_scan_count": 0,
        "decision_log_comparison_pass": False,
    }

    live_bundle_exists = bool(live_bundle.get("exists", False))
    backtest_bundle_exists = bool(backtest_bundle.get("exists", False))
    same_bundle = bool(
        live_bundle_exists and
        backtest_bundle_exists and
        str(live_bundle.get("sha256", "")) != "" and
        str(live_bundle.get("sha256", "")) == str(backtest_bundle.get("sha256", ""))
    )
    if not same_bundle:
        errors.append("shadow_runtime_bundle_missing_or_mismatch")

    if not bool(comparison.get("same_candles", False)):
        errors.append("shadow_candle_sequence_mismatch")
    if not bool(comparison.get("decision_log_comparison_pass", False)):
        errors.append("shadow_decision_log_mismatch")

    checks = {
        "decision_log_comparison_pass": bool(comparison.get("decision_log_comparison_pass", False)),
        "same_bundle": bool(same_bundle),
        "same_candles": bool(comparison.get("same_candles", False)),
        "live_log_ready": bool(live_exists and live_parse_ok),
        "backtest_log_ready": bool(backtest_exists and backtest_parse_ok),
        "distinct_log_paths": bool(not same_log_path),
    }
    overall_pass = bool(
        checks["decision_log_comparison_pass"] and
        checks["same_bundle"] and
        checks["same_candles"] and
        checks["distinct_log_paths"]
    )
    gate_profile_name = "v2_strict" if resolved_pipeline == "v2" else "v1"
    status = "pass" if (overall_pass and len(errors) == 0) else "fail"

    runtime_bundle_version = ""
    live_bundle_version = str(live_bundle.get("version", "")).strip()
    backtest_bundle_version = str(backtest_bundle.get("version", "")).strip()
    if live_bundle_version and live_bundle_version == backtest_bundle_version:
        runtime_bundle_version = live_bundle_version
    elif live_bundle_version and not backtest_bundle_version:
        runtime_bundle_version = live_bundle_version
    elif backtest_bundle_version and not live_bundle_version:
        runtime_bundle_version = backtest_bundle_version
    elif live_bundle_version or backtest_bundle_version:
        runtime_bundle_version = "mixed"

    out = {
        "generated_at_utc": utc_now_iso(),
        "status": status,
        "pipeline_version": resolved_pipeline,
        "runtime_bundle_version": runtime_bundle_version,
        "gate_profile": {
            "name": gate_profile_name,
            "checks": {
                "decision_log_comparison_pass": bool(checks["decision_log_comparison_pass"]),
                "same_bundle": bool(checks["same_bundle"]),
                "same_candles": bool(checks["same_candles"]),
                "distinct_log_paths": bool(checks["distinct_log_paths"]),
            },
            "all_pass": bool(overall_pass),
        },
        "overall_gate_pass": bool(overall_pass),
        "shadow_pass": bool(overall_pass),
        "checks": checks,
        "comparison": {
            "compared_scan_count": int(comparison.get("compared_scan_count", 0)),
            "compared_decision_count": int(comparison.get("compared_decision_count", 0)),
            "mismatch_count": int(comparison.get("mismatch_count", 0)),
            "timestamp_mismatch_count": int(comparison.get("timestamp_mismatch_count", 0)),
            "header_mismatch_count": int(comparison.get("header_mismatch_count", 0)),
            "decision_mismatch_count": int(comparison.get("decision_mismatch_count", 0)),
            "unmatched_scan_count": int(comparison.get("unmatched_scan_count", 0)),
        },
        "metadata": {
            "compared_decision_count": int(comparison.get("compared_decision_count", 0)),
            "mismatch_count": int(comparison.get("mismatch_count", 0)),
        },
        "errors": errors,
        "inputs": {
            "live_decision_log_jsonl": str(live_log_path),
            "backtest_decision_log_jsonl": str(backtest_log_path),
            "requested_pipeline_version": str(args.pipeline_version).strip().lower(),
            "runtime_bundle_json": str(resolve_repo_path(shared_bundle)) if shared_bundle else "",
            "live_runtime_bundle_json": str(live_bundle_path) if live_bundle_path is not None else "",
            "backtest_runtime_bundle_json": str(backtest_bundle_path) if backtest_bundle_path is not None else "",
        },
        "artifacts": {
            "output_json": str(output_path),
        },
        "sources": {
            "live_decision_log": {
                "path": str(live_log.get("path", "")),
                "exists": bool(live_log.get("exists", False)),
                "total_lines": int(live_log.get("total_lines", 0)),
                "parsed_lines": int(live_log.get("parsed_lines", 0)),
                "parse_errors": int(live_log.get("parse_errors", 0)),
                "scan_count": int(live_log.get("scan_count", 0)),
                "decision_count": int(live_log.get("decision_count", 0)),
            },
            "backtest_decision_log": {
                "path": str(backtest_log.get("path", "")),
                "exists": bool(backtest_log.get("exists", False)),
                "total_lines": int(backtest_log.get("total_lines", 0)),
                "parsed_lines": int(backtest_log.get("parsed_lines", 0)),
                "parse_errors": int(backtest_log.get("parse_errors", 0)),
                "scan_count": int(backtest_log.get("scan_count", 0)),
                "decision_count": int(backtest_log.get("decision_count", 0)),
            },
            "live_bundle": {
                "path": str(live_bundle.get("path", "")),
                "exists": bool(live_bundle.get("exists", False)),
                "sha256": str(live_bundle.get("sha256", "")),
                "version": str(live_bundle.get("version", "")),
                "pipeline_version": str(live_bundle.get("pipeline_version", "")),
            },
            "backtest_bundle": {
                "path": str(backtest_bundle.get("path", "")),
                "exists": bool(backtest_bundle.get("exists", False)),
                "sha256": str(backtest_bundle.get("sha256", "")),
                "version": str(backtest_bundle.get("version", "")),
                "pipeline_version": str(backtest_bundle.get("pipeline_version", "")),
            },
        },
    }
    dump_json(output_path, out)
    return out


def main(argv=None) -> int:
    args = parse_args(argv)
    out = evaluate(args)
    print("[GenerateProbabilisticShadowReport] completed", flush=True)
    print(f"status={out.get('status', 'fail')}", flush=True)
    print(f"pipeline_version={out.get('pipeline_version', '')}", flush=True)
    print(f"errors={len(out.get('errors', []) or [])}", flush=True)
    print(f"output={out.get('artifacts', {}).get('output_json', '')}", flush=True)
    if bool(args.strict) and str(out.get("status", "fail")).lower() != "pass":
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
