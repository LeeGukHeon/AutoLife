#!/usr/bin/env python3
import unittest

from probabilistic_cost_model import estimate_conditional_cost_bps, normalize_cost_model_config


class ConditionalCostModelTest(unittest.TestCase):
    def setUp(self) -> None:
        self.cfg = normalize_cost_model_config(
            {
                "enabled": True,
                "fee_floor_bps": 6.0,
                "volatility_weight": 3.0,
                "range_weight": 1.5,
                "liquidity_weight": 2.5,
                "volatility_norm_bps": 50.0,
                "range_norm_bps": 80.0,
                "liquidity_ref_ratio": 1.0,
                "liquidity_penalty_cap": 8.0,
                "cost_cap_bps": 200.0,
            }
        )

    def test_volatility_monotonic(self):
        low_vol = estimate_conditional_cost_bps(
            atr_pct_14=0.002,
            bb_width_20=0.008,
            vol_ratio_20=1.0,
            notional_ratio_20=1.0,
            config=self.cfg,
        )
        high_vol = estimate_conditional_cost_bps(
            atr_pct_14=0.012,
            bb_width_20=0.008,
            vol_ratio_20=1.0,
            notional_ratio_20=1.0,
            config=self.cfg,
        )
        self.assertGreater(high_vol, low_vol)

    def test_liquidity_monotonic(self):
        high_liq = estimate_conditional_cost_bps(
            atr_pct_14=0.006,
            bb_width_20=0.010,
            vol_ratio_20=2.0,
            notional_ratio_20=2.0,
            config=self.cfg,
        )
        low_liq = estimate_conditional_cost_bps(
            atr_pct_14=0.006,
            bb_width_20=0.010,
            vol_ratio_20=0.4,
            notional_ratio_20=0.4,
            config=self.cfg,
        )
        self.assertGreater(low_liq, high_liq)


if __name__ == "__main__":
    unittest.main()
