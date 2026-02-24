#!/usr/bin/env python3
import unittest

from run_daily_oos_stability import (
    build_target_day_cell_breakdown,
    compute_gate_checks,
    extract_target_day_metrics_from_trade_history,
    select_recent_days,
    utc_day_from_timestamp_ms,
)


class DailyOosStabilityTest(unittest.TestCase):
    def test_utc_day_from_timestamp_ms(self):
        self.assertEqual(utc_day_from_timestamp_ms("1700000000000"), "2023-11-14")
        self.assertEqual(utc_day_from_timestamp_ms("1700003600000"), "2023-11-14")
        self.assertIsNone(utc_day_from_timestamp_ms("not-a-number"))

    def test_select_recent_days_filters_by_min_rows(self):
        grouped = {
            "2026-02-20": [["a"]] * 700,
            "2026-02-21": [["a"]] * 721,
            "2026-02-22": [["a"]] * 900,
            "2026-02-23": [["a"]] * 800,
        }
        selected = select_recent_days(grouped, days=2, min_rows_per_day=720)
        self.assertEqual(selected, ["2026-02-22", "2026-02-23"])

    def test_compute_gate_checks_pass(self):
        checks = compute_gate_checks(
            evaluated_day_count=12,
            nonpositive_day_ratio=0.33,
            total_profit_sum=1500.0,
            peak_day_dd_pct=8.0,
            gate_min_evaluated_days=10,
            gate_max_nonpositive_ratio=0.45,
            gate_max_day_dd_pct=12.0,
            gate_require_positive_profit_sum=True,
        )
        self.assertTrue(all(bool(x.get("pass", False)) for x in checks.values()))

    def test_compute_gate_checks_fail(self):
        checks = compute_gate_checks(
            evaluated_day_count=4,
            nonpositive_day_ratio=0.8,
            total_profit_sum=-100.0,
            peak_day_dd_pct=25.0,
            gate_min_evaluated_days=10,
            gate_max_nonpositive_ratio=0.45,
            gate_max_day_dd_pct=12.0,
            gate_require_positive_profit_sum=True,
        )
        self.assertFalse(checks["min_evaluated_days"]["pass"])
        self.assertFalse(checks["max_nonpositive_day_ratio"]["pass"])
        self.assertFalse(checks["max_day_drawdown_pct"]["pass"])
        self.assertFalse(checks["positive_profit_sum"]["pass"])

    def test_trade_history_metrics_includes_backtest_eod_by_default(self):
        payload = {
            "trade_history_samples": [
                {
                    "exit_time": 1771200000000,  # 2026-02-16 UTC
                    "exit_reason": "BacktestEOD",
                    "profit_loss_krw": -100.0,
                },
                {
                    "exit_time": 1771200300000,  # 2026-02-16 UTC
                    "exit_reason": "TakeProfit1",
                    "profit_loss_krw": 40.0,
                },
            ]
        }
        metrics = extract_target_day_metrics_from_trade_history(payload, "2026-02-16")
        self.assertIsNotNone(metrics)
        assert metrics is not None
        self.assertEqual(metrics["total_trades"], 2)
        self.assertEqual(metrics["winning_trades"], 1)
        self.assertEqual(metrics["total_profit"], -60.0)

    def test_trade_history_metrics_can_exclude_backtest_eod(self):
        payload = {
            "trade_history_samples": [
                {
                    "exit_time": 1771200000000,  # 2026-02-16 UTC
                    "exit_reason": "BacktestEOD",
                    "profit_loss_krw": -100.0,
                },
                {
                    "exit_time": 1771200300000,  # 2026-02-16 UTC
                    "exit_reason": "TakeProfit1",
                    "profit_loss_krw": 40.0,
                },
            ]
        }
        metrics = extract_target_day_metrics_from_trade_history(
            payload,
            "2026-02-16",
            include_backtest_eod_trades=False,
        )
        self.assertIsNotNone(metrics)
        assert metrics is not None
        self.assertEqual(metrics["total_trades"], 1)
        self.assertEqual(metrics["winning_trades"], 1)
        self.assertEqual(metrics["total_profit"], 40.0)

    def test_trade_history_metrics_returns_zero_when_only_excluded(self):
        payload = {
            "trade_history_samples": [
                {
                    "exit_time": 1771200000000,  # 2026-02-16 UTC
                    "exit_reason": "BacktestEOD",
                    "profit_loss_krw": -100.0,
                }
            ]
        }
        metrics = extract_target_day_metrics_from_trade_history(
            payload,
            "2026-02-16",
            include_backtest_eod_trades=False,
        )
        self.assertIsNotNone(metrics)
        assert metrics is not None
        self.assertEqual(metrics["total_trades"], 0)
        self.assertEqual(metrics["total_profit"], 0.0)

    def test_cell_breakdown_includes_backtest_eod_by_default(self):
        payload = {
            "trade_history_samples": [
                {
                    "exit_time": 1771200000000,
                    "exit_reason": "BacktestEOD",
                    "profit_loss_krw": -100.0,
                    "regime": "TRENDING_UP",
                    "entry_archetype": "CORE_RESCUE_SHOULD_ENTER",
                },
                {
                    "exit_time": 1771200300000,
                    "exit_reason": "TakeProfit1",
                    "profit_loss_krw": 40.0,
                    "regime": "TRENDING_UP",
                    "entry_archetype": "CORE_RESCUE_SHOULD_ENTER",
                },
            ]
        }
        cells = build_target_day_cell_breakdown(payload, "2026-02-16")
        self.assertEqual(len(cells), 1)
        self.assertEqual(cells[0]["trade_count"], 2)
        self.assertEqual(cells[0]["profit_sum"], -60.0)

    def test_cell_breakdown_can_exclude_backtest_eod(self):
        payload = {
            "trade_history_samples": [
                {
                    "exit_time": 1771200000000,
                    "exit_reason": "BacktestEOD",
                    "profit_loss_krw": -100.0,
                    "regime": "TRENDING_UP",
                    "entry_archetype": "CORE_RESCUE_SHOULD_ENTER",
                },
                {
                    "exit_time": 1771200300000,
                    "exit_reason": "TakeProfit1",
                    "profit_loss_krw": 40.0,
                    "regime": "TRENDING_UP",
                    "entry_archetype": "CORE_RESCUE_SHOULD_ENTER",
                },
            ]
        }
        cells = build_target_day_cell_breakdown(
            payload,
            "2026-02-16",
            include_backtest_eod_trades=False,
        )
        self.assertEqual(len(cells), 1)
        self.assertEqual(cells[0]["trade_count"], 1)
        self.assertEqual(cells[0]["profit_sum"], 40.0)


if __name__ == "__main__":
    unittest.main()
