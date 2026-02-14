#!/usr/bin/env python3
import argparse
import csv
import json
import time
from datetime import datetime, timezone
from typing import Any, Dict, List

import walk_forward_validate
from _script_common import parse_last_json_line, resolve_repo_path, run_command


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe-path", "-ExePath", default="./build/Release/AutoLifeTrading.exe")
    parser.add_argument("--config-path", "-ConfigPath", default="./build/Release/config/config.json")
    parser.add_argument("--source-config-path", "-SourceConfigPath", default="./config/config.json")
    parser.add_argument("--dataset-a", "-DatasetA", default="./data/backtest/simulation_2000.csv")
    parser.add_argument("--dataset-b", "-DatasetB", default="./data/backtest/simulation_large.csv")
    parser.add_argument("--output-csv", "-OutputCsv", default="./build/Release/logs/optimization_grid.csv")
    parser.add_argument("--output-json", "-OutputJson", default="./build/Release/logs/optimization_best.json")
    parser.add_argument("--walk-forward-script", "-WalkForwardScript", default="./scripts/walk_forward_validate.py")
    parser.add_argument("--top-k-walk-forward", "-TopKWalkForward", type=int, default=6)
    parser.add_argument("--per-run-delay-ms", "-PerRunDelayMs", type=int, default=120)
    parser.add_argument("--use-walk-forward", "-UseWalkForward", action="store_true")
    parser.add_argument("--apply-best", "-ApplyBest", action="store_true")
    return parser.parse_args(argv)


def invoke_backtest_json(exe_path, csv_path, cwd_path):
    result = run_command([str(exe_path), "--backtest", str(csv_path), "--json"], cwd=cwd_path)
    return parse_last_json_line(result.stdout + "\n" + result.stderr)


def compute_score(a: Dict[str, Any] | None, b: Dict[str, Any] | None) -> float:
    if a is None or b is None:
        return -1e15

    p_a = float(a.get("total_profit", 0.0))
    p_b = float(b.get("total_profit", 0.0))
    pf_a = float(a.get("profit_factor", 0.0))
    pf_b = float(b.get("profit_factor", 0.0))
    exp_a = float(a.get("expectancy_krw", 0.0))
    exp_b = float(b.get("expectancy_krw", 0.0))
    mdd_a = float(a.get("max_drawdown", 0.0)) * 100.0
    mdd_b = float(b.get("max_drawdown", 0.0)) * 100.0
    t_a = int(a.get("total_trades", 0))
    t_b = int(b.get("total_trades", 0))

    score = 0.0
    score += (p_a + p_b)
    score += 180.0 * (exp_a + exp_b)
    score += 1100.0 * ((pf_a - 1.0) + (pf_b - 1.0))
    score -= 260.0 * (max(0.0, mdd_a - 11.5) + max(0.0, mdd_b - 11.5))
    if t_a < 25 or t_b < 25:
        score -= 850.0
    if t_a < 10 or t_b < 10:
        score -= 1400.0
    if p_a <= 0.0 or p_b <= 0.0:
        score -= 500.0
    return round(score, 4)


def set_or_add(obj: Dict[str, Any], key: str, value: Any) -> None:
    obj[key] = value


def invoke_walk_forward_score(exe_path, csv_path, exe_log_dir):
    tmp_wf_json = exe_log_dir / "optimization_wf_tmp.json"
    rc = walk_forward_validate.main(
        [
            "--exe-path",
            str(exe_path),
            "--input-csv",
            str(csv_path),
            "--output-json",
            str(tmp_wf_json),
        ]
    )
    if rc != 0 or not tmp_wf_json.exists():
        return None
    try:
        return json.loads(tmp_wf_json.read_text(encoding="utf-8"))
    except Exception:
        return None


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    config_path = resolve_repo_path(args.config_path)
    source_config_path = resolve_repo_path(args.source_config_path)
    dataset_a = resolve_repo_path(args.dataset_a)
    dataset_b = resolve_repo_path(args.dataset_b)
    walk_forward_script = resolve_repo_path(args.walk_forward_script)
    output_csv = resolve_repo_path(args.output_csv)
    output_json = resolve_repo_path(args.output_json)
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    output_json.parent.mkdir(parents=True, exist_ok=True)

    for p, label in [
        (exe_path, "Executable"),
        (config_path, "Config"),
        (dataset_a, "DatasetA"),
        (dataset_b, "DatasetB"),
    ]:
        if not p.exists():
            raise FileNotFoundError(f"{label} not found: {p}")
    if args.use_walk_forward and not walk_forward_script.exists():
        raise FileNotFoundError(f"Walk-forward script not found: {walk_forward_script}")

    exe_dir = exe_path.parent
    exe_log_dir = exe_dir / "logs"
    if exe_log_dir.exists():
        for old in exe_log_dir.glob("autolife*.log"):
            try:
                old.unlink()
            except Exception:
                pass

    cfg_raw = config_path.read_text(encoding="utf-8-sig")
    cfg = json.loads(cfg_raw)
    cfg.setdefault("trading", {})
    cfg.setdefault("strategies", {})
    cfg["strategies"].setdefault("scalping", {})
    cfg["strategies"].setdefault("momentum", {})

    weak_rr_grid = [1.80]
    strong_rr_grid = [1.20]
    edge_grid = [0.0012]
    ev_pf_grid = [0.95]
    ev_trades_grid = [30]
    ev_exp_grid = [-2.0]
    scalp_min_strength_grid = [0.66, 0.70, 0.74]
    mom_min_strength_grid = [0.60, 0.64, 0.68, 0.72]

    rows: List[Dict[str, Any]] = []
    combo_total = (
        len(weak_rr_grid)
        * len(strong_rr_grid)
        * len(edge_grid)
        * len(ev_pf_grid)
        * len(ev_trades_grid)
        * len(ev_exp_grid)
        * len(scalp_min_strength_grid)
        * len(mom_min_strength_grid)
    )
    combo_index = 0

    try:
        for weak in weak_rr_grid:
            for strong in strong_rr_grid:
                if strong > weak:
                    continue
                for edge in edge_grid:
                    for ev_pf in ev_pf_grid:
                        for ev_trades in ev_trades_grid:
                            for ev_exp in ev_exp_grid:
                                for scalp_min in scalp_min_strength_grid:
                                    for mom_min in mom_min_strength_grid:
                                        combo_index += 1
                                        print(
                                            f"[{combo_index}/{combo_total}] weakRR={weak} strongRR={strong} edge={edge} "
                                            f"evPF={ev_pf} evTrades={ev_trades} evExp={ev_exp} scalpingMin={scalp_min} momentumMin={mom_min}"
                                        )

                                        cfg["trading"]["min_rr_weak_signal"] = float(weak)
                                        cfg["trading"]["min_rr_strong_signal"] = float(strong)
                                        cfg["trading"]["min_expected_edge_pct"] = float(edge)
                                        cfg["trading"]["min_strategy_profit_factor"] = float(ev_pf)
                                        cfg["trading"]["min_strategy_trades_for_ev"] = int(ev_trades)
                                        cfg["trading"]["min_strategy_expectancy_krw"] = float(ev_exp)
                                        set_or_add(cfg["strategies"]["scalping"], "min_signal_strength", float(scalp_min))
                                        set_or_add(cfg["strategies"]["momentum"], "min_signal_strength", float(mom_min))
                                        config_path.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

                                        a = invoke_backtest_json(exe_path, dataset_a, exe_dir)
                                        b = invoke_backtest_json(exe_path, dataset_b, exe_dir)
                                        if args.per_run_delay_ms > 0:
                                            time.sleep(args.per_run_delay_ms / 1000.0)
                                        score = compute_score(a, b)

                                        rows.append(
                                            {
                                                "weak_rr": weak,
                                                "strong_rr": strong,
                                                "min_expected_edge_pct": edge,
                                                "min_strategy_profit_factor": ev_pf,
                                                "min_strategy_trades_for_ev": ev_trades,
                                                "min_strategy_expectancy_krw": ev_exp,
                                                "scalping_min_signal_strength": scalp_min,
                                                "momentum_min_signal_strength": mom_min,
                                                "score": score,
                                                "final_score": score,
                                                "wf_score": None,
                                                "wf_ratio": None,
                                                "wf_profit": None,
                                                "wf_mdd_pct": None,
                                                "profit_a": float(a.get("total_profit", 0.0)) if a else None,
                                                "pf_a": float(a.get("profit_factor", 0.0)) if a else None,
                                                "exp_a": float(a.get("expectancy_krw", 0.0)) if a else None,
                                                "mdd_a_pct": (float(a.get("max_drawdown", 0.0)) * 100.0) if a else None,
                                                "trades_a": int(a.get("total_trades", 0)) if a else None,
                                                "profit_b": float(b.get("total_profit", 0.0)) if b else None,
                                                "pf_b": float(b.get("profit_factor", 0.0)) if b else None,
                                                "exp_b": float(b.get("expectancy_krw", 0.0)) if b else None,
                                                "mdd_b_pct": (float(b.get("max_drawdown", 0.0)) * 100.0) if b else None,
                                                "trades_b": int(b.get("total_trades", 0)) if b else None,
                                            }
                                        )
    finally:
        config_path.write_text(cfg_raw, encoding="utf-8")

    if not rows:
        raise RuntimeError("No optimization rows generated.")

    rows_sorted = sorted(rows, key=lambda x: float(x["score"]), reverse=True)
    if args.use_walk_forward:
        top_k = min(int(args.top_k_walk_forward), len(rows_sorted))
        for i in range(top_k):
            cand = rows_sorted[i]
            cfg["trading"]["min_rr_weak_signal"] = float(cand["weak_rr"])
            cfg["trading"]["min_rr_strong_signal"] = float(cand["strong_rr"])
            cfg["trading"]["min_expected_edge_pct"] = float(cand["min_expected_edge_pct"])
            cfg["trading"]["min_strategy_profit_factor"] = float(cand["min_strategy_profit_factor"])
            cfg["trading"]["min_strategy_trades_for_ev"] = int(cand["min_strategy_trades_for_ev"])
            cfg["trading"]["min_strategy_expectancy_krw"] = float(cand["min_strategy_expectancy_krw"])
            set_or_add(cfg["strategies"]["scalping"], "min_signal_strength", float(cand["scalping_min_signal_strength"]))
            set_or_add(cfg["strategies"]["momentum"], "min_signal_strength", float(cand["momentum_min_signal_strength"]))
            config_path.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

            wf = invoke_walk_forward_score(exe_path, dataset_a, exe_log_dir)
            if wf is not None:
                ratio = float(wf.get("oos_profitable_ratio", 0.0))
                profit = float(wf.get("oos_total_profit", 0.0))
                mdd = float(wf.get("oos_max_mdd_pct", 0.0))
                ready_bonus = 350.0 if bool(wf.get("is_ready_for_live_walkforward", False)) else 0.0
                wf_score = (profit * 0.08) + (600.0 * ratio) - (180.0 * max(0.0, mdd - 12.0)) + ready_bonus
                cand["wf_score"] = round(wf_score, 4)
                cand["wf_ratio"] = ratio
                cand["wf_profit"] = profit
                cand["wf_mdd_pct"] = mdd
                cand["final_score"] = round(float(cand["score"]) + wf_score, 4)
        config_path.write_text(cfg_raw, encoding="utf-8")

    rows_sorted = sorted(rows, key=lambda x: float(x["final_score"]), reverse=True)
    with output_csv.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows_sorted[0].keys()))
        writer.writeheader()
        writer.writerows(rows_sorted)

    best = rows_sorted[0]
    best_report = {
        "generated_at_utc": datetime.now(tz=timezone.utc).isoformat(),
        "best": best,
        "top10": rows_sorted[:10],
    }
    output_json.write_text(json.dumps(best_report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("=== Optimization Top 10 ===")
    for item in rows_sorted[:10]:
        print(f"weak={item['weak_rr']} strong={item['strong_rr']} score={item['final_score']}")
    print(f"saved_csv={output_csv}")
    print(f"saved_json={output_json}")

    if args.apply_best:
        cfg_apply = json.loads(config_path.read_text(encoding="utf-8-sig"))
        cfg_apply.setdefault("trading", {})
        cfg_apply.setdefault("strategies", {})
        cfg_apply["strategies"].setdefault("scalping", {})
        cfg_apply["strategies"].setdefault("momentum", {})
        cfg_apply["trading"]["min_rr_weak_signal"] = float(best["weak_rr"])
        cfg_apply["trading"]["min_rr_strong_signal"] = float(best["strong_rr"])
        cfg_apply["trading"]["min_expected_edge_pct"] = float(best["min_expected_edge_pct"])
        cfg_apply["trading"]["min_strategy_profit_factor"] = float(best["min_strategy_profit_factor"])
        cfg_apply["trading"]["min_strategy_trades_for_ev"] = int(best["min_strategy_trades_for_ev"])
        cfg_apply["trading"]["min_strategy_expectancy_krw"] = float(best["min_strategy_expectancy_krw"])
        cfg_apply["strategies"]["scalping"]["min_signal_strength"] = float(best["scalping_min_signal_strength"])
        cfg_apply["strategies"]["momentum"]["min_signal_strength"] = float(best["momentum_min_signal_strength"])
        config_path.write_text(json.dumps(cfg_apply, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"Applied best parameters to {config_path}")
        if source_config_path.exists():
            source_cfg = json.loads(source_config_path.read_text(encoding="utf-8-sig"))
            source_cfg.setdefault("trading", {})
            source_cfg.setdefault("strategies", {})
            source_cfg["strategies"].setdefault("scalping", {})
            source_cfg["strategies"].setdefault("momentum", {})
            source_cfg["trading"]["min_rr_weak_signal"] = float(best["weak_rr"])
            source_cfg["trading"]["min_rr_strong_signal"] = float(best["strong_rr"])
            source_cfg["trading"]["min_expected_edge_pct"] = float(best["min_expected_edge_pct"])
            source_cfg["trading"]["min_strategy_profit_factor"] = float(best["min_strategy_profit_factor"])
            source_cfg["trading"]["min_strategy_trades_for_ev"] = int(best["min_strategy_trades_for_ev"])
            source_cfg["trading"]["min_strategy_expectancy_krw"] = float(best["min_strategy_expectancy_krw"])
            source_cfg["strategies"]["scalping"]["min_signal_strength"] = float(best["scalping_min_signal_strength"])
            source_cfg["strategies"]["momentum"]["min_signal_strength"] = float(best["momentum_min_signal_strength"])
            source_config_path.write_text(json.dumps(source_cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            print(f"Synced best parameters to {source_config_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
