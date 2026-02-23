#!/usr/bin/env python3
import unittest

from run_verification import (
    build_dataset_diagnostics,
    build_loss_tail_decomposition,
    build_top_loss_trade_samples,
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

    def test_build_top_loss_trade_samples_filters_and_sorts_losses(self):
        trade_history_samples = [
            {
                "market": "KRW-BTC",
                "strategy_name": "s1",
                "entry_archetype": "A1",
                "regime": "RANGING",
                "profit_loss_krw": -100.0,
                "profit_loss_pct": -0.01,
                "holding_minutes": 5.0,
                "signal_filter": 0.7,
                "signal_strength": 0.6,
                "liquidity_score": 0.8,
                "volatility": 0.2,
                "expected_value": -0.3,
                "reward_risk_ratio": 0.9,
                "probabilistic_h5_calibrated": 0.55,
                "probabilistic_h5_margin": 0.03,
                "exit_reason": "StopLoss",
            },
            {
                "market": "KRW-ETH",
                "strategy_name": "s2",
                "entry_archetype": "A2",
                "regime": "RANGING",
                "profit_loss_krw": -350.0,
                "profit_loss_pct": -0.025,
                "holding_minutes": 10.0,
                "signal_filter": 0.5,
                "signal_strength": 0.4,
                "liquidity_score": 0.6,
                "volatility": 0.3,
                "expected_value": -0.5,
                "reward_risk_ratio": 0.7,
                "probabilistic_h5_calibrated": 0.51,
                "probabilistic_h5_margin": 0.01,
                "exit_reason": "TimeStop",
            },
            {
                "market": "KRW-SOL",
                "strategy_name": "s3",
                "entry_archetype": "A3",
                "regime": "TRENDING_UP",
                "profit_loss_krw": 40.0,
            },
        ]
        out = build_top_loss_trade_samples(trade_history_samples, limit=2)
        self.assertEqual(2, len(out))
        self.assertEqual("KRW-ETH", str(out[0].get("market", "")))
        self.assertLess(float(out[0].get("profit_loss_krw", 0.0)), float(out[1].get("profit_loss_krw", 0.0)))
        self.assertNotEqual("KRW-SOL", str(out[0].get("market", "")))

    def test_build_dataset_diagnostics_includes_top_loss_trade_samples(self):
        backtest_result = {
            "entry_funnel": {},
            "candidate_generation": {},
            "sizing_diagnostics": {},
            "strategy_summaries": [],
            "pattern_summaries": [],
            "strategy_signal_funnel": [],
            "strategy_collection_summaries": [],
            "risk_telemetry": {},
            "trade_history_samples": [
                {
                    "market": "KRW-SOL",
                    "strategy_name": "s1",
                    "entry_archetype": "A1",
                    "regime": "RANGING",
                    "profit_loss_krw": -77.0,
                    "profit_loss_pct": -0.01,
                },
                {
                    "market": "KRW-BTC",
                    "strategy_name": "s2",
                    "entry_archetype": "A2",
                    "regime": "TRENDING_UP",
                    "profit_loss_krw": 11.0,
                    "profit_loss_pct": 0.002,
                },
            ],
        }
        out = build_dataset_diagnostics("d.csv", backtest_result)
        samples = list(out.get("top_loss_trade_samples", []))
        self.assertEqual(1, len(samples))
        self.assertEqual("KRW-SOL", str(samples[0].get("market", "")))
        self.assertAlmostEqual(-77.0, float(samples[0].get("profit_loss_krw", 0.0)))


if __name__ == "__main__":
    unittest.main()
