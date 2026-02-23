#!/usr/bin/env python3
import unittest

from run_verification import (
    build_loss_tail_decomposition,
    build_risk_adjusted_failure_decomposition,
    compute_risk_adjusted_score_components,
)


class VerificationRiskTailDecompositionTest(unittest.TestCase):
    def test_compute_risk_adjusted_score_components(self):
        out = compute_risk_adjusted_score_components(
            expectancy_krw=-20.0,
            profit_factor=0.5,
            max_drawdown_pct=6.0,
            downtrend_loss_per_trade_krw=16.0,
            downtrend_trade_share=0.75,
            total_trades=5,
        )
        self.assertAlmostEqual(-2.0, float(out.get("expectancy_term", 0.0)))
        self.assertAlmostEqual(-1.0, float(out.get("profit_factor_term", 0.0)))
        self.assertAlmostEqual(1.0, float(out.get("drawdown_penalty", 0.0)))
        self.assertAlmostEqual(2.0, float(out.get("downtrend_loss_penalty", 0.0)))
        self.assertAlmostEqual(1.0, float(out.get("downtrend_share_penalty", 0.0)))
        self.assertAlmostEqual(0.5, float(out.get("low_trade_penalty", 0.0)))
        self.assertAlmostEqual(-7.5, float(out.get("score", 0.0)))

    def test_build_loss_tail_decomposition(self):
        pattern_rows = [
            {
                "regime": "RANGING",
                "volatility_bucket": "vol_low",
                "liquidity_bucket": "liq_mid",
                "strength_bucket": "strength_low",
                "entry_archetype": "A1",
                "total_trades": 10,
                "total_profit": -100.0,
            },
            {
                "regime": "RANGING",
                "volatility_bucket": "vol_low",
                "liquidity_bucket": "liq_low",
                "strength_bucket": "strength_low",
                "entry_archetype": "A2",
                "total_trades": 5,
                "total_profit": -50.0,
            },
            {
                "regime": "TRENDING_UP",
                "volatility_bucket": "vol_low",
                "liquidity_bucket": "liq_high",
                "strength_bucket": "strength_low",
                "entry_archetype": "A1",
                "total_trades": 3,
                "total_profit": 30.0,
            },
        ]
        out = build_loss_tail_decomposition(pattern_rows=pattern_rows, dataset_name="d1.csv", top_cell_limit=5)
        self.assertAlmostEqual(150.0, float(out.get("negative_profit_abs_krw", 0.0)))
        self.assertEqual(2, int(out.get("negative_cell_count", 0)))
        top_regimes = list(out.get("top_loss_regimes", []))
        self.assertTrue(top_regimes)
        self.assertEqual("RANGING", str(top_regimes[0].get("regime", "")))
        self.assertAlmostEqual(1.0, float(top_regimes[0].get("loss_share", 0.0)))
        top_cells = list(out.get("top_loss_cells", []))
        self.assertEqual(2, len(top_cells))
        self.assertEqual("d1.csv", str(top_cells[0].get("dataset", "")))

    def test_build_risk_adjusted_failure_decomposition(self):
        aggregates = {
            "avg_risk_adjusted_score": -2.0,
            "avg_risk_adjusted_score_components": {
                "expectancy_term": -1.0,
                "profit_factor_term": -0.5,
                "drawdown_penalty": 1.0,
                "downtrend_loss_penalty": 0.7,
                "downtrend_share_penalty": 0.0,
                "low_trade_penalty": 0.1,
            },
            "loss_tail_aggregate": {
                "negative_profit_abs_krw": 320.0,
                "avg_top3_loss_concentration": 0.82,
                "top_loss_regimes": [{"regime": "RANGING", "loss_abs_krw": 210.0, "loss_share": 0.6563}],
                "top_loss_archetypes": [{"entry_archetype": "A1", "loss_abs_krw": 180.0, "loss_share": 0.5625}],
                "top_loss_cells": [{"pattern": "p1", "loss_abs_krw": 120.0}],
            },
        }
        verdict = {
            "checks": {
                "risk_adjusted_score_guard_pass": False,
            }
        }
        out = build_risk_adjusted_failure_decomposition(
            adaptive_aggregates=aggregates,
            adaptive_verdict=verdict,
            min_risk_adjusted_score=-0.1,
        )
        self.assertTrue(bool(out.get("active", False)))
        self.assertFalse(bool(out.get("risk_adjusted_score_guard_pass", True)))
        self.assertGreater(float(out.get("score_gap_to_threshold", 0.0)), 0.0)
        self.assertTrue(list(out.get("dominant_penalties", [])))
        self.assertTrue(list(out.get("recommended_focus", [])))


if __name__ == "__main__":
    unittest.main()
