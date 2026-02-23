#!/usr/bin/env python3
import json
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from build_probabilistic_feature_dataset import main as build_features_main
from fetch_probabilistic_training_bundle import build_job_list


class ProbabilisticUniverseScopeTest(unittest.TestCase):
    def test_fetch_job_list_scopes_1m_to_universe(self):
        start_utc = datetime(2024, 1, 1, tzinfo=timezone.utc)
        end_utc = datetime(2024, 1, 2, tzinfo=timezone.utc)
        markets = ["KRW-BTC", "KRW-ETH"]
        jobs = build_job_list(
            markets=markets,
            timeframe_units=[1, 5],
            start_utc=start_utc,
            end_utc=end_utc,
            output_dir=Path("."),
            universe_1m_markets=["KRW-BTC"],
        )
        keys = {(str(x["market"]), int(x["unit_min"])) for x in jobs}
        self.assertIn(("KRW-BTC", 1), keys)
        self.assertNotIn(("KRW-ETH", 1), keys)
        self.assertIn(("KRW-BTC", 5), keys)
        self.assertIn(("KRW-ETH", 5), keys)

    def test_build_features_missing_anchor_non_universe_is_warning_skip(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            input_dir = root / "input"
            output_dir = root / "output"
            summary_json = root / "summary.json"
            manifest_json = root / "manifest.json"
            input_dir.mkdir(parents=True, exist_ok=True)
            output_dir.mkdir(parents=True, exist_ok=True)

            rc = build_features_main(
                [
                    "--input-dir",
                    str(input_dir),
                    "--output-dir",
                    str(output_dir),
                    "--summary-json",
                    str(summary_json),
                    "--manifest-json",
                    str(manifest_json),
                    "--markets",
                    "KRW-AAA",
                ]
            )
            self.assertEqual(0, int(rc))
            summary = json.loads(summary_json.read_text(encoding="utf-8"))
            self.assertEqual(0, int(summary.get("failed_count", -1)))
            self.assertEqual(1, int(summary.get("warning_count", 0)))
            self.assertEqual(1, int(summary.get("skipped_missing_anchor_non_universe_count", 0)))

    def test_build_features_missing_anchor_universe_is_strict_fail(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            input_dir = root / "input"
            output_dir = root / "output"
            summary_json = root / "summary.json"
            manifest_json = root / "manifest.json"
            universe_json = root / "universe.json"
            input_dir.mkdir(parents=True, exist_ok=True)
            output_dir.mkdir(parents=True, exist_ok=True)
            universe_json.write_text(
                json.dumps(
                    {
                        "generated_at": "2026-02-23T00:00:00Z",
                        "final_1m_markets": ["KRW-AAA"],
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )

            rc = build_features_main(
                [
                    "--input-dir",
                    str(input_dir),
                    "--output-dir",
                    str(output_dir),
                    "--summary-json",
                    str(summary_json),
                    "--manifest-json",
                    str(manifest_json),
                    "--markets",
                    "KRW-AAA",
                    "--universe-file",
                    str(universe_json),
                ]
            )
            self.assertEqual(2, int(rc))
            summary = json.loads(summary_json.read_text(encoding="utf-8"))
            self.assertGreater(int(summary.get("failed_count", 0)), 0)
            missing = list(summary.get("missing_required_universe_anchors", []))
            self.assertIn("KRW-AAA", missing)


if __name__ == "__main__":
    unittest.main()
