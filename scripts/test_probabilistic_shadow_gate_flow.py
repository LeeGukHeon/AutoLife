#!/usr/bin/env python3
import argparse
import json
import os
import tempfile
import unittest
from pathlib import Path

from run_probabilistic_shadow_gate_flow import (
    DEFAULT_BACKTEST_DECISION_LOG_JSONL,
    DEFAULT_FEATURE_VALIDATION_JSON,
    DEFAULT_LIVE_DECISION_LOG_JSONL,
    DEFAULT_PARITY_JSON,
    DEFAULT_RUNTIME_CONFIG_JSON,
    DEFAULT_VERIFICATION_JSON,
    evaluate,
)


def write_json(path: Path, payload) -> None:
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def write_jsonl(path: Path, rows) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as fp:
        for row in rows:
            fp.write(json.dumps(row, ensure_ascii=False) + "\n")


class ProbabilisticShadowGateFlowTest(unittest.TestCase):
    def test_gate_flow_pass_v2_live_enable(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live_log = root / "policy_decisions.jsonl"
            backtest_log = root / "policy_decisions_backtest.jsonl"
            runtime_bundle = root / "runtime_bundle_v2.json"
            feature_validation = root / "feature_validation.json"
            parity = root / "parity.json"
            verification = root / "verification.json"
            runtime_config = root / "config.json"
            shadow_report = root / "shadow_report.json"
            shadow_validation = root / "shadow_validation.json"
            promotion = root / "promotion.json"
            output = root / "flow.json"

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
            # Keep mtimes deterministic: live should remain discoverable even if
            # backtest-tagged log is newer in the same directory.
            os.utime(backtest_log, (2000000000, 2000000000))
            os.utime(live_log, (2000000001, 2000000001))
            write_json(
                runtime_bundle,
                {
                    "version": "probabilistic_runtime_bundle_v2_draft",
                    "pipeline_version": "v2",
                    "feature_contract_version": "v2_draft",
                    "runtime_bundle_contract_version": "v2_draft",
                },
            )
            write_json(feature_validation, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict", "preflight_errors": []})
            write_json(parity, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict"})
            write_json(
                verification,
                {
                    "overall_gate_pass": True,
                    "pipeline_version": "v2",
                    "gate_profile": {"name": "v2_strict"},
                },
            )
            write_json(runtime_config, {"trading": {"allow_live_orders": False}})

            out = evaluate(
                argparse.Namespace(
                    pipeline_version="v2",
                    target_stage="live_enable",
                    runtime_bundle_json=str(runtime_bundle),
                    live_decision_log_jsonl=str(live_log),
                    backtest_decision_log_jsonl=str(backtest_log),
                    feature_validation_json=str(feature_validation),
                    parity_json=str(parity),
                    verification_json=str(verification),
                    runtime_config_json=str(runtime_config),
                    shadow_report_json=str(shadow_report),
                    shadow_validation_json=str(shadow_validation),
                    promotion_output_json=str(promotion),
                    output_json=str(output),
                )
            )
            self.assertEqual("pass", out.get("status"))
            self.assertEqual(3, len(list(out.get("steps", []))))

    def test_gate_flow_fail_when_allow_live_orders_true(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live_log = root / "policy_decisions.jsonl"
            backtest_log = root / "policy_decisions_backtest.jsonl"
            runtime_bundle = root / "runtime_bundle_v2.json"
            feature_validation = root / "feature_validation.json"
            parity = root / "parity.json"
            verification = root / "verification.json"
            runtime_config = root / "config.json"

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
            write_json(
                runtime_bundle,
                {
                    "version": "probabilistic_runtime_bundle_v2_draft",
                    "pipeline_version": "v2",
                    "feature_contract_version": "v2_draft",
                    "runtime_bundle_contract_version": "v2_draft",
                },
            )
            write_json(feature_validation, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict", "preflight_errors": []})
            write_json(parity, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict"})
            write_json(
                verification,
                {
                    "overall_gate_pass": True,
                    "pipeline_version": "v2",
                    "gate_profile": {"name": "v2_strict"},
                },
            )
            write_json(runtime_config, {"trading": {"allow_live_orders": True}})

            out = evaluate(
                argparse.Namespace(
                    pipeline_version="v2",
                    target_stage="live_enable",
                    runtime_bundle_json=str(runtime_bundle),
                    live_decision_log_jsonl=str(live_log),
                    backtest_decision_log_jsonl=str(backtest_log),
                    feature_validation_json=str(feature_validation),
                    parity_json=str(parity),
                    verification_json=str(verification),
                    runtime_config_json=str(runtime_config),
                    shadow_report_json=str(root / "shadow_report.json"),
                    shadow_validation_json=str(root / "shadow_validation.json"),
                    promotion_output_json=str(root / "promotion.json"),
                    output_json=str(root / "flow.json"),
                )
            )
            self.assertEqual("fail", out.get("status"))
            step_names = [str(x.get("name", "")) for x in list(out.get("steps", []))]
            self.assertIn("evaluate_promotion_readiness", step_names)

    def test_gate_flow_fail_when_decision_logs_mismatch(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live_log = root / "policy_decisions.jsonl"
            backtest_log = root / "policy_decisions_backtest.jsonl"
            runtime_bundle = root / "runtime_bundle_v2.json"
            feature_validation = root / "feature_validation.json"
            parity = root / "parity.json"
            verification = root / "verification.json"
            runtime_config = root / "config.json"

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
                runtime_bundle,
                {
                    "version": "probabilistic_runtime_bundle_v2_draft",
                    "pipeline_version": "v2",
                    "feature_contract_version": "v2_draft",
                    "runtime_bundle_contract_version": "v2_draft",
                },
            )
            write_json(feature_validation, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict", "preflight_errors": []})
            write_json(parity, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict"})
            write_json(
                verification,
                {
                    "overall_gate_pass": True,
                    "pipeline_version": "v2",
                    "gate_profile": {"name": "v2_strict"},
                },
            )
            write_json(runtime_config, {"trading": {"allow_live_orders": False}})

            out = evaluate(
                argparse.Namespace(
                    pipeline_version="v2",
                    target_stage="live_enable",
                    runtime_bundle_json=str(runtime_bundle),
                    live_decision_log_jsonl=str(live_log),
                    backtest_decision_log_jsonl=str(backtest_log),
                    feature_validation_json=str(feature_validation),
                    parity_json=str(parity),
                    verification_json=str(verification),
                    runtime_config_json=str(runtime_config),
                    shadow_report_json=str(root / "shadow_report.json"),
                    shadow_validation_json=str(root / "shadow_validation.json"),
                    promotion_output_json=str(root / "promotion.json"),
                    output_json=str(root / "flow.json"),
                )
            )
            self.assertEqual("fail", out.get("status"))
            self.assertIn("generate_shadow_report_failed", list(out.get("errors", [])))

    def test_gate_flow_auto_resolves_latest_tagged_inputs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            logs = root / "build" / "Release" / "logs"
            cfg_dir = root / "build" / "Release" / "config"
            bundle_dir = root / "config" / "model"
            logs.mkdir(parents=True, exist_ok=True)
            cfg_dir.mkdir(parents=True, exist_ok=True)
            bundle_dir.mkdir(parents=True, exist_ok=True)

            live_log = logs / "policy_decisions_20260223.jsonl"
            backtest_log = logs / "policy_decisions_backtest_20260223.jsonl"
            runtime_bundle = bundle_dir / "probabilistic_runtime_bundle_v2.json"
            feature_validation = logs / "probabilistic_feature_validation_summary_20260223.json"
            parity = logs / "probabilistic_runtime_bundle_parity_20260223.json"
            verification = logs / "verification_report_20260223.json"
            runtime_config = cfg_dir / "config.json"

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
            write_json(
                runtime_bundle,
                {
                    "version": "probabilistic_runtime_bundle_v2_draft",
                    "pipeline_version": "v2",
                    "feature_contract_version": "v2_draft",
                    "runtime_bundle_contract_version": "v2_draft",
                },
            )
            write_json(feature_validation, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict", "preflight_errors": []})
            write_json(parity, {"status": "pass", "pipeline_version": "v2", "gate_profile": "v2_strict"})
            write_json(
                verification,
                {
                    "overall_gate_pass": True,
                    "pipeline_version": "v2",
                    "gate_profile": {"name": "v2_strict"},
                },
            )
            write_json(runtime_config, {"trading": {"allow_live_orders": False}})

            prev_cwd = Path.cwd()
            try:
                os.chdir(root)
                out = evaluate(
                    argparse.Namespace(
                        pipeline_version="auto",
                        target_stage="live_enable",
                        runtime_bundle_json="",
                        live_decision_log_jsonl=DEFAULT_LIVE_DECISION_LOG_JSONL,
                        backtest_decision_log_jsonl=DEFAULT_BACKTEST_DECISION_LOG_JSONL,
                        feature_validation_json=DEFAULT_FEATURE_VALIDATION_JSON,
                        parity_json=DEFAULT_PARITY_JSON,
                        verification_json=DEFAULT_VERIFICATION_JSON,
                        runtime_config_json=DEFAULT_RUNTIME_CONFIG_JSON,
                        shadow_report_json=r".\build\Release\logs\shadow_report.json",
                        shadow_validation_json=r".\build\Release\logs\shadow_validation.json",
                        promotion_output_json=r".\build\Release\logs\promotion.json",
                        output_json=r".\build\Release\logs\flow.json",
                    )
                )
            finally:
                os.chdir(prev_cwd)

            self.assertEqual("pass", out.get("status"))
            resolution = dict(out.get("input_resolution", {}))
            self.assertTrue(bool(resolution.get("live_decision_log_jsonl", {}).get("auto_resolved", False)))
            self.assertTrue(bool(resolution.get("backtest_decision_log_jsonl", {}).get("auto_resolved", False)))
            self.assertTrue(bool(resolution.get("feature_validation_json", {}).get("auto_resolved", False)))
            self.assertTrue(bool(resolution.get("parity_json", {}).get("auto_resolved", False)))
            self.assertTrue(bool(resolution.get("verification_json", {}).get("auto_resolved", False)))


if __name__ == "__main__":
    unittest.main()
