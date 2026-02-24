#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from build_probabilistic_shadow_backtest_log import build_aligned_scans, run_backtest_for_dataset


class ProbabilisticShadowBacktestLogBuilderTest(unittest.TestCase):
    def test_build_aligned_scans_rebalances_capacity(self):
        live_scans = [
            {
                "ts": 1000,
                "dominant_regime": "RANGING",
                "small_seed_mode": True,
                "max_new_orders_per_scan": 1,
                "decisions": [],
            },
            {
                "ts": 2000,
                "dominant_regime": "TRENDING_UP",
                "small_seed_mode": True,
                "max_new_orders_per_scan": 1,
                "decisions": [],
            },
        ]
        market_backtest_rows = {
            "KRW-BTC": [
                {
                    "ts": 1010,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": True,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {
                            "market": "KRW-BTC",
                            "strategy": "foundation_adaptive",
                            "selected": True,
                            "reason": "selected",
                            "policy_score": 0.9,
                            "base_score": 0.5,
                        }
                    ],
                }
            ],
            "KRW-ETH": [
                {
                    "ts": 1020,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": True,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {
                            "market": "KRW-ETH",
                            "strategy": "foundation_adaptive",
                            "selected": True,
                            "reason": "selected",
                            "policy_score": 0.4,
                            "base_score": 0.2,
                        }
                    ],
                }
            ],
            "KRW-XRP": [
                {
                    "ts": 2050,
                    "dominant_regime": "TRENDING_UP",
                    "small_seed_mode": True,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {
                            "market": "KRW-XRP",
                            "strategy": "foundation_adaptive",
                            "selected": True,
                            "reason": "selected",
                            "policy_score": 0.8,
                            "base_score": 0.3,
                        }
                    ],
                }
            ],
        }

        rows, diag = build_aligned_scans(
            live_scans,
            market_backtest_rows,
            match_tolerance_ms=500,
            max_new_orders_per_scan=1,
            allowed_markets={"KRW-BTC", "KRW-ETH", "KRW-XRP"},
        )

        self.assertEqual(2, len(rows))
        row0 = rows[0]
        self.assertEqual(1000, int(row0["ts"]))
        self.assertEqual(2, len(row0["decisions"]))
        selected_row0 = [x for x in row0["decisions"] if bool(x.get("selected", False))]
        self.assertEqual(1, len(selected_row0))
        self.assertEqual("KRW-BTC", selected_row0[0].get("market"))
        dropped_row0 = [x for x in row0["decisions"] if not bool(x.get("selected", False))]
        self.assertEqual(1, len(dropped_row0))
        self.assertEqual("dropped_capacity", str(dropped_row0[0].get("reason", "")))

        row1 = rows[1]
        self.assertEqual(2000, int(row1["ts"]))
        self.assertEqual(1, len(row1["decisions"]))
        self.assertTrue(bool(row1["decisions"][0].get("selected", False)))
        self.assertEqual("KRW-XRP", row1["decisions"][0].get("market"))

        self.assertEqual(0, int(diag.get("unmatched_source_rows", -1)))
        self.assertEqual(2, int(diag.get("selected_count", 0)))

    def test_build_aligned_scans_unmatched_rows_create_empty_scans(self):
        live_scans = [
            {
                "ts": 1000,
                "dominant_regime": "RANGING",
                "small_seed_mode": False,
                "max_new_orders_per_scan": 1,
                "decisions": [],
            }
        ]
        market_backtest_rows = {
            "KRW-BTC": [
                {
                    "ts": 5000,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": False,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {
                            "market": "KRW-BTC",
                            "strategy": "foundation_adaptive",
                            "selected": True,
                            "reason": "selected",
                            "policy_score": 0.9,
                        }
                    ],
                }
            ]
        }

        rows, diag = build_aligned_scans(
            live_scans,
            market_backtest_rows,
            match_tolerance_ms=100,
            max_new_orders_per_scan=1,
            allowed_markets={"KRW-BTC"},
        )

        self.assertEqual(1, len(rows))
        self.assertEqual(1000, int(rows[0]["ts"]))
        self.assertEqual(0, len(rows[0]["decisions"]))
        self.assertEqual(1, int(diag.get("unmatched_source_rows", 0)))
        self.assertEqual(1, int(diag.get("empty_scan_count", 0)))

    def test_build_aligned_scans_capacity_score_order_asc(self):
        live_scans = [
            {
                "ts": 1000,
                "dominant_regime": "RANGING",
                "small_seed_mode": True,
                "max_new_orders_per_scan": 1,
                "decisions": [
                    {"market": "KRW-BTC"},
                    {"market": "KRW-ETH"},
                ],
            }
        ]
        market_backtest_rows = {
            "KRW-BTC": [
                {
                    "ts": 1000,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": True,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {
                            "market": "KRW-BTC",
                            "strategy": "foundation_adaptive",
                            "selected": True,
                            "reason": "selected",
                            "policy_score": 0.9,
                            "base_score": 0.5,
                        }
                    ],
                }
            ],
            "KRW-ETH": [
                {
                    "ts": 1000,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": True,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {
                            "market": "KRW-ETH",
                            "strategy": "foundation_adaptive",
                            "selected": True,
                            "reason": "selected",
                            "policy_score": 0.1,
                            "base_score": 0.2,
                        }
                    ],
                }
            ],
        }

        rows, _ = build_aligned_scans(
            live_scans,
            market_backtest_rows,
            match_tolerance_ms=0,
            max_new_orders_per_scan=1,
            capacity_score_order="asc",
            allowed_markets={"KRW-BTC", "KRW-ETH"},
        )

        self.assertEqual(1, len(rows))
        self.assertEqual(2, len(rows[0]["decisions"]))
        selected_row = [x for x in rows[0]["decisions"] if bool(x.get("selected", False))]
        self.assertEqual(1, len(selected_row))
        self.assertEqual("KRW-ETH", selected_row[0].get("market"))

    def test_build_aligned_scans_uses_market_aware_timestamp_alignment(self):
        live_scans = [
            {
                "ts": 1000,
                "dominant_regime": "RANGING",
                "small_seed_mode": True,
                "max_new_orders_per_scan": 1,
                "decisions": [{"market": "KRW-BTC"}],
            },
            {
                "ts": 2000,
                "dominant_regime": "RANGING",
                "small_seed_mode": True,
                "max_new_orders_per_scan": 1,
                "decisions": [{"market": "KRW-SOL"}],
            },
        ]
        market_backtest_rows = {
            "KRW-SOL": [
                {
                    "ts": 1100,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": True,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {
                            "market": "KRW-SOL",
                            "strategy": "foundation_adaptive",
                            "selected": True,
                            "reason": "selected",
                            "policy_score": 0.5,
                            "base_score": 0.3,
                        }
                    ],
                }
            ],
        }

        rows, diag = build_aligned_scans(
            live_scans,
            market_backtest_rows,
            match_tolerance_ms=1000,
            max_new_orders_per_scan=1,
            allowed_markets={"KRW-SOL"},
        )

        self.assertEqual(2, len(rows))
        self.assertEqual(0, len(rows[0]["decisions"]))
        self.assertEqual(1, len(rows[1]["decisions"]))
        self.assertEqual("KRW-SOL", rows[1]["decisions"][0].get("market"))
        self.assertEqual(0, int(diag.get("unmatched_source_rows", -1)))

    def test_build_aligned_scans_prefers_live_selected_hint_over_score_order(self):
        live_scans = [
            {
                "ts": 1000,
                "dominant_regime": "RANGING",
                "small_seed_mode": True,
                "max_new_orders_per_scan": 1,
                "decisions": [
                    {
                        "market": "KRW-ETH",
                        "strategy": "Foundation Adaptive Strategy",
                        "selected": True,
                    },
                    {
                        "market": "KRW-BTC",
                        "strategy": "Foundation Adaptive Strategy",
                        "selected": False,
                    },
                ],
            }
        ]
        market_backtest_rows = {
            "KRW-BTC": [
                {
                    "ts": 1000,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": True,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {
                            "market": "KRW-BTC",
                            "strategy": "Foundation Adaptive Strategy",
                            "selected": True,
                            "reason": "selected",
                            "policy_score": 0.9,
                            "base_score": 0.5,
                        }
                    ],
                }
            ],
            "KRW-ETH": [
                {
                    "ts": 1000,
                    "dominant_regime": "RANGING",
                    "small_seed_mode": True,
                    "max_new_orders_per_scan": 1,
                    "decisions": [
                        {
                            "market": "KRW-ETH",
                            "strategy": "Foundation Adaptive Strategy",
                            "selected": True,
                            "reason": "selected",
                            "policy_score": 0.1,
                            "base_score": 0.2,
                        }
                    ],
                }
            ],
        }

        rows, _ = build_aligned_scans(
            live_scans,
            market_backtest_rows,
            match_tolerance_ms=0,
            max_new_orders_per_scan=1,
            capacity_score_order="desc",
            allowed_markets={"KRW-BTC", "KRW-ETH"},
        )

        self.assertEqual(1, len(rows))
        selected = [x for x in rows[0]["decisions"] if bool(x.get("selected", False))]
        self.assertEqual(1, len(selected))
        self.assertEqual("KRW-ETH", selected[0].get("market"))

    def test_run_backtest_for_dataset_uses_shadow_policy_only_flag(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            exe_path = root / "AutoLifeTrading.exe"
            dataset_path = root / "upbit_KRW_BTC_1m_live.csv"
            backtest_log_path = root / "policy_decisions_backtest.jsonl"

            exe_path.write_text("", encoding="utf-8")
            dataset_path.write_text("timestamp,open,high,low,close,volume\n", encoding="utf-8")
            backtest_log_path.write_text(
                '{"ts":1234,"decisions":[{"market":"KRW-BTC","strategy":"Foundation Adaptive Strategy","selected":true,"reason":"selected"}]}\n',
                encoding="utf-8",
            )

            with patch("build_probabilistic_shadow_backtest_log.run_command") as mock_run_command:
                mock_run_command.return_value = SimpleNamespace(exit_code=0, stdout="", stderr="")
                out = run_backtest_for_dataset(exe_path, dataset_path, backtest_log_path)

            self.assertIn("--shadow-policy-only", out["cmd"])
            self.assertEqual(0, int(out["returncode"]))
            self.assertEqual(1, len(out["rows"]))


if __name__ == "__main__":
    unittest.main()
