# File Usage Map

Last updated: 2026-02-19

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
  - active strategy runtime units (foundation-only):
    - `include/strategy/FoundationAdaptiveStrategy.h`
    - `src/strategy/FoundationAdaptiveStrategy.cpp`
    - `include/strategy/FoundationRiskPipeline.h`
    - `src/strategy/FoundationRiskPipeline.cpp`
    - `include/strategy/IStrategy.h`
    - `include/strategy/StrategyManager.h`
    - `src/strategy/StrategyManager.cpp`
    - `include/strategy/StrategyConfig.h` (config compatibility type-only header)
  - runtime strategy registration mode:
    - runtime is hard-switched to foundation-only registration:
      - registered: `Foundation Adaptive Strategy`
      - skipped: legacy strategy pack registration
    - enforced in:
      - `src/runtime/LiveTradingRuntime.cpp`
      - `src/runtime/BacktestRuntime.cpp`
  - legacy strategy tree state after B3 stage-1:
    - `include/strategy/` stage-1 unit headers moved to `legacy_archive/include/strategy/`
    - `src/strategy/` active sources removed (moved to `legacy_archive/src/strategy/`)
    - empty legacy directories (`include/strategy`, `src/strategy`) physically removed
    - legacy v2 strategy units physically deleted from active tree:
      - `include/strategy/ScalpingStrategy.h`
      - `include/strategy/MomentumStrategy.h`
      - `include/strategy/BreakoutStrategy.h`
      - `include/strategy/MeanReversionStrategy.h`
      - `include/strategy/GridTradingStrategy.h`
      - `src/strategy/ScalpingStrategy.cpp`
      - `src/strategy/MomentumStrategy.cpp`
      - `src/strategy/BreakoutStrategy.cpp`
      - `src/strategy/MeanReversionStrategy.cpp`
      - `src/strategy/GridTradingStrategy.cpp`
      - `src/strategy/StrategyConfig.cpp`
    - active strategy runtime path is `include/strategy/*` + `src/strategy/*`

### migration-bootstrap (retired)
- Status: retired on 2026-02-19.
- Removed from active tree/build:
  - `include/v2/`, `src/v2/` (already removed)
  - all `autolife::v2` scaffold headers/sources under `include/core/*`, `src/core/*`, `include/engine/*`, `src/engine/*`, `include/backtest/*`, `src/backtest/*`
  - legacy shadow adapter/test stack (`Legacy*PlaneAdapter`, `DecisionKernel`, `TestV2*`, `AutoLifeV2*` targets)
- Current policy:
  - no runtime/compile dependency on v2 shadow-parity path
  - no `*V2` header/cpp files remain in `include/` or `src/`

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
  - `scripts/capture_baseline.py`
  - `scripts/validate_small_seed.py`
  - `README.md`
  - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md`
  - `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
  - `docs/VALIDATION_METHOD_REVIEW_2026-02-19.md`
  - `docs/VERIFICATION_RESET_BASELINE_2026-02-19.md`
  - `docs/FOUNDATION_ENGINE_STRATEGY_REVIEW_2026-02-19.md`
  - `docs/CHAPTER_CURRENT.md`
  - `docs/CHAPTER_HISTORY_BRIEF.md`
  - `docs/TARGET_ARCHITECTURE.md`
  - `scripts/verify_baseline.py`
  - `scripts/run_verification.py`

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
- Docs archive state (2026-02-19):
  - long-form execution logs and one-off script audits moved to `docs/archive/`
  - active execution references are maintained only in root `docs/`

## Safety Policy
- Default retention for moved files: 7 days (overridden to 0 days for current execution).
- Deletion process: move -> verify -> final delete.
- No immediate deletion of `runtime-critical` files.

