#!/usr/bin/env python3
import argparse
import csv
import json
import os
from typing import Any, Dict, List

from _script_common import parse_last_json_line, resolve_repo_path, run_command, verification_lock


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="./build/Release/AutoLifeTrading.exe")
    parser.add_argument("--config-path", "-ConfigPath", default="./build/Release/config/config.json")
    parser.add_argument(
        "--gate-report-json",
        "-GateReportJson",
        default="./build/Release/logs/profitability_gate_report_realdata.json",
    )
    parser.add_argument(
        "--data-dirs",
        "-DataDirs",
        nargs="*",
        default=[
            "./data/backtest",
            "./data/backtest_curated",
            "./data/backtest_real",
            "./data/backtest_real_live",
            "./build/Release/data/backtest_real_live",
            "./build/Debug/data/backtest_real_live",
        ],
    )
    parser.add_argument("--output-market-csv", "-OutputMarketCsv", default="./build/Release/logs/loss_contributor_by_market.csv")
    parser.add_argument(
        "--output-strategy-csv",
        "-OutputStrategyCsv",
        default="./build/Release/logs/loss_contributor_by_strategy.csv",
    )
    parser.add_argument(
        "--output-precat-pattern-csv",
        "-OutputPrecatPatternCsv",
        default="./build/Release/logs/loss_contributor_by_precat_pattern.csv",
    )
    parser.add_argument("--max-workers", "-MaxWorkers", type=int, default=1)
    parser.add_argument(
        "--verification-lock-path",
        "-VerificationLockPath",
        default=r".\build\Release\logs\verification_run.lock",
    )
    parser.add_argument("--verification-lock-timeout-sec", "-VerificationLockTimeoutSec", type=int, default=1800)
    parser.add_argument("--verification-lock-stale-sec", "-VerificationLockStaleSec", type=int, default=14400)
    return parser.parse_args(argv)


def market_label_from_dataset(dataset_file_name: str) -> str:
    name = dataset_file_name.rsplit(".", 1)[0]
    parts = name.split("_")
    if len(parts) >= 3 and parts[0].lower() == "upbit":
        return f"{parts[1].upper()}-{parts[2].upper()}"
    return f"synthetic:{dataset_file_name}"


def safe_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except Exception:
        return default


def parse_json_object(raw: Any) -> Dict[str, Any]:
    if isinstance(raw, dict):
        return dict(raw)
    if raw is None:
        return {}
    text = str(raw).strip()
    if not text:
        return {}
    try:
        payload = json.loads(text)
    except Exception:
        return {}
    return payload if isinstance(payload, dict) else {}


def classify_pre_cat_lock_pattern(
    pre_cat_diag: Dict[str, Any],
    risk_breakdown: Dict[str, Any],
) -> str:
    observed_samples = int(pre_cat_diag.get("observed_samples", 0) or 0)
    observed_total = max(
        observed_samples,
        int(risk_breakdown.get("strategy_ev_pre_cat_observed", 0) or 0),
    )
    if observed_total <= 0:
        return "no_pre_cat_signal"

    blocked_no_soft_share = safe_float(pre_cat_diag.get("blocked_no_soft_path_share", 0.0), 0.0)
    blocked_no_soft_pressure = safe_float(pre_cat_diag.get("blocked_no_soft_avg_full_history_pressure", 0.0), 0.0)
    blocked_no_soft_hist_pf = safe_float(pre_cat_diag.get("blocked_no_soft_avg_history_profit_factor", 0.0), 0.0)
    blocked_no_soft_hist_exp = safe_float(pre_cat_diag.get("blocked_no_soft_avg_history_expectancy_krw", 0.0), 0.0)
    blocked_no_soft_sig_strength = safe_float(pre_cat_diag.get("blocked_no_soft_avg_signal_strength", 0.0), 0.0)
    blocked_no_soft_sig_ev = safe_float(pre_cat_diag.get("blocked_no_soft_avg_signal_expected_value", 0.0), 0.0)
    blocked_no_soft_sig_rr = safe_float(pre_cat_diag.get("blocked_no_soft_avg_signal_reward_risk", 0.0), 0.0)

    blocked_no_soft_count = int(risk_breakdown.get("strategy_ev_pre_cat_blocked_no_soft_path", 0) or 0)
    blocked_severe_sync_count = int(risk_breakdown.get("strategy_ev_pre_cat_blocked_severe_sync", 0) or 0)
    bridge_count = int(risk_breakdown.get("strategy_ev_pre_cat_recovery_evidence_bridge", 0) or 0)
    bridge_surrogate_count = int(
        risk_breakdown.get("strategy_ev_pre_cat_recovery_evidence_bridge_surrogate", 0) or 0
    )
    blocked_no_soft_ratio = (blocked_no_soft_count / float(observed_total)) if observed_total > 0 else 0.0
    blocked_severe_sync_ratio = (blocked_severe_sync_count / float(observed_total)) if observed_total > 0 else 0.0

    if bridge_surrogate_count > 0:
        return "pre_cat_bridge_surrogate_triggered"
    if bridge_count > 0:
        return "pre_cat_bridge_triggered"
    if blocked_severe_sync_ratio >= 0.35:
        return "pre_cat_severe_sync_dominant"
    if (
        blocked_no_soft_share >= 0.90
        and blocked_no_soft_pressure >= 0.95
        and blocked_no_soft_hist_pf < 0.45
        and blocked_no_soft_hist_exp < -10.0
        and blocked_no_soft_sig_strength >= 0.65
        and blocked_no_soft_sig_ev >= 0.0014
        and blocked_no_soft_sig_rr >= 1.35
    ):
        return "pre_cat_no_soft_high_quality_high_pressure_lock"
    if blocked_no_soft_share >= 0.90 and blocked_no_soft_sig_rr < 1.20:
        return "pre_cat_no_soft_low_rr"
    if blocked_no_soft_share >= 0.90 and (
        blocked_no_soft_sig_strength < 0.62 or blocked_no_soft_sig_ev < 0.0010
    ):
        return "pre_cat_no_soft_low_signal_quality"
    if blocked_no_soft_share >= 0.90 and (
        blocked_no_soft_hist_pf < 0.75 or blocked_no_soft_hist_exp < -4.0
    ):
        return "pre_cat_no_soft_negative_history"
    if blocked_no_soft_ratio >= 0.80:
        return "pre_cat_no_soft_mixed"
    return "pre_cat_mixed"


def invoke_backtest_json(exe_file, dataset_path):
    env = os.environ.copy()
    env["AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO"] = "1"
    result = run_command([str(exe_file), "--backtest", str(dataset_path), "--json"], env=env)
    return parse_last_json_line(result.stdout + "\n" + result.stderr)


def main(argv=None) -> int:
    args = parse_args(argv)
    lock_path = resolve_repo_path(args.verification_lock_path)

    with verification_lock(
        lock_path,
        timeout_sec=int(args.verification_lock_timeout_sec),
        stale_sec=int(args.verification_lock_stale_sec),
    ):
        exe_path = resolve_repo_path(args.exe_path)
        gate_report_path = resolve_repo_path(args.gate_report_json)
        output_market_csv = resolve_repo_path(args.output_market_csv)
        output_strategy_csv = resolve_repo_path(args.output_strategy_csv)
        output_precat_pattern_csv = resolve_repo_path(args.output_precat_pattern_csv)
        output_market_csv.parent.mkdir(parents=True, exist_ok=True)
        output_strategy_csv.parent.mkdir(parents=True, exist_ok=True)
        output_precat_pattern_csv.parent.mkdir(parents=True, exist_ok=True)

        for p, label in [(exe_path, "Executable"), (gate_report_path, "Gate report")]:
            if not p.exists():
                raise FileNotFoundError(f"{label} not found: {p}")

        dataset_lookup: Dict[str, str] = {}
        for data_dir in args.data_dirs:
            d = resolve_repo_path(data_dir)
            if not d.exists():
                continue
            for file in d.glob("*.csv"):
                key = file.name.lower()
                if key not in dataset_lookup:
                    dataset_lookup[key] = str(file)
        if not dataset_lookup:
            raise RuntimeError("No data directories were resolved.")

        report = json.loads(gate_report_path.read_text(encoding="utf-8-sig"))
        matrix_rows = report.get("matrix_rows")
        if not isinstance(matrix_rows, list):
            raise RuntimeError(f"Gate report does not contain matrix_rows: {gate_report_path}")

        core_loss_rows = [
            r
            for r in matrix_rows
            if str(r.get("profile_id", "")) == "core_full"
            and float(r.get("total_trades", 0.0)) > 0.0
            and float(r.get("total_profit_krw", 0.0)) < 0.0
        ]
        if not core_loss_rows:
            with output_market_csv.open("w", encoding="utf-8", newline="") as fh:
                fh.write("")
            with output_strategy_csv.open("w", encoding="utf-8", newline="") as fh:
                fh.write("")
            with output_precat_pattern_csv.open("w", encoding="utf-8", newline="") as fh:
                fh.write("")
            print("[LossContrib] No negative core_full rows found. Empty outputs were written.")
            print(f"market_csv={output_market_csv}")
            print(f"strategy_csv={output_strategy_csv}")
            print(f"precat_pattern_csv={output_precat_pattern_csv}")
            return 0

        market_agg: Dict[str, Dict[str, Any]] = {}
        for row in core_loss_rows:
            dataset_name = str(row.get("dataset", ""))
            market = market_label_from_dataset(dataset_name)
            loss_value = abs(float(row.get("total_profit_krw", 0.0)))
            trades = int(row.get("total_trades", 0))
            agg = market_agg.setdefault(
                market,
                {"market": market, "loss_krw": 0.0, "total_trades": 0, "losing_runs": 0},
            )
            agg["loss_krw"] += loss_value
            agg["total_trades"] += trades
            agg["losing_runs"] += 1

        total_market_loss = sum(float(x["loss_krw"]) for x in market_agg.values())
        market_rows = []
        for item in market_agg.values():
            loss = float(item["loss_krw"])
            share = (loss / total_market_loss) * 100.0 if total_market_loss > 0 else 0.0
            market_rows.append(
                {
                    "market": item["market"],
                    "loss_krw": round(loss, 4),
                    "loss_share_pct": round(share, 4),
                    "total_trades": int(item["total_trades"]),
                    "losing_runs": int(item["losing_runs"]),
                }
            )
        market_rows.sort(key=lambda x: float(x["loss_krw"]), reverse=True)
        with output_market_csv.open("w", encoding="utf-8", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(market_rows[0].keys()))
            writer.writeheader()
            writer.writerows(market_rows)

        pattern_agg: Dict[str, Dict[str, Any]] = {}
        for row in core_loss_rows:
            loss_value = abs(float(row.get("total_profit_krw", 0.0)))
            trades = int(row.get("total_trades", 0) or 0)
            pre_cat_diag = parse_json_object(row.get("pre_cat_feature_snapshot_diagnostics_json"))
            risk_breakdown = parse_json_object(row.get("entry_risk_gate_breakdown_json"))
            pattern = classify_pre_cat_lock_pattern(pre_cat_diag, risk_breakdown)
            agg = pattern_agg.setdefault(
                pattern,
                {"pre_cat_pattern": pattern, "loss_krw": 0.0, "total_trades": 0, "losing_runs": 0},
            )
            agg["loss_krw"] += loss_value
            agg["total_trades"] += trades
            agg["losing_runs"] += 1

        pattern_total_loss = sum(float(x["loss_krw"]) for x in pattern_agg.values())
        pattern_rows = []
        for item in pattern_agg.values():
            loss = float(item["loss_krw"])
            share = (loss / pattern_total_loss) * 100.0 if pattern_total_loss > 0 else 0.0
            pattern_rows.append(
                {
                    "pre_cat_pattern": item["pre_cat_pattern"],
                    "loss_krw": round(loss, 4),
                    "loss_share_pct": round(share, 4),
                    "total_trades": int(item["total_trades"]),
                    "losing_runs": int(item["losing_runs"]),
                }
            )
        pattern_rows.sort(key=lambda x: float(x["loss_krw"]), reverse=True)
        with output_precat_pattern_csv.open("w", encoding="utf-8", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(pattern_rows[0].keys()))
            writer.writeheader()
            writer.writerows(pattern_rows)

        strategy_agg: Dict[str, Dict[str, Any]] = {}
        missing_datasets: List[str] = []
        backtest_failures: List[str] = []

        pending_runs: List[tuple[str, str]] = []
        for row in sorted(core_loss_rows, key=lambda r: float(r.get("total_profit_krw", 0.0))):
            dataset_name = str(row.get("dataset", ""))
            dataset_path = dataset_lookup.get(dataset_name.lower())
            if not dataset_path:
                missing_datasets.append(dataset_name)
                continue
            pending_runs.append((dataset_name, dataset_path))

        if pending_runs:
            requested_workers = max(1, int(args.max_workers))
            if requested_workers > 1:
                print(
                    "[LossContrib] Parallel validation is disabled; "
                    "forcing sequential execution (--max-workers=1)."
                )
            for dataset_name, dataset_path in pending_runs:
                try:
                    result = invoke_backtest_json(exe_path, dataset_path)
                except Exception:
                    backtest_failures.append(dataset_name)
                    continue
                if result is None:
                    backtest_failures.append(dataset_name)
                    continue
                summaries = result.get("strategy_summaries")
                if not isinstance(summaries, list):
                    continue
                for summary in summaries:
                    if not isinstance(summary, dict):
                        continue
                    strategy_name = str(summary.get("strategy_name", "")).strip()
                    if not strategy_name:
                        continue
                    strategy_profit = float(summary.get("total_profit", 0.0))
                    if strategy_profit >= 0.0:
                        continue
                    agg = strategy_agg.setdefault(
                        strategy_name,
                        {
                            "strategy_name": strategy_name,
                            "loss_krw": 0.0,
                            "total_trades": 0,
                            "losing_trades": 0,
                            "losing_datasets": 0,
                        },
                    )
                    agg["loss_krw"] += abs(strategy_profit)
                    agg["total_trades"] += int(summary.get("total_trades", 0))
                    agg["losing_trades"] += int(summary.get("losing_trades", 0))
                    agg["losing_datasets"] += 1

        strategy_total_loss = sum(float(x["loss_krw"]) for x in strategy_agg.values())
        strategy_rows = []
        for item in strategy_agg.values():
            loss = float(item["loss_krw"])
            share = (loss / strategy_total_loss) * 100.0 if strategy_total_loss > 0 else 0.0
            strategy_rows.append(
                {
                    "strategy_name": item["strategy_name"],
                    "loss_krw": round(loss, 4),
                    "loss_share_pct": round(share, 4),
                    "total_trades": int(item["total_trades"]),
                    "losing_trades": int(item["losing_trades"]),
                    "losing_datasets": int(item["losing_datasets"]),
                }
            )
        strategy_rows.sort(key=lambda x: float(x["loss_krw"]), reverse=True)
        with output_strategy_csv.open("w", encoding="utf-8", newline="") as fh:
            if strategy_rows:
                writer = csv.DictWriter(fh, fieldnames=list(strategy_rows[0].keys()))
                writer.writeheader()
                writer.writerows(strategy_rows)
            else:
                fh.write("")

        print("[LossContrib] Completed")
        print(f"market_csv={output_market_csv}")
        print(f"strategy_csv={output_strategy_csv}")
        print(f"precat_pattern_csv={output_precat_pattern_csv}")
        for m in market_rows[:5]:
            print(f"[LossContrib][Market] {m['market']} | loss={m['loss_krw']} | share={m['loss_share_pct']}%")
        for p in pattern_rows[:5]:
            print(
                f"[LossContrib][PreCatPattern] {p['pre_cat_pattern']} | "
                f"loss={p['loss_krw']} | share={p['loss_share_pct']}%"
            )
        for s in strategy_rows[:2]:
            print(f"[LossContrib][Strategy] {s['strategy_name']} | loss={s['loss_krw']} | share={s['loss_share_pct']}%")
        if missing_datasets:
            print(f"[LossContrib] missing_datasets={','.join(sorted(set(missing_datasets)))}")
        if backtest_failures:
            print(f"[LossContrib] backtest_failures={','.join(sorted(set(backtest_failures)))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
