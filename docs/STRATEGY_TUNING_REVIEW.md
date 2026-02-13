# Strategy Limits and Tuning Review (2026-02-13)

## Current Validation Snapshot
- Backtest readiness: `false` (`build/Release/logs/readiness_report.json`)
- Walk-forward readiness: `false` (`build/Release/logs/walk_forward_report.json`)
- OOS profitable windows: `0 / 7`
- OOS total profit: `-8102.2541 KRW`
- OOS max MDD: `13.112%` (gate is `<= 12%`)

## Structural Limits Found in Real Strategy Files
1. Regime consistency is weak across strategies.
- Engine uses a global regime gate (`avoid_high_volatility`, `avoid_trending_down`) but each strategy still keeps many static thresholds and local heuristics.
- References: `src/engine/TradingEngine.cpp:610`, `src/engine/TradingEngine.cpp:716`, `src/strategy/ScalpingStrategy.cpp:212`, `src/strategy/BreakoutStrategy.cpp:133`, `src/strategy/MeanReversionStrategy.cpp:133`.

2. Position sizing is effectively hard-clamped by KRW minimum logic, which can overpower strategy risk intent.
- Multiple strategies force `6000 KRW` minimum via ratio conversion and can exceed intended max-size behavior.
- References: `src/strategy/ScalpingStrategy.cpp:1120`, `src/strategy/MomentumStrategy.cpp:1015`.

3. Strategy thresholds are mostly constants, not market-adaptive parameters.
- Examples: fixed `BASE_TAKE_PROFIT/BASE_STOP_LOSS/MAX_POSITION_SIZE` and strength cutoffs.
- References: `include/strategy/ScalpingStrategy.h:243`, `include/strategy/MomentumStrategy.h:257`, `include/strategy/BreakoutStrategy.h:272`, `include/strategy/MeanReversionStrategy.h:338`, `include/strategy/GridTradingStrategy.h:394`.

4. Resampling model is simplistic (5m chunking), which can distort intrabar structure and entry/exit timing.
- References: `src/strategy/MomentumStrategy.cpp:706`, `src/strategy/BreakoutStrategy.cpp:1415`, `src/strategy/MeanReversionStrategy.cpp:1600`.

5. Momentum walk-forward method in strategy code is placeholder-like (fixed metrics), not production validation logic.
- References: `src/strategy/MomentumStrategy.cpp:1494`, `src/strategy/MomentumStrategy.cpp:1510`.

6. Several sample datasets produce zero trades, so readiness matrix has low effective coverage.
- References: `build/Release/logs/backtest_matrix_summary.csv`, `build/Release/logs/readiness_report.json`.

## What Was Implemented Now
1. Runtime strategy ON/OFF state machine by dominant regime + per-strategy EV/PF gate.
- Reference: `src/engine/TradingEngine.cpp:635`.

2. Max drawdown hard-cap behavior in live loop.
- Block new entries when drawdown exceeded: `src/engine/TradingEngine.cpp:550`.
- Force protective exits for all open positions: `src/engine/TradingEngine.cpp:1076`.

3. Same drawdown-protective behavior in backtest path (for consistency).
- Reference: `src/backtest/BacktestEngine.cpp:338`.

4. Broken text cleanup in strategy manager logs/comments (UTF-8 rewrite).
- References: `include/strategy/StrategyManager.h`, `src/strategy/StrategyManager.cpp`.

## Priority Tuning Plan (next)
1. Parameterize strategy constants to config (first batch)
- Move per-strategy fixed values (TP/SL, min signal, max position, volatility cutoffs) into `config.json`.
- Keep safe defaults, allow per-regime overrides.

2. Normalize entry quality by expected net edge per regime
- Already global gate exists; next step is strategy-specific expected edge calibration (Scalping/Momentum/Breakout/MeanReversion each separate).

3. Replace naive 5m resampling
- Use timestamp-aligned bars and deterministic session boundaries.

4. Expand backtest datasets used for readiness
- Ensure each strategy has non-zero-trade representative datasets (trend up/down, high vol, range, liquidity stress).

5. Run optimization loop after 1-4
- `scripts/optimize_strategy_set.ps1`
- `scripts/optimize_profitability.ps1 -ApplyBest`
- `scripts/walk_forward_validate.ps1`
- `scripts/validate_readiness.ps1`
