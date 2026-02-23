#!/usr/bin/env python3
import argparse
import json
import tempfile
import unittest
from pathlib import Path

from run_codex_context_refresh_checks import evaluate


class CodexContextRefreshChecksTest(unittest.TestCase):
    def test_dry_run_feature_only_passes_with_explicit_inputs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = root / "feature_manifest.json"
            contract = root / "contract.json"
            output = root / "out.json"
            manifest.write_text("{}", encoding="utf-8")
            contract.write_text("{}", encoding="utf-8")

            out = evaluate(
                argparse.Namespace(
                    python_exe="python",
                    touched_areas="feature",
                    pipeline_version="v1",
                    skip_missing=False,
                    dry_run=True,
                    output_json=str(output),
                    feature_dataset_manifest_json=str(manifest),
                    feature_contract_json=str(contract),
                    feature_output_json=str(root / "feature_validate.json"),
                    runtime_bundle_json="",
                    train_summary_json="",
                    split_manifest_json="",
                    parity_output_json=str(root / "parity.json"),
                    verification_exe_path=str(root / "AutoLifeTrading.exe"),
                    verification_config_path=str(root / "config.json"),
                    verification_source_config_path=str(root / "source_config.json"),
                    verification_data_dir=str(root),
                    verification_datasets="",
                    verification_baseline_report_path=str(root / "baseline.json"),
                    verification_output_json=str(root / "verification.json"),
                )
            )
            self.assertEqual("pass", out.get("status"))
            self.assertEqual(["feature"], out.get("touched_areas"))
            steps = list(out.get("steps", []))
            self.assertEqual(1, len(steps))
            self.assertEqual("validate_features", steps[0].get("name"))
            self.assertTrue(bool(steps[0].get("dry_run", False)))

    def test_skip_missing_model_marks_skipped(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "out_skip.json"
            out = evaluate(
                argparse.Namespace(
                    python_exe="python",
                    touched_areas="model",
                    pipeline_version="v1",
                    skip_missing=True,
                    dry_run=True,
                    output_json=str(output),
                    feature_dataset_manifest_json="",
                    feature_contract_json="",
                    feature_output_json=str(root / "feature_validate.json"),
                    runtime_bundle_json=str(root / "missing_bundle.json"),
                    train_summary_json=str(root / "missing_train_summary.json"),
                    split_manifest_json=str(root / "missing_split_manifest.json"),
                    parity_output_json=str(root / "parity.json"),
                    verification_exe_path=str(root / "AutoLifeTrading.exe"),
                    verification_config_path=str(root / "config.json"),
                    verification_source_config_path=str(root / "source_config.json"),
                    verification_data_dir=str(root),
                    verification_datasets="",
                    verification_baseline_report_path=str(root / "baseline.json"),
                    verification_output_json=str(root / "verification.json"),
                )
            )
            self.assertEqual("pass", out.get("status"))
            steps = list(out.get("steps", []))
            self.assertEqual(1, len(steps))
            self.assertEqual("validate_bundle_parity", steps[0].get("name"))
            self.assertTrue(bool(steps[0].get("skipped", False)))

    def test_missing_runtime_datasets_fails_without_skip(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "out_fail.json"
            exe = root / "AutoLifeTrading.exe"
            cfg = root / "config.json"
            src_cfg = root / "source_config.json"
            data_dir = root / "data"
            data_dir.mkdir(parents=True, exist_ok=True)
            exe.write_text("", encoding="utf-8")
            cfg.write_text("{}", encoding="utf-8")
            src_cfg.write_text("{}", encoding="utf-8")

            out = evaluate(
                argparse.Namespace(
                    python_exe="python",
                    touched_areas="runtime",
                    pipeline_version="v1",
                    skip_missing=False,
                    dry_run=True,
                    output_json=str(output),
                    feature_dataset_manifest_json="",
                    feature_contract_json="",
                    feature_output_json=str(root / "feature_validate.json"),
                    runtime_bundle_json="",
                    train_summary_json="",
                    split_manifest_json="",
                    parity_output_json=str(root / "parity.json"),
                    verification_exe_path=str(exe),
                    verification_config_path=str(cfg),
                    verification_source_config_path=str(src_cfg),
                    verification_data_dir=str(data_dir),
                    verification_datasets="",
                    verification_baseline_report_path=str(root / "baseline.json"),
                    verification_output_json=str(root / "verification.json"),
                )
            )
            self.assertEqual("fail", out.get("status"))
            self.assertIn("missing_required:verification_datasets", list(out.get("errors", [])))

            persisted = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual("fail", persisted.get("status"))


if __name__ == "__main__":
    unittest.main()
