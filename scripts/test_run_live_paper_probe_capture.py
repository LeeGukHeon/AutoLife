#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_live_paper_probe_capture as probe


class RunLivePaperProbeCaptureTests(unittest.TestCase):
    def test_parse_trading_overrides_json_and_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            overrides_file = root / "overrides.json"
            overrides_file.write_text(
                json.dumps({"trading": {"min_order_krw": 1000.0, "max_new_orders_per_scan": 2}}, ensure_ascii=False),
                encoding="utf-8",
            )

            parsed = probe.parse_trading_overrides(
                trading_overrides_json='{"min_order_krw": 2000.0, "probabilistic_runtime_scan_prefilter_enabled": false}',
                trading_overrides_file=str(overrides_file),
            )

            self.assertEqual(2000.0, float(parsed.get("min_order_krw", 0.0)))
            self.assertEqual(2, int(parsed.get("max_new_orders_per_scan", 0)))
            self.assertFalse(bool(parsed.get("probabilistic_runtime_scan_prefilter_enabled", True)))

    def test_build_probe_config_payload_keeps_base_immutable(self):
        base_payload = {
            "trading": {
                "allow_live_orders": False,
                "min_order_krw": 5000.0,
            }
        }
        overrides = {"min_order_krw": 1000.0}

        merged = probe.build_probe_config_payload(base_payload, overrides)
        self.assertEqual(1000.0, float(merged["trading"]["min_order_krw"]))
        self.assertEqual(5000.0, float(base_payload["trading"]["min_order_krw"]))

    def test_ensure_safe_config_payload_blocks_live_orders_true(self):
        payload = {"trading": {"allow_live_orders": True}}
        safe_ok, safe_reason = probe.ensure_safe_config_payload(payload, allow_live_orders_override=False)
        self.assertFalse(safe_ok)
        self.assertEqual("safety_blocked_allow_live_orders_true", safe_reason)

        safe_ok_with_override, _ = probe.ensure_safe_config_payload(payload, allow_live_orders_override=True)
        self.assertTrue(safe_ok_with_override)

    def test_managed_probe_config_restores_original_on_exception(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            config_path = root / "config.json"
            original_payload = {
                "trading": {
                    "allow_live_orders": False,
                    "min_order_krw": 5000.0,
                }
            }
            config_path.write_text(
                json.dumps(original_payload, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
                newline="\n",
            )

            with self.assertRaisesRegex(RuntimeError, "forced_probe_failure"):
                with probe.managed_probe_config(
                    config_path,
                    {"min_order_krw": 1000.0},
                    allow_live_orders_override=False,
                ):
                    runtime_payload = json.loads(config_path.read_text(encoding="utf-8"))
                    self.assertEqual(1000.0, float(runtime_payload["trading"]["min_order_krw"]))
                    raise RuntimeError("forced_probe_failure")

            restored = json.loads(config_path.read_text(encoding="utf-8"))
            self.assertEqual(5000.0, float(restored["trading"]["min_order_krw"]))
            self.assertFalse(bool(restored["trading"]["allow_live_orders"]))

    def test_managed_probe_config_rejects_unsafe_override(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            config_path = root / "config.json"
            config_path.write_text(
                json.dumps({"trading": {"allow_live_orders": False}}, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
                newline="\n",
            )

            with self.assertRaisesRegex(RuntimeError, "safety_check_failed:safety_blocked_allow_live_orders_true"):
                with probe.managed_probe_config(
                    config_path,
                    {"allow_live_orders": True},
                    allow_live_orders_override=False,
                ):
                    pass

    def test_merge_exit_snapshots_accumulates_counts(self):
        left = {
            "total": 1,
            "reason_counts": {"stop_loss": 1},
            "market_counts": {"KRW-BTC": 1},
            "market_reason_counts": {"KRW-BTC|stop_loss": 1},
        }
        right = {
            "total": 2,
            "reason_counts": {"stop_loss": 1, "take_profit_1": 1},
            "market_counts": {"KRW-BTC": 1, "KRW-ETH": 1},
            "market_reason_counts": {"KRW-BTC|stop_loss": 1, "KRW-ETH|take_profit_1": 1},
        }

        merged = probe.merge_exit_snapshots(left, right)
        self.assertEqual(3, int(merged.get("total", 0)))
        self.assertEqual(2, int((merged.get("reason_counts", {}) or {}).get("stop_loss", 0)))
        self.assertEqual(1, int((merged.get("reason_counts", {}) or {}).get("take_profit_1", 0)))
        self.assertEqual(2, int((merged.get("market_counts", {}) or {}).get("KRW-BTC", 0)))
        self.assertEqual(1, int((merged.get("market_counts", {}) or {}).get("KRW-ETH", 0)))


if __name__ == "__main__":
    unittest.main()
