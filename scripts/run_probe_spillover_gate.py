#!/usr/bin/env python3
"""Run delta -> v15 distortion -> v16 spillover lock in one workflow."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List


def _run(cmd: List[str], cwd: Path) -> None:
    completed = subprocess.run(
        cmd,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        message = "\n".join(
            [
                f"command failed ({completed.returncode}): {' '.join(cmd)}",
                "--- stdout ---",
                completed.stdout[-4000:],
                "--- stderr ---",
                completed.stderr[-4000:],
            ]
        )
        raise RuntimeError(message)


def _load_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def _safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return default


def _safe_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run occupancy spillover gate workflow for a probe candidate."
    )
    parser.add_argument("--baseline-daily-json", required=True)
    parser.add_argument("--candidate-daily-json", required=True)
    parser.add_argument(
        "--target-cells",
        default="TRENDING_UP|CORE_RESCUE_SHOULD_ENTER",
        help="Comma-separated intended target cells.",
    )
    parser.add_argument("--top-k", type=int, default=20)
    parser.add_argument(
        "--output-prefix",
        required=True,
        help="Output prefix path. Workflow writes *_delta.json, *_v15_distortion.json, *_v16_lock.json, *_workflow.json",
    )
    parser.add_argument("--python-exe", default=sys.executable)
    return parser


def main() -> int:
    args = _build_arg_parser().parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    baseline_path = Path(args.baseline_daily_json).resolve()
    candidate_path = Path(args.candidate_daily_json).resolve()

    prefix = Path(args.output_prefix)
    if not prefix.is_absolute():
        prefix = (repo_root / prefix).resolve()
    prefix.parent.mkdir(parents=True, exist_ok=True)

    delta_json = prefix.with_name(prefix.name + "_delta.json")
    v15_json = prefix.with_name(prefix.name + "_v15_distortion.json")
    v16_json = prefix.with_name(prefix.name + "_v16_lock.json")
    workflow_json = prefix.with_name(prefix.name + "_workflow.json")

    _run(
        [
            args.python_exe,
            "scripts/analyze_daily_oos_delta.py",
            "--baseline-json",
            str(baseline_path),
            "--candidate-json",
            str(candidate_path),
            "--top-k",
            str(int(args.top_k)),
            "--output-json",
            str(delta_json),
        ],
        cwd=repo_root,
    )
    _run(
        [
            args.python_exe,
            "scripts/analyze_v15_occupancy_distortion.py",
            "--baseline-json",
            str(baseline_path),
            "--candidate-json",
            str(candidate_path),
            "--top-k",
            str(int(args.top_k)),
            "--output-json",
            str(v15_json),
        ],
        cwd=repo_root,
    )
    _run(
        [
            args.python_exe,
            "scripts/evaluate_v16_spillover_lock.py",
            "--distortion-json",
            str(v15_json),
            "--target-cells",
            str(args.target_cells),
            "--output-json",
            str(v16_json),
        ],
        cwd=repo_root,
    )

    delta_payload = _load_json(delta_json)
    v15_payload = _load_json(v15_json)
    v16_payload = _load_json(v16_json)
    delta_summary = delta_payload.get("summary", {})
    if not isinstance(delta_summary, dict):
        delta_summary = {}
    gate_aligned = delta_summary.get("gate_aligned", {})
    if not isinstance(gate_aligned, dict):
        gate_aligned = {}
    daily_profit_sum_delta = _safe_float(delta_summary.get("profit_sum_delta"))
    daily_nonpositive_day_count_delta = _safe_int(delta_summary.get("nonpositive_day_count_delta"))
    gate_profit_sum_delta = _safe_float(gate_aligned.get("profit_sum_delta"), daily_profit_sum_delta)
    gate_nonpositive_day_count_delta = _safe_int(
        gate_aligned.get("nonpositive_day_count_delta"),
        daily_nonpositive_day_count_delta,
    )

    result = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": "pass" if str(v16_payload.get("status", "")).lower() == "pass" else "fail",
        "inputs": {
            "baseline_daily_json": str(baseline_path),
            "candidate_daily_json": str(candidate_path),
            "target_cells": str(args.target_cells),
            "top_k": int(args.top_k),
        },
        "artifacts": {
            "delta_json": str(delta_json),
            "v15_distortion_json": str(v15_json),
            "v16_lock_json": str(v16_json),
        },
        "summary": {
            "daily_profit_sum_delta": daily_profit_sum_delta,
            "daily_nonpositive_day_count_delta": daily_nonpositive_day_count_delta,
            "daily_profit_sum_delta_gate_aligned": gate_profit_sum_delta,
            "daily_nonpositive_day_count_delta_gate_aligned": gate_nonpositive_day_count_delta,
            "v15_adverse_day_cell_count": v15_payload.get("summary", {}).get("adverse_day_cell_count"),
            "v16_lock_status": v16_payload.get("status"),
            "v16_fail_reasons": v16_payload.get("fail_reasons", []),
        },
    }
    with workflow_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(result, fp, ensure_ascii=False, indent=2)

    print(f"[ProbeSpilloverGate] wrote {workflow_json}")
    print(
        "[ProbeSpilloverGate] "
        f"status={result['status']} "
        f"daily_profit_sum_delta={result['summary']['daily_profit_sum_delta']} "
        f"daily_profit_sum_delta_gate_aligned={result['summary']['daily_profit_sum_delta_gate_aligned']} "
        f"v16_fail_reasons={result['summary']['v16_fail_reasons']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
