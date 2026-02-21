# Delete Wave Manifest

Last updated: 2026-02-16

## Cleanup Policy
- Strategy: aggressive cleanup with two-stage deletion.
- Stage 1: move cleanup targets to `legacy_archive/`.
- Stage 2: permanently delete archived targets after verification.
- Retention window: overridden to `0 days` by operator request (immediate final delete).

## Wave A (Final Delete Executed)
- Status: complete (hard delete done).
- Archive record: `legacy_archive/manifest.json`.
- Scope:
  - experiment candidate presets
  - auxiliary analysis scripts
  - historical archive worklog document

## Wave A Verification Checklist
1. Source paths removed and (archive targets present OR final-delete mode enabled).
2. Runtime test executables pass.
3. Core operation scripts still run (`--help`/basic invocation checks).
4. No broken references to old source paths in active docs/scripts/CI.

### Verification Command
```powershell
python scripts/verify_script_suite.py
python scripts/run_ci_operational_gate.py --include-backtest
```

Expected report artifact:
- `build/Release/logs/operational_readiness_report.json`

## Wave B (Final Delete Executed)
- Stage-1 move and replacement completed batch-by-batch after v2 replacement and shadow parity gates.
- Final hard delete executed (retention override enabled).
- Candidate batches:
  - B1: legacy core adapters
    - `include/core/adapters/LegacyExecutionPlaneAdapter.h`
    - `include/core/adapters/LegacyPolicyLearningPlaneAdapter.h`
    - `src/core/adapters/LegacyExecutionPlaneAdapter.cpp`
    - `src/core/adapters/LegacyPolicyLearningPlaneAdapter.cpp`
  - B2: v1 engine/backtest
    - `include/engine/TradingEngine.h`
    - `src/engine/TradingEngine.cpp`
    - `include/backtest/BacktestEngine.h`
    - `src/backtest/BacktestEngine.cpp`
  - B3: v1 strategy layer (incremental)
    - `include/strategy/*.h` (only after each strategy is replaced in v2)
    - `src/strategy/*.cpp` (only after each strategy is replaced in v2)

### Wave B1 Progress (2026-02-16)
- Stage-1 move executed (legacy -> archive):
  - `include/core/adapters/LegacyExecutionPlaneAdapter.h`
  - `include/core/adapters/LegacyPolicyLearningPlaneAdapter.h`
  - `src/core/adapters/LegacyExecutionPlaneAdapter.cpp`
  - `src/core/adapters/LegacyPolicyLearningPlaneAdapter.cpp`
- Replacement wiring in active tree:
  - `include/core/adapters/ExecutionPlaneAdapter.h`
  - `include/core/adapters/PolicyLearningPlaneAdapter.h`
  - `src/core/adapters/ExecutionPlaneAdapter.cpp`
  - `src/core/adapters/PolicyLearningPlaneAdapter.cpp`
- Replacement wired to runtime/tests:
  - `src/runtime/LiveTradingRuntime.cpp`
  - `tests/TestV2ShadowParity.cpp`
  - `CMakeLists.txt`

### Wave B2 Progress (2026-02-16)
- Stage-1 move executed (legacy -> archive):
  - `include/engine/TradingEngine.h`
  - `src/engine/TradingEngine.cpp`
  - `include/backtest/BacktestEngine.h`
  - `src/backtest/BacktestEngine.cpp`
- Runtime replacement wiring in active tree:
  - `include/runtime/LiveTradingRuntime.h`
  - `src/runtime/LiveTradingRuntime.cpp`
  - `include/runtime/BacktestRuntime.h`
  - `src/runtime/BacktestRuntime.cpp`
- Active include/source wiring updated:
  - `src/main.cpp`
  - `CMakeLists.txt`
  - `include/strategy/ScalpingStrategy.h`
  - `include/strategy/MomentumStrategy.h`
  - `include/strategy/GridTradingStrategy.h`

## Wave B Readiness Assessment
- Readiness assessor script:
  - `scripts/assess_wave_b_readiness.py`
- Purpose:
  - evaluate global gate readiness (`Wave A`, `OperationalReadiness`, `v2 shadow parity`)
  - compute per-batch delete readiness (`B1`, `B2`, `B3 incremental`) from active build references
- Verification command:
```powershell
python scripts/assess_wave_b_readiness.py --run-refresh-checks
```
- Expected report artifact:
  - `build/Release/logs/wave_b_readiness_report.json`
- Current status (2026-02-16):
  - `global_ready=true`
  - `B1=true` (legacy adapter files moved + replacement wiring active)
  - `B2=true` (legacy engine/backtest moved + runtime replacement wiring active)
  - `B3_any=true` (strategy replacement readiness opened)
  - `B3_all=true` (strategy layer stage-1 migration baseline complete)
  - `ready_for_full_wave_b=true` (all Wave B batches readiness gates satisfied)
  - final delete: executed (retention override)

### Wave B3 Bootstrap Progress (2026-02-16)
- First unit readiness opened:
  - `StrategyConfig` unit is now sourced from:
    - `include/strategy/StrategyConfig.h`
    - `src/strategy/StrategyConfig.cpp`
  - `include/common/Config.h` switched include from:
    - `strategy/StrategyConfig.h` -> `strategy/StrategyConfig.h`
- Readiness assessor robustness fix:
  - `scripts/assess_wave_b_readiness.py` path matching changed from raw substring to token-boundary matching.
  - Purpose: prevent false-positive reference hits where nested strategy paths were counted as shorter legacy candidates.

### Wave B3 Partial Stage-1 Move (2026-02-16)
- Moved to archive:
  - `include/strategy/BreakoutStrategy.h`
  - `src/strategy/BreakoutStrategy.cpp`
- Active v2 replacement wiring:
  - `include/strategy/BreakoutStrategy.h`
  - `src/strategy/BreakoutStrategy.cpp`
  - `src/runtime/LiveTradingRuntime.cpp`
  - `src/runtime/BacktestRuntime.cpp`
  - `CMakeLists.txt` (`src/strategy/BreakoutStrategy.cpp` linked)

### Wave B3 Stage-1 Expansion (2026-02-16)
- Additional moved strategy files:
  - `include/strategy/IStrategy.h`
  - `include/strategy/StrategyManager.h`
  - `include/strategy/ScalpingStrategy.h`
  - `include/strategy/MomentumStrategy.h`
  - `include/strategy/MeanReversionStrategy.h`
  - `include/strategy/GridTradingStrategy.h`
  - `src/strategy/StrategyManager.cpp`
  - `src/strategy/ScalpingStrategy.cpp`
  - `src/strategy/MomentumStrategy.cpp`
  - `src/strategy/MeanReversionStrategy.cpp`
  - `src/strategy/GridTradingStrategy.cpp`
- Active v2 strategy replacements:
  - `include/strategy/IStrategy.h`
  - `include/strategy/StrategyManager.h`
  - `include/strategy/ScalpingStrategy.h`
  - `include/strategy/MomentumStrategy.h`
  - `include/strategy/MeanReversionStrategy.h`
  - `include/strategy/GridTradingStrategy.h`
  - `src/strategy/StrategyManager.cpp`
  - `src/strategy/ScalpingStrategy.cpp`
  - `src/strategy/MomentumStrategy.cpp`
  - `src/strategy/MeanReversionStrategy.cpp`
  - `src/strategy/GridTradingStrategy.cpp`
- Wiring updates:
  - `src/runtime/LiveTradingRuntime.cpp` strategy includes switched to `strategy/*`
  - `src/runtime/BacktestRuntime.cpp` strategy includes switched to `strategy/*`
  - `include/runtime/LiveTradingRuntime.h` / `include/runtime/BacktestRuntime.h` now include `strategy/StrategyManager.h`
  - core/risk/engine contracts switched to `strategy/IStrategy.h`:
    - `include/risk/RiskManager.h`
    - `include/engine/AdaptivePolicyController.h`
    - `include/core/model/PlaneTypes.h`
    - `include/core/contracts/IPolicyLearningPlane.h`
    - `include/core/contracts/IRiskCompliancePlane.h`
    - `include/core/orchestration/TradingCycleCoordinator.h`
  - `CMakeLists.txt` strategy compilation paths switched to `src/strategy/*.cpp`
- Final stage-1 completion marker:
  - `include/strategy/StrategyConfig.h` moved to `legacy_archive/include/strategy/StrategyConfig.h`
  - active `include/strategy/` now has no v1 strategy unit headers
  - readiness note from report:
    - `no_remaining_v1_strategy_units_detected`

## Final Delete Execution (2026-02-16)
- Operator decision:
  - retention wait waived, immediate hard delete requested.
- Deleted scope:
  - Wave A moved artifacts
  - Wave B moved artifacts (`B1`, `B2`, `B3`)
- Post-delete archive state:
  - `legacy_archive` keeps only `legacy_archive/manifest.json` as audit record.

## v2 Bootstrap Verification (2026-02-16)
- Dedicated smoke target added:
  - `AutoLifeV2KernelSmokeTest`
  - `AutoLifeV2EngineBacktestSmokeTest`
- Dedicated compile-guard target added:
  - `AutoLifeV2CompileObjects`
- Scope of wiring:
  - `src/core/orchestration/DecisionKernel.cpp`
  - `src/engine/TradingEngineV2.cpp`
  - `src/backtest/BacktestEngineV2.cpp`
  - `tests/TestV2DecisionKernel.cpp`
  - `tests/TestV2EngineBacktestScaffold.cpp`
  - `src/core/adapters/Legacy*PlaneAdapter.cpp`
- Verification:
  - build: `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release --target AutoLifeV2CompileObjects AutoLifeV2KernelSmokeTest AutoLifeV2EngineBacktestSmokeTest`
  - run: `.\build\Release\AutoLifeV2KernelSmokeTest.exe`
  - run: `.\build\Release\AutoLifeV2EngineBacktestSmokeTest.exe`
  - result: PASS (`[TEST] V2DecisionKernel PASSED`)

## v2 Shadow Parity Harness (2026-02-16)
- Dedicated shadow parity target added:
  - `AutoLifeV2ShadowParityTest`
- Verification script added:
  - `scripts/validate_v2_shadow_parity.py`
- Optional gate integration added:
  - `scripts/validate_operational_readiness.py --include-v2-shadow-parity --strict-v2-shadow-parity`
  - `scripts/run_ci_operational_gate.py --include-v2-shadow-parity --strict-v2-shadow-parity`
  - runtime-aware strict option:
    - `--check-runtime-v2-shadow-parity` (backtest runtime shadow)
    - `--check-runtime-live-v2-shadow-parity` (live runtime shadow)
- Current parity scope:
  - policy selected set parity
  - rejection taxonomy parity
  - execution update schema compatibility
  - backtest runtime policy-input shadow parity (`v2_shadow_policy_parity_backtest.jsonl`)
  - live runtime policy-input shadow parity (`v2_shadow_policy_parity_live.jsonl`)
- Verification:
  - build: `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release --target AutoLifeV2ShadowParityTest`
  - run: `python scripts/validate_v2_shadow_parity.py -Strict`
  - runtime strict: `python scripts/validate_v2_shadow_parity.py -CheckRuntimeShadow -Strict`
  - runtime strict (live ?ы븿): `python scripts/validate_v2_shadow_parity.py -CheckRuntimeShadow -CheckRuntimeLiveShadow -Strict`
  - note: live strict requires at least one `LiveTradingRuntime::executeSignals` cycle to populate `v2_shadow_policy_parity_live.jsonl`.
  - artifact: `build/Release/logs/v2_shadow_parity_report.json`
  - result: PASS

## Rollback
- Because final delete is executed, file-level rollback by archive move reversal is no longer available.
- Recovery path is git history-based restore for explicitly selected files.

## Notes
- `scripts/validate_small_seed.py` is intentionally kept because `scripts/capture_baseline.py` imports it.
- `data/backtest/simulation_large.csv` is intentionally kept because it is used as a default dataset in operational scripts.
