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


class ValidateShouldExitParityTest(unittest.TestCase):
    def test_backtest_eod_only_is_allowed_difference(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "validate_should_exit_parity.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            audit_json = root / "audit.json"
            output_json = root / "report.json"

            _write_json(
                audit_json,
                {
                    "dataset_results": [
                        {
                            "dataset": "upbit_KRW_BTC_1m_full.csv",
                            "exit_reason_counts": {
                                "StopLoss": 2,
                                "TakeProfit1": 1,
                                "BacktestEOD": 5,
                            },
                        }
                    ],
                    "live_exit_reason_snapshot": {
                        "exit_market_reason_counts": {
                            "KRW-BTC|stop_loss": 2,
                            "KRW-BTC|take_profit_1": 1,
                        }
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
                "--min-live-core-exits",
                "1",
                "--min-backtest-core-exits",
                "1",
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            report = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual("pass", report.get("verdict"))
            allowed = report.get("allowed_findings", [])
            self.assertTrue(any(item.get("reason") == "BACKTEST_EOD" for item in allowed if isinstance(item, dict)))

    def test_missing_critical_reason_is_failure(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "validate_should_exit_parity.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            audit_json = root / "audit.json"
            output_json = root / "report.json"

            _write_json(
                audit_json,
                {
                    "dataset_results": [
                        {
                            "dataset": "upbit_KRW_BTC_1m_full.csv",
                            "exit_reason_counts": {
                                "StopLoss": 4,
                                "TakeProfit1": 1,
                            },
                        }
                    ],
                    "live_exit_reason_snapshot": {
                        "exit_market_reason_counts": {
                            "KRW-BTC|take_profit_1": 5,
                        }
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
                "--min-live-core-exits",
                "1",
                "--min-backtest-core-exits",
                "1",
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            report = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual("fail", report.get("verdict"))
            critical = report.get("critical_findings", [])
            self.assertTrue(
                any(
                    item.get("type") == "critical_reason_missing" and item.get("reason") == "STOP_LOSS"
                    for item in critical
                    if isinstance(item, dict)
                )
            )

    def test_tp1_gap_is_inconclusive_when_tp1_not_observable_in_live_snapshot(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "validate_should_exit_parity.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            audit_json = root / "audit.json"
            output_json = root / "report.json"

            _write_json(
                audit_json,
                {
                    "dataset_results": [
                        {
                            "dataset": "upbit_KRW_BTC_1m_full.csv",
                            "exit_reason_counts": {
                                "StopLoss": 3,
                                "TakeProfit1": 2,
                            },
                        }
                    ],
                    "live_exit_reason_snapshot": {
                        "partial_tp1_observable": False,
                        "exit_market_reason_counts": {
                            "KRW-BTC|stop_loss": 5,
                        },
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
                "--min-live-core-exits",
                "1",
                "--min-backtest-core-exits",
                "1",
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            report = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual("inconclusive", report.get("verdict"))
            self.assertEqual(
                "collect_tp1_observable_live_exit_logs_before_tp1_gap_decision",
                report.get("next_step_hint"),
            )
            allowed = report.get("allowed_findings", [])
            self.assertTrue(
                any(
                    item.get("type") == "inconclusive_tp1_unobservable"
                    for item in allowed
                    if isinstance(item, dict)
                )
            )

    def test_low_sample_is_inconclusive(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "validate_should_exit_parity.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            audit_json = root / "audit.json"
            output_json = root / "report.json"

            _write_json(
                audit_json,
                {
                    "dataset_results": [
                        {
                            "dataset": "upbit_KRW_BTC_1m_full.csv",
                            "exit_reason_counts": {
                                "StopLoss": 1,
                            },
                        }
                    ],
                    "live_exit_reason_snapshot": {
                        "exit_market_reason_counts": {
                            "KRW-BTC|stop_loss": 1,
                        }
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
                "--min-live-core-exits",
                "10",
                "--min-backtest-core-exits",
                "10",
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            report = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual("inconclusive", report.get("verdict"))
            self.assertEqual("increase_core_exit_sample_size_before_should_exit_verdict", report.get("next_step_hint"))

    def test_tp1_unobservable_policy_pass_allows_non_blocking_verdict(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "validate_should_exit_parity.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            audit_json = root / "audit.json"
            output_json = root / "report.json"

            _write_json(
                audit_json,
                {
                    "dataset_results": [
                        {
                            "dataset": "upbit_KRW_BTC_1m_full.csv",
                            "exit_reason_counts": {
                                "StopLoss": 4,
                                "TakeProfit1": 2,
                                "TakeProfit2": 2,
                            },
                        }
                    ],
                    "live_exit_reason_snapshot": {
                        "partial_tp1_observable": False,
                        "exit_market_reason_counts": {
                            "KRW-BTC|stop_loss": 4,
                            "KRW-BTC|take_profit_2": 2,
                        },
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
                "--min-live-core-exits",
                "1",
                "--min-backtest-core-exits",
                "1",
                "--tp1-unobservable-policy",
                "pass",
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            report = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual("pass", report.get("verdict"))
            self.assertEqual(
                "collect_tp1_observable_live_exit_logs_for_stronger_confidence",
                report.get("next_step_hint"),
            )
            allowed = report.get("allowed_findings", [])
            self.assertTrue(
                any(
                    item.get("type") == "tp1_unobservable_pass_override"
                    for item in allowed
                    if isinstance(item, dict)
                )
            )

    def test_take_profit_full_is_grouped_with_take_profit_2_family(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "validate_should_exit_parity.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            audit_json = root / "audit.json"
            output_json = root / "report.json"

            _write_json(
                audit_json,
                {
                    "dataset_results": [
                        {
                            "dataset": "upbit_KRW_BTC_1m_full.csv",
                            "exit_reason_counts": {
                                "StopLoss": 4,
                                "TakeProfit2": 2,
                            },
                        }
                    ],
                    "live_exit_reason_snapshot": {
                        "partial_tp1_observable": True,
                        "exit_market_reason_counts": {
                            "KRW-BTC|stop_loss": 4,
                            "KRW-BTC|take_profit_full_due_to_min_order": 2,
                        },
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
                "--min-live-core-exits",
                "1",
                "--min-backtest-core-exits",
                "1",
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            report = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual("pass", report.get("verdict"))
            live_core = report.get("live", {}).get("reason_counts_core", {})
            self.assertEqual(2, int(live_core.get("TAKE_PROFIT_FINAL", 0)))
            critical = report.get("critical_findings", [])
            self.assertFalse(
                any(
                    item.get("type") == "critical_reason_missing"
                    and item.get("reason") == "TAKE_PROFIT_FINAL"
                    for item in critical
                    if isinstance(item, dict)
                )
            )

    def test_tp1_can_be_allowed_when_collapsed_to_min_order_full_exit(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "validate_should_exit_parity.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            audit_json = root / "audit.json"
            output_json = root / "report.json"

            _write_json(
                audit_json,
                {
                    "dataset_results": [
                        {
                            "dataset": "upbit_KRW_BTC_1m_full.csv",
                            "exit_reason_counts": {
                                "StopLoss": 4,
                                "TakeProfit1": 2,
                                "TakeProfit2": 2,
                            },
                        }
                    ],
                    "live_exit_reason_snapshot": {
                        "partial_tp1_observable": False,
                        "exit_market_reason_counts": {
                            "KRW-BTC|stop_loss": 4,
                            "KRW-BTC|take_profit_full_due_to_min_order": 2,
                        },
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
                "--min-live-core-exits",
                "1",
                "--min-backtest-core-exits",
                "1",
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            report = json.loads(output_json.read_text(encoding="utf-8"))
            self.assertEqual("pass", report.get("verdict"))
            self.assertTrue(bool(report.get("comparison", {}).get("tp1_collapsed_to_min_order_full_exit", False)))
            allowed = report.get("allowed_findings", [])
            self.assertTrue(
                any(
                    item.get("type") == "allowed_tp1_collapsed_to_full_due_to_min_order"
                    for item in allowed
                    if isinstance(item, dict)
                )
            )


if __name__ == "__main__":
    unittest.main()
