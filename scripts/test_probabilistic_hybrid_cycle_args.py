#!/usr/bin/env python3
import unittest

import run_probabilistic_hybrid_cycle as hybrid_cycle


class ProbabilisticHybridCycleArgsTest(unittest.TestCase):
    def test_generate_shadow_requires_promotion_eval(self):
        with self.assertRaises(ValueError) as cm:
            hybrid_cycle.main([
                "--generate-shadow-report",
            ])
        self.assertIn("--generate-shadow-report/--validate-shadow-report require --evaluate-promotion-readiness", str(cm.exception))

    def test_validate_shadow_requires_promotion_eval(self):
        with self.assertRaises(ValueError) as cm:
            hybrid_cycle.main([
                "--validate-shadow-report",
            ])
        self.assertIn("--generate-shadow-report/--validate-shadow-report require --evaluate-promotion-readiness", str(cm.exception))

    def test_live_enable_requires_shadow_or_generation(self):
        with self.assertRaises(ValueError) as cm:
            hybrid_cycle.main([
                "--run-verification",
                "--evaluate-promotion-readiness",
                "--promotion-target-stage",
                "live_enable",
            ])
        self.assertIn("--promotion-shadow-report-json is required when --promotion-target-stage live_enable", str(cm.exception))


if __name__ == "__main__":
    unittest.main()
