#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import collect_strategyless_exit_audit as audit_script


class CollectStrategylessExitAuditTest(unittest.TestCase):
    def test_live_exit_snapshot_dedup_and_denylist(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            log1 = root / "live1.log"
            log2 = root / "live2.log"
            log1.write_text(
                "\n".join(
                    [
                        "[2026-02-24 23:37:15] [info] Position exited: KRW-BTC | pnl -1 | reason=stop_loss",
                        "[2026-02-24 23:42:44] [info] Position exited: KRW-BTC | pnl 17 | reason=take_profit_full_due_to_min_order",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            # Duplicate first event + one denied reason.
            log2.write_text(
                "\n".join(
                    [
                        "[2026-02-24 23:37:15] [info] Position exited: KRW-BTC | pnl -1 | reason=stop_loss",
                        "[2026-02-25 09:59:40] [info] Position exited: KRW-DOGE | pnl 823 | reason=BacktestEOD",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            snapshot = audit_script.summarize_live_exit_reasons(
                [log1, log2],
                deny_reasons={"backtesteod"},
            )
            self.assertTrue(bool(snapshot.get("exists")))
            self.assertEqual(4, int(snapshot.get("matched_exit_count_raw", 0)))
            self.assertEqual(1, int(snapshot.get("duplicate_event_count", 0)))
            self.assertEqual(2, int(snapshot.get("matched_exit_count", 0)))
            reason_counts = snapshot.get("exit_reason_counts", {})
            self.assertEqual(1, int(reason_counts.get("stop_loss", 0)))
            self.assertEqual(1, int(reason_counts.get("take_profit_full_due_to_min_order", 0)))
            self.assertEqual(0, int(reason_counts.get("BacktestEOD", 0)))


if __name__ == "__main__":
    unittest.main()
