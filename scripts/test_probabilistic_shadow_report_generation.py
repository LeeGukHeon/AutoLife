#!/usr/bin/env python3
import argparse
import json
import tempfile
import unittest
from pathlib import Path

from generate_probabilistic_shadow_report import evaluate


def write_json(path: Path, payload) -> None:
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def write_jsonl(path: Path, rows) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as fp:
        for row in rows:
            fp.write(json.dumps(row, ensure_ascii=False) + "\n")


class ProbabilisticShadowReportGenerationTest(unittest.TestCase):
    def test_generate_shadow_report_pass_v2(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live_log = root / "live.jsonl"
            backtest_log = root / "backtest.jsonl"
            out = root / "shadow_report.json"
            bundle = root / "bundle_v2.json"

            rows = [
                {
                    "ts": 1700000000000,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": False,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {"market": "KRW-BTC", "strategy": "foundation_adaptive", "selected": True, "reason": "selected"},
                        {"market": "KRW-ETH", "strategy": "foundation_adaptive", "selected": False, "reason": "dropped_capacity"},
                    ],
                },
                {
                    "ts": 1700000060000,
                    "dominant_regime": "TRENDING_UP",
                    "small_seed_mode": False,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {"market": "KRW-BTC", "strategy": "foundation_adaptive", "selected": True, "reason": "selected"},
                    ],
                },
            ]
            write_jsonl(live_log, rows)
            write_jsonl(backtest_log, rows)
            write_json(
                bundle,
                {
                    "version": "probabilistic_runtime_bundle_v2_draft",
                    "pipeline_version": "v2",
                    "feature_contract_version": "v2_draft",
                    "runtime_bundle_contract_version": "v2_draft",
                },
            )

            result = evaluate(
                argparse.Namespace(
                    live_decision_log_jsonl=str(live_log),
                    backtest_decision_log_jsonl=str(backtest_log),
                    runtime_bundle_json=str(bundle),
                    live_runtime_bundle_json="",
                    backtest_runtime_bundle_json="",
                    pipeline_version="v2",
                    output_json=str(out),
                    strict=True,
                )
            )
            self.assertEqual("pass", result.get("status"))
            checks = result.get("checks", {})
            self.assertTrue(bool(checks.get("decision_log_comparison_pass", False)))
            self.assertTrue(bool(checks.get("same_bundle", False)))
            self.assertTrue(bool(checks.get("same_candles", False)))
            self.assertEqual(0, int(result.get("metadata", {}).get("mismatch_count", -1)))

    def test_generate_shadow_report_fail_on_decision_mismatch(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live_log = root / "live.jsonl"
            backtest_log = root / "backtest.jsonl"
            out = root / "shadow_report_fail.json"
            bundle = root / "bundle_v1.json"

            write_jsonl(
                live_log,
                [
                    {
                        "ts": 1700000000000,
                        "dominant_regime": "RANGING",
                        "small_seed_mode": False,
                        "max_new_orders_per_scan": 1,
                        "decisions": [
                            {"market": "KRW-BTC", "strategy": "foundation_adaptive", "selected": True, "reason": "selected"},
                        ],
                    }
                ],
            )
            write_jsonl(
                backtest_log,
                [
                    {
                        "ts": 1700000000000,
                        "dominant_regime": "RANGING",
                        "small_seed_mode": False,
                        "max_new_orders_per_scan": 1,
                        "decisions": [
                            {"market": "KRW-BTC", "strategy": "foundation_adaptive", "selected": False, "reason": "dropped_capacity"},
                        ],
                    }
                ],
            )
            write_json(
                bundle,
                {
                    "version": "probabilistic_runtime_bundle_v1",
                    "pipeline_version": "v1",
                },
            )

            result = evaluate(
                argparse.Namespace(
                    live_decision_log_jsonl=str(live_log),
                    backtest_decision_log_jsonl=str(backtest_log),
                    runtime_bundle_json=str(bundle),
                    live_runtime_bundle_json="",
                    backtest_runtime_bundle_json="",
                    pipeline_version="v1",
                    output_json=str(out),
                    strict=True,
                )
            )
            self.assertEqual("fail", result.get("status"))
            self.assertGreater(int(result.get("metadata", {}).get("mismatch_count", 0)), 0)
            self.assertIn("shadow_decision_log_mismatch", list(result.get("errors", [])))

    def test_generate_shadow_report_fail_without_bundle_evidence(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live_log = root / "live.jsonl"
            backtest_log = root / "backtest.jsonl"
            out = root / "shadow_report_fail_bundle.json"

            rows = [
                {
                    "ts": 1700000000000,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": False,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {"market": "KRW-BTC", "strategy": "foundation_adaptive", "selected": True, "reason": "selected"},
                    ],
                }
            ]
            write_jsonl(live_log, rows)
            write_jsonl(backtest_log, rows)

            result = evaluate(
                argparse.Namespace(
                    live_decision_log_jsonl=str(live_log),
                    backtest_decision_log_jsonl=str(backtest_log),
                    runtime_bundle_json="",
                    live_runtime_bundle_json="",
                    backtest_runtime_bundle_json="",
                    pipeline_version="v1",
                    output_json=str(out),
                    strict=True,
                )
            )
            self.assertEqual("fail", result.get("status"))
            self.assertFalse(bool(result.get("checks", {}).get("same_bundle", True)))
            self.assertIn("shadow_runtime_bundle_missing_or_mismatch", list(result.get("errors", [])))


if __name__ == "__main__":
    unittest.main()
