# Chapter History (Brief)

This file is intentionally concise.
Keep each completed chapter to 2-4 lines.

## Entries
- 2026-02-19 | `CH-00` | Reset docs baseline and archive oversized logs.
  - Active TODO reduced to short actionable format.
  - Validation method risks documented in `docs/VALIDATION_METHOD_REVIEW_2026-02-19.md`.
- 2026-02-19 | `CH-01` (in progress) | Verification reset baseline implementation started.
  - Added `scripts/run_verification.py` as new minimal entrypoint.
  - Added `scripts/verify_baseline.py` wrapper and updated README/runbook commands.
  - Baseline smoke completed (`simulation_2000.csv`): gate pass true.
  - Started `main.cpp` reporting-layer extraction (`BacktestReportFormatter`) and build-smoke passed.
  - Split CLI backtest path into `BacktestCliHandler` and re-verified JSON/wrapper smoke.
  - Split live interactive path into `LiveModeHandler`; `main.cpp` reduced to 367 lines and re-verified smoke.
  - Split interactive backtest path into `BacktestInteractiveHandler`; `main.cpp` reduced to 81 lines and re-verified smoke.
  - Added shared data-window parity policy and strict live-equivalent backtest mode (companion-set gated).
  - Added `fixed/refresh_*` data-mode separation; gate baseline fixed, refresh for robustness only.
  - Added data-mode smoke artifacts:
    - `build/Release/logs/verification_report_data_mode_fixed_smoke.json`
    - `build/Release/logs/verification_report_data_mode_refresh_if_missing_smoke.json`
  - Added verification report bottleneck diagnostics (`diagnostics.aggregate/per_dataset/failure_attribution`) for engine/strategy root-cause analysis.
  - Added strategy collection-stage diagnostics (`top_no_signal`, `strategy_collect_exception_count`) to separate generation bottleneck from gate bottleneck.
  - Added foundation strategy/risk prefilter pipeline (`FoundationRiskPipeline`) and connected `StrategyManager` to the new decomposed rule path.
  - Disconnected runtime path from legacy select/prefilter logic and switched verification baseline default to adaptive profile (`validation_profile=adaptive`).
  - Physically pruned unreachable legacy selection block in `StrategyManager` and re-verified adaptive/legacy compatibility smokes.
  - Removed residual empty legacy strategy directories (`include/strategy`, `src/strategy`) to simplify tree view.
  - Added `FoundationAdaptiveStrategy` and hard-switched runtime registration to foundation-only mode.
  - Removed remaining scalping-specific runtime downcast logic from live/backtest monitoring path.
  - Excluded/deleted unused legacy v2 strategy units from compile and active tree.
  - Flattened `include/v2`, `src/v2` into canonical locations (`strategy`, `core`, `engine`, `backtest`) and removed both folders.
  - Purged remaining v2/legacy shadow stack (runtime probe, `AutoLifeV2*` targets, `Legacy*PlaneAdapter`, `DecisionKernel`, `*V2` engine/backtest/contracts/tests).
  - Finalized active-path naming cleanup: `run_verification.py` entrypoint, `verification_report/matrix` artifacts, `getPrimaryTakeProfit()`.
  - Removed residual preset fallback file: `config/presets/legacy_fallback.json`.
  - Audited core-vs-external runtime ownership and confirmed active dependency chain (`core` bridge + `execution/engine/risk`).
  - Validated backtest data-loader ownership (`DataHistory`) and kept it in active path (`BacktestRuntime` direct dependency).
  - Flattened active runtime bridge tree from `core/*` folders into domain folders (`engine/execution/risk/state`) and removed `include/core`, `src/core`.
  - Hardened overfit-risk gate rule: `requiresTypedArchetype(...)` narrowed from near-global enforcement to archetype-sensitive strategy families only.
  - Started runtime gate commonization by promoting `isAlphaHeadFallbackCandidate(...)` and `normalizeSignalStopLossByRegime(...)` into `common/SignalPolicyShared`.
  - Default runtime profile hardened: `config/config.json` now sets `use_strategy_alpha_head_mode=false`.
  - Reduced default CMake build to `AutoLifeTrading` only (`AUTOLIFE_BUILD_GATE_TESTS=OFF`) and revalidated with fresh `build_fresh` configure/build.
  - Completed runtime commonization phase-2A by extracting duplicated edge stats/key-builder helpers into `common/StrategyEdgeStatsShared` and wiring both runtimes to shared helpers.
  - Normalized core runtime/risk/config files to UTF-8 to unblock safe large-file patching and reduce `C4828` warning noise.

