# Adaptive Architecture Roadmap

## Problem Statement
Static strategy thresholds create a structural trade-off:
- tighten filters -> fewer entries -> low sample throughput
- loosen filters -> lower win-rate and weaker edge

Goal is to move from static strategy gating to adaptive policy decisions that react to volatility, liquidity, and execution stress.

## Target Operating Model
1. Strategies become alpha generators.
- Each strategy emits candidates with features and confidence.

2. Policy layer becomes final decision maker.
- Candidate selection, thresholding, and sizing are adjusted by regime and recent performance.

3. Risk layer remains hard guard.
- Fee/slippage, min order, exposure caps, and drawdown constraints always apply.

## Architecture Components
1. `StrategySignalBus`
- Normalize candidate signals from all strategies.

2. `PerformanceStore`
- Rolling stats by `(strategy, regime, seed_bucket, liquidity_bucket)`.
- Includes trades, win rate, PF, expectancy, drawdown proxy.

3. `AdaptivePolicyController`
- Input: signal candidates + market regime + account state + performance stats.
- Output: selected candidates, dynamic thresholds, and priority ordering.

4. `DecisionAuditLog`
- Structured logs for policy input/output and rejection reasons.

5. `LearningStateStore`
- Persistent online-learning checkpoint (`state/learning_state.json`).
- Stores rolling stats, decay factors, and adaptive gate multipliers.
- Restart-safe warm-start for enterprise operation.

## Current Status (2026-02-13)
Completed:
- `AdaptivePolicyController` scaffold implemented and linked in engine.
- `PerformanceStore` scaffold implemented and initialized from trade history.
- `AdaptivePolicyController` upgraded to regime/performance-aware re-ranking and small-seed quality pruning.
- `PerformanceStore` expanded with bucket stats: `(strategy, regime, liquidity_bucket)`.
- `BreakoutStrategy` upgraded with adaptive regime/liquidity/strength/ATR-volume gates.
- `MeanReversionStrategy` upgraded with adaptive regime/liquidity/strength/RR floors.
- Risk/Trade data model expanded with regime/liquidity/volatility/EV/RR metadata for each trade.
- Fee-inclusive small-seed matrix re-run completed (`build/Release/logs/small_seed_summary.json`).
- Release build passes.
- Real-data candidate loop established:
  - `scripts/run_realdata_candidate_loop.py` (fetch -> matrix -> tuning).
  - Upbit historical 1m datasets integrated into profitability gate workflow.
- Live/Backtest operator UX simplification:
  - `main` live mode supports `SIMPLE(SAFE/BALANCED/ACTIVE)` profile mode.
  - Interactive backtest supports running an existing real-data CSV directly.
- Backtest parity fixes:
  - Backtest now injects MTF candles (`1m/5m/1h/4h/1d`) into `CoinMetrics`.
  - Companion TF datasets (`5m/60m/240m`) are auto-loaded when present; fallback aggregation remains.
  - Core plane flags now alter backtest decision path (legacy vs bridge vs policy/risk/execution).

In progress:
- Decision records are structured in policy output, but persistent JSONL verification in backtest path is still open.
- Shared adaptive utility layer is not yet unified across all strategies.
- 100k seed currently shows negative expectancy across all curated datasets.
- Parameter sweeps over top-level knobs showed invariant results, indicating current bottleneck is deeper than config-level RR/edge/floor values.

## Next Implementation Phases
1. Phase 1: Policy scoring enhancement
- Add candidate score adjustment based on regime stress and strategy recent expectancy.
- Add drop reason codes for auditability.

2. Phase 2: Shared adaptive utilities
- Centralize adaptive floor formulas and apply to all strategies.
- Remove duplicated threshold logic from strategy files.

3. Phase 3: Online adaptation
- Add rolling decay updates and drift-aware penalty.
- Prepare Thompson/UCB path for strategy weighting.
- Persist learning state and support restart warm-load (`LearningStateStore`).

4. Phase 4: Validation closure
- Same decision path for backtest and live.
- Automated reports for 50k/100k seed scenarios.

## Exit Criteria
1. Small-seed viability
- 50k and 100k KRW runs meet minimum profitable ratio and tradable ratio targets.

2. Stability under regime shifts
- No severe entry collapse or over-trading spike during regime transitions.

3. Explainability
- Policy decisions can be replayed from audit logs.

4. Continuity
- Adaptive learning state survives process restart and resumes without cold-start reset.

## Immediate TODO
1. Verify persistent policy decision logs in backtest path and include them in sweep analysis.
2. Implement `LearningStateStore` checkpoint/load for online adaptive parameters.
3. Unify adaptive threshold helpers across all strategy classes.
4. Add backtest gate-rejection telemetry and retune based on telemetry, not only top-level config knobs.
5. Promote scanner preloaded timeframe candles (`candles_by_tf`) to first-class strategy inputs across all strategies (1m/5m/1h).
6. Add fallback-quality flags (preloaded vs resampled) to signal metadata for policy weighting.
7. Add market-scan telemetry: per-TF cache hit ratio, full-sync vs incremental ratio, stale-candle detection.
8. Shift candidate gate tuning objective from trade-density to edge-quality:
- Prioritize fixing negative expectancy/profitable-ratio bottlenecks on real-data matrix.
- Add per-market loss-tail diagnostics and strategy/regime attribution for worst contributors.
9. Close profile-parity gap in backtest:
- Add explicit regression test/CI check that `profiles_identical_by_dataset` is `False` on at least one real-data fixture.
10. After parameter sweep, tune strategy-internal filters:
- Focus on per-strategy loss-tail controls (breakout/scalping first) to lift expectancy and profitable-ratio without collapsing trade count.
