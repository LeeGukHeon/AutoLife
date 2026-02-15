# Stage 15 Gate Funnel Analysis (2026-02-14)

## Purpose
- Analyze full path: `strategy filter -> signal generation -> buy execution` with code-backed counters.
- Identify whether micro-tuning is still valid, or if structural rewrite is required.

## Instrumentation Added
- `BacktestEngine` now exports:
  - `entry_funnel`:
    - `entry_rounds`, `no_signal_generated`, `filtered_out_by_manager`, `filtered_out_by_policy`, `no_best_signal`
    - `blocked_pattern_gate`, `blocked_rr_rebalance`, `blocked_risk_gate`, `blocked_risk_manager`
    - `blocked_min_order_or_capital`, `blocked_order_sizing`, `entries_executed`
  - `strategy_signal_funnel`:
    - `generated_signals`, `selected_best`, `blocked_by_risk_manager`, `entries_executed` by strategy
- JSON path: `AutoLifeTrading --backtest <csv> --json`

## Structural Issue Found and Fixed
- Found mismatch in backtest entry ordering:
  - `RiskManager::canEnterPosition` was called with raw `position_size` before minimum-order normalization.
  - This caused repeated risk rejects (`buy blocked: <5000 KRW`) even when one-lot entry was possible.
- Fix:
  - Normalize minimum order size before `canEnterPosition` in backtest flow.
  - Use normalized effective size consistently for risk check and sizing.

## Evidence (Same Dataset, Before/After)
- Dataset: `data/backtest_real/upbit_KRW_ADA_1m_12000.csv`

### Before fix
- `entry_funnel`: `entry_rounds=11816`, `no_signal_generated=11680`, `blocked_risk_manager=127`, `entries_executed=4`
- Result: `total_trades=4`, `expectancy_krw=-46.04`, `profit_factor=0.000`

### After fix
- `entry_funnel`: `entry_rounds=11759`, `no_signal_generated=11752`, `blocked_risk_manager=0`, `entries_executed=7`
- Result: `total_trades=10`, `expectancy_krw=-3.18`, `profit_factor=0.814`

## 8-Dataset Spot Check (Post-fix)
- `blocked_risk_manager` dropped to `0` on all checked datasets.
- Trades increased into `8~14` range.
- Remaining bottleneck is still upstream:
  - `no_signal_generated` remains very high (`~11,7xx / ~11,8xx rounds`), i.e. signal generation is sparse.

## Interpretation
- Core bottleneck has shifted:
  1. Risk-order mismatch issue resolved (execution-stage false rejects removed).
  2. Primary bottleneck now is strategy-stage hard gating / low signal density.
- This supports moving from micro-threshold tuning to strategy-layer redesign.

## Recommended Next Step (Rewrite Direction)
1. Replace strategy hard early-return chains with weighted scoring + single final gate per strategy.
2. Consolidate duplicated gates across `StrategyManager` and engine entry path.
3. Keep one adaptive calibration layer (not multiple overlapping RR/EV gates).
4. Re-run realdata matrix and compare:
   - signal density
   - profitable ratio
   - expectancy
   - candidate gate pass.
