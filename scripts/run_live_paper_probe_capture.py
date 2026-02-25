#!/usr/bin/env python3
"""Run interactive live mode for a bounded time and capture exit evidence.

Safety defaults:
- Requires trading.allow_live_orders=false in config unless --allow-live-orders is passed.
- Sends only mode selection input ("1") and never attempts direct order API calls from script.
- Optional trading overrides are applied temporarily and the config is restored automatically.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import json
import re
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple

from _script_common import dump_json, resolve_repo_path


EXIT_PATTERN = re.compile(r"Position exited:\s*([A-Z0-9\-]+)\s*\|.*?\|\s*reason=([A-Za-z0-9_]+)")


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run AutoLifeTrading live mode for bounded duration and capture exit samples."
    )
    parser.add_argument("--exe-path", default="build/Release/AutoLifeTrading.exe")
    parser.add_argument("--config-path", default="build/Release/config/config.json")
    parser.add_argument("--duration-seconds", type=int, default=300)
    parser.add_argument("--repeat-count", type=int, default=1)
    parser.add_argument("--cooldown-seconds", type=int, default=0)
    parser.add_argument("--mode-input", default="1", help="Interactive mode input. '1' means live mode.")
    parser.add_argument("--stdout-log", default="")
    parser.add_argument("--summary-json", default="")
    parser.add_argument(
        "--trading-overrides-json",
        default="",
        help="JSON object for temporary trading config overrides. Applied only during probe run.",
    )
    parser.add_argument(
        "--trading-overrides-file",
        default="",
        help="Path to JSON file containing temporary trading overrides (root object or {\"trading\": {...}}).",
    )
    parser.add_argument(
        "--target-runtime-exit-delta",
        type=int,
        default=0,
        help="Stop early when cumulative runtime exit delta reaches this value (0 disables).",
    )
    parser.add_argument("--runtime-log-glob", default="build/Release/logs/autolife*.log")
    parser.add_argument(
        "--allow-live-orders",
        action="store_true",
        help="Disable safety check for trading.allow_live_orders=false.",
    )
    return parser.parse_args(argv)


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def format_ts(value: datetime) -> str:
    return value.strftime("%Y%m%d_%H%M%S")


def load_config_payload(config_path: Path) -> Dict[str, Any]:
    return json.loads(config_path.read_text(encoding="utf-8-sig"))


def _normalize_trading_overrides(payload: Any) -> Dict[str, Any]:
    if not isinstance(payload, dict):
        return {}
    trading_raw = payload.get("trading")
    if isinstance(trading_raw, dict):
        return dict(trading_raw)
    return dict(payload)


def parse_trading_overrides(
    trading_overrides_json: str,
    trading_overrides_file: str,
) -> Dict[str, Any]:
    overrides: Dict[str, Any] = {}
    if str(trading_overrides_file).strip():
        payload = json.loads(resolve_repo_path(str(trading_overrides_file)).read_text(encoding="utf-8-sig"))
        overrides.update(_normalize_trading_overrides(payload))
    if str(trading_overrides_json).strip():
        payload = json.loads(str(trading_overrides_json))
        overrides.update(_normalize_trading_overrides(payload))
    return overrides


def build_probe_config_payload(
    base_payload: Dict[str, Any],
    trading_overrides: Dict[str, Any],
) -> Dict[str, Any]:
    merged = copy.deepcopy(base_payload)
    trading = merged.get("trading")
    if not isinstance(trading, dict):
        trading = {}
    if trading_overrides:
        trading.update(trading_overrides)
    merged["trading"] = trading
    return merged


def ensure_safe_config_payload(config_payload: Dict[str, Any], allow_live_orders_override: bool) -> Tuple[bool, str]:
    trading = config_payload.get("trading", {})
    if not isinstance(trading, dict):
        trading = {}
    allow_live_orders = bool(trading.get("allow_live_orders", False))
    if allow_live_orders and not allow_live_orders_override:
        return False, "safety_blocked_allow_live_orders_true"
    return True, ""


def ensure_safe_config(config_path: Path, allow_live_orders_override: bool) -> Tuple[bool, str]:
    if not config_path.exists():
        return False, f"config_not_found:{config_path}"
    try:
        cfg = load_config_payload(config_path)
    except Exception as exc:
        return False, f"config_parse_failed:{exc}"
    return ensure_safe_config_payload(cfg, allow_live_orders_override)


@contextlib.contextmanager
def managed_probe_config(
    config_path: Path,
    trading_overrides: Dict[str, Any],
    allow_live_orders_override: bool,
) -> Any:
    if not config_path.exists():
        raise FileNotFoundError(f"config_not_found:{config_path}")
    original_raw = config_path.read_text(encoding="utf-8")
    try:
        base_payload = load_config_payload(config_path)
        probe_payload = build_probe_config_payload(base_payload, trading_overrides)
        safe_ok, safe_reason = ensure_safe_config_payload(probe_payload, allow_live_orders_override)
        if not safe_ok:
            raise RuntimeError(f"safety_check_failed:{safe_reason}")

        config_changed = bool(trading_overrides)
        if config_changed:
            config_path.write_text(
                json.dumps(probe_payload, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
                newline="\n",
            )
        yield {
            "config_changed": config_changed,
            "trading_overrides": dict(trading_overrides),
            "probe_payload": probe_payload,
        }
    finally:
        config_path.write_text(original_raw, encoding="utf-8")


def resolve_output_paths(stdout_log_raw: str, summary_json_raw: str) -> Tuple[Path, Path]:
    stamp = format_ts(utc_now())
    default_stdout = resolve_repo_path(f"build/Release/logs/live_probe_stdout_{stamp}.txt")
    default_summary = resolve_repo_path(f"build/Release/logs/live_probe_summary_{stamp}.json")

    stdout_path = resolve_repo_path(stdout_log_raw) if str(stdout_log_raw).strip() else default_stdout
    summary_path = resolve_repo_path(summary_json_raw) if str(summary_json_raw).strip() else default_summary
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    return stdout_path, summary_path


def parse_exit_counts_from_lines(lines: Iterable[str]) -> Dict[str, Any]:
    reason_counts: Dict[str, int] = {}
    market_counts: Dict[str, int] = {}
    market_reason_counts: Dict[str, int] = {}
    total = 0
    for raw in lines:
        match = EXIT_PATTERN.search(raw)
        if not match:
            continue
        total += 1
        market = str(match.group(1)).strip() or "unknown"
        reason = str(match.group(2)).strip() or "unknown"
        reason_counts[reason] = reason_counts.get(reason, 0) + 1
        market_counts[market] = market_counts.get(market, 0) + 1
        key = f"{market}|{reason}"
        market_reason_counts[key] = market_reason_counts.get(key, 0) + 1
    return {
        "total": total,
        "reason_counts": reason_counts,
        "market_counts": market_counts,
        "market_reason_counts": market_reason_counts,
    }


def collect_runtime_log_exit_snapshot(pattern: str) -> Dict[str, Any]:
    snapshot: Dict[str, Any] = {
        "pattern": pattern,
        "files": [],
        "total": 0,
        "reason_counts": {},
        "market_counts": {},
        "market_reason_counts": {},
    }
    reason_counts: Dict[str, int] = {}
    market_counts: Dict[str, int] = {}
    market_reason_counts: Dict[str, int] = {}
    total = 0

    repo_root = resolve_repo_path(".")
    files: List[Path] = sorted([x for x in repo_root.glob(pattern) if x.is_file()])
    snapshot["files"] = [str(x.resolve()) for x in files]
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        parsed = parse_exit_counts_from_lines(text.splitlines())
        total += int(parsed.get("total", 0))
        for key, value in (parsed.get("reason_counts", {}) or {}).items():
            reason_counts[str(key)] = reason_counts.get(str(key), 0) + int(value)
        for key, value in (parsed.get("market_counts", {}) or {}).items():
            market_counts[str(key)] = market_counts.get(str(key), 0) + int(value)
        for key, value in (parsed.get("market_reason_counts", {}) or {}).items():
            market_reason_counts[str(key)] = market_reason_counts.get(str(key), 0) + int(value)

    snapshot["total"] = total
    snapshot["reason_counts"] = reason_counts
    snapshot["market_counts"] = market_counts
    snapshot["market_reason_counts"] = market_reason_counts
    return snapshot


def merge_exit_snapshots(left: Dict[str, Any], right: Dict[str, Any]) -> Dict[str, Any]:
    out = {
        "total": int(left.get("total", 0)) + int(right.get("total", 0)),
        "reason_counts": dict(left.get("reason_counts", {}) or {}),
        "market_counts": dict(left.get("market_counts", {}) or {}),
        "market_reason_counts": dict(left.get("market_reason_counts", {}) or {}),
    }
    for key, value in (right.get("reason_counts", {}) or {}).items():
        out["reason_counts"][str(key)] = int(out["reason_counts"].get(str(key), 0)) + int(value)
    for key, value in (right.get("market_counts", {}) or {}).items():
        out["market_counts"][str(key)] = int(out["market_counts"].get(str(key), 0)) + int(value)
    for key, value in (right.get("market_reason_counts", {}) or {}).items():
        out["market_reason_counts"][str(key)] = int(out["market_reason_counts"].get(str(key), 0)) + int(value)
    return out


def build_iteration_stdout_path(base_stdout_path: Path, index: int, repeat_count: int) -> Path:
    if repeat_count <= 1:
        return base_stdout_path
    stem = base_stdout_path.stem
    suffix = base_stdout_path.suffix
    path = base_stdout_path.with_name(f"{stem}_run{index:02d}{suffix}")
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def run_probe(exe_path: Path, mode_input: str, duration_seconds: int) -> Tuple[int, bool, str]:
    process = subprocess.Popen(
        [str(exe_path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    timed_out = False
    stdout_text = ""
    return_code = -1
    try:
        stdout_text, _ = process.communicate(
            input=f"{mode_input.strip()}\n",
            timeout=max(1, int(duration_seconds)),
        )
        return_code = int(process.returncode or 0)
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        stdout_text = str(exc.stdout or "")
        process.terminate()
        try:
            tail, _ = process.communicate(timeout=20)
            stdout_text += str(tail or "")
        except subprocess.TimeoutExpired:
            process.kill()
            tail, _ = process.communicate()
            stdout_text += str(tail or "")
        return_code = int(process.returncode or 0)
    return return_code, timed_out, stdout_text


def main(argv=None) -> int:
    args = parse_args(argv)
    exe_path = resolve_repo_path(args.exe_path)
    config_path = resolve_repo_path(args.config_path)
    duration_seconds = max(1, int(args.duration_seconds))
    repeat_count = max(1, int(args.repeat_count))
    cooldown_seconds = max(0, int(args.cooldown_seconds))
    target_runtime_exit_delta = max(0, int(args.target_runtime_exit_delta))
    stdout_log_path, summary_json_path = resolve_output_paths(args.stdout_log, args.summary_json)

    if not exe_path.exists():
        raise FileNotFoundError(f"exe_not_found:{exe_path}")

    trading_overrides = parse_trading_overrides(args.trading_overrides_json, args.trading_overrides_file)
    safe_ok, safe_reason = ensure_safe_config(config_path, bool(args.allow_live_orders))
    if not safe_ok and not trading_overrides:
        raise RuntimeError(f"safety_check_failed:{safe_reason}")

    with managed_probe_config(config_path, trading_overrides, bool(args.allow_live_orders)) as config_ctx:
        before_snapshot = collect_runtime_log_exit_snapshot(str(args.runtime_log_glob))
        started_at = utc_now()
        run_rows: List[Dict[str, Any]] = []
        stdout_exit_snapshot: Dict[str, Any] = {
            "total": 0,
            "reason_counts": {},
            "market_counts": {},
            "market_reason_counts": {},
        }
        timed_out_any = False
        final_return_code = 0

        for index in range(1, repeat_count + 1):
            iter_started = utc_now()
            return_code, timed_out, stdout_text = run_probe(exe_path, str(args.mode_input), duration_seconds)
            iter_ended = utc_now()
            timed_out_any = timed_out_any or bool(timed_out)
            final_return_code = int(return_code)

            iter_stdout_path = build_iteration_stdout_path(stdout_log_path, index, repeat_count)
            iter_stdout_path.write_text(stdout_text, encoding="utf-8", newline="\n")
            iter_stdout_exit = parse_exit_counts_from_lines(stdout_text.splitlines())
            stdout_exit_snapshot = merge_exit_snapshots(stdout_exit_snapshot, iter_stdout_exit)

            iter_runtime_after = collect_runtime_log_exit_snapshot(str(args.runtime_log_glob))
            iter_runtime_delta = int(iter_runtime_after.get("total", 0)) - int(before_snapshot.get("total", 0))
            run_rows.append(
                {
                    "index": index,
                    "started_at_utc": iter_started.isoformat(),
                    "ended_at_utc": iter_ended.isoformat(),
                    "duration_seconds": duration_seconds,
                    "return_code": int(return_code),
                    "timed_out": bool(timed_out),
                    "stdout_log_path": str(iter_stdout_path),
                    "stdout_exit_snapshot": iter_stdout_exit,
                    "runtime_exit_total_delta_from_start": iter_runtime_delta,
                }
            )

            if target_runtime_exit_delta > 0 and iter_runtime_delta >= target_runtime_exit_delta:
                break
            if cooldown_seconds > 0 and index < repeat_count:
                time.sleep(float(cooldown_seconds))

        ended_at = utc_now()
        after_snapshot = collect_runtime_log_exit_snapshot(str(args.runtime_log_glob))

    delta_total = int(after_snapshot.get("total", 0)) - int(before_snapshot.get("total", 0))
    summary = {
        "started_at_utc": started_at.isoformat(),
        "ended_at_utc": ended_at.isoformat(),
        "duration_seconds": duration_seconds,
        "repeat_count_requested": repeat_count,
        "repeat_count_executed": len(run_rows),
        "inputs": {
            "exe_path": str(exe_path),
            "config_path": str(config_path),
            "mode_input": str(args.mode_input),
            "runtime_log_glob": str(args.runtime_log_glob),
            "allow_live_orders_override": bool(args.allow_live_orders),
            "trading_overrides": trading_overrides,
            "target_runtime_exit_delta": target_runtime_exit_delta,
            "cooldown_seconds": cooldown_seconds,
        },
        "config_context": {
            "config_changed": bool(config_ctx.get("config_changed", False)),
            "trading_overrides_applied": dict(config_ctx.get("trading_overrides", {})),
        },
        "run_result": {
            "return_code": final_return_code,
            "timed_out": timed_out_any,
            "stdout_log_path": str(stdout_log_path),
        },
        "runs": run_rows,
        "stdout_exit_snapshot": stdout_exit_snapshot,
        "runtime_exit_snapshot_before": before_snapshot,
        "runtime_exit_snapshot_after": after_snapshot,
        "runtime_exit_total_delta": delta_total,
    }
    dump_json(summary_json_path, summary)

    for row in run_rows:
        print(f"[LivePaperProbe] wrote_stdout={row.get('stdout_log_path')}")
    print(f"[LivePaperProbe] wrote_summary={summary_json_path}")
    print(
        "[LivePaperProbe] "
        f"repeat={len(run_rows)}/{repeat_count} "
        f"timed_out_any={str(timed_out_any).lower()} return_code={final_return_code} "
        f"stdout_exit_total={stdout_exit_snapshot.get('total', 0)} "
        f"runtime_exit_delta={delta_total}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
