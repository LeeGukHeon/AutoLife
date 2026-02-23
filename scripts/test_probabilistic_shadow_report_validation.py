#!/usr/bin/env python3
import argparse
import json
import tempfile
import unittest
from pathlib import Path

from validate_probabilistic_shadow_report import evaluate


def write_json(path: Path, payload):
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


class ProbabilisticShadowReportValidationTest(unittest.TestCase):
    def test_v2_shadow_validation_pass(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            shadow = root / "shadow_v2.json"
            out = root / "out.json"
            write_json(
                shadow,
                {
                    "status": "pass",
                    "pipeline_version": "v2",
                    "gate_profile": "v2_strict",
                    "checks": {
                        "decision_log_comparison_pass": True,
                        "same_bundle": True,
                        "same_candles": True,
                    },
                    "metadata": {
                        "compared_decision_count": 120,
                        "mismatch_count": 0,
                    },
                },
            )
            result = evaluate(
                argparse.Namespace(
                    shadow_report_json=str(shadow),
                    pipeline_version="v2",
                    output_json=str(out),
                    strict=True,
                )
            )
            self.assertEqual(result.get("status"), "pass")
            self.assertTrue(bool(result.get("shadow_report_ok", False)))

    def test_v2_shadow_validation_fail_when_evidence_missing(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            shadow = root / "shadow_v2_missing_evidence.json"
            out = root / "out_fail.json"
            write_json(
                shadow,
                {
                    "status": "pass",
                    "pipeline_version": "v2",
                    "gate_profile": "v2_strict",
                    "checks": {
                        "same_bundle": True,
                        "same_candles": True,
                    },
                    "metadata": {
                        "compared_decision_count": 120,
                        "mismatch_count": 0,
                    },
                },
            )
            result = evaluate(
                argparse.Namespace(
                    shadow_report_json=str(shadow),
                    pipeline_version="v2",
                    output_json=str(out),
                    strict=True,
                )
            )
            self.assertEqual(result.get("status"), "fail")
            self.assertIn(
                "shadow_report_missing_or_failed_decision_parity_evidence",
                list(result.get("errors", [])),
            )

    def test_v2_shadow_validation_fail_on_pipeline_mismatch(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            shadow = root / "shadow_v1.json"
            out = root / "out_fail_pipeline.json"
            write_json(
                shadow,
                {
                    "status": "pass",
                    "pipeline_version": "v1",
                    "checks": {
                        "decision_log_comparison_pass": True,
                        "same_bundle": True,
                        "same_candles": True,
                    },
                    "metadata": {
                        "compared_decision_count": 120,
                        "mismatch_count": 0,
                    },
                },
            )
            result = evaluate(
                argparse.Namespace(
                    shadow_report_json=str(shadow),
                    pipeline_version="v2",
                    output_json=str(out),
                    strict=True,
                )
            )
            self.assertEqual(result.get("status"), "fail")
            self.assertIn("shadow_report_pipeline_mismatch", list(result.get("errors", [])))


if __name__ == "__main__":
    unittest.main()
