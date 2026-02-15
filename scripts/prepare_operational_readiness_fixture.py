#!/usr/bin/env python3
import argparse
import json
from datetime import datetime, timezone

from _script_common import ensure_parent_directory, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshot-path", "-SnapshotPath", default="build/Release/state/snapshot_state.json")
    parser.add_argument("--legacy-state-path", "-LegacyStatePath", default="build/Release/state/state.json")
    parser.add_argument("--journal-path", "-JournalPath", default="build/Release/state/event_journal.jsonl")
    parser.add_argument("--log-path", "-LogPath", default="build/Release/logs/ci_fixture/autolife_ci_fixture.log")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    snapshot_path = resolve_repo_path(args.snapshot_path)
    legacy_path = resolve_repo_path(args.legacy_state_path)
    journal_path = resolve_repo_path(args.journal_path)
    log_path = resolve_repo_path(args.log_path)

    for p in (snapshot_path, legacy_path, journal_path, log_path):
        ensure_parent_directory(p)

    ts_ms = int(datetime.now(tz=timezone.utc).timestamp() * 1000)
    state = {
        "timestamp": ts_ms,
        "snapshot_last_event_seq": 0,
        "open_positions": [],
    }

    serialized_state = json.dumps(state, ensure_ascii=False, indent=2) + "\n"
    snapshot_path.write_text(serialized_state, encoding="utf-8")
    legacy_path.write_text(serialized_state, encoding="utf-8")

    journal_event = {
        "seq": 0,
        "ts_ms": ts_ms,
        "type": "POLICY_DECISION",
        "market": "KRW-BTC",
        "source": "ci_fixture",
        "payload": {"note": "stage8_ci_fixture"},
    }
    journal_path.write_text(json.dumps(journal_event, ensure_ascii=False) + "\n", encoding="utf-8")

    snapshot_log_path = str(snapshot_path).replace("\\", "\\\\")
    log_lines = [
        f"[2026-02-13 00:00:00.000] [main] [info] State snapshot loaded: path={snapshot_log_path}, timestamp={ts_ms}, last_event_seq=0",
        "[2026-02-13 00:00:00.001] [main] [info] State restore: no replay events applied (start_seq=1, journal_last_seq=0)",
        "[2026-02-13 00:00:00.002] [main] [info] Account state sync summary: wallet_markets=1, reconcile_candidates=0, restored_positions=0, external_closes=0",
        "[2026-02-13 00:00:00.003] [main] [info] Account state synchronization completed",
    ]
    log_path.write_text("\n".join(log_lines) + "\n", encoding="utf-8")

    print(f"[OperationalFixture] PASSED - snapshot={snapshot_path}")
    print(f"[OperationalFixture] PASSED - journal={journal_path}")
    print(f"[OperationalFixture] PASSED - log={log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
