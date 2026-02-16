# Stage 15 Execution TODO (Active)

Last updated: 2026-02-16 (build/generated outputs + unused warnings cleanup executed)

## Goal
Build a personal-use crypto trading bot that is adaptive, restart-safe, and verifiable.
The system must:
- follow Upbit API/WebSocket call rules,
- train on high-quality data,
- persist learning state across restarts,
- run backtests that match live decision flow.

## Principles
- Keep strict safety chain and strict live approval process.
- Keep legacy fallback until core path is fully proven.
- Prefer robust logic changes over narrow parameter overfitting.
- If market data is hostile, do not force trade count.

## Execution Discipline (Context Handoff)
- Every completed subtask must update this TODO immediately with:
  - what changed (files/functions),
  - what was verified (build/tests/commands),
  - next subtask and expected artifact path.
- Keep one active in-progress item only to avoid context drift.
- If a run fails, record the exact failing command + first root-cause clue before any new patch.

## Active Snapshot (2026-02-16)
- Historical progress logs were archived to:
  - `docs/archive/STAGE15_WORKLOG_ARCHIVE_2026-02-16.md`
- Current execution baseline:
  - Latest completed branch updates: `Stage-2.1` to `Stage-2.6`
  - Current major bottlenecks: `filtered_out_by_manager_ev_quality_floor`, `skipped_due_to_open_position`
  - Next action order: manager EV prefilter calibration -> position-state skip policy calibration

## P0 (Must do first)
1. Upbit compliance hardening
- Enforce shared rate budget across scanner/live/backtest fetch tools.
- Respect `Remaining-Req`, and implement deterministic handling for `429` and `418`.
- Add compliance telemetry to logs:
  - request bucket usage,
  - throttle/degrade events,
  - recover timestamps.

2. Live-backtest parity (critical)
- Use same candle inputs and same decision path in live and backtest:
  - `1m`, `5m`, `15m`, `1h`, `4h` (when available).
- Enforce identical candle ordering/normalization before strategy logic.
- Backtest must use rolling-window semantics equivalent to live cache updates.
- Status update (2026-02-15):
  - `15m` companion timeframe is now provided on both paths:
    - live scanner derives `15m` from cached `1m` (fallback from `5m`) and stores into `candles_by_tf`.
    - trading engine enforces `15m` parity companion fill before strategy collection.
    - backtest now loads `15m` companion csv when present and exposes `candles_by_tf["15m"]` with rolling cursor semantics.
  - changed files:
    - `src/analytics/MarketScanner.cpp`
    - `src/engine/TradingEngine.cpp`
    - `src/backtest/BacktestEngine.cpp`
  - next:
    - completed: parity invariant check/report linked into realdata profitability artifact chain.
    - next: apply dataset quality gate policy using parity diagnostics (`gap/dup/stale`) before tuning selection.

3. Data quality gate
- Validate each dataset before tuning:
  - timestamp order,
  - duplicate ratio,
  - gap ratio,
  - stale tail,
  - companion TF coverage.
- Compute and store `market_hostility_score` and apply adaptive gate floors from hostility band.

4. Learning persistence
- Keep adaptive state in `state/learning_state.json` with atomic write.
- Add schema version + migration guard.
- Persist per-bucket performance stats used by core policy scoring.
- Restart must warm-load state without silent reset.
- Status update (2026-02-15):
  - `LearningStateStoreJson` now enforces `kCurrentSchemaVersion` and validates required fields on load.
  - Legacy/unversioned payloads are migrated to current schema (policy params normalization) and written back atomically.
  - Unsupported/future schema payloads are rejected with warning logs instead of silent fallback parse.

5. Verification split enforcement (started)
- Use `train/validation/holdout` market split before any candidate promotion decision.
- Keep tuning/learning on `train` split only, and force deterministic eval on `validation/holdout` with `skip-tune`.
- Enforce walk-forward checks per split before promotion.

## P1 (After P0)
1. Strategy pipeline refactor (5 strategies)
- Audit end-to-end path per strategy:
  - filter -> signal -> entry -> risk sizing -> exit.
- Remove duplicated ad-hoc thresholds and move shared logic into core adaptive utilities.
- Add consistent rejection reason codes for every strategy decision.
- Stage-2 started:
  - `use_strategy_alpha_head_mode` added (strategy prefilter minimization, final gating in engine/core).
  - `NO_TRADE` journal event added for hostile/policy/execution rejection visibility.
  - Alpha-head mode now bypasses strategy auto-disable by historical EV gate in live loop.
- Stage-2.1 update (2026-02-16):
  - Structural contradiction fix in `ScalpingStrategy`:
    - removed unconditional `RANGING/UNKNOWN` early exits that made lower regime-quality logic unreachable,
    - aligned `shouldEnter` with the same `ranging/unknown` quality gates used by `generateSignal`.
  - Regime ownership fix in `GridTradingStrategy`:
    - `shouldEnter` no longer ignores regime,
    - both `generateSignal` and `shouldEnter` now block weak `TRENDING_DOWN` entries and allow `TRENDING_UP` only under stronger liquidity/volume conditions,
    - `generateSignal` now also enforces `shouldGenerateGridSignal(...)` so invalid/non-profitable grid opportunities cannot bypass through score-only path.
  - Entry gate consistency fix in `MomentumStrategy`:
    - `shouldEnter` MACD gate relaxed from strict positive-only to `positive OR histogram-rising`,
    - minimum momentum floor aligned to exploratory candidate generation (`0.2%+`).
  - verification:
    - build PASS:
      - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release`
    - runtime PASS:
      - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions`
  - current bottleneck remains:
    - `entry_rejection_top_reason=no_signal_generated` still dominant, so next step is manager/engine-side candidate recovery and gate ownership cleanup.
- Stage-2.2 update (2026-02-16):
  - Strategy-manager ownership refactor:
    - `StrategyManager` now supports `core_rescue_candidate` path:
      - if strategy `generateSignal(...)` returns `NONE` but `shouldEnter(...)` is true, manager builds a recoverable candidate for core-layer final gating,
      - preserves strategy-specific stop/take-profit/position sizing calls while moving final reject authority to engine core gates.
    - manager hard policy block is softened in core mode:
      - `selectRobustSignalWithDiagnostics(...)` no longer hard-drops `BLOCK` roles when core bridge+risk plane is active,
      - `filterSignalsWithDiagnostics(...)` also softens `BLOCK` to stricter `HOLD`-like requirements in core mode.
  - Code cleanup (unused API removal):
    - removed unused `StrategyManager::synthesizeSignals(...)` declaration/definition,
    - removed unused `StrategyManager::getOverallWinRate()` declaration/definition.
  - verification:
    - build PASS:
      - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release`
    - runtime PASS:
      - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions`
- Stage-2.3 update (2026-02-16):
  - Additional unused-code cleanup in `StrategyManager`:
    - removed unused wrapper APIs:
      - `selectRobustSignal(...)`
      - `filterSignals(...)`
    - removed unused strategy state toggling/read APIs:
      - `enableStrategy(...)`
      - `getActiveStrategies()`
  - cleanup principle:
    - remove only functions with repo-wide reference count `0` (declaration/definition only),
    - preserve active interfaces used by engine/backtest (`collectSignals`, `filterSignalsWithDiagnostics`, `selectRobustSignalWithDiagnostics`, `getStrategy`, `getStrategies`).
  - verification:
    - build PASS:
      - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release`
- Stage-2.4 update (2026-02-16):
  - Strategy API surface cleanup (unused accessor removal):
    - removed `getRollingStatistics()` declaration/definition from:
      - `ScalpingStrategy`
      - `MomentumStrategy`
      - `BreakoutStrategy`
      - `MeanReversionStrategy`
      - `GridTradingStrategy`
  - cleanup basis:
    - repo-wide search showed no production/test call sites for these methods.
  - verification:
    - build PASS (`Release`)
    - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions` PASS
- Stage-2.5 update (2026-02-16):
  - Comment/encoding stabilization for strategy refactor files:
    - restored strategy files to clean baseline and reapplied only intended logic changes,
    - removed accidental text-noise churn from previous bulk rewrite attempt,
    - added Korean context comments around newly relaxed/realigned regime-entry gates.
  - verification:
    - build PASS:
      - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release`
    - runtime PASS:
      - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions`
  - current readout (same bottleneck class):
    - `entry_rejection_top_reason=no_signal_generated` remains dominant.
- Stage-2.6 update (2026-02-16):
  - Bottom-line priority execution (`2/16` latest branch first):
    - candidate tuning rerun:
      - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode quality_focus -MaxScenarios 6 -RealDataOnly -RequireHigherTfCompanions`
    - result:
      - `best_combo=scenario_quality_focus_004`
      - all combos `overall_gate_pass=false`, `profile_gate_pass=false`
      - effective hostility/quality blend:
        - `hostility_blended_level=low`, `hostility_blended_score=31.7817`
      - top scenario family remained `signal_generation_boost` (bottleneck priority active).
  - train/eval split cycle rerun:
    - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune`
    - split summary:
      - train(7): `avg_profit_factor=0.5422`, `avg_expectancy_krw=-7.7148`, top rejection `no_signal_generated`
      - validation(2): `avg_profit_factor=0.5384`, `avg_expectancy_krw=-6.6658`, top rejection `filtered_out_by_manager_ev_quality_floor`
      - holdout(3): `avg_profit_factor=0.5066`, `avg_expectancy_krw=-7.2827`, top rejection `skipped_due_to_open_position`
    - walk-forward:
      - validation ready ratio `0.0` (min `0.55`) -> fail
      - holdout ready ratio `0.0` (min `0.55`) -> fail
    - promotion verdict:
      - `promotion_gate_pass=false`
      - recommendation: `hold_candidate_improve_validation_bottlenecks`
  - next bottleneck target by bottom-line chain:
    - manager prefilter EV floor calibration (`filtered_out_by_manager_ev_quality_floor`)
    - position state/open-slot policy calibration (`skipped_due_to_open_position`)

2. Expectancy-first improvement loop
- Optimize for:
  - `avg_expectancy_krw`,
  - `profitable_ratio`,
  - drawdown stability.
- Keep a minimum trade floor, but make it hostility-adaptive instead of fixed-only.

3. Candidate promotion readiness
- Repeat matrix/tuning on expanded real datasets.
- Record baseline vs candidate delta with explicit failure attribution.

## P2 (Migration closure)
1. Core migration finalization
- Make core path default for all decision planes in production profile.
- Keep one explicit rollback preset during burn-in.
- Remove legacy-only branches after burn-in evidence is complete.

2. Documentation and CI alignment
- Keep only active operational docs.
- Ensure CI PR Gate includes exploratory profitability report upload path checks.

## Validation Commands
- Exploratory profitability:
  - `python scripts/run_profitability_exploratory.py`
- Real-data candidate loop:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
- Candidate tuning:
  - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode quality_focus -MaxScenarios 6 -RealDataOnly -RequireHigherTfCompanions`
- Split train/eval cycle with walk-forward gate:
  - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune`

## Exit Criteria
- Backtest/live parity checks pass on decision-path and candle-path invariants.
- Adaptive learning state survives restart and remains usable.
- Candidate gate passes expectancy/profitable-ratio focused thresholds on real-data matrix.
- Core path is proven and legacy cleanup can proceed in controlled PR batches.
