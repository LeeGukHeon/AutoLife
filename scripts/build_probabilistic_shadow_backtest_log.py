#!/usr/bin/env python3
import argparse
import bisect
import json
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

from _script_common import dump_json, read_nonempty_lines, resolve_repo_path, run_command


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build a live-timestamp-aligned backtest policy decision log for Gate4 "
            "shadow evidence from live-captured multi-market datasets."
        )
    )
    parser.add_argument(
        "--exe-path",
        default=r".\build\Release\AutoLifeTrading.exe",
    )
    parser.add_argument(
        "--live-decision-log-jsonl",
        default=r".\build\Release\logs\policy_decisions.jsonl",
    )
    parser.add_argument(
        "--dataset-dir",
        default=r".\build\Release\data\backtest_real_live",
    )
    parser.add_argument(
        "--backtest-policy-log-jsonl",
        default=r".\build\Release\logs\vnext_policy_decisions_backtest.jsonl",
        help="Path written by backtest runtime per replay run.",
    )
    parser.add_argument(
        "--output-jsonl",
        default=r".\build\Release\logs\policy_decisions_backtest_shadow_aligned.jsonl",
    )
    parser.add_argument(
        "--summary-json",
        default=r".\build\Release\logs\policy_decisions_backtest_shadow_aligned_summary.json",
    )
    parser.add_argument(
        "--match-tolerance-ms",
        type=int,
        default=600000,
    )
    parser.add_argument(
        "--max-new-orders-per-scan",
        type=int,
        default=1,
    )
    parser.add_argument(
        "--capacity-score-order",
        choices=("desc", "asc"),
        default="desc",
        help="Sort direction for per-scan capacity selection by policy/base score.",
    )
    parser.add_argument(
        "--markets",
        default="",
        help="Optional comma-separated market allowlist. Empty uses live decision-log markets.",
    )
    parser.add_argument("--strict", action="store_true")
    return parser.parse_args(argv)


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


def to_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    return str(value).strip().lower() in ("1", "true", "yes", "y", "on")


def canonical_strategy_token(value: Any) -> str:
    token = str(value or "").strip().lower()
    if token in (
        "foundation adaptive strategy",
        "foundation_adaptive",
        "foundation_adaptive_strategy",
        "probabilistic primary runtime",
        "probabilistic_primary_runtime",
    ):
        return "foundation adaptive strategy"
    return token


def normalize_strategy(value: Any) -> str:
    return canonical_strategy_token(value)


def parse_csv_markets(raw: str) -> List[str]:
    out: List[str] = []
    seen = set()
    for token in str(raw or "").split(","):
        market = token.strip().upper()
        if not market or market in seen:
            continue
        seen.add(market)
        out.append(market)
    return out


def parse_jsonl(path_value: Path) -> Tuple[List[Dict[str, Any]], int]:
    rows: List[Dict[str, Any]] = []
    parse_errors = 0
    for line in read_nonempty_lines(path_value):
        try:
            payload = json.loads(line)
        except Exception:
            parse_errors += 1
            continue
        if isinstance(payload, dict):
            rows.append(payload)
        else:
            parse_errors += 1
    return rows, parse_errors


def load_live_scans(path_value: Path) -> Dict[str, Any]:
    rows, parse_errors = parse_jsonl(path_value)
    scans: List[Dict[str, Any]] = []
    markets: set[str] = set()
    valid_ts = 0
    for row in rows:
        ts = to_int(row.get("ts", 0))
        decisions_raw = row.get("decisions", [])
        if not isinstance(decisions_raw, list):
            decisions_raw = []
        decisions: List[Dict[str, Any]] = []
        for item in decisions_raw:
            if not isinstance(item, dict):
                continue
            market = str(item.get("market", "")).strip().upper()
            if market:
                markets.add(market)
            decisions.append(item)
        if ts > 0:
            valid_ts += 1
        scans.append(
            {
                "ts": int(ts),
                "dominant_regime": str(row.get("dominant_regime", "")).strip(),
                "small_seed_mode": bool(to_bool(row.get("small_seed_mode", False))),
                "max_new_orders_per_scan": max(1, to_int(row.get("max_new_orders_per_scan", 1))),
                "decisions": decisions,
            }
        )
    return {
        "rows": scans,
        "markets": sorted(markets),
        "parse_errors": int(parse_errors),
        "valid_timestamp_rows": int(valid_ts),
    }


def market_to_dataset_token(market: str) -> str:
    return str(market).strip().upper().replace("-", "_").replace("/", "_")


def find_dataset_path(dataset_dir: Path, market: str) -> Optional[Path]:
    token = market_to_dataset_token(market)
    exact = dataset_dir / f"upbit_{token}_1m_live.csv"
    if exact.exists():
        return exact
    candidates: List[Path] = []
    for pattern in (f"upbit_{token}_1m_*.csv", f"upbit_{token}_1m*.csv"):
        for item in dataset_dir.glob(pattern):
            if item.is_file():
                candidates.append(item)
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


def nearest_live_timestamp(
    source_ts: int,
    live_sorted_timestamps: List[int],
    tolerance_ms: int,
) -> Tuple[int, int]:
    if source_ts <= 0 or not live_sorted_timestamps:
        return 0, 0
    idx = bisect.bisect_left(live_sorted_timestamps, source_ts)
    candidates: List[int] = []
    if idx < len(live_sorted_timestamps):
        candidates.append(live_sorted_timestamps[idx])
    if idx > 0:
        candidates.append(live_sorted_timestamps[idx - 1])
    if not candidates:
        return 0, 0
    best = min(candidates, key=lambda ts: (abs(source_ts - ts), ts))
    diff = abs(source_ts - best)
    if diff > max(0, int(tolerance_ms)):
        return 0, diff
    return int(best), int(diff)


def summarize_counter_mode(values: Iterable[Any], fallback: Any) -> Any:
    normalized = [x for x in values if x is not None]
    if not normalized:
        return fallback
    counter = Counter(normalized)
    return sorted(counter.items(), key=lambda item: (-item[1], str(item[0])))[0][0]


def run_backtest_for_dataset(
    exe_path: Path,
    dataset_path: Path,
    backtest_policy_log_path: Path,
) -> Dict[str, Any]:
    command = [
        str(exe_path),
        "--backtest",
        str(dataset_path),
        "--json",
        "--shadow-policy-only",
    ]
    result = run_command(command)
    rows: List[Dict[str, Any]] = []
    parse_errors = 0
    if result.exit_code == 0 and backtest_policy_log_path.exists():
        rows, parse_errors = parse_jsonl(backtest_policy_log_path)
    return {
        "cmd": command,
        "returncode": int(result.exit_code),
        "stdout_tail": [x for x in str(result.stdout or "").splitlines()[-3:]],
        "stderr_tail": [x for x in str(result.stderr or "").splitlines()[-3:]],
        "rows": rows,
        "parse_errors": int(parse_errors),
    }


def extract_decision_items(
    row: Dict[str, Any],
    market_hint: str,
) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    decisions_raw = row.get("decisions", [])
    if not isinstance(decisions_raw, list):
        return out
    for item in decisions_raw:
        if not isinstance(item, dict):
            continue
        market = str(item.get("market", market_hint)).strip().upper() or market_hint
        strategy = str(item.get("strategy", item.get("strategy_name", ""))).strip()
        if not market or not strategy:
            continue
        out.append(
            {
                "market": market,
                "strategy": strategy,
                "selected": bool(to_bool(item.get("selected", False))),
                "reason": str(item.get("reason", "")).strip(),
                "base_score": to_float(item.get("base_score", 0.0)),
                "policy_score": to_float(item.get("policy_score", 0.0)),
                "strength": to_float(item.get("strength", 0.0)),
                "expected_value": to_float(item.get("expected_value", 0.0)),
                "liquidity": to_float(item.get("liquidity", 0.0)),
                "volatility": to_float(item.get("volatility", 0.0)),
                "trades": max(0, to_int(item.get("trades", 0))),
                "win_rate": to_float(item.get("win_rate", 0.0)),
                "profit_factor": to_float(item.get("profit_factor", 0.0)),
                "used_preloaded_tf_5m": bool(to_bool(item.get("used_preloaded_tf_5m", False))),
                "used_preloaded_tf_1h": bool(to_bool(item.get("used_preloaded_tf_1h", False))),
                "used_resampled_tf_fallback": bool(to_bool(item.get("used_resampled_tf_fallback", False))),
            }
        )
    return out


def build_aligned_scans(
    live_scans: List[Dict[str, Any]],
    market_backtest_rows: Dict[str, List[Dict[str, Any]]],
    *,
    match_tolerance_ms: int,
    max_new_orders_per_scan: int,
    capacity_score_order: str = "desc",
    allowed_markets: Optional[set[str]] = None,
    restrict_to_live_scan_markets: bool = True,
) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    live_ts_order = [to_int(x.get("ts", 0)) for x in live_scans if to_int(x.get("ts", 0)) > 0]
    live_ts_sorted = sorted(set(live_ts_order))
    allowed = allowed_markets if allowed_markets is not None else set()
    live_markets_by_ts: Dict[int, set[str]] = {}
    live_decision_hints_by_ts: Dict[int, Dict[Tuple[str, str], Dict[str, Any]]] = defaultdict(dict)
    live_timestamps_by_market: Dict[str, List[int]] = defaultdict(list)
    for scan in live_scans:
        ts = to_int(scan.get("ts", 0))
        if ts <= 0:
            continue
        markets: set[str] = set()
        decisions_raw = scan.get("decisions", [])
        if isinstance(decisions_raw, list):
            for idx, item in enumerate(decisions_raw):
                if not isinstance(item, dict):
                    continue
                market = str(item.get("market", "")).strip().upper()
                if market:
                    markets.add(market)
                strategy_token = normalize_strategy(item.get("strategy", item.get("strategy_name", "")))
                if market and strategy_token:
                    live_decision_hints_by_ts[ts][(market, strategy_token)] = {
                        "selected": bool(to_bool(item.get("selected", False))),
                        "order": int(idx),
                        "policy_score": to_float(item.get("policy_score", 0.0)),
                        "base_score": to_float(item.get("base_score", 0.0)),
                    }
        live_markets_by_ts[ts] = markets
        for market in markets:
            live_timestamps_by_market[market].append(ts)

    for market, timestamps in list(live_timestamps_by_market.items()):
        live_timestamps_by_market[market] = sorted(set(to_int(x) for x in timestamps if to_int(x) > 0))

    candidate_by_key: Dict[Tuple[int, str, str], Dict[str, Any]] = {}
    keys_by_ts: Dict[int, set[Tuple[int, str, str]]] = defaultdict(set)
    header_votes: Dict[int, List[Dict[str, Any]]] = defaultdict(list)
    history_by_market_strategy: Dict[Tuple[str, str], List[Dict[str, Any]]] = defaultdict(list)
    history_timestamps_by_market_strategy: Dict[Tuple[str, str], List[int]] = {}
    strategies_by_market: Dict[str, set[str]] = defaultdict(set)

    unmatched_source_rows = 0
    matched_source_rows = 0
    raw_decision_count = 0
    fallback_candidate_count = 0

    for market, rows in market_backtest_rows.items():
        market_live_timestamps = live_timestamps_by_market.get(market, live_ts_sorted)
        for row in rows:
            source_ts = to_int(row.get("ts", 0))
            aligned_ts, diff_ms = nearest_live_timestamp(
                source_ts,
                market_live_timestamps if market_live_timestamps else live_ts_sorted,
                match_tolerance_ms,
            )
            if aligned_ts <= 0:
                unmatched_source_rows += 1
                continue
            matched_source_rows += 1
            header_votes[aligned_ts].append(
                {
                    "dominant_regime": str(row.get("dominant_regime", "")).strip(),
                    "small_seed_mode": bool(to_bool(row.get("small_seed_mode", False))),
                    "max_new_orders_per_scan": max(1, to_int(row.get("max_new_orders_per_scan", 1))),
                }
            )

            decision_items = extract_decision_items(row, market)
            for decision in decision_items:
                raw_decision_count += 1
                market_name = decision.get("market", "")
                if allowed and market_name not in allowed:
                    continue
                if bool(restrict_to_live_scan_markets):
                    live_markets = live_markets_by_ts.get(aligned_ts, set())
                    if live_markets and market_name not in live_markets:
                        continue
                strategy_token = normalize_strategy(decision.get("strategy", ""))
                if not strategy_token:
                    continue
                key = (aligned_ts, market_name, strategy_token)
                candidate = dict(decision)
                candidate["source_ts"] = int(source_ts)
                candidate["aligned_ts"] = int(aligned_ts)
                candidate["diff_ms"] = int(diff_ms)
                candidate["dominant_regime"] = str(row.get("dominant_regime", "")).strip()
                candidate["small_seed_mode"] = bool(to_bool(row.get("small_seed_mode", False)))
                candidate["max_new_orders_per_scan"] = max(1, to_int(row.get("max_new_orders_per_scan", 1)))
                history_key = (market_name, strategy_token)
                history_by_market_strategy[history_key].append(dict(candidate))
                strategies_by_market[market_name].add(strategy_token)
                prev = candidate_by_key.get(key)
                if prev is None:
                    candidate_by_key[key] = candidate
                    keys_by_ts[aligned_ts].add(key)
                    continue
                prefer_new = False
                prev_diff = to_int(prev.get("diff_ms", 0))
                if to_int(candidate.get("diff_ms", 0)) < prev_diff:
                    prefer_new = True
                elif to_int(candidate.get("diff_ms", 0)) == prev_diff:
                    if to_float(candidate.get("policy_score", 0.0)) > to_float(prev.get("policy_score", 0.0)):
                        prefer_new = True
                if prefer_new:
                    candidate_by_key[key] = candidate

    for history_key, history_items in history_by_market_strategy.items():
        sorted_items = sorted(
            history_items,
            key=lambda item: (
                to_int(item.get("source_ts", 0)),
                to_float(item.get("policy_score", 0.0)),
            ),
        )
        history_by_market_strategy[history_key] = sorted_items
        history_timestamps_by_market_strategy[history_key] = [
            to_int(item.get("source_ts", 0)) for item in sorted_items
        ]

    for scan in live_scans:
        ts = to_int(scan.get("ts", 0))
        if ts <= 0:
            continue
        if bool(restrict_to_live_scan_markets):
            target_scan_markets = set(live_markets_by_ts.get(ts, set()))
        elif allowed:
            target_scan_markets = set(allowed)
        else:
            target_scan_markets = set(strategies_by_market.keys())

        for market_name in sorted(target_scan_markets):
            if allowed and market_name not in allowed:
                continue
            strategy_tokens = sorted(strategies_by_market.get(market_name, set()))
            for strategy_token in strategy_tokens:
                key = (ts, market_name, strategy_token)
                if key in candidate_by_key:
                    continue
                history_key = (market_name, strategy_token)
                ts_index = history_timestamps_by_market_strategy.get(history_key, [])
                if not ts_index:
                    continue
                pos = bisect.bisect_right(ts_index, ts) - 1
                if pos < 0:
                    continue
                source_item = history_by_market_strategy[history_key][pos]
                source_ts = to_int(source_item.get("source_ts", 0))
                diff_ms = abs(ts - source_ts)
                if match_tolerance_ms > 0 and diff_ms > match_tolerance_ms:
                    continue
                fallback = dict(source_item)
                fallback["aligned_ts"] = int(ts)
                fallback["diff_ms"] = int(diff_ms)
                fallback["alignment_mode"] = "latest_before"
                candidate_by_key[key] = fallback
                keys_by_ts[ts].add(key)
                fallback_candidate_count += 1

    out_rows: List[Dict[str, Any]] = []
    empty_scan_count = 0
    dropped_capacity_reassign_count = 0
    selected_count = 0

    for scan in live_scans:
        ts = to_int(scan.get("ts", 0))
        if ts <= 0:
            continue
        candidate_keys = sorted(
            list(keys_by_ts.get(ts, set())),
            key=lambda item: (item[1], item[2]),
        )
        candidates = [candidate_by_key[k] for k in candidate_keys]
        live_hints = live_decision_hints_by_ts.get(ts, {})
        score_order = str(capacity_score_order or "desc").strip().lower()

        def hint_priority(item: Dict[str, Any]) -> Tuple[int, int]:
            hint = live_hints.get(
                (
                    str(item.get("market", "")).strip().upper(),
                    normalize_strategy(item.get("strategy", "")),
                )
            )
            if not isinstance(hint, dict):
                return (2, 1_000_000_000)
            selected_hint = bool(to_bool(hint.get("selected", False)))
            order_hint = max(0, to_int(hint.get("order", 1_000_000_000)))
            return (0 if selected_hint else 1, order_hint)

        if score_order == "asc":
            def sort_key(item: Dict[str, Any]) -> Tuple[Any, ...]:
                hint_bucket, hint_order = hint_priority(item)
                return (
                    hint_bucket,
                    hint_order,
                    to_float(item.get("policy_score", 0.0)),
                    to_float(item.get("base_score", 0.0)),
                    str(item.get("market", "")),
                    normalize_strategy(item.get("strategy", "")),
                )
            candidates.sort(
                key=sort_key
            )
        else:
            def sort_key(item: Dict[str, Any]) -> Tuple[Any, ...]:
                hint_bucket, hint_order = hint_priority(item)
                return (
                    hint_bucket,
                    hint_order,
                    -to_float(item.get("policy_score", 0.0)),
                    -to_float(item.get("base_score", 0.0)),
                    str(item.get("market", "")),
                    normalize_strategy(item.get("strategy", "")),
                )
            candidates.sort(
                key=sort_key
            )

        decisions_out: List[Dict[str, Any]] = []
        keep_n = max(1, int(max_new_orders_per_scan))
        for idx, item in enumerate(candidates):
            selected = idx < keep_n
            reason = str(item.get("reason", "")).strip()
            if selected:
                reason = "selected"
                selected_count += 1
            else:
                original_reason = str(reason).strip().lower()
                if original_reason in ("", "selected", "dropped_capacity"):
                    reason = "dropped_capacity"
                    dropped_capacity_reassign_count += 1
            decisions_out.append(
                {
                    "market": str(item.get("market", "")),
                    "strategy": str(item.get("strategy", "")),
                    "selected": bool(selected),
                    "reason": reason,
                    "base_score": to_float(item.get("base_score", 0.0)),
                    "policy_score": to_float(item.get("policy_score", 0.0)),
                    "strength": to_float(item.get("strength", 0.0)),
                    "expected_value": to_float(item.get("expected_value", 0.0)),
                    "liquidity": to_float(item.get("liquidity", 0.0)),
                    "volatility": to_float(item.get("volatility", 0.0)),
                    "trades": max(0, to_int(item.get("trades", 0))),
                    "win_rate": to_float(item.get("win_rate", 0.0)),
                    "profit_factor": to_float(item.get("profit_factor", 0.0)),
                    "used_preloaded_tf_5m": bool(to_bool(item.get("used_preloaded_tf_5m", False))),
                    "used_preloaded_tf_1h": bool(to_bool(item.get("used_preloaded_tf_1h", False))),
                    "used_resampled_tf_fallback": bool(to_bool(item.get("used_resampled_tf_fallback", False))),
                }
            )

        header_items = list(header_votes.get(ts, []))
        dominant_regime = summarize_counter_mode(
            [str(x.get("dominant_regime", "")).strip() for x in header_items if str(x.get("dominant_regime", "")).strip()],
            str(scan.get("dominant_regime", "")).strip(),
        )
        small_seed_mode = bool(
            summarize_counter_mode(
                [bool(to_bool(x.get("small_seed_mode", False))) for x in header_items],
                bool(to_bool(scan.get("small_seed_mode", False))),
            )
        )
        max_orders = max(
            1,
            to_int(
                summarize_counter_mode(
                    [max(1, to_int(x.get("max_new_orders_per_scan", keep_n))) for x in header_items],
                    keep_n,
                )
            ),
        )
        if not decisions_out:
            empty_scan_count += 1
        out_rows.append(
            {
                "ts": int(ts),
                "dominant_regime": str(dominant_regime),
                "small_seed_mode": bool(small_seed_mode),
                "max_new_orders_per_scan": int(max_orders),
                "source": "backtest_shadow_aligned",
                "decisions": decisions_out,
            }
        )

    diagnostics = {
        "live_scan_count": int(len(live_ts_order)),
        "unique_live_timestamp_count": int(len(live_ts_sorted)),
        "matched_source_rows": int(matched_source_rows),
        "unmatched_source_rows": int(unmatched_source_rows),
        "raw_decision_count": int(raw_decision_count),
        "aligned_candidate_count": int(len(candidate_by_key)),
        "fallback_candidate_count": int(fallback_candidate_count),
        "selected_count": int(selected_count),
        "dropped_capacity_reassign_count": int(dropped_capacity_reassign_count),
        "empty_scan_count": int(empty_scan_count),
    }
    return out_rows, diagnostics


def write_jsonl(path_value: Path, rows: List[Dict[str, Any]]) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)
    with path_value.open("w", encoding="utf-8", newline="\n") as fp:
        for row in rows:
            fp.write(json.dumps(row, ensure_ascii=False) + "\n")


def evaluate(args: argparse.Namespace) -> Dict[str, Any]:
    exe_path = resolve_repo_path(str(args.exe_path))
    live_log_path = resolve_repo_path(str(args.live_decision_log_jsonl))
    dataset_dir = resolve_repo_path(str(args.dataset_dir))
    backtest_policy_log_path = resolve_repo_path(str(args.backtest_policy_log_jsonl))
    output_jsonl_path = resolve_repo_path(str(args.output_jsonl))
    summary_json_path = resolve_repo_path(str(args.summary_json))
    match_tolerance_ms = max(0, to_int(args.match_tolerance_ms))
    max_new_orders_per_scan = max(1, to_int(args.max_new_orders_per_scan))
    capacity_score_order = str(getattr(args, "capacity_score_order", "desc")).strip().lower()
    if capacity_score_order not in ("desc", "asc"):
        capacity_score_order = "desc"
    requested_markets = parse_csv_markets(str(args.markets))

    errors: List[str] = []
    warnings: List[str] = []
    replay_runs: List[Dict[str, Any]] = []

    if not exe_path.exists():
        errors.append(f"exe_not_found:{exe_path}")
    if not live_log_path.exists():
        errors.append(f"live_decision_log_missing:{live_log_path}")
    if not dataset_dir.exists():
        errors.append(f"dataset_dir_missing:{dataset_dir}")

    live_payload = load_live_scans(live_log_path) if not errors else {"rows": [], "markets": [], "parse_errors": 0, "valid_timestamp_rows": 0}
    if to_int(live_payload.get("parse_errors", 0)) > 0:
        warnings.append("live_decision_log_parse_errors_detected")
    live_scans = list(live_payload.get("rows", []))
    if not live_scans:
        errors.append("live_decision_log_empty")

    live_markets = list(live_payload.get("markets", []))
    if requested_markets:
        target_markets = [m for m in requested_markets if m in set(live_markets)]
        missing_from_live = sorted(set(requested_markets) - set(target_markets))
        for market in missing_from_live:
            warnings.append(f"market_not_present_in_live_log:{market}")
    else:
        target_markets = live_markets
    if not target_markets:
        errors.append("no_target_markets")

    market_datasets: Dict[str, Path] = {}
    if not errors:
        for market in target_markets:
            dataset = find_dataset_path(dataset_dir, market)
            if dataset is None:
                errors.append(f"market_dataset_missing:{market}")
                continue
            market_datasets[market] = dataset

    market_backtest_rows: Dict[str, List[Dict[str, Any]]] = {}
    if not errors:
        for market in target_markets:
            dataset_path = market_datasets.get(market)
            if dataset_path is None:
                continue
            run_info = run_backtest_for_dataset(exe_path, dataset_path, backtest_policy_log_path)
            run_record = {
                "market": market,
                "dataset_path": str(dataset_path),
                "returncode": int(run_info.get("returncode", 1)),
                "rows": int(len(run_info.get("rows", []) or [])),
                "parse_errors": int(run_info.get("parse_errors", 0)),
                "stdout_tail": list(run_info.get("stdout_tail", [])),
                "stderr_tail": list(run_info.get("stderr_tail", [])),
            }
            replay_runs.append(run_record)
            if int(run_info.get("returncode", 1)) != 0:
                errors.append(f"backtest_replay_failed:{market}")
                continue
            if int(run_info.get("parse_errors", 0)) > 0:
                warnings.append(f"backtest_policy_log_parse_errors:{market}")
            market_backtest_rows[market] = list(run_info.get("rows", []))

    aligned_rows: List[Dict[str, Any]] = []
    diagnostics: Dict[str, Any] = {}
    if not errors:
        allowed = set(target_markets)
        aligned_rows, diagnostics = build_aligned_scans(
            live_scans,
            market_backtest_rows,
            match_tolerance_ms=match_tolerance_ms,
            max_new_orders_per_scan=max_new_orders_per_scan,
            capacity_score_order=capacity_score_order,
            allowed_markets=allowed,
        )
        write_jsonl(output_jsonl_path, aligned_rows)
        if len(aligned_rows) == 0:
            errors.append("aligned_backtest_log_empty")

    status = "pass" if len(errors) == 0 else "fail"
    summary = {
        "generated_at_utc": utc_now_iso(),
        "status": status,
        "errors": errors,
        "warnings": warnings,
        "params": {
            "match_tolerance_ms": int(match_tolerance_ms),
            "max_new_orders_per_scan": int(max_new_orders_per_scan),
            "capacity_score_order": capacity_score_order,
            "requested_markets": requested_markets,
        },
        "inputs": {
            "exe_path": str(exe_path),
            "live_decision_log_jsonl": str(live_log_path),
            "dataset_dir": str(dataset_dir),
            "backtest_policy_log_jsonl": str(backtest_policy_log_path),
        },
        "artifacts": {
            "output_jsonl": str(output_jsonl_path),
            "summary_json": str(summary_json_path),
        },
        "live": {
            "scan_count": int(len(live_scans)),
            "market_count": int(len(live_markets)),
            "markets": list(live_markets),
            "parse_errors": int(live_payload.get("parse_errors", 0)),
            "valid_timestamp_rows": int(live_payload.get("valid_timestamp_rows", 0)),
        },
        "target_markets": list(target_markets),
        "replay_runs": replay_runs,
        "alignment": diagnostics,
        "output_scan_count": int(len(aligned_rows)),
    }
    dump_json(summary_json_path, summary)
    return summary


def main(argv=None) -> int:
    args = parse_args(argv)
    out = evaluate(args)
    print("[BuildProbabilisticShadowBacktestLog] completed", flush=True)
    print(f"status={out.get('status', 'fail')}", flush=True)
    print(f"errors={len(out.get('errors', []) or [])}", flush=True)
    print(f"warnings={len(out.get('warnings', []) or [])}", flush=True)
    print(f"output={out.get('artifacts', {}).get('output_jsonl', '')}", flush=True)
    if bool(args.strict) and str(out.get("status", "fail")).lower() != "pass":
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

