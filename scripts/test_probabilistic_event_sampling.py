#!/usr/bin/env python3
import unittest

from build_probabilistic_feature_dataset import normalize_sample_mode, update_sampling_state


def run_sequence(mode: str, threshold: float, lookback: int, notionals, rets):
    state = {}
    decisions = []
    metrics = []
    for notional, ret in zip(notionals, rets):
        keep, metric = update_sampling_state(
            mode=mode,
            threshold=threshold,
            lookback_minutes=lookback,
            notional_value=float(notional),
            ret_1m_value=float(ret),
            state=state,
        )
        decisions.append(bool(keep))
        metrics.append(float(metric))
    return decisions, metrics


class ProbabilisticEventSamplingTest(unittest.TestCase):
    def test_time_mode_keeps_all_rows(self):
        decisions, _ = run_sequence(
            mode="time",
            threshold=0.0,
            lookback=5,
            notionals=[1, 2, 3, 4],
            rets=[0.1, -0.2, 0.3, -0.4],
        )
        self.assertEqual(decisions, [True, True, True, True])

    def test_dollar_mode_deterministic(self):
        params = dict(
            mode="dollar",
            threshold=50.0,
            lookback=2,
            notionals=[10, 20, 30, 40],
            rets=[0, 0, 0, 0],
        )
        d1, _ = run_sequence(**params)
        d2, _ = run_sequence(**params)
        self.assertEqual(d1, d2)
        self.assertEqual(d1, [False, False, True, True])

    def test_volatility_threshold_controls_density(self):
        notionals = [1, 1, 1, 1, 1, 1]
        rets = [0.0, 0.001, -0.001, 0.001, -0.001, 0.001]
        low_threshold_decisions, _ = run_sequence(
            mode="volatility",
            threshold=0.0005,
            lookback=3,
            notionals=notionals,
            rets=rets,
        )
        high_threshold_decisions, _ = run_sequence(
            mode="volatility",
            threshold=0.01,
            lookback=3,
            notionals=notionals,
            rets=rets,
        )
        self.assertGreater(sum(1 for x in low_threshold_decisions if x), sum(1 for x in high_threshold_decisions if x))

    def test_sample_mode_normalization(self):
        self.assertEqual(normalize_sample_mode("time"), "time")
        self.assertEqual(normalize_sample_mode("DOLLAR"), "dollar")
        self.assertEqual(normalize_sample_mode("VOLATILITY"), "volatility")
        self.assertEqual(normalize_sample_mode("unknown"), "time")


if __name__ == "__main__":
    unittest.main()
