#!/usr/bin/env python3
"""Fail-closed promotion evaluator for v22 guard naming/criteria."""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict


def _to_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return default


def _to_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def _load_json(path_value: Path) -> Dict[str, Any]:
    with path_value.open("r", encoding="utf-8") as fp:
        payload = json.load(fp)
    if not isinstance(payload, dict):
        raise RuntimeError(f"JSON root must be object: {path_value}")
    return payload


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Evaluate v22 guard promotion criteria from OFF/ON artifacts.")
    parser.add_argument("--probe-name", default="v22_uptrend_rescue_prefilter_tail_guard")
    parser.add_argument("--verification-off-json", required=True)
    parser.add_argument("--verification-on-json", required=True)
    parser.add_argument("--daily-off-json", required=True)
    parser.add_argument("--daily-on-json", required=True)
    parser.add_argument("--workflow-json", required=True)
    parser.add_argument("--max-verification-pf-diff", type=float, default=1e-12)
    parser.add_argument("--max-verification-exp-diff", type=float, default=1e-12)
    parser.add_argument("--max-verification-trades-diff", type=float, default=1e-12)
    parser.add_argument("--min-profit-sum-delta", type=float, default=0.0)
    parser.add_argument("--max-nonpositive-day-count-delta", type=int, default=0)
    parser.add_argument("--require-daily-pass", action="store_true")
    parser.add_argument("--no-require-daily-pass", dest="require_daily_pass", action="store_false")
    parser.set_defaults(require_daily_pass=True)
    parser.add_argument("--require-workflow-pass", action="store_true")
    parser.add_argument("--no-require-workflow-pass", dest="require_workflow_pass", action="store_false")
    parser.set_defaults(require_workflow_pass=True)
    parser.add_argument("--require-v16-reasons-empty", action="store_true")
    parser.add_argument("--no-require-v16-reasons-empty", dest="require_v16_reasons_empty", action="store_false")
    parser.set_defaults(require_v16_reasons_empty=True)
    parser.add_argument("--output-json", required=True)
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    repo_root = Path(__file__).resolve().parent.parent

    verification_off_json = Path(args.verification_off_json).resolve()
    verification_on_json = Path(args.verification_on_json).resolve()
    daily_off_json = Path(args.daily_off_json).resolve()
    daily_on_json = Path(args.daily_on_json).resolve()
    workflow_json = Path(args.workflow_json).resolve()

    output_json = Path(args.output_json)
    if not output_json.is_absolute():
        output_json = (repo_root / output_json).resolve()
    output_json.parent.mkdir(parents=True, exist_ok=True)

    ver_off = _load_json(verification_off_json)
    ver_on = _load_json(verification_on_json)
    daily_off = _load_json(daily_off_json)
    daily_on = _load_json(daily_on_json)
    workflow = _load_json(workflow_json)

    ver_off_agg = ver_off.get("aggregates", {})
    ver_on_agg = ver_on.get("aggregates", {})
    pf_diff = abs(_to_float(ver_on_agg.get("avg_profit_factor")) - _to_float(ver_off_agg.get("avg_profit_factor")))
    exp_diff = abs(_to_float(ver_on_agg.get("avg_expectancy_krw")) - _to_float(ver_off_agg.get("avg_expectancy_krw")))
    trades_diff = abs(_to_float(ver_on_agg.get("avg_total_trades")) - _to_float(ver_off_agg.get("avg_total_trades")))

    daily_off_status = str(daily_off.get("status", "")).strip().lower()
    daily_on_status = str(daily_on.get("status", "")).strip().lower()
    daily_off_agg = daily_off.get("aggregates", {})
    daily_on_agg = daily_on.get("aggregates", {})
    profit_sum_delta = _to_float(daily_on_agg.get("total_profit_sum")) - _to_float(daily_off_agg.get("total_profit_sum"))
    nonpositive_day_count_delta = _to_int(daily_on_agg.get("nonpositive_day_count")) - _to_int(daily_off_agg.get("nonpositive_day_count"))

    workflow_status = str(workflow.get("status", "")).strip().lower()
    v16_fail_reasons = workflow.get("summary", {}).get("v16_fail_reasons", [])
    if not isinstance(v16_fail_reasons, list):
        v16_fail_reasons = []

    checks = {
        "verification_pf_diff": {
            "pass": bool(pf_diff <= float(args.max_verification_pf_diff)),
            "actual": pf_diff,
            "threshold_max": float(args.max_verification_pf_diff),
        },
        "verification_exp_diff": {
            "pass": bool(exp_diff <= float(args.max_verification_exp_diff)),
            "actual": exp_diff,
            "threshold_max": float(args.max_verification_exp_diff),
        },
        "verification_trades_diff": {
            "pass": bool(trades_diff <= float(args.max_verification_trades_diff)),
            "actual": trades_diff,
            "threshold_max": float(args.max_verification_trades_diff),
        },
        "daily_profit_sum_delta": {
            "pass": bool(profit_sum_delta >= float(args.min_profit_sum_delta)),
            "actual": profit_sum_delta,
            "threshold_min": float(args.min_profit_sum_delta),
        },
        "daily_nonpositive_day_count_delta": {
            "pass": bool(nonpositive_day_count_delta <= int(args.max_nonpositive_day_count_delta)),
            "actual": nonpositive_day_count_delta,
            "threshold_max": int(args.max_nonpositive_day_count_delta),
        },
        "daily_status": {
            "pass": bool((not args.require_daily_pass) or (daily_off_status == "pass" and daily_on_status == "pass")),
            "actual": {"off": daily_off_status, "on": daily_on_status},
            "required": bool(args.require_daily_pass),
        },
        "workflow_status": {
            "pass": bool((not args.require_workflow_pass) or (workflow_status == "pass")),
            "actual": workflow_status,
            "required": bool(args.require_workflow_pass),
        },
        "workflow_v16_fail_reasons": {
            "pass": bool((not args.require_v16_reasons_empty) or (len(v16_fail_reasons) == 0)),
            "actual": list(v16_fail_reasons),
            "required_empty": bool(args.require_v16_reasons_empty),
        },
    }
    failed_checks = [name for name, payload in checks.items() if not bool(payload.get("pass", False))]
    status = "pass" if not failed_checks else "fail"

    result = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "probe_name": str(args.probe_name),
        "status": status,
        "failed_checks": failed_checks,
        "inputs": {
            "verification_off_json": str(verification_off_json),
            "verification_on_json": str(verification_on_json),
            "daily_off_json": str(daily_off_json),
            "daily_on_json": str(daily_on_json),
            "workflow_json": str(workflow_json),
        },
        "thresholds": {
            "max_verification_pf_diff": float(args.max_verification_pf_diff),
            "max_verification_exp_diff": float(args.max_verification_exp_diff),
            "max_verification_trades_diff": float(args.max_verification_trades_diff),
            "min_profit_sum_delta": float(args.min_profit_sum_delta),
            "max_nonpositive_day_count_delta": int(args.max_nonpositive_day_count_delta),
            "require_daily_pass": bool(args.require_daily_pass),
            "require_workflow_pass": bool(args.require_workflow_pass),
            "require_v16_reasons_empty": bool(args.require_v16_reasons_empty),
        },
        "summary": {
            "verification": {
                "off_avg_profit_factor": _to_float(ver_off_agg.get("avg_profit_factor")),
                "on_avg_profit_factor": _to_float(ver_on_agg.get("avg_profit_factor")),
                "off_avg_expectancy_krw": _to_float(ver_off_agg.get("avg_expectancy_krw")),
                "on_avg_expectancy_krw": _to_float(ver_on_agg.get("avg_expectancy_krw")),
                "off_avg_total_trades": _to_float(ver_off_agg.get("avg_total_trades")),
                "on_avg_total_trades": _to_float(ver_on_agg.get("avg_total_trades")),
            },
            "daily": {
                "off_status": daily_off_status,
                "on_status": daily_on_status,
                "off_total_profit_sum": _to_float(daily_off_agg.get("total_profit_sum")),
                "on_total_profit_sum": _to_float(daily_on_agg.get("total_profit_sum")),
                "profit_sum_delta": profit_sum_delta,
                "off_nonpositive_day_count": _to_int(daily_off_agg.get("nonpositive_day_count")),
                "on_nonpositive_day_count": _to_int(daily_on_agg.get("nonpositive_day_count")),
                "nonpositive_day_count_delta": nonpositive_day_count_delta,
            },
            "workflow": {
                "status": workflow_status,
                "v16_fail_reasons": list(v16_fail_reasons),
            },
        },
        "checks": checks,
    }

    with output_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(result, fp, ensure_ascii=False, indent=2)

    print(
        "[V22Promotion] "
        f"status={status} "
        f"profit_sum_delta={profit_sum_delta:.6f} "
        f"nonpositive_day_count_delta={nonpositive_day_count_delta} "
        f"failed_checks={failed_checks} "
        f"report={output_json}"
    )
    return 0 if status == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
