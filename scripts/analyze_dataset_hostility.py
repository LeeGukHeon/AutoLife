#!/usr/bin/env python3
import argparse
import csv
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class DatasetStats:
    dataset: str
    path: str
    candles: int
    total_return_pct: float
    max_drawdown_pct: float
    down_candle_ratio: float
    negative_return_ratio: float
    avg_range_pct: float
    trend_efficiency: float
    daily_volatility_pct: float
    adversarial_score: float
    profitable_profile_ratio: float


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--gate-report-json",
        "-GateReportJson",
        default="./build/Release/logs/profitability_gate_report_realdata.json",
    )
    parser.add_argument(
        "--data-dirs",
        "-DataDirs",
        nargs="*",
        default=["./data/backtest_real", "./data/backtest_curated", "./data/backtest"],
    )
    parser.add_argument(
        "--output-json",
        "-OutputJson",
        default="./build/Release/logs/dataset_hostility_summary.json",
    )
    parser.add_argument(
        "--output-csv",
        "-OutputCsv",
        default="./build/Release/logs/dataset_hostility_details.csv",
    )
    return parser.parse_args(argv)


def resolve_dataset_paths(report: dict[str, Any], data_dirs: list[str]) -> dict[str, Path]:
    out: dict[str, Path] = {}

    for raw in report.get("inputs", {}).get("datasets", []):
        p = Path(str(raw))
        if p.exists():
            out[p.name.lower()] = p

    for d in data_dirs:
        root = Path(d)
        if not root.exists():
            continue
        for f in root.glob("*.csv"):
            out.setdefault(f.name.lower(), f.resolve())

    return out


def welford_update(count: int, mean: float, m2: float, x: float) -> tuple[int, float, float]:
    count += 1
    delta = x - mean
    mean += delta / count
    delta2 = x - mean
    m2 += delta * delta2
    return count, mean, m2


def analyze_csv(path: Path) -> dict[str, float]:
    with path.open("r", encoding="utf-8-sig", newline="") as fh:
        reader = csv.DictReader(fh)
        first_close = None
        prev_close = None
        last_close = None
        running_peak = 0.0
        max_drawdown = 0.0
        candles = 0
        down_candles = 0
        neg_returns = 0
        range_sum = 0.0
        abs_return_sum = 0.0
        lr_count = 0
        lr_mean = 0.0
        lr_m2 = 0.0

        for row in reader:
            try:
                o = float(row.get("open", "0") or 0.0)
                h = float(row.get("high", "0") or 0.0)
                l = float(row.get("low", "0") or 0.0)
                c = float(row.get("close", "0") or 0.0)
            except ValueError:
                continue
            if c <= 0.0:
                continue

            candles += 1
            if first_close is None:
                first_close = c
                running_peak = c
            last_close = c

            if o > 0.0 and c < o:
                down_candles += 1

            if c > running_peak:
                running_peak = c
            if running_peak > 0.0:
                dd = (running_peak - c) / running_peak
                if dd > max_drawdown:
                    max_drawdown = dd

            if h > 0.0 and l > 0.0:
                range_sum += max(0.0, (h - l) / c)

            if prev_close is not None and prev_close > 0.0:
                ret = (c - prev_close) / prev_close
                if ret < 0.0:
                    neg_returns += 1
                abs_return_sum += abs(ret)
                ratio = c / prev_close
                if ratio > 0.0:
                    lr = math.log(ratio)
                    lr_count, lr_mean, lr_m2 = welford_update(lr_count, lr_mean, lr_m2, lr)
            prev_close = c

    if candles < 2 or first_close is None or last_close is None:
        return {
            "candles": candles,
            "total_return_pct": 0.0,
            "max_drawdown_pct": 0.0,
            "down_candle_ratio": 0.0,
            "negative_return_ratio": 0.0,
            "avg_range_pct": 0.0,
            "trend_efficiency": 0.0,
            "daily_volatility_pct": 0.0,
            "adversarial_score": 0.0,
        }

    total_return = (last_close - first_close) / first_close
    down_candle_ratio = down_candles / max(1, candles)
    neg_return_ratio = neg_returns / max(1, candles - 1)
    avg_range = range_sum / max(1, candles)
    trend_efficiency = abs(total_return) / max(1e-9, abs_return_sum)
    lr_var = (lr_m2 / (lr_count - 1)) if lr_count > 1 else 0.0
    daily_vol = math.sqrt(max(0.0, lr_var)) * math.sqrt(1440.0)

    score = 0.0
    if total_return < 0.0:
        score += min(25.0, abs(total_return) * 160.0)
    score += min(25.0, max_drawdown * 125.0)
    score += min(20.0, max(0.0, daily_vol - 0.04) * 220.0)
    score += min(20.0, max(0.0, 1.0 - trend_efficiency) * 16.0)
    score += min(10.0, max(0.0, down_candle_ratio - 0.5) * 100.0)
    score = max(0.0, min(100.0, score))

    return {
        "candles": candles,
        "total_return_pct": total_return * 100.0,
        "max_drawdown_pct": max_drawdown * 100.0,
        "down_candle_ratio": down_candle_ratio,
        "negative_return_ratio": neg_return_ratio,
        "avg_range_pct": avg_range * 100.0,
        "trend_efficiency": trend_efficiency,
        "daily_volatility_pct": daily_vol * 100.0,
        "adversarial_score": score,
    }


def main(argv=None) -> int:
    args = parse_args(argv)
    report_path = Path(args.gate_report_json).resolve()
    output_json = Path(args.output_json).resolve()
    output_csv = Path(args.output_csv).resolve()
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_csv.parent.mkdir(parents=True, exist_ok=True)

    if not report_path.exists():
        raise FileNotFoundError(f"Gate report not found: {report_path}")

    report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    matrix_rows = report.get("matrix_rows", [])
    dataset_paths = resolve_dataset_paths(report, args.data_dirs)

    dataset_profiles: dict[str, list[dict[str, Any]]] = {}
    for row in matrix_rows:
        ds = str(row.get("dataset", "")).strip()
        if not ds:
            continue
        dataset_profiles.setdefault(ds, []).append(row)

    details: list[DatasetStats] = []
    unresolved: list[str] = []

    for dataset_name, rows in sorted(dataset_profiles.items(), key=lambda x: x[0].lower()):
        path = dataset_paths.get(dataset_name.lower())
        if not path or not path.exists():
            unresolved.append(dataset_name)
            continue

        profile_count = len(rows)
        profitable_profiles = sum(1 for r in rows if bool(r.get("profitable", False)))
        profitable_profile_ratio = (profitable_profiles / profile_count) if profile_count > 0 else 0.0

        raw = analyze_csv(path)
        details.append(
            DatasetStats(
                dataset=dataset_name,
                path=str(path),
                candles=int(raw["candles"]),
                total_return_pct=float(raw["total_return_pct"]),
                max_drawdown_pct=float(raw["max_drawdown_pct"]),
                down_candle_ratio=float(raw["down_candle_ratio"]),
                negative_return_ratio=float(raw["negative_return_ratio"]),
                avg_range_pct=float(raw["avg_range_pct"]),
                trend_efficiency=float(raw["trend_efficiency"]),
                daily_volatility_pct=float(raw["daily_volatility_pct"]),
                adversarial_score=float(raw["adversarial_score"]),
                profitable_profile_ratio=float(profitable_profile_ratio),
            )
        )

    details.sort(key=lambda x: x.adversarial_score, reverse=True)

    if details:
        n = len(details)
        avg_score = sum(d.adversarial_score for d in details) / n
        negative_return_share = sum(1 for d in details if d.total_return_pct < 0.0) / n
        high_drawdown_share = sum(1 for d in details if d.max_drawdown_pct >= 8.0) / n
        all_profiles_loss_share = sum(1 for d in details if d.profitable_profile_ratio <= 0.0) / n
        very_low_profitability_share = sum(1 for d in details if d.profitable_profile_ratio <= 0.25) / n
    else:
        avg_score = 0.0
        negative_return_share = 0.0
        high_drawdown_share = 0.0
        all_profiles_loss_share = 0.0
        very_low_profitability_share = 0.0

    if avg_score >= 60.0 or all_profiles_loss_share >= 0.70:
        hostility_level = "high"
    elif avg_score >= 45.0 or very_low_profitability_share >= 0.60:
        hostility_level = "medium"
    else:
        hostility_level = "low"

    summary = {
        "gate_report_json": str(report_path),
        "dataset_count": len(details),
        "unresolved_datasets": unresolved,
        "hostility_level": hostility_level,
        "hostility_metrics": {
            "avg_adversarial_score": round(avg_score, 4),
            "negative_return_share": round(negative_return_share, 4),
            "high_drawdown_share_ge_8pct": round(high_drawdown_share, 4),
            "all_profiles_loss_share": round(all_profiles_loss_share, 4),
            "very_low_profitability_share_le_25pct": round(very_low_profitability_share, 4),
        },
        "top_hostile_datasets": [
            {
                "dataset": d.dataset,
                "adversarial_score": round(d.adversarial_score, 4),
                "total_return_pct": round(d.total_return_pct, 4),
                "max_drawdown_pct": round(d.max_drawdown_pct, 4),
                "daily_volatility_pct": round(d.daily_volatility_pct, 4),
                "trend_efficiency": round(d.trend_efficiency, 4),
                "profitable_profile_ratio": round(d.profitable_profile_ratio, 4),
            }
            for d in details[:10]
        ],
    }
    output_json.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    with output_csv.open("w", encoding="utf-8", newline="") as fh:
        fields = [
            "dataset",
            "path",
            "candles",
            "total_return_pct",
            "max_drawdown_pct",
            "daily_volatility_pct",
            "avg_range_pct",
            "down_candle_ratio",
            "negative_return_ratio",
            "trend_efficiency",
            "adversarial_score",
            "profitable_profile_ratio",
        ]
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for d in details:
            writer.writerow(
                {
                    "dataset": d.dataset,
                    "path": d.path,
                    "candles": d.candles,
                    "total_return_pct": round(d.total_return_pct, 6),
                    "max_drawdown_pct": round(d.max_drawdown_pct, 6),
                    "daily_volatility_pct": round(d.daily_volatility_pct, 6),
                    "avg_range_pct": round(d.avg_range_pct, 6),
                    "down_candle_ratio": round(d.down_candle_ratio, 6),
                    "negative_return_ratio": round(d.negative_return_ratio, 6),
                    "trend_efficiency": round(d.trend_efficiency, 6),
                    "adversarial_score": round(d.adversarial_score, 6),
                    "profitable_profile_ratio": round(d.profitable_profile_ratio, 6),
                }
            )

    print("[DatasetHostility] Completed")
    print(f"summary_json={output_json}")
    print(f"details_csv={output_csv}")
    print(f"[DatasetHostility] hostility_level={hostility_level}, avg_score={avg_score:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
