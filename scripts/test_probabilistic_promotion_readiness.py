#!/usr/bin/env python3
import argparse
import json
import tempfile
import unittest
from pathlib import Path

from evaluate_probabilistic_promotion_readiness import evaluate


def write_json(path: Path, payload):
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


class ProbabilisticPromotionReadinessTest(unittest.TestCase):
    def test_v1_prelive_pass(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "feature_manifest.json"
            feature_validation = root / "feature_validation.json"
            parity = root / "parity.json"
            verification = root / "verification.json"
            runtime_cfg = root / "config.json"
            out = root / "out.json"

            write_json(manifest, {"version": "prob_features_v1"})
            write_json(
                feature_validation,
                {
                    "status": "pass",
                    "dataset_manifest_json": str(manifest),
                    "pipeline_version": "v1",
                    "gate_profile": "v1",
                    "preflight_errors": [],
                },
            )
            write_json(parity, {"status": "pass", "pipeline_version": "v1", "gate_profile": "v1"})
            write_json(
                verification,
                {
                    "overall_gate_pass": True,
                    "pipeline_version": "v1",
                    "gate_profile": {"name": "v1_legacy"},
                },
            )
            write_json(runtime_cfg, {"trading": {"allow_live_orders": False}})

            result = evaluate(
                argparse.Namespace(
                    feature_validation_json=str(feature_validation),
                    parity_json=str(parity),
                    verification_json=str(verification),
                    shadow_report_json="",
                    shadow_validation_json="",
                    runtime_config_json=str(runtime_cfg),
                    target_stage="prelive",
                    pipeline_version="auto",
                    output_json=str(out),
                )
            )
            self.assertEqual(result.get("status"), "pass")
            self.assertTrue(bool(result.get("promotion_ready", False)))

    def test_v2_live_enable_requires_shadow(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "feature_manifest_v2.json"
            feature_validation = root / "feature_validation_v2.json"
            parity = root / "parity_v2.json"
            verification = root / "verification_v2.json"
            runtime_cfg = root / "config_v2.json"
            out = root / "out_v2.json"

            write_json(manifest, {"version": "prob_features_v2_draft", "pipeline_version": "v2"})
            write_json(
                feature_validation,
                {
                    "status": "pass",
                    "dataset_manifest_json": str(manifest),
                    "pipeline_version": "v2",
                    "gate_profile": "v2_strict",
                    "preflight_errors": [],
                },
            )
            write_json(
                parity,
                {
                    "status": "pass",
                    "pipeline_version": "v2",
                    "gate_profile": "v2_strict",
                },
            )
            write_json(
                verification,
                {
                    "overall_gate_pass": True,
                    "pipeline_version": "v2",
                    "gate_profile": {"name": "v2_strict"},
                },
            )
            write_json(runtime_cfg, {"trading": {"allow_live_orders": False}})

            result = evaluate(
                argparse.Namespace(
                    feature_validation_json=str(feature_validation),
                    parity_json=str(parity),
                    verification_json=str(verification),
                    shadow_report_json="",
                    shadow_validation_json="",
                    runtime_config_json=str(runtime_cfg),
                    target_stage="live_enable",
                    pipeline_version="v2",
                    output_json=str(out),
                )
            )
            self.assertEqual(result.get("status"), "fail")
            self.assertIn("gate4_shadow_failed_or_missing", list(result.get("errors", [])))

    def test_v2_live_enable_pass(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "feature_manifest_v2.json"
            feature_validation = root / "feature_validation_v2.json"
            parity = root / "parity_v2.json"
            verification = root / "verification_v2.json"
            shadow = root / "shadow_v2.json"
            runtime_cfg = root / "config_v2.json"
            out = root / "out_v2.json"

            write_json(manifest, {"version": "prob_features_v2_draft", "pipeline_version": "v2"})
            write_json(
                feature_validation,
                {
                    "status": "pass",
                    "dataset_manifest_json": str(manifest),
                    "pipeline_version": "v2",
                    "gate_profile": "v2_strict",
                    "preflight_errors": [],
                },
            )
            write_json(parity, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict"})
            write_json(
                verification,
                {
                    "overall_gate_pass": True,
                    "pipeline_version": "v2",
                    "gate_profile": {"name": "v2_strict"},
                },
            )
            write_json(shadow, {"status": "pass", "pipeline_version": "v2"})
            write_json(runtime_cfg, {"trading": {"allow_live_orders": False}})

            result = evaluate(
                argparse.Namespace(
                    feature_validation_json=str(feature_validation),
                    parity_json=str(parity),
                    verification_json=str(verification),
                    shadow_report_json=str(shadow),
                    shadow_validation_json="",
                    runtime_config_json=str(runtime_cfg),
                    target_stage="live_enable",
                    pipeline_version="v2",
                    output_json=str(out),
                )
            )
            self.assertEqual(result.get("status"), "pass")
            self.assertTrue(bool(result.get("promotion_ready", False)))

    def test_v2_live_enable_shadow_pipeline_mismatch_fail(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "feature_manifest_v2.json"
            feature_validation = root / "feature_validation_v2.json"
            parity = root / "parity_v2.json"
            verification = root / "verification_v2.json"
            shadow = root / "shadow_v1.json"
            runtime_cfg = root / "config_v2.json"
            out = root / "out_v2_fail_shadow_pipeline.json"

            write_json(manifest, {"version": "prob_features_v2_draft", "pipeline_version": "v2"})
            write_json(
                feature_validation,
                {
                    "status": "pass",
                    "dataset_manifest_json": str(manifest),
                    "pipeline_version": "v2",
                    "gate_profile": "v2_strict",
                    "preflight_errors": [],
                },
            )
            write_json(parity, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict"})
            write_json(
                verification,
                {
                    "overall_gate_pass": True,
                    "pipeline_version": "v2",
                    "gate_profile": {"name": "v2_strict"},
                },
            )
            write_json(shadow, {"status": "pass", "pipeline_version": "v1"})
            write_json(runtime_cfg, {"trading": {"allow_live_orders": False}})

            result = evaluate(
                argparse.Namespace(
                    feature_validation_json=str(feature_validation),
                    parity_json=str(parity),
                    verification_json=str(verification),
                    shadow_report_json=str(shadow),
                    shadow_validation_json="",
                    runtime_config_json=str(runtime_cfg),
                    target_stage="live_enable",
                    pipeline_version="v2",
                    output_json=str(out),
                )
            )
            self.assertEqual(result.get("status"), "fail")
            self.assertIn("gate4_shadow_pipeline_mismatch", list(result.get("errors", [])))

    def test_v2_live_enable_shadow_validation_fail(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "feature_manifest_v2.json"
            feature_validation = root / "feature_validation_v2.json"
            parity = root / "parity_v2.json"
            verification = root / "verification_v2.json"
            shadow = root / "shadow_v2.json"
            shadow_validation = root / "shadow_validation_fail.json"
            runtime_cfg = root / "config_v2.json"
            out = root / "out_v2_fail_shadow_validation.json"

            write_json(manifest, {"version": "prob_features_v2_draft", "pipeline_version": "v2"})
            write_json(
                feature_validation,
                {
                    "status": "pass",
                    "dataset_manifest_json": str(manifest),
                    "pipeline_version": "v2",
                    "gate_profile": "v2_strict",
                    "preflight_errors": [],
                },
            )
            write_json(parity, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict"})
            write_json(
                verification,
                {
                    "overall_gate_pass": True,
                    "pipeline_version": "v2",
                    "gate_profile": {"name": "v2_strict"},
                },
            )
            write_json(shadow, {"status": "pass", "pipeline_version": "v2"})
            write_json(shadow_validation, {"status": "fail", "pipeline_version": "v2"})
            write_json(runtime_cfg, {"trading": {"allow_live_orders": False}})

            result = evaluate(
                argparse.Namespace(
                    feature_validation_json=str(feature_validation),
                    parity_json=str(parity),
                    verification_json=str(verification),
                    shadow_report_json=str(shadow),
                    shadow_validation_json=str(shadow_validation),
                    runtime_config_json=str(runtime_cfg),
                    target_stage="live_enable",
                    pipeline_version="v2",
                    output_json=str(out),
                )
            )
            self.assertEqual(result.get("status"), "fail")
            self.assertIn("gate4_shadow_validation_failed_or_missing", list(result.get("errors", [])))


if __name__ == "__main__":
    unittest.main()
