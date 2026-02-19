# Current Chapter

Last updated: 2026-02-19
Chapter ID: `CH-01`
Title: `Verification Reset Baseline`

## Goal
- Replace large verification entry flow with a minimal, sequential, reproducible baseline.
- Keep chapter document short and always actionable.

## Latest Update (2026-02-19)
1. CMake target set was reduced to single runtime binary by default (`AutoLifeTrading` only; tests/tools are opt-in).
2. Runtime core bridge files were flattened from `include/core` + `src/core` into domain folders:
   - `engine/*`, `execution/*`, `risk/*`, `state/*`
3. Old `core` folder tree was removed from active source/include path.
   - compatibility note: namespace `autolife::core` is retained for runtime API stability.

## Completed (Detailed)
1. Added minimal verification entrypoint:
   - `scripts/run_verification.py`
2. Switched user-facing verification commands to baseline:
   - `README.md`
   - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md`
3. Added reset governance docs:
   - `docs/VERIFICATION_RESET_BASELINE_2026-02-19.md`
   - `docs/CHAPTER_CURRENT.md`
   - `docs/CHAPTER_HISTORY_BRIEF.md`
4. Fixed operational principles for reset:
   - sequential-only validation
   - fixed threshold gate as baseline
   - one report + one matrix output
5. Reset execution TODO and archived oversized logs:
   - `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
   - `docs/archive/TODO_STAGE15_EXECUTION_PLAN_2026-02-13_FULLLOG_2026-02-19.md`
6. Ran baseline smoke verification:
   - command:
     - `python scripts/run_verification.py --dataset-names simulation_2000.csv --output-json build/Release/logs/verification_report_smoke.json --output-csv build/Release/logs/verification_matrix_smoke.csv`
   - result:
     - `overall_gate_pass=True`
     - `avg_profit_factor=1.1331`
     - `avg_expectancy_krw=2.4473`
   - artifacts:
     - `build/Release/logs/verification_report_smoke.json`
     - `build/Release/logs/verification_matrix_smoke.csv`
7. Added minimal user-facing verification CLI:
   - `scripts/verify_baseline.py`
   - default command now uses the baseline wrapper.
8. Started `main.cpp` decomposition:
   - extracted repeated backtest console reporting into:
     - `include/app/BacktestReportFormatter.h`
     - `src/app/BacktestReportFormatter.cpp`
   - `main.cpp` now calls shared formatter for CLI/interactive backtest output.
9. Added foundation-level structural review:
   - `docs/FOUNDATION_ENGINE_STRATEGY_REVIEW_2026-02-19.md`
10. Verified build/runtime integrity after `main.cpp` split:
   - build:
     - `cmake --build build --config Release --target AutoLifeTrading` PASS
   - JSON backtest smoke:
     - `build/Release/AutoLifeTrading.exe --backtest data/backtest/simulation_2000.csv --json` PASS
   - wrapper smoke:
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --output-tag wrapper_smoke` PASS
     - artifact: `build/Release/logs/verification_report_wrapper_smoke.json`
11. Split CLI backtest handler out of `main.cpp`:
   - added:
     - `include/app/BacktestCliHandler.h`
     - `src/app/BacktestCliHandler.cpp`
   - `main.cpp` now delegates `--backtest` path to `app::tryRunCliBacktest(...)`.
   - follow-up smoke:
     - `build/Release/AutoLifeTrading.exe --backtest data/backtest/simulation_2000.csv --json` PASS
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --output-tag wrapper_smoke_after_split` PASS
     - artifact: `build/Release/logs/verification_report_wrapper_smoke_after_split.json`
12. Split live interactive setup/bootstrapping out of `main.cpp`:
   - added:
     - `src/app/LiveModeHandler.cpp`
   - integrated existing interface:
     - `include/app/LiveModeHandler.h`
   - `main.cpp` now delegates live path to `app::runInteractiveLiveMode(...)`.
   - build/smoke:
     - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release --target AutoLifeTrading` PASS
     - `build/Release/AutoLifeTrading.exe --backtest data/backtest/simulation_2000.csv --json` PASS
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --output-tag wrapper_smoke_after_live_split` PASS
     - artifact: `build/Release/logs/verification_report_wrapper_smoke_after_live_split.json`
   - size reduction:
     - `src/main.cpp`: 579 -> 367 lines
13. Split interactive backtest setup/execution out of `main.cpp`:
   - added:
     - `include/app/BacktestInteractiveHandler.h`
     - `src/app/BacktestInteractiveHandler.cpp`
   - `main.cpp` now delegates interactive backtest path to `app::runInteractiveBacktest(...)`.
   - build/smoke:
     - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release --target AutoLifeTrading` PASS
     - `build/Release/AutoLifeTrading.exe --backtest data/backtest/simulation_2000.csv --json` PASS
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --output-tag wrapper_smoke_after_main_slim` PASS
     - artifact: `build/Release/logs/verification_report_wrapper_smoke_after_main_slim.json`
   - size reduction:
     - `src/main.cpp`: 367 -> 81 lines
14. Added root-cause data-window parity policy (live/backtest shared):
   - added:
     - `include/common/MarketDataWindowPolicy.h`
   - unified TF window caps:
     - `src/analytics/MarketScanner.cpp`
     - `src/runtime/BacktestRuntime.cpp`
     - `src/runtime/LiveTradingRuntime.cpp`
   - live runtime now skips signal generation on insufficient parity windows:
     - rejection reason: `data_parity_window_insufficient`
   - backtest strict parity mode auto-enabled when companion set(5m/1h/4h) exists:
     - `strict_live_equivalent_data_parity_` in `include/runtime/BacktestRuntime.h`
     - enabled log: `Backtest live-equivalent data parity mode: enabled`
   - verification:
      - `python scripts/verify_baseline.py --datasets simulation_2000.csv --output-tag wrapper_smoke_after_data_window_policy` PASS
      - `python scripts/verify_baseline.py --datasets simulation_2000.csv --output-tag wrapper_smoke_after_data_window_policy_recheck` PASS
      - artifact: `build/Release/logs/verification_report_wrapper_smoke_after_data_window_policy_recheck.json`
15. Added verification data-mode separation (`fixed` vs `refresh_*`) and report traceability:
   - `scripts/run_verification.py`:
     - accepts `--data-mode fixed|refresh_if_missing|refresh_force`
     - writes `data_mode` into report JSON
   - `scripts/verify_baseline.py`:
     - refresh controls (`--refresh-chunk-size`, `--refresh-sleep-ms`, `--refresh-end-utc`)
     - refresh naming contract guard (`--allow-unsupported-refresh-datasets`)
   - docs update:
     - `README.md`
     - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md`
     - `docs/VERIFICATION_RESET_BASELINE_2026-02-19.md`
   - smoke:
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --output-tag data_mode_fixed_smoke` PASS
     - `python scripts/verify_baseline.py --realdata-only --datasets upbit_KRW_BTC_1m_12000.csv --data-mode refresh_if_missing --output-tag data_mode_refresh_if_missing_smoke` PASS (script execution), gate false on dataset
   - artifacts:
     - `build/Release/logs/verification_report_data_mode_fixed_smoke.json`
     - `build/Release/logs/verification_report_data_mode_refresh_if_missing_smoke.json`
16. Added engine/strategy bottleneck diagnostics inside verification report:
   - `scripts/run_verification.py` now aggregates `entry_funnel` into 3 structural groups:
     - `candidate_generation`
     - `quality_and_risk_gate`
     - `execution_constraints`
   - report additions:
     - `diagnostics.aggregate`
     - `diagnostics.per_dataset`
     - `diagnostics.failure_attribution`
   - strategy-level conversion diagnostics added:
     - `generated_signals -> selected_best -> entries_executed`
     - `weak_execution_under_load` shortlist
   - strategy collection-stage diagnostics added (C++ runtime -> JSON):
     - `strategy_collection_summaries` in backtest JSON
     - `strategy_collection.top_no_signal` / `top_skipped_disabled` in verification report
     - `strategy_collect_exception_count` for runtime stability visibility
   - smoke:
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --output-tag engine_diag_smoke3` PASS
   - artifact:
     - `build/Release/logs/verification_report_engine_diag_smoke3.json`
17. Rebuilt strategy/risk manager prefilter from baseline (decomposed rule pipeline):
   - added:
     - `include/strategy/FoundationRiskPipeline.h`
     - `src/strategy/FoundationRiskPipeline.cpp`
   - integrated:
     - `src/strategy/StrategyManager.cpp` now routes manager prefilter through `foundation::evaluateFilter(...)`
     - `CMakeLists.txt` updated to compile the new pipeline source
   - intent:
     - replace monolithic threshold tweaking with deterministic rule composition (`policy`, `regime`, `history`, `RR`, `no-trade-bias`)
   - build/smoke:
     - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release --target AutoLifeTrading` PASS
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --output-tag foundation_rebuild_smoke` PASS
   - artifact:
     - `build/Release/logs/verification_report_foundation_rebuild_smoke.json`
18. Disconnected legacy selection/prefilter execution path and switched validation default to adaptive profile:
   - selection path:
     - `src/strategy/StrategyManager.cpp`
     - `selectRobustSignalWithDiagnostics(...)` now returns via new foundation-first rule path.
   - prefilter path:
     - `src/strategy/StrategyManager.cpp`
     - dead legacy branch in `filterSignalsWithDiagnostics(...)` removed from active flow.
   - verification v3 adaptive default:
     - `scripts/run_verification.py`:
       - `validation_profile=adaptive` default
       - new adaptive metrics:
         - downtrend loss per trade
         - downtrend trade share
         - uptrend expectancy
         - risk-adjusted score
       - legacy gate retained only under `validation_profile=legacy_gate`
     - `scripts/verify_baseline.py`:
       - adaptive validation arguments wired through
   - smoke:
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --output-tag adaptive_rebuild_smoke` PASS
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile legacy_gate --output-tag legacy_compat_smoke` PASS
   - artifacts:
     - `build/Release/logs/verification_report_adaptive_rebuild_smoke.json`
     - `build/Release/logs/verification_report_legacy_compat_smoke.json`
19. Established strict execution order for the rebuild batch:
   - documentation sync first:
     - `docs/ADAPTIVE_ENGINE_REBUILD_PLAN_2026-02-19.md`
     - `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
     - `docs/CHAPTER_CURRENT.md`
   - then runtime legacy path hardening + verification script alignment.
20. Completed legacy selection dead-path pruning and command canonicalization:
   - C++ runtime path hardening:
     - `src/strategy/StrategyManager.cpp`
     - physically removed unreachable legacy block from `selectRobustSignalWithDiagnostics(...)`
   - README baseline normalization:
     - `README.md`
     - fixed verification section encoding/content and made adaptive baseline command canonical
   - verification recheck:
     - build:
       - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release --target AutoLifeTrading` PASS
     - adaptive smoke:
       - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile adaptive --output-tag adaptive_rebuild_smoke3` PASS
       - artifact: `build/Release/logs/verification_report_adaptive_rebuild_smoke3.json`
     - legacy compatibility smoke:
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile legacy_gate --output-tag legacy_compat_smoke3` PASS
      - artifact: `build/Release/logs/verification_report_legacy_compat_smoke3.json`
21. Cleaned residual legacy strategy tree folders:
   - removed empty directories:
     - `include/strategy`
     - `src/strategy`
   - note:
     - active strategy implementation remains only in `include/strategy/*` and `src/strategy/*`.
22. Switched runtime execution to foundation-only strategy path:
   - added:
     - `include/strategy/FoundationAdaptiveStrategy.h`
     - `src/strategy/FoundationAdaptiveStrategy.cpp`
   - runtime registration switch:
      - `src/runtime/BacktestRuntime.cpp`
      - `src/runtime/LiveTradingRuntime.cpp`
     - runtime registration is hard-switched to foundation-only:
       - only `Foundation Adaptive Strategy` is registered
       - legacy strategy pack registration code path is disconnected
     - removed residual scalping-specific runtime downcast handling blocks from live/backtest monitoring path.
   - config normalization:
     - `src/common/Config.cpp` (`foundation` alias -> `foundation_adaptive`)
   - active config changed:
     - `config/config.json` `enabled_strategies=["foundation_adaptive"]`
   - verification:
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile adaptive --output-tag foundation_only_smoke` PASS
     - artifact: `build/Release/logs/verification_report_foundation_only_smoke.json`
     - hard-switch recheck:
       - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile adaptive --output-tag foundation_only_hardswitch_smoke` PASS
       - artifact: `build/Release/logs/verification_report_foundation_only_hardswitch_smoke.json`
     - hard-switch cleanup recheck:
       - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile adaptive --output-tag foundation_only_hardswitch2_smoke` PASS
       - artifact: `build/Release/logs/verification_report_foundation_only_hardswitch2_smoke.json`
     - strategy funnel confirms single registered strategy:
       - `Foundation Adaptive Strategy`
     - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile legacy_gate --output-tag foundation_only_legacy_smoke` PASS (script), legacy gate result `overall_gate_pass=False`
     - artifact: `build/Release/logs/verification_report_foundation_only_legacy_smoke.json`
     - hard-switch legacy compatibility recheck:
       - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile legacy_gate --output-tag foundation_only_hardswitch_legacy_smoke` PASS (script), legacy gate result `overall_gate_pass=False`
       - artifact: `build/Release/logs/verification_report_foundation_only_hardswitch_legacy_smoke.json`
23. Removed unused legacy strategy units from compile and active tree:
   - compile exclusion:
     - `CMakeLists.txt`
     - removed legacy strategy units from `LIB_SOURCES` and `AutoLifeV2CompileObjects`
   - deleted legacy strategy files:
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
   - active strategy tree now:
     - `include/strategy/{IStrategy.h,StrategyManager.h,FoundationRiskPipeline.h,FoundationAdaptiveStrategy.h,StrategyConfig.h}`
     - `src/strategy/{StrategyManager.cpp,FoundationRiskPipeline.cpp,FoundationAdaptiveStrategy.cpp}`
   - verification:
     - build:
       - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release --target AutoLifeTrading` PASS
    - adaptive smoke:
      - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile adaptive --output-tag foundation_only_tree_cleanup_smoke` PASS
      - artifact: `build/Release/logs/verification_report_foundation_only_tree_cleanup_smoke.json`
    - legacy compatibility smoke:
      - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile legacy_gate --output-tag foundation_only_tree_cleanup_legacy_smoke` PASS (script), legacy gate result `overall_gate_pass=False`
      - artifact: `build/Release/logs/verification_report_foundation_only_tree_cleanup_legacy_smoke.json`
24. Flattened `v2` folders into canonical project layout and removed `include/v2`, `src/v2`:
   - strategy layer moved:
     - `include/strategy/{IStrategy.h,StrategyConfig.h,StrategyManager.h,FoundationRiskPipeline.h,FoundationAdaptiveStrategy.h}`
     - `src/strategy/{StrategyManager.cpp,FoundationRiskPipeline.cpp,FoundationAdaptiveStrategy.cpp}`
   - kernel/bridge/runtime scaffolds moved:
     - `include/core/orchestration/DecisionKernel.h`
     - `src/core/orchestration/DecisionKernel.cpp`
     - `include/core/contracts/{IExecutionPlaneV2.h,IPolicyPlaneV2.h,IRiskPlaneV2.h,ILearningStateStoreV2.h}`
     - `include/core/adapters/{LegacyExecutionPlaneAdapter.h,LegacyPolicyPlaneAdapter.h,LegacyRiskPlaneAdapter.h}`
     - `src/core/adapters/{LegacyExecutionPlaneAdapter.cpp,LegacyPolicyPlaneAdapter.cpp,LegacyRiskPlaneAdapter.cpp}`
     - `include/engine/TradingEngineV2.h` / `src/engine/TradingEngineV2.cpp`
     - `include/backtest/BacktestEngineV2.h` / `src/backtest/BacktestEngineV2.cpp`
   - build/script wiring updated:
     - `CMakeLists.txt`
     - `tests/TestV2*.cpp`
     - `scripts/assess_wave_b_readiness.py`
   - verification:
     - build PASS:
       - `AutoLifeTrading`
       - `AutoLifeV2CompileObjects`
       - `AutoLifeV2KernelSmokeTest`
       - `AutoLifeV2EngineBacktestSmokeTest`
       - `AutoLifeV2ShadowParityTest`
     - run PASS:
       - `build/Release/AutoLifeV2KernelSmokeTest.exe`
       - `build/Release/AutoLifeV2EngineBacktestSmokeTest.exe`
       - `build/Release/AutoLifeV2ShadowParityTest.exe`
      - readiness script PASS:
        - `python scripts/assess_wave_b_readiness.py`
25. Purged remaining legacy shadow/v2 codepaths from runtime/build:
   - runtime removal:
     - `src/runtime/BacktestRuntime.cpp`
     - `src/runtime/LiveTradingRuntime.cpp`
     - removed v2 shadow parity artifact generation/probe calls
   - config removal:
     - `include/engine/EngineConfig.h` (`enable_v2_shadow_policy_probe` removed)
     - `src/common/Config.cpp` (same key load removed)
     - `config/config.json` (same key removed)
   - build removal:
     - `CMakeLists.txt` from `LIB_SOURCES`: `DecisionKernel.cpp`, `LegacyPolicyPlaneAdapter.cpp`
     - removed targets: `AutoLifeV2KernelSmokeTest`, `AutoLifeV2EngineBacktestSmokeTest`, `AutoLifeV2CompileObjects`, `AutoLifeV2ShadowParityTest`
   - deleted files:
     - `include/core/contracts/*V2.h`
     - `include/core/model/KernelTypes.h`
     - `include/core/orchestration/DecisionKernel.h`
     - `src/core/orchestration/DecisionKernel.cpp`
     - `include/core/adapters/Legacy*PlaneAdapter.h`
     - `src/core/adapters/Legacy*PlaneAdapter.cpp`
     - `include/engine/TradingEngineV2.h`
     - `src/engine/TradingEngineV2.cpp`
     - `include/backtest/BacktestEngineV2.h`
     - `src/backtest/BacktestEngineV2.cpp`
     - `tests/TestV2*.cpp`
     - `scripts/validate_v2_shadow_parity.py`
     - `scripts/assess_wave_b_readiness.py`
   - verification:
     - build PASS:
       - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release --target AutoLifeTrading`
     - scripts PASS:
       - `python scripts/verify_script_suite.py --skip-help`
     - baseline smoke PASS:
       - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile adaptive --output-tag legacy_v2_purge_smoke`
       - artifact: `build/Release/logs/verification_report_legacy_v2_purge_smoke.json`
26. Removed remaining v2/legacy naming artifacts from active path and deleted residual v2 strategy files:
   - deleted residual tracked files:
     - `include/v2/strategy/*.h`
     - `src/v2/strategy/*.cpp`
   - renamed verification entrypoint:
     - `scripts/run_verification.py` (replaces `run_verification_v2.py`)
     - `scripts/verify_baseline.py` now calls `run_verification.py`
   - normalized verification artifact naming:
     - `verification_report*.json`
     - `verification_matrix*.csv`
   - removed legacy rollback preset:
     - deleted `config/presets/legacy_fallback.json`
     - updated `scripts/apply_trading_preset.py` preset choices to `safe|active`
     - updated `README.md` preset section accordingly
   - runtime naming cleanup:
     - `include/strategy/IStrategy.h`: `getPrimaryTakeProfit()`
     - updated call sites:
       - `src/strategy/StrategyManager.cpp`
       - `src/strategy/FoundationRiskPipeline.cpp`
   - verification:
     - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release --target AutoLifeTrading` PASS
    - `python scripts/verify_script_suite.py --skip-help` PASS
    - `python scripts/verify_baseline.py --datasets simulation_2000.csv --data-mode fixed --validation-profile adaptive --output-tag naming_cleanup_smoke` PASS
    - artifact: `build/Release/logs/verification_report_naming_cleanup_smoke.json`
27. Audited `core` vs non-core execution ownership:
   - usage audit result:
     - active runtime bridge path is now canonical domain folders:
       - `execution/*`, `engine/*`, `risk/*`, `state/*`
     - `include/core/*`, `src/core/*` are removed from active source/include path.
     - namespace `autolife::core` is retained for API compatibility only.
   - backtest data loader ownership validation:
     - `include/backtest/DataHistory.h`
     - `src/backtest/DataHistory.cpp`
     - `BacktestRuntime`에서 `DataHistory::loadCSV/loadJSON`를 직접 사용하므로 유지.
     - `CMakeLists.txt`에서 active build source로 유지.
    - intent:
      - keep only files with verified runtime/build ownership and reduce tree ambiguity.
28. Runtime gate overfit-risk hardening (phase-1):
   - identified root issue:
     - `requiresTypedArchetype(...)` was effectively `true` for almost all non-empty strategy names.
     - this could over-reject valid signals when `entry_archetype` is unavailable in non-archetype-driven strategies.
   - code change:
     - `include/common/SignalPolicyShared.h`
     - `src/common/SignalPolicyShared.cpp`
   - updated policy:
     - typed-archetype enforcement now applies to archetype-sensitive strategy families only
       (`foundation_adaptive`, momentum/breakout/scalp/trend token families).
   - baseline config hardening:
     - `config/config.json`: `trading.use_strategy_alpha_head_mode` switched to `false`
     - intent: avoid fallback-heuristic over-reliance in default runtime profile.
   - started runtime gate commonization:
     - moved canonical implementations into shared policy module:
       - `isAlphaHeadFallbackCandidate(...)`
       - `normalizeSignalStopLossByRegime(...)`
   - verification:
     - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build --config Release` PASS
     - `build/Release/AutoLifeTest.exe` PASS
     - `build/Release/AutoLifeStateTest.exe` PASS
     - `build/Release/AutoLifeExecutionStateTest.exe` PASS
     - `build/Release/AutoLifeEventJournalTest.exe` PASS
29. CMake default build surface minimization + clean build revalidation:
   - CMake default options hardened:
     - `AUTOLIFE_BUILD_GATE_TESTS=OFF` (default)
     - `AUTOLIFE_BUILD_TOOL_BINARIES=OFF` (default)
     - `AUTOLIFE_BUILD_EXTRA_TESTS=OFF` (default)
   - startup target fixed for VS:
     - `AutoLifeTrading`
   - clean configure/build verification (fresh build dir):
     - configure:
       - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe -S . -B build_fresh` PASS
     - build:
       - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build_fresh --config Release --target AutoLifeTrading` PASS
   - artifact check:
     - `build_fresh/Release/AutoLifeTrading.exe` only runtime executable generated by default.
30. Runtime commonization phase-2A + source encoding normalization:
   - UTF-8 normalization (invalid-byte warning reduction + safe patch prerequisite):
     - `src/runtime/BacktestRuntime.cpp`
     - `src/runtime/LiveTradingRuntime.cpp`
     - `include/risk/RiskManager.h`
     - `include/common/Config.h`
     - `include/runtime/LiveTradingRuntime.h`
   - edge-stats commonization:
     - added shared module:
       - `include/common/StrategyEdgeStatsShared.h`
       - `src/common/StrategyEdgeStatsShared.cpp`
     - moved duplicated helpers from runtime files into shared module:
       - `StrategyEdgeStats`
       - `makeStrategyRegimeKey(...)`
       - `makeMarketStrategyRegimeKey(...)`
       - `buildStrategyEdgeStats(...)`
       - `buildStrategyRegimeEdgeStats(...)`
       - `buildMarketStrategyRegimeEdgeStats(...)`
     - runtime integration:
       - `src/runtime/BacktestRuntime.cpp`
       - `src/runtime/LiveTradingRuntime.cpp`
   - gate helper de-dup completion (phase-1 follow-through):
     - runtime wrappers now delegate to shared policy module:
       - `isAlphaHeadFallbackCandidate(...)`
       - `normalizeSignalStopLossByRegime(...)`
   - verification:
     - `D:/MyApps/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin/cmake.exe --build build_fresh --config Release --target AutoLifeTrading` PASS
     - `build_fresh/Release/AutoLifeTrading.exe --backtest data/backtest/simulation_2000.csv --json` PASS (process exit 0)
     - UTF-8 integrity scan (`include/`, `src/`): `invalid_count=0`

## Remaining In This Chapter
1. Complete runtime gate commonization phase-2:
   - phase-2B: unify remaining duplicated helper blocks between `LiveTradingRuntime.cpp` and `BacktestRuntime.cpp`
   - candidates: `regimeToLabel/strengthBucket/expectedValueBucket/rewardRiskBucket` and rejection taxonomy helpers.
   - target: one shared implementation source of truth.
2. Add deterministic reproducibility check:
   - run identical `fixed` command twice and assert same gate-critical fields.
3. Continue engine/strategy root-cause deep dive based on diagnostics:
   - if `candidate_generation` dominates: inspect manager prefilter + per-strategy conversion.
   - if `quality_and_risk_gate` dominates: inspect risk-gate decomposition before threshold tuning.
4. Start adaptive lifecycle risk rewrite:
   - dynamic TP/SL + trailing + post-entry re-evaluation state machine extraction.
5. Decide which legacy verification scripts are archive targets for CH-02.
6. Normalize source/header file encodings to UTF-8 (no invalid byte warnings):
   - remove recurring `C4828` warning flood in runtime/risk/config headers.
   - prerequisite for safe large-file patching and deterministic diff review.

## Exit Criteria
- Team can run one command and get reproducible verification output.
- Current chapter doc contains:
  - exact completed items
  - exact remaining items
  - artifact file paths

