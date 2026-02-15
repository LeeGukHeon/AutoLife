#!/usr/bin/env python3
import argparse
import csv
import json
import os
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from _script_common import parse_last_json_line, resolve_repo_path, run_command


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="./build/Release/AutoLifeTrading.exe")
    parser.add_argument(
        "--gate-report-json",
        "-GateReportJson",
        default="./build/Release/logs/profitability_gate_report_realdata.json",
    )
    parser.add_argument("--profile-id", "-ProfileId", default="core_full")
    parser.add_argument(
        "--data-dirs",
        "-DataDirs",
        nargs="*",
        default=["./data/backtest_real", "./data/backtest_curated", "./data/backtest"],
    )
    parser.add_argument("--max-datasets", "-MaxDatasets", type=int, default=0)
    parser.add_argument("--max-workers", "-MaxWorkers", type=int, default=4)
    parser.add_argument(
        "--output-json",
        "-OutputJson",
        default="./build/Release/logs/root_cause_diagnostics_summary.json",
    )
    parser.add_argument(
        "--output-pattern-csv",
        "-OutputPatternCsv",
        default="./build/Release/logs/root_cause_loss_patterns.csv",
    )
    return parser.parse_args(argv)


def aggregate_pattern_rows(agg: dict[str, dict[str, Any]], rows: list[dict[str, Any]]) -> None:
    for row in rows:
        strategy = str(row.get("strategy_name", "unknown"))
        archetype = str(row.get("entry_archetype", "UNSPECIFIED"))
        regime = str(row.get("regime", "UNKNOWN"))
        ev_bucket = str(row.get("expected_value_bucket", "unknown"))
        key = f"{strategy}|{archetype}|{regime}|{ev_bucket}"
        item = agg.setdefault(
            key,
            {
                "strategy_name": strategy,
                "entry_archetype": archetype,
                "regime": regime,
                "expected_value_bucket": ev_bucket,
                "total_trades": 0,
                "winning_trades": 0,
                "losing_trades": 0,
                "total_profit": 0.0,
            },
        )
        item["total_trades"] += int(row.get("total_trades", 0))
        item["winning_trades"] += int(row.get("winning_trades", 0))
        item["losing_trades"] += int(row.get("losing_trades", 0))
        item["total_profit"] += float(row.get("total_profit", 0.0))


def apply_profile_flags(cfg: dict[str, Any], profile_id: str) -> None:
    profile_map = {
        "legacy_default": (False, False, False, False),
        "core_bridge_only": (True, False, False, False),
        "core_policy_risk": (True, True, True, False),
        "core_full": (True, True, True, True),
    }
    bridge, policy, risk, execution = profile_map.get(profile_id, profile_map["core_full"])
    backtest = cfg.setdefault("backtest", {})
    backtest["enabled"] = True
    trading = cfg.setdefault("trading", {})
    trading["enable_core_plane_bridge"] = bridge
    trading["enable_core_policy_plane"] = policy
    trading["enable_core_risk_plane"] = risk
    trading["enable_core_execution_plane"] = execution


def resolve_source_config_path(report: dict[str, Any]) -> Path | None:
    inputs = report.get("inputs") or {}
    for key in ("source_config_path", "config_path"):
        val = inputs.get(key)
        if isinstance(val, str) and val.strip():
            p = Path(val).expanduser()
            if not p.is_absolute():
                p = resolve_repo_path(str(p))
            if p.exists():
                return p
    fallback = resolve_repo_path("./config/config.json")
    if fallback.exists():
        return fallback
    return None


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    report_path = resolve_repo_path(args.gate_report_json)
    output_json = resolve_repo_path(args.output_json)
    output_pattern_csv = resolve_repo_path(args.output_pattern_csv)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_pattern_csv.parent.mkdir(parents=True, exist_ok=True)

    if not exe_path.exists():
        raise FileNotFoundError(f"Executable not found: {exe_path}")
    if not report_path.exists():
        raise FileNotFoundError(f"Gate report not found: {report_path}")

    report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    matrix_rows = [
        r
        for r in report.get("matrix_rows", [])
        if str(r.get("profile_id", "")) == args.profile_id
    ]
    if not matrix_rows:
        raise RuntimeError(f"No matrix rows for profile_id={args.profile_id}")

    dataset_lookup: dict[str, str] = {}
    for d in args.data_dirs:
        root = resolve_repo_path(d)
        if not root.exists():
            continue
        for f in root.glob("*.csv"):
            dataset_lookup.setdefault(f.name.lower(), str(f))
    if not dataset_lookup:
        raise RuntimeError("No dataset files found in data dirs.")

    selected_rows = sorted(matrix_rows, key=lambda r: str(r.get("dataset", "")).lower())
    if args.max_datasets > 0:
        selected_rows = selected_rows[: args.max_datasets]

    pending: list[tuple[str, str]] = []
    missing: list[str] = []
    for r in selected_rows:
        name = str(r.get("dataset", ""))
        path = dataset_lookup.get(name.lower())
        if not path:
            missing.append(name)
            continue
        pending.append((name, path))
    if not pending:
        raise RuntimeError("No runnable datasets after path resolution.")

    env = os.environ.copy()
    env["AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO"] = "1"

    source_config_path = resolve_source_config_path(report)
    if source_config_path is None:
        raise FileNotFoundError("Could not resolve source config path from gate report inputs/config defaults.")
    runtime_config_path = resolve_repo_path("./build/Release/config/config.json")
    runtime_config_path.parent.mkdir(parents=True, exist_ok=True)
    base_cfg = json.loads(source_config_path.read_text(encoding="utf-8-sig"))
    apply_profile_flags(base_cfg, args.profile_id)
    original_runtime_raw = ""
    if runtime_config_path.exists():
        original_runtime_raw = runtime_config_path.read_text(encoding="utf-8-sig")
    runtime_config_path.write_text(
        json.dumps(base_cfg, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    totals = {
        "dataset_runs": 0,
        "total_trades": 0,
        "total_profit_krw": 0.0,
        "total_fees_krw": 0.0,
        "gross_profit_before_fees_krw": 0.0,
        "intrabar_stop_tp_collision_count": 0,
    }
    exit_reason_counts: dict[str, int] = {}
    intrabar_collision_by_strategy: dict[str, int] = {}
    strategy_agg: dict[str, dict[str, Any]] = {}
    pattern_agg: dict[str, dict[str, Any]] = {}
    failed: list[str] = []

    cpu_bound = os.cpu_count() or 4
    max_workers = max(1, min(len(pending), cpu_bound, args.max_workers if args.max_workers > 0 else cpu_bound))
    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        fut_to_ds = {
            pool.submit(
                run_command,
                [str(exe_path), "--backtest", dataset_path, "--json"],
                env=env,
            ): dataset_name
            for dataset_name, dataset_path in pending
        }
        for fut in as_completed(fut_to_ds):
            dataset_name = fut_to_ds[fut]
            try:
                cp = fut.result()
                payload = parse_last_json_line((cp.stdout or "") + "\n" + (cp.stderr or ""))
            except Exception:
                failed.append(dataset_name)
                continue
            if not isinstance(payload, dict):
                failed.append(dataset_name)
                continue

            trades = int(payload.get("total_trades", 0))
            pnl = float(payload.get("total_profit", 0.0))
            avg_fee = float(payload.get("avg_fee_krw", 0.0))
            total_fee = avg_fee * trades

            totals["dataset_runs"] += 1
            totals["total_trades"] += trades
            totals["total_profit_krw"] += pnl
            totals["total_fees_krw"] += total_fee
            totals["gross_profit_before_fees_krw"] += (pnl + total_fee)
            totals["intrabar_stop_tp_collision_count"] += int(payload.get("intrabar_stop_tp_collision_count", 0))

            for k, v in (payload.get("exit_reason_counts") or {}).items():
                exit_reason_counts[k] = exit_reason_counts.get(k, 0) + int(v)
            for k, v in (payload.get("intrabar_collision_by_strategy") or {}).items():
                intrabar_collision_by_strategy[k] = intrabar_collision_by_strategy.get(k, 0) + int(v)

            for s in payload.get("strategy_summaries", []):
                name = str(s.get("strategy_name", "unknown"))
                item = strategy_agg.setdefault(
                    name,
                    {"strategy_name": name, "total_trades": 0, "winning_trades": 0, "losing_trades": 0, "total_profit": 0.0},
                )
                item["total_trades"] += int(s.get("total_trades", 0))
                item["winning_trades"] += int(s.get("winning_trades", 0))
                item["losing_trades"] += int(s.get("losing_trades", 0))
                item["total_profit"] += float(s.get("total_profit", 0.0))

            aggregate_pattern_rows(pattern_agg, payload.get("pattern_summaries", []))

    total_trades = int(totals["total_trades"])
    total_profit = float(totals["total_profit_krw"])
    total_fees = float(totals["total_fees_krw"])
    gross_pre_fee = float(totals["gross_profit_before_fees_krw"])

    avg_expectancy = (total_profit / total_trades) if total_trades > 0 else 0.0
    avg_gross_expectancy = (gross_pre_fee / total_trades) if total_trades > 0 else 0.0
    fee_per_trade = (total_fees / total_trades) if total_trades > 0 else 0.0
    loss_abs = abs(total_profit) if total_profit < 0 else 0.0
    fee_share_of_loss_pct = (total_fees / loss_abs * 100.0) if loss_abs > 1e-9 else 0.0

    strategy_rows = []
    for item in strategy_agg.values():
        t = max(1, int(item["total_trades"]))
        strategy_rows.append(
            {
                "strategy_name": item["strategy_name"],
                "total_trades": int(item["total_trades"]),
                "winning_trades": int(item["winning_trades"]),
                "losing_trades": int(item["losing_trades"]),
                "win_rate": round(float(item["winning_trades"]) / float(t), 4),
                "total_profit": round(float(item["total_profit"]), 4),
                "avg_profit_krw": round(float(item["total_profit"]) / float(t), 4),
            }
        )
    strategy_rows.sort(key=lambda r: float(r["total_profit"]))

    pattern_rows = []
    for item in pattern_agg.values():
        t = max(1, int(item["total_trades"]))
        pattern_rows.append(
            {
                "strategy_name": item["strategy_name"],
                "entry_archetype": item["entry_archetype"],
                "regime": item["regime"],
                "expected_value_bucket": item["expected_value_bucket"],
                "total_trades": int(item["total_trades"]),
                "winning_trades": int(item["winning_trades"]),
                "losing_trades": int(item["losing_trades"]),
                "win_rate": round(float(item["winning_trades"]) / float(t), 4),
                "total_profit": round(float(item["total_profit"]), 4),
                "avg_profit_krw": round(float(item["total_profit"]) / float(t), 4),
            }
        )
    pattern_rows.sort(key=lambda r: float(r["total_profit"]))

    with output_pattern_csv.open("w", encoding="utf-8", newline="") as fh:
        if pattern_rows:
            writer = csv.DictWriter(fh, fieldnames=list(pattern_rows[0].keys()))
            writer.writeheader()
            writer.writerows(pattern_rows)
        else:
            fh.write("")

    try:
        summary = {
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "profile_id": args.profile_id,
            "dataset_runs": totals["dataset_runs"],
            "datasets_missing": missing,
            "datasets_failed": failed,
            "totals": {
                "total_trades": total_trades,
                "total_profit_krw": round(total_profit, 4),
                "total_fees_krw": round(total_fees, 4),
                "gross_profit_before_fees_krw": round(gross_pre_fee, 4),
                "avg_expectancy_krw": round(avg_expectancy, 4),
                "avg_gross_expectancy_krw": round(avg_gross_expectancy, 4),
                "avg_fee_krw_per_trade": round(fee_per_trade, 4),
                "fee_share_of_net_loss_pct": round(fee_share_of_loss_pct, 4),
                "intrabar_stop_tp_collision_count": int(totals["intrabar_stop_tp_collision_count"]),
            },
            "exit_reason_counts": dict(sorted(exit_reason_counts.items(), key=lambda kv: kv[1], reverse=True)),
            "intrabar_collision_by_strategy": dict(
                sorted(intrabar_collision_by_strategy.items(), key=lambda kv: kv[1], reverse=True)
            ),
            "strategy_rows": strategy_rows,
            "top_loss_patterns": pattern_rows[:20],
        }
        output_json.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    finally:
        if original_runtime_raw:
            runtime_config_path.write_text(original_runtime_raw, encoding="utf-8", newline="\n")

    print("[RootCause] Completed")
    print(f"summary_json={output_json}")
    print(f"pattern_csv={output_pattern_csv}")
    print(f"[RootCause] total_trades={total_trades}, avg_expectancy_krw={avg_expectancy:.4f}, avg_fee_krw={fee_per_trade:.4f}")
    print(f"[RootCause] intrabar_stop_tp_collision_count={int(totals['intrabar_stop_tp_collision_count'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
