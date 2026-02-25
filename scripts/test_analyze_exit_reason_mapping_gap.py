#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


def _write_json(path_value: Path, payload) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)
    path_value.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


class AnalyzeExitReasonMappingGapTest(unittest.TestCase):
    def test_canonical_reason_mapping_avoids_false_gap(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "analyze_exit_reason_mapping_gap.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live_log = root / "live.log"
            audit_json = root / "audit.json"
            output_json = root / "gap.json"

            live_log.write_text(
                "\n".join(
                    [
                        "Position exited: KRW-BTC | qty=1 | reason=stop_loss",
                        "Position exited: KRW-BTC | qty=1 | reason=take_profit_full_due_to_min_order",
                        "Position exited: KRW-BTC | qty=1 | reason=stop_loss",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            _write_json(
                audit_json,
                {
                    "dataset_results": [
                        {
                            "dataset": "upbit_KRW_BTC_1m_12000.csv",
                            "runtime_trade_distribution": {
                                "runtime_exit_reason_counts_in_samples": {
                                    "StopLoss": 2,
                                    "TakeProfitFullDueToMinOrder": 1,
                                }
                            },
                        }
                    ],
                    "live_exit_reason_snapshot": {
                        "path": str(live_log),
                    },
                },
            )

            cmd = [
                sys.executable,
                str(script_path),
                "--audit-json",
                str(audit_json),
                "--output-json",
                str(output_json),
                "--min-live-exits",
                "1",
                "--min-backtest-exits",
                "1",
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            result = json.loads(output_json.read_text(encoding="utf-8"))
            readiness = result.get("readiness", {})
            self.assertTrue(bool(readiness.get("comparable")))
            self.assertTrue(bool(readiness.get("sample_size_ready")))
            self.assertFalse(bool(readiness.get("mapping_gap_observed")))
            self.assertEqual("no_material_reason_mapping_gap_detected", readiness.get("next_step_hint"))

    def test_gap_marked_inconclusive_when_sample_too_small(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "analyze_exit_reason_mapping_gap.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live_log = root / "live.log"
            audit_json = root / "audit.json"
            output_json = root / "gap.json"

            live_log.write_text(
                "Position exited: KRW-BTC | qty=1 | reason=stop_loss\n",
                encoding="utf-8",
            )
            _write_json(
                audit_json,
                {
                    "dataset_results": [
                        {
                            "dataset": "upbit_KRW_BTC_1m_12000.csv",
                            "runtime_trade_distribution": {
                                "runtime_exit_reason_counts_in_samples": {
                                    "BacktestEOD": 1,
                                }
                            },
                        }
                    ],
                    "live_exit_reason_snapshot": {
                        "path": str(live_log),
                    },
                },
            )

            cmd = [
                sys.executable,
                str(script_path),
                "--audit-json",
                str(audit_json),
                "--output-json",
                str(output_json),
                "--min-live-exits",
                "10",
                "--min-backtest-exits",
                "10",
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            result = json.loads(output_json.read_text(encoding="utf-8"))
            readiness = result.get("readiness", {})
            self.assertFalse(bool(readiness.get("sample_size_ready")))
            self.assertTrue(bool(readiness.get("mapping_gap_observed_raw")))
            self.assertFalse(bool(readiness.get("mapping_gap_observed")))
            self.assertTrue(bool(readiness.get("mapping_gap_inconclusive_due_to_sample_size")))
            self.assertEqual("increase_overlap_exit_samples_before_mapping_decision", readiness.get("next_step_hint"))


if __name__ == "__main__":
    unittest.main()
