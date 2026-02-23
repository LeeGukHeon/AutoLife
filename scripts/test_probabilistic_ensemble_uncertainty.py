#!/usr/bin/env python3
import unittest

from probabilistic_ensemble_utils import compute_prob_mean_std, uncertainty_size_multiplier


class ProbabilisticEnsembleUncertaintyTest(unittest.TestCase):
    def test_std_increases_with_disagreement(self):
        _, std_low = compute_prob_mean_std([0.51, 0.50, 0.49])
        _, std_high = compute_prob_mean_std([0.80, 0.50, 0.20])
        self.assertGreater(std_high, std_low)

    def test_linear_scale_monotonic(self):
        s_low = uncertainty_size_multiplier(0.01, mode="linear", u_max=0.06, min_scale=0.10)
        s_high = uncertainty_size_multiplier(0.05, mode="linear", u_max=0.06, min_scale=0.10)
        self.assertGreater(s_low, s_high)

    def test_exp_scale_monotonic(self):
        s_low = uncertainty_size_multiplier(0.01, mode="exp", exp_k=8.0, min_scale=0.10)
        s_high = uncertainty_size_multiplier(0.06, mode="exp", exp_k=8.0, min_scale=0.10)
        self.assertGreater(s_low, s_high)

    def test_scale_clamped(self):
        self.assertAlmostEqual(
            uncertainty_size_multiplier(10.0, mode="linear", u_max=0.06, min_scale=0.15),
            0.15,
        )


if __name__ == "__main__":
    unittest.main()
