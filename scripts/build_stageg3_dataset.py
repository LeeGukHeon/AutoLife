
#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import hashlib
import math
import re
import subprocess
import sys
from collections import deque
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from _script_common import dump_json, load_json_or_none, resolve_repo_path


TF_LIST = (1, 5, 15, 60, 240)
MARKET_1M_RE = re.compile(r"^upbit_(KRW_[A-Z0-9]+)_1m_full\.csv$")


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Stage G3 dataset prep from backtest_probabilistic (top-N markets, recent window, companion TF check)."
    )
    p.add_argument("--source-dir", default=r".\data\backtest_probabilistic")
    p.add_argument(
        "--output-feature-dir",
        default=r".\data\model_input\probabilistic_features_v3_full_label_tp1H20_20260301",
    )
    p.add_argument(
        "--temp-input-dir",
        default=r".\build\Release\tmp\stageG3_input_full_20260301",
    )
    p.add_argument(
        "--report-json",
        default=r".\build\Release\logs\stageG3_dataset_build_report.json",
    )
    p.add_argument(
        "--feature-build-summary-json",
        default=r".\build\Release\logs\probabilistic_feature_build_summary_stageG3_20260301.json",
    )
    p.add_argument(
        "--feature-manifest-json",
        default=r".\data\model_input\probabilistic_features_v3_full_label_tp1H20_20260301\feature_dataset_manifest.json",
    )
    p.add_argument(
        "--label-report-json",
        default=r".\build\Release\logs\stageG3_label_report.json",
    )
    p.add_argument(
        "--label-policy-json",
        default=r".\scripts\label_policies\label_tp1_policy.json",
    )
    p.add_argument("--top-n", type=int, default=50)
    p.add_argument("--window-days", type=int, default=180)
    p.add_argument("--tail-score-rows", type=int, default=10080, help="Rows for liquidity score (~7 days).")
    p.add_argument("--max-rows-per-market", type=int, default=0)
    p.add_argument("--skip-existing", action="store_true")
    p.add_argument("--markets", default="", help="Optional comma-separated market override.")
    return p.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def token_to_market(token: str) -> str:
    parts = token.split("_", 1)
    if len(parts) != 2:
        return token.replace("_", "-")
    return f"{parts[0]}-{parts[1]}"


def market_to_token(market: str) -> str:
    return str(market).strip().upper().replace("-", "_")


def parse_int(cell: Any, default: int = -1) -> int:
    try:
        return int(float(cell))
    except Exception:
        return int(default)


def parse_float(cell: Any, default: float = 0.0) -> float:
    try:
        value = float(cell)
    except Exception:
        return float(default)
    return float(value) if math.isfinite(value) else float(default)


def sha256_file(path_value: Path) -> str:
    h = hashlib.sha256()
    with path_value.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def discover_candidates(source_dir: Path) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    for path in sorted(source_dir.glob("upbit_KRW_*_1m_full.csv")):
        m = MARKET_1M_RE.match(path.name)
        if not m:
            continue
        token = m.group(1)
        market = token_to_market(token)
        tf_paths = {tf: source_dir / f"upbit_{token}_{tf}m_full.csv" for tf in TF_LIST}
        if not all(p.exists() for p in tf_paths.values()):
            continue
        out.append({"market": market, "token": token, "tf_paths": tf_paths})
    return out


def recent_notional_score(path_1m: Path, tail_rows: int) -> Dict[str, Any]:
    q: deque[float] = deque(maxlen=max(10, int(tail_rows)))
    rows = 0
    first_ts = -1
    last_ts = -1
    with path_1m.open("r", encoding="utf-8", errors="ignore", newline="") as fp:
        reader = csv.DictReader(fp)
        for row in reader:
            ts = parse_int(row.get("timestamp"), -1)
            if ts <= 0:
                continue
            close = parse_float(row.get("close"), 0.0)
            volume = max(0.0, parse_float(row.get("volume"), 0.0))
            if close <= 0.0:
                continue
            if first_ts <= 0:
                first_ts = ts
            last_ts = ts
            rows += 1
            q.append(close * volume)

    if rows <= 0:
        return {
            "rows": 0,
            "first_ts": -1,
            "last_ts": -1,
            "tail_count": 0,
            "recent_notional_avg": 0.0,
            "recent_notional_sum": 0.0,
        }
    tail_sum = float(sum(q))
    tail_count = int(len(q))
    return {
        "rows": int(rows),
        "first_ts": int(first_ts),
        "last_ts": int(last_ts),
        "tail_count": int(tail_count),
        "recent_notional_avg": (tail_sum / float(tail_count)) if tail_count > 0 else 0.0,
        "recent_notional_sum": float(tail_sum),
    }

def filter_copy_csv_by_start_ts(source_path: Path, dest_path: Path, start_ts_ms: int) -> Dict[str, Any]:
    dest_path.parent.mkdir(parents=True, exist_ok=True)
    rows = 0
    first_ts = -1
    last_ts = -1
    fieldnames: List[str] = []

    with source_path.open("r", encoding="utf-8", errors="ignore", newline="") as fp_in:
        reader = csv.DictReader(fp_in)
        fieldnames = list(reader.fieldnames or [])
        with dest_path.open("w", encoding="utf-8", newline="") as fp_out:
            writer = csv.DictWriter(fp_out, fieldnames=fieldnames)
            writer.writeheader()
            for row in reader:
                ts = parse_int(row.get("timestamp"), -1)
                if ts < int(start_ts_ms):
                    continue
                writer.writerow(row)
                rows += 1
                if first_ts <= 0:
                    first_ts = ts
                last_ts = ts

    return {
        "rows": int(rows),
        "first_ts": int(first_ts),
        "last_ts": int(last_ts),
        "sha256": sha256_file(dest_path),
        "dest_path": str(dest_path),
    }


def select_markets(
    *,
    candidates: List[Dict[str, Any]],
    top_n: int,
    tail_rows: int,
    forced_markets: List[str],
) -> List[Dict[str, Any]]:
    forced_set = set(m.strip().upper() for m in forced_markets if m.strip())

    scored: List[Dict[str, Any]] = []
    for cand in candidates:
        market = str(cand["market"]).strip().upper()
        stats = recent_notional_score(Path(cand["tf_paths"][1]), tail_rows=tail_rows)
        item = {
            "market": market,
            "token": cand["token"],
            "tf_paths": cand["tf_paths"],
            "score": float(stats["recent_notional_avg"]),
            "score_tail_count": int(stats["tail_count"]),
            "rows_1m": int(stats["rows"]),
            "first_ts_1m": int(stats["first_ts"]),
            "last_ts_1m": int(stats["last_ts"]),
        }
        scored.append(item)

    if forced_set:
        selected = [x for x in scored if x["market"] in forced_set]
        selected.sort(key=lambda x: x["market"])
        return selected

    scored.sort(key=lambda x: (x["score"], x["rows_1m"], x["market"]), reverse=True)
    n = max(1, int(top_n))
    return scored[:n]


def build_temp_input(
    *,
    selected: List[Dict[str, Any]],
    temp_input_dir: Path,
    window_days: int,
) -> Dict[str, Any]:
    if temp_input_dir.exists():
        for p in sorted(temp_input_dir.glob("*")):
            if p.is_file():
                p.unlink(missing_ok=True)
            elif p.is_dir():
                import shutil

                shutil.rmtree(p, ignore_errors=True)
    temp_input_dir.mkdir(parents=True, exist_ok=True)

    window_ms = int(max(1, int(window_days)) * 24 * 60 * 60 * 1000)
    market_reports: Dict[str, Any] = {}
    total_tf_files_expected = len(selected) * len(TF_LIST)
    total_tf_files_written = 0
    global_start = None
    global_end = None

    for item in selected:
        market = item["market"]
        token = item["token"]
        end_ts = int(item["last_ts_1m"])
        start_ts = max(0, end_ts - window_ms)

        tf_rows: Dict[str, Any] = {}
        for tf in TF_LIST:
            src = Path(item["tf_paths"][tf])
            dst = temp_input_dir / f"upbit_{token}_{tf}m_full.csv"
            meta = filter_copy_csv_by_start_ts(src, dst, start_ts_ms=start_ts)
            tf_rows[f"{tf}m"] = meta
            if int(meta["rows"]) > 0:
                total_tf_files_written += 1
                ts_first = int(meta["first_ts"])
                ts_last = int(meta["last_ts"])
                if ts_first > 0:
                    global_start = ts_first if global_start is None else min(global_start, ts_first)
                if ts_last > 0:
                    global_end = ts_last if global_end is None else max(global_end, ts_last)

        market_reports[market] = {
            "token": token,
            "liquidity_score_recent_notional_avg": float(item["score"]),
            "liquidity_score_tail_count": int(item["score_tail_count"]),
            "source_rows_1m": int(item["rows_1m"]),
            "source_first_ts_1m": int(item["first_ts_1m"]),
            "source_last_ts_1m": int(item["last_ts_1m"]),
            "window_start_ts_ms": int(start_ts),
            "window_end_ts_ms": int(end_ts),
            "timeframes": tf_rows,
        }

    tf_present_rate = (
        float(total_tf_files_written) / float(total_tf_files_expected)
        if total_tf_files_expected > 0
        else 0.0
    )
    return {
        "temp_input_dir": str(temp_input_dir),
        "market_reports": market_reports,
        "selected_markets": [x["market"] for x in selected],
        "selected_tokens": [x["token"] for x in selected],
        "tf_files_present_rate": float(tf_present_rate),
        "global_window_start_ts_ms": int(global_start) if global_start is not None else -1,
        "global_window_end_ts_ms": int(global_end) if global_end is not None else -1,
    }


def run_feature_builder(
    *,
    temp_input_dir: Path,
    output_feature_dir: Path,
    feature_manifest_json: Path,
    feature_build_summary_json: Path,
    label_report_json: Path,
    label_policy_json: Path,
    selected_markets: List[str],
    max_rows_per_market: int,
    skip_existing: bool,
) -> Dict[str, Any]:
    cmd = [
        sys.executable,
        str((Path.cwd() / "scripts" / "build_probabilistic_feature_dataset.py").resolve()),
        "--input-dir",
        str(temp_input_dir),
        "--output-dir",
        str(output_feature_dir),
        "--manifest-json",
        str(feature_manifest_json),
        "--summary-json",
        str(feature_build_summary_json),
        "--markets",
        ",".join(selected_markets),
        "--max-rows-per-market",
        str(int(max_rows_per_market)),
        "--label-policy-json",
        str(label_policy_json),
        "--stageg2-label-report-json",
        str(label_report_json),
    ]
    if bool(skip_existing):
        cmd.append("--skip-existing")

    proc = subprocess.run(
        cmd,
        text=True,
        capture_output=True,
        encoding="utf-8",
        errors="ignore",
        cwd=str(Path.cwd()),
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "build_probabilistic_feature_dataset failed\n"
            f"cmd={' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )

    manifest = load_json_or_none(feature_manifest_json)
    if not isinstance(manifest, dict):
        raise RuntimeError(f"invalid feature manifest after build: {feature_manifest_json}")
    return {
        "command": cmd,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "manifest": manifest,
    }


def to_iso(ts_ms: int) -> str:
    if int(ts_ms) <= 0:
        return ""
    return datetime.fromtimestamp(float(ts_ms) / 1000.0, tz=timezone.utc).isoformat()


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    source_dir = resolve_repo_path(args.source_dir)
    output_feature_dir = resolve_repo_path(args.output_feature_dir)
    temp_input_dir = resolve_repo_path(args.temp_input_dir)
    report_json = resolve_repo_path(args.report_json)
    feature_build_summary_json = resolve_repo_path(args.feature_build_summary_json)
    feature_manifest_json = resolve_repo_path(args.feature_manifest_json)
    label_report_json = resolve_repo_path(args.label_report_json)
    label_policy_json = resolve_repo_path(args.label_policy_json)

    if not source_dir.exists():
        raise FileNotFoundError(f"source-dir not found: {source_dir}")
    if not label_policy_json.exists():
        raise FileNotFoundError(f"label-policy-json not found: {label_policy_json}")

    forced_markets = [x.strip().upper() for x in str(args.markets).split(",") if x.strip()]

    candidates = discover_candidates(source_dir)
    selected = select_markets(
        candidates=candidates,
        top_n=int(args.top_n),
        tail_rows=int(args.tail_score_rows),
        forced_markets=forced_markets,
    )
    if not selected:
        raise RuntimeError("no eligible markets selected (need 1m+5m+15m+60m+240m full files)")

    prepared = build_temp_input(
        selected=selected,
        temp_input_dir=temp_input_dir,
        window_days=int(args.window_days),
    )

    built = run_feature_builder(
        temp_input_dir=temp_input_dir,
        output_feature_dir=output_feature_dir,
        feature_manifest_json=feature_manifest_json,
        feature_build_summary_json=feature_build_summary_json,
        label_report_json=label_report_json,
        label_policy_json=label_policy_json,
        selected_markets=prepared["selected_markets"],
        max_rows_per_market=int(args.max_rows_per_market),
        skip_existing=bool(args.skip_existing),
    )

    manifest = built["manifest"]
    jobs = manifest.get("jobs", []) if isinstance(manifest.get("jobs"), list) else []
    from_utc_values = [str(j.get("from_utc", "")).strip() for j in jobs if str(j.get("from_utc", "")).strip()]
    to_utc_values = [str(j.get("to_utc", "")).strip() for j in jobs if str(j.get("to_utc", "")).strip()]

    report = {
        "generated_at_utc": utc_now_iso(),
        "status": "pass",
        "source_dir": str(source_dir),
        "label_policy_json": str(label_policy_json),
        "selection": {
            "candidate_market_count": int(len(candidates)),
            "selected_market_count": int(len(selected)),
            "selected_markets": prepared["selected_markets"],
            "selection_mode": "forced_markets" if forced_markets else "topN_recent_notional",
            "top_n": int(args.top_n),
            "tail_score_rows": int(args.tail_score_rows),
        },
        "window": {
            "window_days": int(args.window_days),
            "global_window_start_ts_ms": int(prepared["global_window_start_ts_ms"]),
            "global_window_end_ts_ms": int(prepared["global_window_end_ts_ms"]),
            "global_window_start_utc": to_iso(int(prepared["global_window_start_ts_ms"])),
            "global_window_end_utc": to_iso(int(prepared["global_window_end_ts_ms"])),
        },
        "tf_files_present_rate": float(prepared["tf_files_present_rate"]),
        "feature_dataset": {
            "output_feature_dir": str(output_feature_dir),
            "feature_manifest_json": str(feature_manifest_json),
            "feature_build_summary_json": str(feature_build_summary_json),
            "label_report_json": str(label_report_json),
            "rows_total": int(manifest.get("total_feature_rows", 0) or 0),
            "date_range_from_utc": min(from_utc_values) if from_utc_values else "",
            "date_range_to_utc": max(to_utc_values) if to_utc_values else "",
        },
        "market_details": prepared["market_reports"],
    }
    dump_json(report_json, report)

    print("[StageG3Dataset] Completed")
    print(f"report_json={report_json}")
    print(f"feature_manifest_json={feature_manifest_json}")
    print(f"label_report_json={label_report_json}")
    print(f"selected_markets={len(prepared['selected_markets'])}")
    print(f"rows_total={int(manifest.get('total_feature_rows', 0) or 0)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
