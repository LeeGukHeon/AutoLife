# Pre-DryRun Backtest Validation Report

Date: 2026-02-13

## Scope
- Objective: validate profitability and structural readiness before dry-run/live progression.
- Executable: `build/Release/AutoLifeTrading.exe`
- Command mode: `--backtest <csv> --json`
- Dataset sets (meaningful historical synthetic regimes):
  - `data/backtest/adversarial_50` (50)
  - `data/backtest/engine_compare_36` (36)
  - `data/backtest/regime_mix_80` (80)

## Fresh Results (current build)

### 1) Full-set backtest matrix
- `adversarial_50`
  - datasets: 50
  - evaluated (`trades >= 20`): 39
  - profitable: 0
  - profitable ratio: 0.00
  - total profit (evaluated sum): `-41,067 KRW`
  - avg MDD: `14.02%`
- `engine_compare_36`
  - datasets: 36
  - evaluated: 32
  - profitable: 0
  - profitable ratio: 0.00
  - total profit (evaluated sum): `-80,137 KRW`
  - avg MDD: `13.28%`
- `regime_mix_80`
  - datasets: 80
  - evaluated: 70
  - profitable: 0
  - profitable ratio: 0.00
  - total profit (evaluated sum): `-152,142 KRW`
  - avg MDD: `13.33%`

### 2) Recursive readiness gate
- Report: `build/Release/logs/readiness_all_recursive.json`
- Summary:
  - dataset_total: 177
  - dataset_evaluated (`trades >= 20`): 144
  - profitable_datasets: 0
  - strict_pass_datasets: 0
  - profitable_ratio: 0.00
  - `is_ready_for_live_by_backtest = false`

### 3) Walk-forward OOS checks (representative regimes)
- Uptrend: `build/Release/logs/wf_uptrend_report.json`
  - oos_profitable_ratio: 0.00
  - oos_total_profit: `-23,442`
  - ready: false
- Range chop: `build/Release/logs/wf_range_chop_report.json`
  - oos_profitable_ratio: 0.00
  - oos_total_profit: `-36,259`
  - ready: false
- Whipsaw: `build/Release/logs/wf_whipsaw_report.json`
  - oos_profitable_ratio: 0.00
  - oos_total_profit: `-42,557`
  - ready: false

## Structural Findings
1. Expected value is negative across all major regimes.
- No evaluated dataset (`trades >= 20`) is profitable in fresh runs.

2. High win-rate does not translate to PnL.
- Uptrend/trend-pullback cases often show very high win-rate (near 100%) but still negative total PnL.
- Interpretation: gross edge per trade is too small vs fees/slippage and occasional larger losses.

3. Whipsaw/fake-breakout sensitivity is severe.
- Largest losses cluster in whipsaw, fake-breakout, and spike/gap subsets.

4. Drawdown ceiling remains high while return is negative.
- Typical MDD ~12% to 16% under evaluated sets, with no compensating positive return.

## Readiness Verdict
- Profitability validation before dry-run: **FAILED**
- Proceeding to production-like dry-run for "profit expectation" verification is not justified yet.

## Recommended Next Work (before dry-run expansion)
1. Reduce trade frequency and fee drag first.
- Increase minimum expected edge filter per entry.
- Add explicit `expected_edge_after_fee_slippage > 0` gate.

2. Add regime-aware kill-switch.
- Disable/scale down entries in whipsaw/fake-breakout/low-liquidity regimes.

3. Recalibrate exits for R-multiple improvement.
- Raise average winner size relative to loser size.
- Verify partial-take-profit + trailing logic for net EV (after fees).

4. Re-run same validation gates unchanged.
- Keep same dataset sets and thresholds; only promote if recursive readiness and walk-forward both pass.
