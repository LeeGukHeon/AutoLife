#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List


def resolve_repo_path(path: str, repo_root: Path) -> Path:
    p = Path(path)
    if p.is_absolute():
        return p
    return (repo_root / p).resolve()


def load_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def list_fresh14_primary_datasets(data_dir: Path) -> List[str]:
    names = sorted(p.name for p in data_dir.glob("upbit_KRW_*_1m_fresh14.csv"))
    if not names:
        names = sorted(p.name for p in data_dir.glob("*_1m_*.csv"))
    return names


def run_verification_case(
    repo_root: Path,
    python_exe: str,
    exe_path: Path,
    build_config_path: Path,
    source_config_path: Path,
    run_dir: Path,
    data_dir: Path,
    dataset_names: List[str],
    split_manifest: Path,
    output_json: Path,
    output_csv: Path,
    split_name: str,
    prewarm_hours: int,
) -> None:
    run_dir.mkdir(parents=True, exist_ok=True)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_csv.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        python_exe,
        str((repo_root / "scripts" / "run_verification.py").resolve()),
        "--exe-path",
        str(exe_path),
        "--config-path",
        str(build_config_path),
        "--source-config-path",
        str(source_config_path),
        "--run-dir",
        str(run_dir),
        "--data-dir",
        str(data_dir),
        "--split-manifest-json",
        str(split_manifest),
        "--split-name",
        split_name,
        "--split-time-based",
        "--split-execution-prewarm-hours",
        str(prewarm_hours),
        "--output-json",
        str(output_json),
        "--output-csv",
        str(output_csv),
    ]
    if dataset_names:
        cmd.extend(["--dataset-names", *dataset_names])

    result = subprocess.run(cmd, cwd=repo_root, check=False)
    if result.returncode != 0 and not output_json.exists():
        raise RuntimeError(f"run_verification failed ({result.returncode}): {' '.join(cmd)}")

    params_dump = run_dir / "run_params_dump.json"
    if not params_dump.exists():
        raise RuntimeError(f"missing required params dump: {params_dump}")


def extract_case_summary(report: Dict[str, Any], run_dir: Path) -> Dict[str, Any]:
    run_provenance = load_json(run_dir / "run_provenance.json")
    run_params = load_json(run_dir / "run_params_dump.json")

    split = report.get("split_filter", {})
    aggs = report.get("aggregates", {})
    diag = report.get("diagnostics", {})
    g8 = diag.get("stage_g8_backend_provenance", {})
    sem = diag.get("semantics_lock_report", {})
    risk = diag.get("aggregate", {}).get("post_entry_risk_telemetry", {})
    funnel = diag.get("a10_2_stage_funnel", {}).get("funnel_summary_core", {})
    core_no_signal = diag.get("core_no_signal_summary", {}).get("aggregate", {})
    per_dataset = diag.get("per_dataset", [])
    gate_versions: Dict[str, int] = {}
    topk_values: Dict[int, int] = {}
    stage_vnext_totals = {
        "s0_snapshots_valid": 0,
        "s1_selected_topk": 0,
        "s2_sized_count": 0,
        "s3_exec_gate_pass": 0,
        "s4_submitted": 0,
        "s5_filled": 0,
        "drop_ev_negative_count": 0,
    }
    if isinstance(per_dataset, list):
        for row in per_dataset:
            if not isinstance(row, dict):
                continue
            gate_vnext = row.get("gate_vnext", {})
            if not isinstance(gate_vnext, dict):
                continue
            gate_version = str(
                gate_vnext.get("gate_system_version_effective", "vnext")
            ).strip().lower()
            if gate_version:
                gate_versions[gate_version] = gate_versions.get(gate_version, 0) + 1
            topk = int(gate_vnext.get("quality_topk_effective", 0) or 0)
            if topk > 0:
                topk_values[topk] = topk_values.get(topk, 0) + 1
            stage = gate_vnext.get("stage_funnel_vnext", {})
            if isinstance(stage, dict):
                for key in stage_vnext_totals.keys():
                    stage_vnext_totals[key] += int(stage.get(key, 0) or 0)

    gate_effective = "unknown"
    if gate_versions:
        gate_effective = sorted(gate_versions.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]
    quality_topk_effective = 0
    if topk_values:
        quality_topk_effective = sorted(topk_values.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]

    backend_request = run_provenance.get("prob_model_backend")
    backend_effective = g8.get("prob_model_backend_effective_dominant")
    if (
        backend_request
        and backend_effective
        and backend_effective not in {"unknown", "n/a"}
        and backend_request != backend_effective
    ):
        raise RuntimeError(
            f"backend mismatch: request={backend_request}, effective={backend_effective}, run_dir={run_dir}"
        )

    return {
        "run_dir": str(run_dir),
        "split_applied": split.get("split_applied"),
        "total_trades_core_effective": split.get("total_trades_core_effective"),
        "avg_expectancy_krw": aggs.get("avg_expectancy_krw"),
        "avg_profit_factor": aggs.get("avg_profit_factor"),
        "stop_loss_trigger_count": risk.get("stop_loss_trigger_count"),
        "partial_tp_exit_count": risk.get("partial_tp_exit_count"),
        "take_profit_full_count": risk.get("take_profit_full_count"),
        "semantics_lock_status": sem.get("status"),
        "backend_request": backend_request,
        "backend_effective": backend_effective,
        "lgbm_model_sha256": run_provenance.get("lgbm_model_sha256"),
        "gate_system_version": run_params.get("bundle", {}).get("gate_system_version"),
        "quality_topk": run_params.get("bundle", {}).get("quality_topk"),
        "gate_system_version_effective": gate_effective,
        "quality_topk_effective": quality_topk_effective,
        "stage_funnel_vnext_totals": stage_vnext_totals,
        "ev_scale_bps": run_params.get("bundle", {}).get("ev_scale_bps"),
        "stage_funnel_core": funnel,
        "execution_no_signal_top3": core_no_signal.get("execution_no_signal_top3", [])[:3],
        "core_no_signal_top3": core_no_signal.get("core_no_signal_top3", [])[:3],
    }


def write_summary_csv(path: Path, rows: List[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.writer(fp)
        writer.writerow(
            [
                "case",
                "backend_request",
                "backend_effective",
                "gate_system_version",
                "quality_topk",
                "gate_system_version_effective",
                "quality_topk_effective",
                "ev_scale_bps",
                "split_applied",
                "trades_core",
                "expectancy_krw",
                "profit_factor",
                "stop_loss_count",
                "partial_tp_count",
                "tp_full_count",
                "semantics_lock_status",
            ]
        )
        for row in rows:
            writer.writerow(
                [
                    row["case"],
                    row.get("backend_request"),
                    row.get("backend_effective"),
                    row.get("gate_system_version"),
                    row.get("quality_topk"),
                    row.get("gate_system_version_effective"),
                    row.get("quality_topk_effective"),
                    row.get("ev_scale_bps"),
                    row.get("split_applied"),
                    row.get("total_trades_core_effective"),
                    row.get("avg_expectancy_krw"),
                    row.get("avg_profit_factor"),
                    row.get("stop_loss_trigger_count"),
                    row.get("partial_tp_exit_count"),
                    row.get("take_profit_full_count"),
                    row.get("semantics_lock_status"),
                ]
            )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run fixed Fresh14 Gate vNext matrix (SGD/LGBM) and produce a comparison summary."
    )
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--exe-path", default="build/Release/AutoLifeTrading.exe")
    parser.add_argument("--build-config-sgd", default="build/Release/config/config_rnd_sgd.json")
    parser.add_argument("--build-config-lgbm", default="build/Release/config/config_rnd_lgbm.json")
    parser.add_argument("--source-config-sgd", default="config/config_rnd_sgd.json")
    parser.add_argument("--source-config-lgbm", default="config/config_rnd_lgbm.json")
    parser.add_argument("--data-dir", default="data/backtest_fresh_14d")
    parser.add_argument("--split-manifest-json", default="build/Release/logs/time_split_manifest_r21_prefix.json")
    parser.add_argument("--split-name", default="quarantine")
    parser.add_argument("--split-execution-prewarm-hours", type=int, default=168)
    parser.add_argument("--logs-root", default="build/Release/logs")
    parser.add_argument("--run-tag", default="gatevnext_fresh14_matrix")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    exe_path = resolve_repo_path(args.exe_path, repo_root)
    build_cfg_sgd = resolve_repo_path(args.build_config_sgd, repo_root)
    build_cfg_lgbm = resolve_repo_path(args.build_config_lgbm, repo_root)
    src_cfg_sgd = resolve_repo_path(args.source_config_sgd, repo_root)
    src_cfg_lgbm = resolve_repo_path(args.source_config_lgbm, repo_root)
    data_dir = resolve_repo_path(args.data_dir, repo_root)
    split_manifest = resolve_repo_path(args.split_manifest_json, repo_root)
    logs_root = resolve_repo_path(args.logs_root, repo_root)

    dataset_names = list_fresh14_primary_datasets(data_dir)
    if not dataset_names:
        raise RuntimeError(f"no fresh14 primary datasets found in {data_dir}")

    cases = [
        {
            "case": "sgd",
            "build_config": build_cfg_sgd,
            "source_config": src_cfg_sgd,
            "run_dir": logs_root / f"{args.run_tag}_sgd",
            "output_json": logs_root / f"verification_{args.run_tag}_sgd.json",
            "output_csv": logs_root / f"verification_{args.run_tag}_sgd.csv",
        },
        {
            "case": "lgbm",
            "build_config": build_cfg_lgbm,
            "source_config": src_cfg_lgbm,
            "run_dir": logs_root / f"{args.run_tag}_lgbm",
            "output_json": logs_root / f"verification_{args.run_tag}_lgbm.json",
            "output_csv": logs_root / f"verification_{args.run_tag}_lgbm.csv",
        },
    ]

    summaries: List[Dict[str, Any]] = []
    for case in cases:
        run_verification_case(
            repo_root=repo_root,
            python_exe=args.python,
            exe_path=exe_path,
            build_config_path=case["build_config"],
            source_config_path=case["source_config"],
            run_dir=case["run_dir"],
            data_dir=data_dir,
            dataset_names=dataset_names,
            split_manifest=split_manifest,
            output_json=case["output_json"],
            output_csv=case["output_csv"],
            split_name=args.split_name,
            prewarm_hours=args.split_execution_prewarm_hours,
        )

        report = load_json(case["output_json"])
        summary = extract_case_summary(report, case["run_dir"])
        summary["case"] = case["case"]
        summary["report_json"] = str(case["output_json"])
        summaries.append(summary)

    out_json = logs_root / f"{args.run_tag}_summary.json"
    out_csv = logs_root / f"{args.run_tag}_summary.csv"
    out_json.write_text(json.dumps({"cases": summaries}, ensure_ascii=False, indent=2), encoding="utf-8")
    write_summary_csv(out_csv, summaries)

    print(out_json)
    print(out_csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
