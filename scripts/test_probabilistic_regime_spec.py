#!/usr/bin/env python3
import unittest

from probabilistic_regime_spec import (
    analyze_regime_state,
    regime_blocks_entries,
    regime_size_multiplier,
    regime_threshold_add,
)


def make_series(length: int, start: float = 100.0, drift: float = 0.0, wiggle: float = 0.0):
    out = []
    price = float(start)
    for i in range(length):
        phase = (i % 6) - 3
        step = drift + (wiggle * phase / 3.0)
        price = max(1.0, price * (1.0 + step))
        out.append(price)
    return out


class ProbabilisticRegimeSpecTest(unittest.TestCase):
    def setUp(self):
        self.cfg = {
            "enabled": True,
            "volatility_window": 36,
            "drawdown_window": 24,
            "volatile_zscore": 1.0,
            "hostile_zscore": 1.8,
            "volatile_drawdown_speed_bps": 2.0,
            "hostile_drawdown_speed_bps": 6.0,
            "hostile_block_new_entries": True,
            "volatile_threshold_add": 0.01,
            "hostile_threshold_add": 0.03,
            "volatile_size_multiplier": 0.5,
            "hostile_size_multiplier": 0.2,
        }

    def test_normal_state_on_stable_series(self):
        closes = make_series(80, drift=0.0002, wiggle=0.0003)
        out = analyze_regime_state(closes, self.cfg)
        self.assertEqual(out["state"], "normal")

    def test_hostile_state_on_sharp_drawdown(self):
        closes = make_series(80, drift=0.0001, wiggle=0.0002)
        closes[-1] = closes[-2] * 0.88
        out = analyze_regime_state(closes, self.cfg)
        self.assertEqual(out["state"], "hostile")
        self.assertTrue(regime_blocks_entries(self.cfg, out["state"]))

    def test_volatile_state_on_spike_without_large_drawdown(self):
        closes = make_series(80, drift=0.0001, wiggle=0.0001)
        closes[-2] = closes[-3] * 1.04
        closes[-1] = closes[-2] * 0.99
        out = analyze_regime_state(closes, self.cfg)
        self.assertEqual(out["state"], "volatile")
        self.assertAlmostEqual(regime_threshold_add(self.cfg, out["state"]), 0.01)
        self.assertAlmostEqual(regime_size_multiplier(self.cfg, out["state"]), 0.5)

    def test_deterministic_repeated_eval(self):
        closes = make_series(80, drift=0.00015, wiggle=0.0004)
        first = analyze_regime_state(closes, self.cfg)
        second = analyze_regime_state(closes, self.cfg)
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
