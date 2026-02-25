# Phase 2/3 Migration Cleanup (Path-First)
Last updated: 2026-02-25

## Goal
- Keep Phase 2/3 validation baseline intact while removing code paths that are not needed for active runtime behavior.
- Prioritize deterministic parity safety over broad refactors.

## Baseline Verification Before Cleanup
- Command:
  - `python scripts/run_ci_operational_gate.py --include-backtest --strict-execution-parity --run-should-exit-parity-analysis --refresh-should-exit-audit-from-logs --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" --should-exit-audit-live-runtime-log-mode-filter exclude_backtest --strict-should-exit-parity`
- Result:
  - `CIGate PASSED`
  - `ExecutionParity PASSED`
  - `ShouldExitParity verdict=pass, critical_findings=0`

## Cleanup Applied (Wave 1)
1. Removed deprecated config alias:
   - deleted: `enable_v21_rescue_prefiltered_pair_probe`
2. Removed alias bridge parsing path:
   - `src/common/Config.cpp` no longer maps old key to `enable_uptrend_rescue_prefilter_tail_guard`
3. Simplified runtime guard condition:
   - `src/runtime/LiveTradingRuntime.cpp`
   - `src/runtime/BacktestRuntime.cpp`
   - now uses only `enable_uptrend_rescue_prefilter_tail_guard`

## Cleanup Applied (Wave 2)
1. Removed backtest-only correctness probe flags:
   - deleted: `backtest_strategyless_runtime_live_exit_mapping`
   - deleted: `backtest_strategyless_runtime_live_exit_mapping_hard_exit_only`
2. Removed disabled-by-default strategyless mapping branch in backtest exit path:
   - `src/runtime/BacktestRuntime.cpp`
3. Tightened websocket activation to real-live-order mode only:
   - `src/runtime/LiveTradingRuntime.cpp`
   - myOrder WS now starts only when `mode=LIVE && !dry_run && allow_live_orders=true`

## Why This Is Safe
- Alias default was OFF and no active config uses this key.
- Effective runtime behavior remains unchanged unless old key was explicitly used.
- Live/Backtest use the same single flag after cleanup.

## Next Cleanup Waves (Candidate)
1. Remove stale references to retired probe flags from historical codex logs.
2. Separate analysis-only scripts from gate-critical scripts via explicit manifest.
3. Add workflow reachability check to fail when unreferenced scripts re-accumulate.
