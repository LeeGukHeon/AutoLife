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


class AnalyzeDailyOosDeltaTest(unittest.TestCase):
    def test_gate_aligned_summary_present(self):
        repo_root = Path(__file__).resolve().parent.parent
        script_path = repo_root / "scripts" / "analyze_daily_oos_delta.py"
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            baseline_json = root / "baseline.json"
            candidate_json = root / "candidate.json"
            output_json = root / "delta.json"

            baseline_payload = {
                "aggregates": {
                    "evaluated_day_count": 2,
                    "nonpositive_day_count": 1,
                    "total_profit_sum": 50.0,
                },
                "daily_results": [
                    {
                        "dataset": "upbit_KRW_BTC_1m_12000.csv",
                        "day_utc": "2026-02-20",
                        "total_profit": 100.0,
                        "total_trades": 2,
                        "evaluated": True,
                        "nonpositive_profit": False,
                        "profitable": True,
                        "cell_breakdown": [],
                    },
                    {
                        "dataset": "upbit_KRW_BTC_1m_12000.csv",
                        "day_utc": "2026-02-21",
                        "total_profit": -50.0,
                        "total_trades": 1,
                        "evaluated": True,
                        "nonpositive_profit": True,
                        "profitable": False,
                        "cell_breakdown": [],
                    },
                    {
                        "dataset": "upbit_KRW_BTC_1m_12000.csv",
                        "day_utc": "2026-02-22",
                        "total_profit": 0.0,
                        "total_trades": 0,
                        "evaluated": False,
                        "nonpositive_profit": True,
                        "profitable": False,
                        "cell_breakdown": [],
                    },
                ],
            }
            candidate_payload = {
                "aggregates": {
                    "evaluated_day_count": 2,
                    "nonpositive_day_count": 0,
                    "total_profit_sum": 80.0,
                },
                "daily_results": [
                    {
                        "dataset": "upbit_KRW_BTC_1m_12000.csv",
                        "day_utc": "2026-02-20",
                        "total_profit": 110.0,
                        "total_trades": 2,
                        "evaluated": True,
                        "nonpositive_profit": False,
                        "profitable": True,
                        "cell_breakdown": [],
                    },
                    {
                        "dataset": "upbit_KRW_BTC_1m_12000.csv",
                        "day_utc": "2026-02-21",
                        "total_profit": 20.0,
                        "total_trades": 1,
                        "evaluated": True,
                        "nonpositive_profit": False,
                        "profitable": True,
                        "cell_breakdown": [],
                    },
                    {
                        "dataset": "upbit_KRW_BTC_1m_12000.csv",
                        "day_utc": "2026-02-22",
                        "total_profit": -40.0,
                        "total_trades": 0,
                        "evaluated": False,
                        "nonpositive_profit": True,
                        "profitable": False,
                        "cell_breakdown": [],
                    },
                ],
            }
            _write_json(baseline_json, baseline_payload)
            _write_json(candidate_json, candidate_payload)

            cmd = [
                sys.executable,
                str(script_path),
                "--baseline-json",
                str(baseline_json),
                "--candidate-json",
                str(candidate_json),
                "--output-json",
                str(output_json),
            ]
            completed = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(0, completed.returncode, msg=completed.stderr)

            result = json.loads(output_json.read_text(encoding="utf-8"))
            summary = result.get("summary", {})
            self.assertEqual("all_day_rows_union", summary.get("summary_basis"))
            self.assertAlmostEqual(40.0, float(summary.get("profit_sum_delta", 0.0)), places=6)
            self.assertEqual(-1, int(summary.get("nonpositive_day_count_delta", 0)))

            gate_aligned = summary.get("gate_aligned", {})
            self.assertEqual(2, int(gate_aligned.get("baseline_evaluated_day_count", 0)))
            self.assertEqual(2, int(gate_aligned.get("candidate_evaluated_day_count", 0)))
            self.assertAlmostEqual(30.0, float(gate_aligned.get("profit_sum_delta", 0.0)), places=6)
            self.assertEqual(-1, int(gate_aligned.get("nonpositive_day_count_delta", 0)))

            evaluated_row_alignment = summary.get("evaluated_row_alignment", {})
            self.assertEqual(2, int(evaluated_row_alignment.get("union_day_count", 0)))
            self.assertEqual(2, int(evaluated_row_alignment.get("intersection_day_count", 0)))


if __name__ == "__main__":
    unittest.main()
