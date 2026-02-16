# File Usage Map

Last updated: 2026-02-16

## Purpose
This document classifies repository files by operational criticality so cleanup can be done without breaking runtime or CI flows.

## Categories

### runtime-critical
- Definition: files directly used in build/runtime paths (`CMakeLists.txt`, `src/*`, `include/*`, `tests/*` link targets).
- Rule: do not delete until replacement path is implemented and validated.
- Current scope:
  - `src/**/*.cpp` (all 36 are currently linked by `CMakeLists.txt`)
  - `include/**/*.h` (runtime and compile contracts)
  - `tests/*.cpp` (verification binaries)
  - runtime replacement paths (Wave B2 stage-1):
    - `include/runtime/LiveTradingRuntime.h`
    - `src/runtime/LiveTradingRuntime.cpp`
    - `include/runtime/BacktestRuntime.h`
    - `src/runtime/BacktestRuntime.cpp`
  - active core adapter replacements:
    - `include/core/adapters/ExecutionPlaneAdapter.h`
    - `include/core/adapters/PolicyLearningPlaneAdapter.h`
    - `src/core/adapters/ExecutionPlaneAdapter.cpp`
    - `src/core/adapters/PolicyLearningPlaneAdapter.cpp`
  - B3 first replacement-ready unit:
    - `include/v2/strategy/StrategyConfig.h`
    - `src/v2/strategy/StrategyConfig.cpp`
    - `include/common/Config.h` now reads strategy config types from v2 path
  - B3 partial stage-1 moved unit replacement:
    - `include/v2/strategy/BreakoutStrategy.h`
    - `src/v2/strategy/BreakoutStrategy.cpp`
    - runtime wiring switched in:
      - `src/runtime/LiveTradingRuntime.cpp`
      - `src/runtime/BacktestRuntime.cpp`
  - B3 stage-1 expanded strategy replacements:
    - `include/v2/strategy/IStrategy.h`
    - `include/v2/strategy/StrategyManager.h`
    - `include/v2/strategy/ScalpingStrategy.h`
    - `include/v2/strategy/MomentumStrategy.h`
    - `include/v2/strategy/MeanReversionStrategy.h`
    - `include/v2/strategy/GridTradingStrategy.h`
    - `src/v2/strategy/StrategyManager.cpp`
    - `src/v2/strategy/ScalpingStrategy.cpp`
    - `src/v2/strategy/MomentumStrategy.cpp`
    - `src/v2/strategy/MeanReversionStrategy.cpp`
    - `src/v2/strategy/GridTradingStrategy.cpp`
  - legacy strategy tree state after B3 stage-1:
    - `include/strategy/` stage-1 unit headers moved to `legacy_archive/include/strategy/`
    - `src/strategy/` active sources removed (moved to `legacy_archive/src/strategy/`)
    - active strategy runtime path is `include/v2/strategy/*` + `src/v2/strategy/*`

### migration-bootstrap (v2)
- Definition: new rewrite scaffold files for v2 migration.
- Rule: keep isolated and additive; only wire through dedicated smoke targets until shadow parity is proven.
- Current scope:
  - `include/v2/**/*`
  - `src/v2/**/*`
  - `tests/TestV2DecisionKernel.cpp`
  - `tests/TestV2ShadowParity.cpp`
- Current build wiring:
  - `AutoLifeV2KernelSmokeTest` (v2 `DecisionKernel` + smoke test)
  - `AutoLifeV2EngineBacktestSmokeTest` (v2 engine/backtest scaffold smoke test)
  - `AutoLifeV2CompileObjects` (v2 kernel + legacy bridge adapters compile-only guard)
  - `AutoLifeV2ShadowParityTest` (v1 core policy adapter vs v2 policy adapter shadow parity)
- Runtime shadow artifacts:
  - `build/Release/logs/v2_shadow_policy_parity_backtest.jsonl` (BacktestEngine policy-input parity stream)
  - `build/Release/logs/v2_shadow_policy_parity_live.jsonl` (LiveTradingRuntime executeSignals policy-input parity stream)
  - `build/Release/logs/wave_b_readiness_report.json` (delete-batch readiness snapshot for B1/B2/B3)

### ops-critical
- Definition: files invoked by CI gates, runbooks, or day-to-day operations.
- Rule: delete only after replacement command and runbook update are complete.
- Current scope:
  - `.github/workflows/*`
  - `scripts/run_ci_operational_gate.py`
  - `scripts/run_realdata_candidate_loop.py`
  - `scripts/run_candidate_train_eval_cycle.py`
  - `scripts/validate_operational_readiness.py`
  - `scripts/validate_execution_parity.py`
  - `scripts/validate_recovery_state.py`
  - `scripts/validate_recovery_e2e.py`
  - `scripts/validate_replay_reconcile_diff.py`
  - `scripts/validate_v2_shadow_parity.py`
  - `scripts/assess_wave_b_readiness.py`
  - `scripts/capture_baseline.py`
  - `scripts/validate_small_seed.py`
  - `README.md`
  - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md`
  - `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
  - `docs/TARGET_ARCHITECTURE.md`

### research-aux
- Definition: experimental assets not required for normal build/runtime/CI gate path.
- Rule: move to `legacy_archive/` first; finalize hard delete after verification.
- Wave A moved examples:
  - candidate preset snapshots
  - auxiliary analysis scripts
  - archived stage worklog document
- Current state (2026-02-16):
  - retention override applied (`0 days`)
  - archived payload files were hard-deleted
  - `legacy_archive/manifest.json` is retained as audit metadata only

## Safety Policy
- Default retention for moved files: 7 days (overridden to 0 days for current execution).
- Deletion process: move -> verify -> final delete.
- No immediate deletion of `runtime-critical` files.
