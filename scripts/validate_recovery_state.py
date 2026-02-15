#!/usr/bin/env python3
import argparse
import json
from datetime import datetime, timezone
from typing import Any, Dict, List

from _script_common import dump_json, read_nonempty_lines, resolve_repo_path


def _parse_json_line(line: str) -> Dict[str, Any] | None:
    try:
        value = json.loads(line)
    except Exception:
        return None
    if not isinstance(value, dict):
        return None
    return value


def _to_i64(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except Exception:
        return default


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshot-path", "-SnapshotPath", default="build/Release/state/snapshot_state.json")
    parser.add_argument("--legacy-state-path", "-LegacyStatePath", default="build/Release/state/state.json")
    parser.add_argument("--journal-path", "-JournalPath", default="build/Release/state/event_journal.jsonl")
    parser.add_argument("--output-json", "-OutputJson", default="build/Release/logs/recovery_state_validation.json")
    return parser.parse_args()


def main_with_paths(
    snapshot_path,
    legacy_path,
    journal_path,
    output_path,
) -> int:
    state_source = snapshot_path if snapshot_path.exists() else (legacy_path if legacy_path.exists() else None)
    result: Dict[str, Any] = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "snapshot_path": str(snapshot_path),
        "legacy_state_path": str(legacy_path),
        "journal_path": str(journal_path),
        "state_source": str(state_source) if state_source else None,
        "checks": {
            "state_file_exists": False,
            "journal_exists": False,
            "state_parsed": False,
            "seq_increasing": False,
            "snapshot_watermark_valid": False,
        },
        "metrics": {
            "snapshot_last_event_seq": 0,
            "snapshot_timestamp": 0,
            "journal_event_count": 0,
            "journal_parse_errors": 0,
            "journal_last_seq": 0,
            "replayable_event_count": 0,
            "predicted_open_positions_after_replay": 0,
        },
        "errors": [],
    }

    if state_source is None:
        result["errors"].append("state_file_missing")
    else:
        result["checks"]["state_file_exists"] = True

    if not journal_path.exists():
        result["errors"].append("journal_missing")
    else:
        result["checks"]["journal_exists"] = True

    state_json: Dict[str, Any] | None = None
    if result["checks"]["state_file_exists"]:
        try:
            loaded = json.loads(state_source.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                state_json = loaded
                result["checks"]["state_parsed"] = True
            else:
                result["errors"].append("state_parse_error")
        except Exception:
            result["errors"].append("state_parse_error")

    events: List[Dict[str, Any]] = []
    if result["checks"]["journal_exists"]:
        for line in read_nonempty_lines(journal_path):
            parsed = _parse_json_line(line)
            if parsed is None:
                result["metrics"]["journal_parse_errors"] += 1
                continue
            events.append(parsed)
        result["metrics"]["journal_event_count"] = len(events)
        if events:
            events.sort(key=lambda e: _to_i64(e.get("seq"), 0))
            result["metrics"]["journal_last_seq"] = _to_i64(events[-1].get("seq"), 0)

            increasing = True
            previous = -1
            for event in events:
                seq = _to_i64(event.get("seq"), 0)
                if seq <= previous:
                    increasing = False
                    break
                previous = seq
            result["checks"]["seq_increasing"] = increasing
            if not increasing:
                result["errors"].append("journal_seq_not_increasing")
        else:
            result["checks"]["seq_increasing"] = True

    snapshot_last_seq = 0
    snapshot_timestamp = 0
    if state_json:
        snapshot_last_seq = _to_i64(state_json.get("snapshot_last_event_seq"), 0)
        snapshot_timestamp = _to_i64(state_json.get("timestamp"), 0)
    result["metrics"]["snapshot_last_event_seq"] = snapshot_last_seq
    result["metrics"]["snapshot_timestamp"] = snapshot_timestamp

    if result["checks"]["journal_exists"]:
        if snapshot_last_seq <= result["metrics"]["journal_last_seq"]:
            result["checks"]["snapshot_watermark_valid"] = True
        else:
            result["errors"].append("snapshot_watermark_exceeds_journal_last_seq")

    replayable = [e for e in events if _to_i64(e.get("seq"), 0) > snapshot_last_seq]
    result["metrics"]["replayable_event_count"] = len(replayable)

    open_map: Dict[str, Dict[str, Any]] = {}
    if state_json and isinstance(state_json.get("open_positions"), list):
        for position in state_json["open_positions"]:
            if not isinstance(position, dict):
                continue
            market = str(position.get("market", "")).strip()
            if not market:
                continue
            open_map[market] = {
                "market": market,
                "entry_price": float(position.get("entry_price", 0.0)),
                "quantity": float(position.get("quantity", 0.0)),
            }

    for event in replayable:
        market = str(event.get("market", "")).strip()
        if not market:
            continue
        payload = event.get("payload")
        if not isinstance(payload, dict):
            payload = {}

        event_type = str(event.get("type", ""))
        if event_type == "POSITION_OPENED":
            open_map[market] = {
                "market": market,
                "entry_price": float(payload.get("entry_price", 0.0)),
                "quantity": float(payload.get("quantity", 0.0)),
            }
        elif event_type == "FILL_APPLIED":
            side = str(payload.get("side", "")).lower()
            if side == "buy" and market not in open_map:
                open_map[market] = {
                    "market": market,
                    "entry_price": float(payload.get("avg_price", 0.0)),
                    "quantity": float(payload.get("filled_volume", 0.0)),
                }
        elif event_type == "POSITION_REDUCED" and market in open_map:
            qty = float(open_map[market].get("quantity", 0.0))
            reduced = float(payload.get("quantity", 0.0))
            if reduced > 0.0:
                qty = max(0.0, qty - reduced)
            if qty <= 0.0:
                open_map.pop(market, None)
            else:
                open_map[market]["quantity"] = qty
        elif event_type == "POSITION_CLOSED":
            open_map.pop(market, None)

    result["metrics"]["predicted_open_positions_after_replay"] = len(open_map)
    dump_json(output_path, result)

    hard_failures = [
        not result["checks"]["state_file_exists"],
        not result["checks"]["journal_exists"],
        not result["checks"]["state_parsed"],
        not result["checks"]["seq_increasing"],
        not result["checks"]["snapshot_watermark_valid"],
    ]
    if any(hard_failures):
        print(f"[RecoveryValidate] FAILED - see {output_path}")
        return 1
    print(f"[RecoveryValidate] PASSED - see {output_path}")
    return 0


def main() -> int:
    args = parse_args()
    snapshot_path = resolve_repo_path(args.snapshot_path)
    legacy_path = resolve_repo_path(args.legacy_state_path)
    journal_path = resolve_repo_path(args.journal_path)
    output_path = resolve_repo_path(args.output_json)
    return main_with_paths(snapshot_path, legacy_path, journal_path, output_path)


if __name__ == "__main__":
    raise SystemExit(main())
