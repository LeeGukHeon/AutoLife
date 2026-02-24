#!/usr/bin/env python3
import argparse
import csv
from datetime import datetime, timezone
from pathlib import Path
import shutil
from typing import Any, Dict, List, Optional, Tuple

from _script_common import dump_json, parse_last_json_line, resolve_repo_path, run_command


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run day-sliced out-of-sample stability checks on backtest datasets. "
            "This is a fail-closed guardrail for overfitting risk."
        )
    )
    parser.add_argument("--exe-path", default=r".\build\Release\AutoLifeTrading.exe")
    parser.add_argument("--data-dir", default=r".\data\backtest_real")
    parser.add_argument(
        "--datasets",
        default="",
        help="Comma-separated dataset names/paths. Default: discover upbit_*_1m_*.csv in data-dir.",
    )
    parser.add_argument("--days", type=int, default=7)
    parser.add_argument(
        "--warmup-days",
        type=int,
        default=-1,
        help="Warmup days before target day; use -1 to include all prior history (default).",
    )
    parser.add_argument("--min-rows-per-day", type=int, default=720)
    parser.add_argument("--min-trades-per-day", type=int, default=1)
    parser.add_argument("--gate-min-evaluated-days", type=int, default=10)
    parser.add_argument("--gate-max-nonpositive-ratio", type=float, default=0.45)
    parser.add_argument("--gate-max-day-dd-pct", type=float, default=12.0)
    parser.add_argument("--gate-require-positive-profit-sum", action="store_true")
    parser.add_argument("--no-gate-require-positive-profit-sum", dest="gate_require_positive_profit_sum", action="store_false")
    parser.set_defaults(gate_require_positive_profit_sum=True)
    parser.add_argument("--require-higher-tf-companions", action="store_true")
    parser.add_argument("--allow-missing-higher-tf-companions", dest="require_higher_tf_companions", action="store_false")
    parser.set_defaults(require_higher_tf_companions=True)
    parser.add_argument("--temp-dir", default=r".\build\Release\logs\daily_oos_tmp")
    parser.add_argument("--keep-temp", action="store_true")
    parser.add_argument("--output-csv", default=r".\build\Release\logs\daily_oos_stability_windows.csv")
    parser.add_argument("--output-json", default=r".\build\Release\logs\daily_oos_stability_report.json")
    return parser.parse_args(argv)


def parse_csv_tokens(raw: str) -> List[str]:
    out: List[str] = []
    seen = set()
    for token in str(raw or "").split(","):
        value = token.strip()
        if not value or value in seen:
            continue
        seen.add(value)
        out.append(value)
    return out


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


def utc_day_from_timestamp_ms(raw_value: str) -> Optional[str]:
    try:
        timestamp_ms = int(float(str(raw_value).strip()))
    except Exception:
        return None
    try:
        return datetime.fromtimestamp(timestamp_ms / 1000.0, tz=timezone.utc).date().isoformat()
    except Exception:
        return None


def discover_default_datasets(data_dir: Path) -> List[Path]:
    if not data_dir.exists():
        return []
    return sorted(
        [x.resolve() for x in data_dir.glob("upbit_*_1m_*.csv") if x.is_file()],
        key=lambda p: p.name.lower(),
    )


def resolve_datasets(data_dir: Path, dataset_arg: str) -> List[Path]:
    tokens = parse_csv_tokens(dataset_arg)
    if not tokens:
        return discover_default_datasets(data_dir)

    resolved: List[Path] = []
    seen: set[Path] = set()
    for token in tokens:
        candidate = Path(token)
        path_value: Optional[Path] = None
        if candidate.is_absolute():
            path_value = candidate
        elif any(sep in token for sep in ("/", "\\")):
            path_value = resolve_repo_path(token)
        else:
            path_value = (data_dir / token).resolve()
            if not path_value.exists():
                path_value = resolve_repo_path(token)

        if path_value is None:
            continue
        path_value = path_value.resolve()
        if path_value in seen:
            continue
        seen.add(path_value)
        resolved.append(path_value)
    return resolved


def infer_upbit_prefix_from_1m_dataset(dataset_path: Path) -> str:
    stem = dataset_path.stem
    marker = "_1m_"
    if marker not in stem:
        return ""
    return stem.split(marker, 1)[0]


def find_companion_sources(dataset_path: Path) -> Tuple[Dict[str, Path], List[str]]:
    prefix = infer_upbit_prefix_from_1m_dataset(dataset_path)
    if not prefix:
        return {}, []
    sources: Dict[str, Path] = {}
    missing: List[str] = []
    for tf in ("5m", "15m", "60m", "240m"):
        matches = sorted(
            [x.resolve() for x in dataset_path.parent.glob(f"{prefix}_{tf}_*.csv") if x.is_file()],
            key=lambda p: p.name.lower(),
        )
        if not matches:
            missing.append(tf)
            continue
        sources[tf] = matches[0]
    return sources, missing


def load_rows_grouped_by_day(csv_path: Path) -> Tuple[List[str], Dict[str, List[List[str]]], int]:
    grouped: Dict[str, List[List[str]]] = {}
    skipped_rows = 0
    with csv_path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.reader(fh)
        try:
            header = next(reader)
        except StopIteration:
            return [], grouped, 0
        if not header:
            return [], grouped, 0

        ts_index = 0
        for idx, name in enumerate(header):
            if str(name).strip().lower() == "timestamp":
                ts_index = idx
                break

        for row in reader:
            if not row or len(row) <= ts_index:
                skipped_rows += 1
                continue
            day = utc_day_from_timestamp_ms(row[ts_index])
            if not day:
                skipped_rows += 1
                continue
            grouped.setdefault(day, []).append(row)
    return header, grouped, skipped_rows


def select_recent_days(grouped: Dict[str, List[List[str]]], days: int, min_rows_per_day: int) -> List[str]:
    if days <= 0:
        return []
    ordered = sorted(grouped.keys())
    selected: List[str] = []
    for day in reversed(ordered):
        row_count = len(grouped.get(day, []))
        if row_count < int(min_rows_per_day):
            continue
        selected.append(day)
        if len(selected) >= int(days):
            break
    selected.reverse()
    return selected


def collect_rows_for_day_window(
    grouped: Dict[str, List[List[str]]],
    *,
    target_day: str,
    warmup_days: int,
) -> Tuple[List[List[str]], List[str]]:
    ordered_days = sorted(grouped.keys())
    if target_day not in grouped:
        return [], []
    try:
        idx = ordered_days.index(target_day)
    except ValueError:
        return [], []
    if int(warmup_days) < 0:
        start_idx = 0
    else:
        start_idx = max(0, idx - int(warmup_days))
    window_days = ordered_days[start_idx : idx + 1]
    rows: List[List[str]] = []
    for day in window_days:
        rows.extend(grouped.get(day, []))
    return rows, window_days


def write_slice_csv(path_value: Path, header: List[str], rows: List[List[str]]) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)
    with path_value.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(header)
        writer.writerows(rows)


def run_backtest_json(exe_path: Path, dataset_path: Path, require_higher_tf_companions: bool) -> Dict[str, Any]:
    cmd = [str(exe_path), "--backtest", str(dataset_path), "--json"]
    if require_higher_tf_companions:
        cmd.append("--require-higher-tf-companions")
    result = run_command(cmd, cwd=exe_path.parent)
    merged = f"{result.stdout}\n{result.stderr}"
    parsed = parse_last_json_line(merged)
    if result.exit_code != 0:
        tail_lines = [line.strip() for line in merged.splitlines() if line.strip()]
        tail_preview = " || ".join(tail_lines[-5:])[:800]
        raise RuntimeError(
            f"Backtest failed exit={result.exit_code} dataset={dataset_path} tail={tail_preview}"
        )
    if not isinstance(parsed, dict):
        raise RuntimeError(f"Backtest JSON output not found for dataset={dataset_path}")
    return parsed


def extract_day_metrics(payload: Dict[str, Any]) -> Dict[str, Any]:
    total_trades = to_int(payload.get("total_trades", 0))
    wins = to_int(payload.get("winning_trades", 0))
    win_rate = to_float(payload.get("win_rate", 0.0))
    if win_rate <= 0.0 and total_trades > 0:
        win_rate = wins / float(total_trades)
    return {
        "total_trades": total_trades,
        "winning_trades": wins,
        "win_rate": float(win_rate),
        "total_profit": to_float(payload.get("total_profit", 0.0)),
        "profit_factor": to_float(payload.get("profit_factor", 0.0)),
        "expectancy_krw": to_float(payload.get("expectancy_krw", 0.0)),
        "max_drawdown_pct": to_float(payload.get("max_drawdown", 0.0)) * 100.0,
    }


def extract_target_day_metrics_from_trade_history(payload: Dict[str, Any], target_day: str) -> Optional[Dict[str, Any]]:
    trades = payload.get("trade_history_samples", [])
    if not isinstance(trades, list) or not trades:
        return None

    day_pnls: List[float] = []
    for item in trades:
        if not isinstance(item, dict):
            continue
        exit_time = item.get("exit_time", item.get("entry_time", 0))
        day = utc_day_from_timestamp_ms(str(exit_time))
        if day != target_day:
            continue
        day_pnls.append(to_float(item.get("profit_loss_krw", 0.0)))

    total_trades = len(day_pnls)
    if total_trades <= 0:
        return None
    wins = len([x for x in day_pnls if x > 0.0])
    losses = len([x for x in day_pnls if x < 0.0])
    total_profit = sum(day_pnls)
    gross_profit = sum(x for x in day_pnls if x > 0.0)
    gross_loss_abs = sum(abs(x) for x in day_pnls if x < 0.0)
    if gross_loss_abs > 0.0:
        profit_factor = gross_profit / gross_loss_abs
    elif gross_profit > 0.0:
        profit_factor = 99.9
    else:
        profit_factor = 0.0
    return {
        "total_trades": int(total_trades),
        "winning_trades": int(wins),
        "win_rate": float(wins / float(total_trades)),
        "total_profit": float(total_profit),
        "profit_factor": float(profit_factor),
        "expectancy_krw": float(total_profit / float(total_trades)),
    }


def build_target_day_cell_breakdown(payload: Dict[str, Any], target_day: str) -> List[Dict[str, Any]]:
    trades = payload.get("trade_history_samples", [])
    if not isinstance(trades, list) or not trades:
        return []
    cells: Dict[str, Dict[str, Any]] = {}
    for item in trades:
        if not isinstance(item, dict):
            continue
        exit_time = item.get("exit_time", item.get("entry_time", 0))
        day = utc_day_from_timestamp_ms(str(exit_time))
        if day != target_day:
            continue
        regime = str(item.get("regime", "unknown")).strip() or "unknown"
        archetype = str(item.get("entry_archetype", "unknown")).strip() or "unknown"
        cell_key = f"{regime}|{archetype}"
        cell = cells.get(cell_key)
        if cell is None:
            cell = {
                "cell": cell_key,
                "regime": regime,
                "archetype": archetype,
                "trade_count": 0,
                "profit_sum": 0.0,
            }
            cells[cell_key] = cell
        cell["trade_count"] = int(cell.get("trade_count", 0)) + 1
        cell["profit_sum"] = to_float(cell.get("profit_sum", 0.0)) + to_float(item.get("profit_loss_krw", 0.0))

    rows = list(cells.values())
    rows.sort(key=lambda x: (to_float(x.get("profit_sum", 0.0)), -to_int(x.get("trade_count", 0))))
    out: List[Dict[str, Any]] = []
    for row in rows:
        out.append(
            {
                "cell": str(row.get("cell", "")),
                "regime": str(row.get("regime", "")),
                "archetype": str(row.get("archetype", "")),
                "trade_count": int(row.get("trade_count", 0)),
                "profit_sum": float(round(to_float(row.get("profit_sum", 0.0)), 6)),
            }
        )
    return out


def safe_ratio(numerator: int, denominator: int) -> float:
    if denominator <= 0:
        return 0.0
    return float(numerator) / float(denominator)


def compute_gate_checks(
    *,
    evaluated_day_count: int,
    nonpositive_day_ratio: float,
    total_profit_sum: float,
    peak_day_dd_pct: float,
    gate_min_evaluated_days: int,
    gate_max_nonpositive_ratio: float,
    gate_max_day_dd_pct: float,
    gate_require_positive_profit_sum: bool,
) -> Dict[str, Dict[str, Any]]:
    checks: Dict[str, Dict[str, Any]] = {
        "min_evaluated_days": {
            "pass": bool(evaluated_day_count >= int(gate_min_evaluated_days)),
            "actual": int(evaluated_day_count),
            "threshold_min": int(gate_min_evaluated_days),
        },
        "max_nonpositive_day_ratio": {
            "pass": bool(nonpositive_day_ratio <= float(gate_max_nonpositive_ratio)),
            "actual": float(round(nonpositive_day_ratio, 6)),
            "threshold_max": float(gate_max_nonpositive_ratio),
        },
        "max_day_drawdown_pct": {
            "pass": bool(peak_day_dd_pct <= float(gate_max_day_dd_pct)),
            "actual": float(round(peak_day_dd_pct, 6)),
            "threshold_max": float(gate_max_day_dd_pct),
        },
    }
    checks["positive_profit_sum"] = {
        "pass": bool((not gate_require_positive_profit_sum) or (total_profit_sum > 0.0)),
        "actual": float(round(total_profit_sum, 6)),
        "required": bool(gate_require_positive_profit_sum),
    }
    return checks


def materialize_companion_slices_for_day(
    *,
    dataset_path: Path,
    day: str,
    window_days: List[str],
    run_dir: Path,
    companion_sources: Dict[str, Path],
    companion_cache: Dict[Path, Tuple[List[str], Dict[str, List[List[str]]]]],
) -> List[Path]:
    created: List[Path] = []
    prefix = infer_upbit_prefix_from_1m_dataset(dataset_path)
    if not prefix:
        return created
    day_tag = day.replace("-", "")
    for tf, source_path in companion_sources.items():
        cached = companion_cache.get(source_path)
        if cached is None:
            header, grouped, _ = load_rows_grouped_by_day(source_path)
            companion_cache[source_path] = (header, grouped)
            cached = companion_cache[source_path]
        header, grouped = cached
        if not header:
            continue
        rows: List[List[str]] = []
        for day_key in window_days:
            rows.extend(grouped.get(day_key, []))
        out_path = run_dir / f"{prefix}_{tf}_{day_tag}.csv"
        write_slice_csv(out_path, header, rows)
        created.append(out_path)
    return created


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    data_dir = resolve_repo_path(args.data_dir)
    temp_dir = resolve_repo_path(args.temp_dir)
    output_csv = resolve_repo_path(args.output_csv)
    output_json = resolve_repo_path(args.output_json)

    if not exe_path.exists():
        raise FileNotFoundError(f"Executable not found: {exe_path}")
    if not data_dir.exists():
        raise FileNotFoundError(f"Data directory not found: {data_dir}")

    datasets = resolve_datasets(data_dir, str(args.datasets))
    if not datasets:
        raise RuntimeError("No datasets resolved. Provide --datasets or ensure data-dir has upbit_*_1m_*.csv")

    temp_dir.mkdir(parents=True, exist_ok=True)
    daily_rows: List[Dict[str, Any]] = []
    dataset_summaries: List[Dict[str, Any]] = []
    fatal_errors: List[str] = []
    companion_cache: Dict[Path, Tuple[List[str], Dict[str, List[List[str]]]]] = {}
    aggregate_loss_cells: Dict[str, Dict[str, Any]] = {}

    for dataset_path in datasets:
        if not dataset_path.exists():
            fatal_errors.append(f"missing_dataset:{dataset_path}")
            continue
        header, grouped, skipped_rows = load_rows_grouped_by_day(dataset_path)
        if not header:
            fatal_errors.append(f"invalid_csv:{dataset_path}")
            continue

        chosen_days = select_recent_days(grouped, int(args.days), int(args.min_rows_per_day))
        dataset_rows = [x for x in daily_rows if x.get("dataset") == dataset_path.name]
        companion_sources: Dict[str, Path] = {}
        companion_missing: List[str] = []
        if bool(args.require_higher_tf_companions):
            companion_sources, companion_missing = find_companion_sources(dataset_path)
            if companion_missing:
                fatal_errors.append(
                    f"missing_companions:{dataset_path.name}:{','.join(companion_missing)}"
                )
                continue

        for day in chosen_days:
            rows, window_days = collect_rows_for_day_window(
                grouped,
                target_day=day,
                warmup_days=int(args.warmup_days),
            )
            day_tag = day.replace("-", "")
            run_dir = temp_dir / f"{dataset_path.stem}_{day_tag}"
            run_dir.mkdir(parents=True, exist_ok=True)
            tmp_csv = run_dir / f"{dataset_path.stem}_{day}.csv"
            write_slice_csv(tmp_csv, header, rows)
            created_companions: List[str] = []
            if bool(args.require_higher_tf_companions):
                created_paths = materialize_companion_slices_for_day(
                    dataset_path=dataset_path,
                    day=day,
                    window_days=window_days,
                    run_dir=run_dir,
                    companion_sources=companion_sources,
                    companion_cache=companion_cache,
                )
                created_companions = [str(x) for x in created_paths]
            record: Dict[str, Any] = {
                "dataset": dataset_path.name,
                "day_utc": day,
                "rows": len(rows),
                "status": "ok",
                "error": "",
                "slice_csv": str(tmp_csv),
                "slice_companion_csvs": created_companions,
            }
            try:
                payload = run_backtest_json(
                    exe_path=exe_path,
                    dataset_path=tmp_csv,
                    require_higher_tf_companions=bool(args.require_higher_tf_companions),
                )
                metrics = extract_day_metrics(payload)
                day_metrics = extract_target_day_metrics_from_trade_history(payload, day)
                if isinstance(day_metrics, dict):
                    metrics["total_trades"] = int(day_metrics.get("total_trades", 0))
                    metrics["winning_trades"] = int(day_metrics.get("winning_trades", 0))
                    metrics["win_rate"] = float(day_metrics.get("win_rate", 0.0))
                    metrics["total_profit"] = float(day_metrics.get("total_profit", 0.0))
                    metrics["profit_factor"] = float(day_metrics.get("profit_factor", 0.0))
                    metrics["expectancy_krw"] = float(day_metrics.get("expectancy_krw", 0.0))
                    metrics["metric_source"] = "trade_history_day"
                else:
                    metrics["metric_source"] = "summary_fallback"
                cell_breakdown = build_target_day_cell_breakdown(payload, day)
                metrics["cell_breakdown"] = cell_breakdown
                record.update(metrics)
                record["evaluated"] = bool(metrics["total_trades"] >= int(args.min_trades_per_day))
                record["nonpositive_profit"] = bool(metrics["total_profit"] <= 0.0)
                record["profitable"] = bool(metrics["total_profit"] > 0.0)
                for cell in cell_breakdown:
                    cell_key = str(cell.get("cell", "")).strip()
                    if not cell_key:
                        continue
                    agg = aggregate_loss_cells.get(cell_key)
                    if agg is None:
                        agg = {
                            "cell": cell_key,
                            "regime": str(cell.get("regime", "")),
                            "archetype": str(cell.get("archetype", "")),
                            "trade_count": 0,
                            "profit_sum": 0.0,
                            "day_count": 0,
                        }
                        aggregate_loss_cells[cell_key] = agg
                    agg["trade_count"] = int(agg.get("trade_count", 0)) + int(cell.get("trade_count", 0))
                    agg["profit_sum"] = to_float(agg.get("profit_sum", 0.0)) + to_float(cell.get("profit_sum", 0.0))
                    agg["day_count"] = int(agg.get("day_count", 0)) + 1
            except Exception as exc:
                record["status"] = "error"
                record["error"] = str(exc)
                record["total_trades"] = 0
                record["winning_trades"] = 0
                record["win_rate"] = 0.0
                record["total_profit"] = 0.0
                record["profit_factor"] = 0.0
                record["expectancy_krw"] = 0.0
                record["max_drawdown_pct"] = 0.0
                record["metric_source"] = "error"
                record["cell_breakdown"] = []
                record["evaluated"] = False
                record["nonpositive_profit"] = False
                record["profitable"] = False
            daily_rows.append(record)
            dataset_rows.append(record)

        successful = [x for x in dataset_rows if x.get("status") == "ok"]
        evaluated = [x for x in successful if bool(x.get("evaluated", False))]
        nonpositive = [x for x in evaluated if bool(x.get("nonpositive_profit", False))]
        profitable = [x for x in evaluated if bool(x.get("profitable", False))]
        peak_dd = max((to_float(x.get("max_drawdown_pct", 0.0)) for x in evaluated), default=0.0)
        profit_sum = sum(to_float(x.get("total_profit", 0.0)) for x in evaluated)

        dataset_summaries.append(
            {
                "dataset": dataset_path.name,
                "source_path": str(dataset_path),
                "days_selected": len(chosen_days),
                "days_ok": len(successful),
                "days_evaluated": len(evaluated),
                "days_profitable": len(profitable),
                "days_nonpositive": len(nonpositive),
                "nonpositive_day_ratio": round(safe_ratio(len(nonpositive), len(evaluated)), 6),
                "profit_sum": round(profit_sum, 6),
                "peak_day_drawdown_pct": round(peak_dd, 6),
                "skipped_rows_invalid_ts": int(skipped_rows),
            }
        )

    successful_rows = [x for x in daily_rows if x.get("status") == "ok"]
    evaluated_rows = [x for x in successful_rows if bool(x.get("evaluated", False))]
    nonpositive_rows = [x for x in evaluated_rows if bool(x.get("nonpositive_profit", False))]
    profitable_rows = [x for x in evaluated_rows if bool(x.get("profitable", False))]
    error_rows = [x for x in daily_rows if x.get("status") != "ok"]

    total_profit_sum = sum(to_float(x.get("total_profit", 0.0)) for x in evaluated_rows)
    peak_day_dd_pct = max((to_float(x.get("max_drawdown_pct", 0.0)) for x in evaluated_rows), default=0.0)
    nonpositive_day_ratio = safe_ratio(len(nonpositive_rows), len(evaluated_rows))

    gate_checks = compute_gate_checks(
        evaluated_day_count=len(evaluated_rows),
        nonpositive_day_ratio=nonpositive_day_ratio,
        total_profit_sum=total_profit_sum,
        peak_day_dd_pct=peak_day_dd_pct,
        gate_min_evaluated_days=int(args.gate_min_evaluated_days),
        gate_max_nonpositive_ratio=float(args.gate_max_nonpositive_ratio),
        gate_max_day_dd_pct=float(args.gate_max_day_dd_pct),
        gate_require_positive_profit_sum=bool(args.gate_require_positive_profit_sum),
    )
    gate_fail_reasons = [name for name, item in gate_checks.items() if not bool(item.get("pass", False))]
    if fatal_errors:
        gate_fail_reasons.append("fatal_dataset_errors")
    if error_rows:
        gate_fail_reasons.append("daily_backtest_errors")
    gate_fail_reasons = sorted(list(dict.fromkeys(gate_fail_reasons)))
    overall_pass = len(gate_fail_reasons) == 0

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with output_csv.open("w", encoding="utf-8", newline="") as fh:
        fieldnames = [
            "dataset",
            "day_utc",
            "rows",
            "status",
            "error",
            "total_trades",
            "winning_trades",
            "win_rate",
            "total_profit",
            "profit_factor",
            "expectancy_krw",
            "max_drawdown_pct",
            "metric_source",
            "evaluated",
            "nonpositive_profit",
            "profitable",
            "slice_csv",
            "slice_companion_csvs",
            "cell_breakdown",
        ]
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(daily_rows)

    report = {
        "generated_at_utc": datetime.now(tz=timezone.utc).isoformat(),
        "status": "pass" if overall_pass else "fail",
        "overall_pass": bool(overall_pass),
        "exe_path": str(exe_path),
        "data_dir": str(data_dir),
        "datasets": [x.name for x in datasets],
        "params": {
            "days": int(args.days),
            "warmup_days": int(args.warmup_days),
            "min_rows_per_day": int(args.min_rows_per_day),
            "min_trades_per_day": int(args.min_trades_per_day),
            "require_higher_tf_companions": bool(args.require_higher_tf_companions),
            "gate_min_evaluated_days": int(args.gate_min_evaluated_days),
            "gate_max_nonpositive_ratio": float(args.gate_max_nonpositive_ratio),
            "gate_max_day_dd_pct": float(args.gate_max_day_dd_pct),
            "gate_require_positive_profit_sum": bool(args.gate_require_positive_profit_sum),
        },
        "artifacts": {
            "daily_csv": str(output_csv),
            "temp_dir": str(temp_dir),
            "keep_temp": bool(args.keep_temp),
        },
        "aggregates": {
            "daily_rows_total": len(daily_rows),
            "daily_rows_ok": len(successful_rows),
            "daily_rows_error": len(error_rows),
            "evaluated_day_count": len(evaluated_rows),
            "profitable_day_count": len(profitable_rows),
            "nonpositive_day_count": len(nonpositive_rows),
            "profitable_day_ratio": round(safe_ratio(len(profitable_rows), len(evaluated_rows)), 6),
            "nonpositive_day_ratio": round(nonpositive_day_ratio, 6),
            "total_profit_sum": round(total_profit_sum, 6),
            "avg_profit_per_evaluated_day": round(
                (total_profit_sum / float(len(evaluated_rows))) if evaluated_rows else 0.0,
                6,
            ),
            "peak_day_drawdown_pct": round(peak_day_dd_pct, 6),
        },
        "gate_checks": gate_checks,
        "gate_fail_reasons": gate_fail_reasons,
        "fatal_errors": fatal_errors,
        "aggregate_loss_cells": sorted(
            [
                {
                    "cell": str(v.get("cell", "")),
                    "regime": str(v.get("regime", "")),
                    "archetype": str(v.get("archetype", "")),
                    "trade_count": int(v.get("trade_count", 0)),
                    "day_count": int(v.get("day_count", 0)),
                    "profit_sum": float(round(to_float(v.get("profit_sum", 0.0)), 6)),
                }
                for v in aggregate_loss_cells.values()
            ],
            key=lambda x: float(x.get("profit_sum", 0.0)),
        ),
        "dataset_summaries": dataset_summaries,
        "daily_results": daily_rows,
    }
    dump_json(output_json, report)

    if not bool(args.keep_temp):
        for run_dir in temp_dir.iterdir():
            if not run_dir.is_dir():
                continue
            try:
                shutil.rmtree(run_dir, ignore_errors=True)
            except Exception:
                pass

    print("=== Daily OOS Stability ===")
    print(f"status={report['status']}")
    print(f"evaluated_day_count={report['aggregates']['evaluated_day_count']}")
    print(f"nonpositive_day_ratio={report['aggregates']['nonpositive_day_ratio']}")
    print(f"total_profit_sum={report['aggregates']['total_profit_sum']}")
    print(f"peak_day_drawdown_pct={report['aggregates']['peak_day_drawdown_pct']}")
    if gate_fail_reasons:
        print(f"gate_fail_reasons={','.join(gate_fail_reasons)}")
    print(f"saved_csv={output_csv}")
    print(f"saved_json={output_json}")
    return 0 if overall_pass else 2


if __name__ == "__main__":
    raise SystemExit(main())
