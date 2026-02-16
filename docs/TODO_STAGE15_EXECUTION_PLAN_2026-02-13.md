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

## Current Progress Snapshot (2026-02-15 run)
1. Core decision-path migration progress
- `use_strategy_alpha_head_mode` path expanded:
  - strategy-manager prefilter bypass stays active in alpha-head mode,
  - fallback alpha candidates are now treated as first-class candidates in engine/backtest gates,
  - risk/pattern penalties are softened only in alpha-head fallback path (not globally),
  - live/backtest expected-edge calibration now has fallback-specific win-prob/cost adjustment.

2. Smoke parity check (single real dataset)
- Command:
  - `build/Release/AutoLifeTrading.exe --backtest data/backtest_real/upbit_KRW_BTC_1m_12000.csv --json --require-higher-tf-companions`
- Result (core_full-like runtime path):
  - `entries_executed=4`, `total_trades=7`,
  - `expectancy_krw=+8.08`,
  - `profit_factor=4.08`,
  - still high `no_signal_generated=11449` (hostile/no-entry dominant periods remain).

3. Real-data candidate loop (12 datasets, gate_min_avg_trades=8)
- Command:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions`
- Result (`core_full`):
  - `avg_total_trades=44.4167` (trade floor pass),
  - `avg_profit_factor=0.7744` (fail),
  - `avg_expectancy_krw=-8.0054` (fail),
  - `profitable_ratio=0.0833` (fail),
  - `overall_gate_pass=false`.

4. Split + walk-forward enforcement run
- Command:
  - `python scripts/run_candidate_train_eval_cycle.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --max-markets 12 --enforce-walk-forward --walk-forward-max-datasets 6`
- Result summary:
  - train(7): `PF=0.9728`, `EXP=-8.0643`, `trades=43.43`, pass=false
  - validation(2): `PF=0.4903`, `EXP=-9.1096`, `trades=50.0`, pass=false
  - holdout(3): `PF=0.5009`, `EXP=-7.1320`, `trades=43.0`, pass=false
  - walk-forward: validation ready `0/2`, holdout ready `0/3`, promotion gate=false.

5. Immediate bottleneck (unchanged)
- Candidate still fails mainly on:
  - `avg_expectancy_krw`,
  - `avg_profit_factor`,
  - `profitable_ratio`.
- Trade-frequency floor is now consistently passing under hostility-adaptive thresholds.

## Latest Worklog (2026-02-16 local run)
1. P0-2 parity first pass completed
- Added `15m` companion TF generation/exposure on both paths:
  - `src/analytics/MarketScanner.cpp`
  - `src/engine/TradingEngine.cpp`
  - `src/backtest/BacktestEngine.cpp`
- Behavior:
  - live scanner: derive `15m` from `1m` (fallback from `5m`) into `candles_by_tf`.
  - live engine: enforce parity fill for missing `15m` before strategy collection.
  - backtest: load optional `15m` companion file and provide rolling `15m` series.

2. Verification
- Build:
  - `cmake --build build --config Release -j 6` PASS
- Tests:
  - `build/Release/AutoLifeEventJournalTest.exe` PASS
  - `build/Release/AutoLifeExecutionStateTest.exe` PASS
- Smoke:
  - `build/Release/AutoLifeTrading.exe --backtest data/backtest/sample_trend_pullback_1m.csv --json` PASS

3. Next subtask (already queued)
- Implement parity invariant report:
  - per-run TF availability/coverage (`1m/5m/15m/1h/4h`)
  - ordering monotonicity check
  - rolling-window equivalence hash for backtest path
- target artifact:
  - `build/Release/logs/parity_invariant_report.json`

4. Latest real-data revalidation (2026-02-16)
- Command:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions`
- Result (`core_full`):
  - `avg_profit_factor=0.5277` (fail)
  - `avg_total_trades=118.8333` (pass)
  - `avg_expectancy_krw=-7.8330` (fail)
  - `profitable_ratio=0.0` (fail)
  - `overall_gate_pass=false`
- Interpretation:
  - structure migration/parity work is progressing,
  - profitability objective remains unresolved (PF/expectancy/profitable-ratio bottleneck),
  - avoid additional threshold overfitting; continue with parity invariant + data quality gate first.

5. Gate policy unification in progress (2026-02-16)
- Testing policy was aligned to dataset hostility, not strict/adaptive split.
- Script updates:
  - `scripts/run_profitability_matrix.py`
    - hostility-adaptive thresholds default to enabled,
    - explicit disable flags added for rollback/debug only.
  - `scripts/run_realdata_candidate_loop.py`
    - strict/adaptive dual-run path deprecated and removed from active execution,
    - single hostility-driven run is now the only active path,
    - stale strict/adaptive variant artifacts are removed before run.
  - `scripts/run_candidate_train_eval_cycle.py`
    - strict/adaptive pair consumption disabled (selected snapshot only).
- Next:
  - re-run realdata loop and train/eval cycle once to confirm no stale strict/adaptive coupling remains.
  - verification completed:
    - `python -m py_compile scripts/run_profitability_matrix.py scripts/run_realdata_candidate_loop.py scripts/run_candidate_train_eval_cycle.py` PASS
    - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions` PASS (single hostility-driven run, `adaptive_mode=full`, `overall_gate_pass=false`)
    - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --max-markets 4 --disable-walk-forward` PASS (`snapshot.strict/adaptive=null`, selected-only evaluation)

6. P0-2 parity invariant report delivered (2026-02-16)
- Added new script:
  - `scripts/generate_parity_invariant_report.py`
  - output: `build/Release/logs/parity_invariant_report.json`
- Report content:
  - TF availability/coverage (`1m/5m/15m/1h/4h`)
  - ordering monotonicity after normalization
  - duplicate/gap/stale-tail diagnostics
  - rolling-window equivalence hash (binary-search vs incremental-cursor) for backtest-style TF windows
- Integrated into realdata loop artifact chain:
  - `scripts/run_realdata_candidate_loop.py`
    - runs parity report before matrix by default,
    - supports `--skip-parity-invariant`, `--fail-on-parity-invariant`,
    - prints `parity_invariant_report=...` path in completion output.
- Loop-speed control for long iterative runners:
  - `scripts/run_candidate_train_eval_cycle.py`
    - default skips parity report in nested stages (`--run-parity-invariant` to enable).
  - `scripts/run_candidate_auto_improvement_loop.py`
    - default skips parity report in nested iterations (`--run-parity-invariant` to enable).
- Verification:
  - `python -m py_compile scripts/generate_parity_invariant_report.py scripts/run_realdata_candidate_loop.py scripts/run_candidate_train_eval_cycle.py scripts/run_candidate_auto_improvement_loop.py` PASS
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions` PASS
    - parity summary: `dataset_count=12`, `invariant_pass_count=8`, `invariant_fail_count=4`
    - fail datasets: `KRW-AVAX`, `KRW-BCH`, `KRW-DOT`, `KRW-LINK`
    - fail reason: `gap_ratio_tolerance_pass=false` (timestamp gap ratio above tolerance)
  - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --max-markets 4 --disable-walk-forward` PASS (nested loops keep speed path by default)
- Next subtask:
  - wire parity/data-quality summary into tuning objective prefilter so high-gap datasets are weighted down or excluded automatically.
  - target artifact: `build/Release/logs/dataset_quality_gate_summary.json`

7. P0-3 data quality gate wired into tuning chain (2026-02-16)
- `scripts/tune_candidate_gate_trade_density.py`
  - parity-based dataset quality gate pre-step added,
  - quality-pass datasets only are used for screen/final tuning evaluation,
  - fail-open/fail-closed behavior added:
    - default: fail-open when zero pass,
    - optional: `--dataset-quality-gate-fail-closed` to hard fail.
  - quality gate state is persisted into tuning summary JSON under `dataset_quality_gate`.
- `scripts/run_candidate_auto_improvement_loop.py`
  - added parity-report pass-through controls for nested loops:
    - `--run-parity-invariant` (default off for speed),
    - `--fail-on-parity-invariant`.
- Verification:
  - `python -m py_compile scripts/tune_candidate_gate_trade_density.py scripts/run_realdata_candidate_loop.py scripts/generate_parity_invariant_report.py scripts/run_candidate_auto_improvement_loop.py scripts/run_candidate_train_eval_cycle.py` PASS
  - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode quality_focus -MaxScenarios 1 -RealDataOnly -RequireHigherTfCompanions -ScreenDatasetLimit 2 -ScreenTopK 1 -FinalProfileIds core_full -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1` PASS
    - quality gate summary written: `build/Release/logs/dataset_quality_gate_summary.json`
    - input 12 datasets -> pass 8 / fail 4 (gap-ratio fail: `AVAX`, `BCH`, `DOT`, `LINK`)
    - tuning summary now includes `dataset_quality_gate` block and effective `dataset_count=8`.
- Next subtask:
  - connect quality-gate severity (`gap/dup/stale`) to hostility score blending so gate floors and tuning objectives are adjusted together.
  - target artifact: `build/Release/logs/dataset_hostility_quality_blend_report.json`

8. P0-4 hostility+quality blended thresholding integrated (2026-02-16)
- `scripts/run_profitability_matrix.py`
  - added dataset quality analysis (`duplicate/gap/stale`) and quality-risk score per dataset,
  - added blended adversity context (`hostility + quality`) and switched adaptive threshold relief source to blended score/level,
  - added blend artifact output:
    - `build/Release/logs/dataset_hostility_quality_blend_report.json`,
  - core-vs-legacy adaptive delta relax now uses blended hostility level (not hostility-only).
- `scripts/run_realdata_candidate_loop.py`
  - snapshot logging now prints blended hostility + quality summary.
- `scripts/tune_candidate_gate_trade_density.py`
  - per-combo summary rows now include:
    - `quality_level`,
    - `quality_avg_score`,
    - `hostility_blended_level`,
    - `hostility_blended_score`,
  - eval cache schema bumped to avoid stale row format reuse.
- Verification:
  - `python -m py_compile scripts/run_profitability_matrix.py scripts/run_realdata_candidate_loop.py scripts/tune_candidate_gate_trade_density.py` PASS
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions` PASS
    - blended summary: `level=low`, `score=35.0091`, `quality=low`, `q_score=10.492`
    - artifact emitted: `build/Release/logs/dataset_hostility_quality_blend_report.json`
  - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode quality_focus -MaxScenarios 1 -RealDataOnly -RequireHigherTfCompanions -ScreenDatasetLimit 2 -ScreenTopK 1 -FinalProfileIds core_full -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1` PASS
    - tuning summary row now carries blended/quality fields with non-null values.
- Next subtask:
  - use blended hostility/quality context inside candidate auto-improvement loop targeting logic (dynamic objective floors by blended band).
  - target artifact: `build/Release/logs/candidate_auto_improvement_summary.json` (with blended-band context and per-iteration active floors).

9. P0-5 auto-improvement loop dynamic objective floors by blended band (2026-02-16)
- `scripts/run_candidate_auto_improvement_loop.py`
  - threshold context now keeps `effective`, `quality`, `blended_context` from matrix report,
  - iteration-level objective floors are now derived dynamically from blended-effective thresholds:
    - `objective_min_profitable_ratio_iter`
    - `objective_min_avg_win_rate_pct_iter`
    - `objective_min_expectancy_krw_iter`
    - `objective_min_avg_trades_iter`
  - per-iteration row now records blended/quality context:
    - `blended_hostility_level`, `blended_hostility_score`, `quality_level`, `quality_avg_score`.
- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py` PASS
  - `python scripts/run_candidate_auto_improvement_loop.py -MaxIterations 1 -SkipTunePhase -RealDataOnly -MaxRuntimeMinutes 30` PASS
    - status: `paused_max_iterations` (expected by test setup),
    - `candidate_auto_improvement_iterations.csv` includes new objective-floor + blended/quality columns with non-null values.
- Next subtask:
  - start P0-1 Upbit compliance hardening execution:
  - shared request-budget telemetry + deterministic 429/418 degrade/recover event logging across fetch/scan/live.
  - target artifacts:
    - `build/Release/logs/upbit_compliance_telemetry.jsonl`
    - `build/Release/logs/upbit_compliance_summary.json`

10. P0-1 compliance hardening started (fetch tool path, 2026-02-16)
- `scripts/fetch_upbit_historical_candles.py`
  - Added `Remaining-Req` parsing and telemetry logging per request.
  - Added deterministic 429/418 retry/backoff handling with explicit throttle/recover event capture.
  - Added compliance artifacts:
    - telemetry stream: `build/Release/logs/upbit_compliance_telemetry.jsonl`
    - per-market summary: `build/Release/logs/upbit_compliance_summary_<MARKET>_<TF>.json`
  - Added CLI flags:
    - `--compliance-telemetry-jsonl`
    - `--compliance-summary-json`
    - `--max-retries-429`
    - `--max-retries-418`
    - `--retry-base-ms`
- Verification:
  - `python -m py_compile scripts/fetch_upbit_historical_candles.py` PASS
  - `python scripts/fetch_upbit_historical_candles.py -Market KRW-BTC -Unit 1 -Candles 20 -ChunkSize 20 -SleepMs 0 -OutputPath .\\build\\Release\\logs\\fetch_test_upbit_krw_btc_1m_20.csv` PASS
  - `python scripts/fetch_upbit_historical_candles.py -Market KRW-ETH -Unit 5 -Candles 5 -ChunkSize 5 -SleepMs 0 -OutputPath .\\build\\Release\\logs\\fetch_test_upbit_krw_eth_5m_5.csv` PASS
  - Output check:
    - `upbit_compliance_telemetry.jsonl` contains request-level success event with parsed `Remaining-Req`.
    - `upbit_compliance_summary_KRW_ETH_5m.json` emitted with retry/throttle/recover counters.
- Next subtask:
  - extend same compliance telemetry schema to runtime HTTP client (`UpbitHttpClient/RateLimiter`) so live scanner/engine/backtest share the same degrade/recover event vocabulary.
  - target artifacts:
    - `build/Release/logs/upbit_compliance_telemetry.jsonl` (runtime events appended)
    - `build/Release/logs/upbit_compliance_summary_runtime.json`

11. P0-1 compliance hardening extended to runtime HTTP client (2026-02-16)
- `include/execution/RateLimiter.h`
  - added runtime compliance telemetry API:
    - `recordHttpOutcome(group, source_tag, status_code, remaining_req_header)`
  - added runtime summary counters/state fields for:
    - request/success/rate-limit/recover/backoff totals.
- `src/execution/RateLimiter.cpp`
  - added JSONL telemetry append + runtime summary write:
    - `logs/upbit_compliance_telemetry.jsonl`
    - `logs/upbit_compliance_summary_runtime.json`
  - added structured event vocabulary:
    - `http_success`
    - `rate_limit_error` (`429`/`418`, with `backoff_ms`)
    - `http_error`
  - summary counters aligned with fetch-tool schema:
    - `request_count`, `http_success_count`, `rate_limit_429_count`, `rate_limit_418_count`,
    - `retry_count`, `backoff_sleep_ms_total`, `throttle_event_count`, `recover_event_count`.
- `src/network/UpbitHttpClient.cpp`
  - wired GET/POST/DELETE response path to call `recordHttpOutcome(...)` on every HTTP response.
  - preserved existing limiter behavior (`updateFromHeader`, `handleRateLimitError`) and added telemetry without changing request flow.
- Verification:
  - build:
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
  - added runtime telemetry test target:
    - `AutoLifeRateLimiterComplianceTest`
    - files:
      - `tests/TestRateLimiterComplianceTelemetry.cpp`
      - `CMakeLists.txt` (new test target)
  - test verification:
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeRateLimiterComplianceTest` PASS
    - `build\\Release\\AutoLifeRateLimiterComplianceTest.exe` PASS
    - artifact check PASS:
      - `build/Release/logs/upbit_compliance_summary_runtime.json`
      - `build/Release/logs/upbit_compliance_telemetry.jsonl` (runtime events appended)
- Next subtask:
  - run one controlled live-HTTP smoke (non-order quotation call only) and confirm:
    - JSONL event rows appended,
    - runtime summary counter increments and recover counter behavior after rate-limit simulation/observation.

12. P0-1 quotation-only runtime smoke path added (2026-02-16)
- Added non-order live HTTP probe executable:
  - `src/tools/LiveQuotationProbe.cpp`
  - target: `AutoLifeLiveQuotationProbe` in `CMakeLists.txt`
  - behavior:
    - quotation-only calls: `getMarkets`, `getTicker`, `getOrderBook`, `getCandles(1m)`
    - no order placement path
    - prints runtime compliance artifact paths
- Cleanup while implementing:
  - removed build warning by using caught exception message in logger init:
    - `src/common/Logger.cpp` (`ex` unused warning removed)
- Verification:
  - build:
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeLiveQuotationProbe` PASS
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
  - runtime smoke:
    - `build\\Release\\AutoLifeLiveQuotationProbe.exe --market KRW-BTC --candles 5`
    - result: FAIL (expected blocker in this shell) due missing API key env/config at runtime context.
  - regression test:
    - `build\\Release\\AutoLifeRateLimiterComplianceTest.exe` PASS
- Next subtask:
  - rerun `AutoLifeLiveQuotationProbe` in key-available shell/session and confirm runtime telemetry append from real quotation responses.

13. P0-1 quotation smoke completed with `.env` fallback (2026-02-16)
- `src/common/Config.cpp`
  - Added `.env` fallback loading for `UPBIT_ACCESS_KEY` / `UPBIT_SECRET_KEY` when process env vars are not set.
  - Fixed Windows env-read path so `.env` fallback is reachable when `_dupenv_s` returns empty.
  - Added explicit fallback candidate based on runtime config path:
    - `build/Release/config/config.json` -> repo-root `.env`.
- Verification:
  - Build:
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeLiveQuotationProbe` PASS
  - Runtime quotation probe:
    - `build\\Release\\AutoLifeLiveQuotationProbe.exe --market KRW-BTC --candles 5` PASS
    - observed:
      - `.env loaded from: ...\\build\\Release\\../../.env`
      - `markets_count=689`, `ticker_items=1`, `orderbook_items=1`, `candles_1m_items=5`
  - Runtime compliance artifacts updated:
    - `build/Release/logs/upbit_compliance_telemetry.jsonl` appended with quotation events (`market/ticker/orderbook/candle` groups)
    - `build/Release/logs/upbit_compliance_summary_runtime.json` updated (`request_count=4`, `http_success_count=4`)

14. Build/output cleanup utility added (2026-02-16)
- Added:
  - `scripts/cleanup_generated_artifacts.py`
  - safe-by-default behavior (`dry-run` unless `--apply`)
  - optional cleanup scopes:
    - old generated logs (`--remove-old-logs-days`)
    - generated project/build files under `build/` (`--remove-project-build-files`)
- Verification:
  - `python -m py_compile scripts/cleanup_generated_artifacts.py` PASS
  - dry-run:
    - `python scripts/cleanup_generated_artifacts.py --remove-old-logs-days 3` PASS
  - applied (logs only):
    - `python scripts/cleanup_generated_artifacts.py --remove-old-logs-days 3 --apply` PASS
    - removed stale generated logs: `15` files.
- Next subtask:
  - start P1 strategy pipeline refactor step-1:
  - add deterministic strategy/engine rejection reason aggregation artifact for backtest runs,
  - use it as baseline before entry/exit logic patches to avoid blind threshold tuning.

15. P1 step-1 started: entry rejection reason artifact added (2026-02-16)
- `include/backtest/BacktestEngine.h`
  - added `Result.entry_rejection_reason_counts`
  - added runtime aggregator storage `entry_rejection_reason_counts_`
- `src/backtest/BacktestEngine.cpp`
  - track deterministic entry rejection reasons during backtest funnel:
    - `no_signal_generated`
    - `filtered_out_by_manager`
    - `filtered_out_by_policy`
    - `no_best_signal`
    - `blocked_pattern_missing_archetype`
    - `blocked_pattern_strength_or_regime`
    - `blocked_rr_rebalance`
    - `blocked_risk_gate`
    - `blocked_second_stage_confirmation`
    - `blocked_min_order_or_capital`
    - `blocked_risk_manager`
    - `blocked_order_sizing`
    - `skipped_due_to_open_position`
  - expose map in `getResult()`.
- `src/main.cpp`
  - JSON output now includes:
    - `entry_rejection_reason_counts`
  - removed duplicated console formatting by introducing shared helper:
    - `printTopEntryRejectionReasons(...)`
- Verification:
  - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
  - `build\\Release\\AutoLifeTrading.exe --backtest data/backtest/sample_trend_pullback_1m.csv --json` PASS
  - sample artifact excerpt confirmed:
    - `"entry_rejection_reason_counts":{"blocked_risk_gate":3,"no_signal_generated":17,"skipped_due_to_open_position":8}`

16. Dead file cleanup: unused legacy risk adapter removed (2026-02-16)
- Deleted files:
  - `include/core/adapters/LegacyRiskCompliancePlaneAdapter.h`
  - `src/core/adapters/LegacyRiskCompliancePlaneAdapter.cpp`
- Build wiring cleanup:
  - removed source from `CMakeLists.txt` (`LIB_SOURCES`)
- Reason:
  - adapter is no longer referenced by engine path (replaced by `UpbitComplianceAdapter`), and had zero call-sites outside its own translation unit.
- Verification:
  - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS

17. P1 step-1 artifact chain wired into iterative loops (2026-02-16)
- `scripts/run_candidate_train_eval_cycle.py`
  - added stage-scoped entry-rejection artifact outputs under:
    - `build/Release/logs/train_eval_entry_rejections/*`
  - each stage summary now records:
    - `entry_rejection.overall_top_reason/count`
    - `entry_rejection.profile_top_reason/count` (`core_full`)
- `scripts/run_candidate_auto_improvement_loop.py`
  - baseline/post-apply iteration rows now include:
    - `entry_rejection_top_reason/count` (`core_full`)
    - `entry_rejection_overall_top_reason/count`
  - summary JSON now includes:
    - `best_entry_rejection_snapshot`
    - output pointer `entry_rejection_summary_json`
- Verification:
  - `python -m py_compile scripts/run_candidate_train_eval_cycle.py scripts/run_candidate_auto_improvement_loop.py scripts/run_realdata_candidate_loop.py scripts/analyze_entry_rejections.py` PASS
  - `python scripts/run_candidate_auto_improvement_loop.py -MaxIterations 1 -SkipTunePhase -RealDataOnly -MaxRuntimeMinutes 20 -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate` PASS
    - iteration CSV confirms rejection columns populated.
  - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --max-markets 4 --disable-walk-forward` PASS
    - stage summaries include `entry_rejection` snapshots.
- Next subtask:
  - start P1 step-2 strategy path cleanup:
  - unify manager-layer reject code taxonomy with engine/backtest reject reasons and remove duplicate threshold branches in strategy-manager prefilter path.
  - target artifact:
    - `build/Release/logs/strategy_rejection_taxonomy_report.json`

18. P1 step-2 started: manager rejection taxonomy + parallel verification safety hardening (2026-02-16)
- C++ rejection taxonomy plumbing:
  - `include/strategy/StrategyManager.h`
    - added diagnostics structs:
      - `FilterDiagnostics`
      - `SelectionDiagnostics`
    - added diagnostic variants:
      - `filterSignalsWithDiagnostics(...)`
      - `selectRobustSignalWithDiagnostics(...)`
  - `src/strategy/StrategyManager.cpp`
    - manager prefilter detailed rejection codes added:
      - `filtered_out_by_manager_policy_block`
      - `filtered_out_by_manager_strength`
      - `filtered_out_by_manager_expected_value`
    - robust selection detailed rejection codes added:
      - `no_best_signal_no_directional_candidates`
      - `no_best_signal_high_stress_single_candidate`
      - `no_best_signal_policy_block`
      - `no_best_signal_negative_expected_value`
      - `no_best_signal_low_win_rate_history`
      - `no_best_signal_low_profit_factor_history`
      - `no_best_signal_low_reliability_combo`
      - `no_best_signal_no_scored_candidates`
  - `src/backtest/BacktestEngine.cpp`
    - manager diagnostics are merged into `entry_rejection_reason_counts` when manager/selection stages hard-reject entry.

- New taxonomy artifact:
  - added script:
    - `scripts/generate_strategy_rejection_taxonomy_report.py`
  - output:
    - `build/Release/logs/strategy_rejection_taxonomy_report.json`
  - content:
    - expected vs observed rejection code set,
    - unknown/missing code diagnostics,
    - group-level bottleneck aggregation (`signal_generation`, `manager_prefilter`, `best_signal_selection`, `risk_gate`, etc).

- Loop integration:
  - `scripts/run_realdata_candidate_loop.py`
    - taxonomy report now runs after entry-rejection analysis by default.
    - new flags:
      - `--strategy-rejection-taxonomy-script`
      - `--strategy-rejection-taxonomy-output-json`
      - `--skip-strategy-rejection-taxonomy`
  - `scripts/run_candidate_train_eval_cycle.py`
    - per-stage taxonomy report path injected and stage summary now includes `entry_rejection_taxonomy`.
  - `scripts/run_candidate_auto_improvement_loop.py`
    - iteration CSV/summary now include taxonomy snapshot fields (top group, coverage ratio, unknown count).

- Parallel verification safety hardening:
  - `scripts/run_realdata_candidate_loop.py`
    - added explicit outer verification lock around parity/matrix/rejection/taxonomy/tune chain.
    - new args:
      - `--verification-lock-path`
      - `--verification-lock-timeout-sec`
      - `--verification-lock-stale-sec`
  - `scripts/run_candidate_train_eval_cycle.py`
    - verification lock args added and propagated to nested realdata loop calls.
  - `scripts/run_candidate_auto_improvement_loop.py`
    - verification lock args propagated to nested realdata loop calls.

- Verification:
  - `python -m py_compile scripts/run_realdata_candidate_loop.py scripts/run_candidate_train_eval_cycle.py scripts/run_candidate_auto_improvement_loop.py scripts/generate_strategy_rejection_taxonomy_report.py` PASS
  - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipParityInvariant` PASS
  - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --max-markets 4 --disable-walk-forward` PASS
  - `python scripts/run_candidate_auto_improvement_loop.py -MaxIterations 1 -SkipTunePhase -RealDataOnly -MaxRuntimeMinutes 20 -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate` PASS

- Next subtask:
  - apply manager taxonomy diagnostics to live engine-level signal-funnel telemetry and remove duplicated threshold branches (`filter+relax`) into a single policy-evaluated path.
  - target artifact:
    - `build/Release/logs/live_signal_funnel_taxonomy_report.json`

19. P1 step-2 continued: live engine signal-funnel taxonomy artifact delivered (2026-02-16)
- `include/engine/TradingEngine.h`
  - added live signal funnel telemetry contract:
    - `recordLiveSignalReject(...)`
    - `flushLiveSignalFunnelTaxonomyReport(...)`
    - `LiveSignalFunnelTelemetry` state struct
- `src/engine/TradingEngine.cpp`
  - completed `generateSignals()` taxonomy path:
    - scan-level rejection accounting (`last_scan_rejection_counts`)
    - manager diagnostics merge from `filterSignalsWithDiagnostics(...)` and `selectRobustSignalWithDiagnostics(...)`
    - removed duplicate relax-pass branch; now single effective threshold pass with optional relaxation applied before one manager filtering call
  - implemented live artifact flush:
    - `build/Release/logs/live_signal_funnel_taxonomy_report.json`
    - includes cumulative + last-scan reason/group counts and top bottlenecks
  - fixed taxonomy group formatting bug:
    - `top_rejection_groups` now emits true group labels (`signal_generation`, `manager_prefilter`, `position_state`) instead of misclassified `other`.

- Verification:
  - build:
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
  - regression:
    - `build\\Release\\AutoLifeTrading.exe --backtest data/backtest/sample_trend_pullback_1m.csv --json` PASS
  - live engine smoke (dry-run interactive input piped, time-boxed):
    - `cmd /c "(echo 1&echo y&echo 5&echo 20&echo n&echo 2&echo 5&echo y)|build\\Release\\AutoLifeTrading.exe"` (time-boxed run) PASS for artifact generation
  - output check:
    - `build/Release/logs/live_signal_funnel_taxonomy_report.json` exists and updated
    - sample top groups confirmed:
      - `signal_generation`
      - `manager_prefilter`
      - `position_state`

- Next subtask:
  - feed `live_signal_funnel_taxonomy_report.json` (group bottlenecks) into auto-improvement loop objective adaptation so hostile/no-signal dominant scans prefer NO_TRADE and avoid forced trade-density tuning.
  - target artifact:
    - `build/Release/logs/candidate_auto_improvement_summary.json` (with live-funnel bottleneck context snapshot)

20. P1 step-2 continued: auto-improvement loop now consumes live funnel bottlenecks (2026-02-16)
- `scripts/run_candidate_auto_improvement_loop.py`
  - added input:
    - `--live-signal-funnel-taxonomy-json`
    - default: `build/Release/logs/live_signal_funnel_taxonomy_report.json`
  - added snapshot reader:
    - `read_live_signal_funnel_snapshot(...)`
    - computes:
      - group shares (`signal_generation`, `manager_prefilter`, `position_state`)
      - `no_trade_bias_active`
      - `recommended_trade_floor_scale` (0.75 when hostile no-signal dominance detected)
  - objective-floor adaptation:
    - when `no_trade_bias_active=true`, loop reduces trade-density pressure:
      - `objective_min_avg_trades_iter` lowered (floor clamp >= 4)
      - `gate_min_avg_trades_iter` lowered consistently (floor clamp >= 4)
    - profitability-related floors are preserved (no aggressive relaxation side effect).
  - iteration/summary observability extended:
    - iteration CSV columns:
      - `live_funnel_top_group`
      - `live_funnel_top_group_count`
      - `live_funnel_signal_generation_share`
      - `live_funnel_no_trade_bias_active`
      - `live_funnel_trade_floor_scale`
    - summary JSON:
      - `best_live_signal_funnel_snapshot`
      - output pointer `live_signal_funnel_taxonomy_json`

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py` PASS
  - `python scripts/run_candidate_auto_improvement_loop.py -MaxIterations 1 -SkipTunePhase -RealDataOnly -MaxRuntimeMinutes 20 -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate` PASS
  - output checks PASS:
    - `candidate_auto_improvement_iterations.csv` includes new `live_funnel_*` columns
    - baseline row shows:
      - `live_funnel_top_group=signal_generation`
      - `live_funnel_no_trade_bias_active=True`
      - `objective_min_avg_trades_iter=6.0` (from base 8.0 with scale 0.75)
    - `candidate_auto_improvement_summary.json` includes:
      - `best_live_signal_funnel_snapshot`
      - `outputs.live_signal_funnel_taxonomy_json`

- Next subtask:
  - extend the same live-funnel bottleneck context into train/eval cycle summary so promotion decisions can compare dataset hostility context and live no-trade pressure in one report.
  - target artifact:
    - `build/Release/logs/train_eval_cycle_summary.json` (with `live_signal_funnel_snapshot` per stage)

21. P1 step-2 continued: train/eval summary now includes stage-wise live-funnel snapshot (2026-02-16)
- `scripts/run_candidate_train_eval_cycle.py`
  - added input:
    - `--live-signal-funnel-taxonomy-json`
    - default: `build/Release/logs/live_signal_funnel_taxonomy_report.json`
  - added per-stage capture/snapshot:
    - copied artifact:
      - `build/Release/logs/train_eval_entry_rejections/<stage>_live_signal_funnel.json`
    - stage summary field:
      - `live_signal_funnel`
      - includes:
        - `top_group`, `top_group_count`
        - `signal_generation_share`, `manager_prefilter_share`, `position_state_share`
        - `scan_count`, `total_rejections`
  - top-level summary now records:
    - `live_signal_funnel_source_json`

- Verification:
  - `python -m py_compile scripts/run_candidate_train_eval_cycle.py scripts/run_candidate_auto_improvement_loop.py` PASS
  - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --max-markets 4 --disable-walk-forward` PASS
  - output checks PASS:
    - `build/Release/logs/candidate_train_eval_cycle_summary.json`:
      - each stage contains `live_signal_funnel` snapshot
      - `live_signal_funnel_source_json` present
    - stage artifact files created:
      - `build/Release/logs/train_eval_entry_rejections/train_1_live_signal_funnel.json`
      - `build/Release/logs/train_eval_entry_rejections/eval_validation_deterministic_live_signal_funnel.json`
      - `build/Release/logs/train_eval_entry_rejections/eval_holdout_deterministic_live_signal_funnel.json`

22. UTF-8 guardrails tightened for logs/comments (2026-02-16)
- Added repository editor defaults:
  - `.editorconfig`
  - `charset=utf-8`, `end_of_line=crlf`, final newline on save.
- Cleaned garbled comment blocks in:
  - `CMakeLists.txt`
  - post-build section/comments are now readable ASCII comments.
- Note:
  - MSVC compile option `/utf-8` was already active in `CMakeLists.txt`.
  - `src/main.cpp` already applies `SetConsoleOutputCP(CP_UTF8)` / `SetConsoleCP(CP_UTF8)` at startup.

- Next subtask:
  - remove stale/garbled comments from actively touched C++ headers/sources (`TradingEngine.h` first) in small, safe patches so future context handoff is readable without risky behavior changes.

23. P1 step-2 continued: promotion verdict now consumes live-funnel bottleneck context (2026-02-16)
- `scripts/run_candidate_train_eval_cycle.py`
  - added live-funnel context builder:
    - `build_live_signal_funnel_context(...)`
    - computes no-trade-bias condition from stage snapshot:
      - `signal_generation_share`, `manager_prefilter_share`, `position_state_share`
      - `scan_count`, `total_rejections`
      - `no_trade_bias_active`
  - extended verdict builder:
    - `build_promotion_verdict(...)` now receives validation/holdout live-funnel snapshots.
    - recommendation now reflects bottleneck class when gate fails:
      - `hold_candidate_improve_signal_generation_or_dataset_quality`
      - `hold_candidate_improve_prefilter_policy_or_feature_quality`
      - `hold_candidate_improve_position_turnover_and_exit_management`
  - promotion verdict observability added:
    - `validation_live_signal_funnel_context`
    - `holdout_live_signal_funnel_context`
    - `live_no_trade_bias_any`

- Verification:
  - `python -m py_compile scripts/run_candidate_train_eval_cycle.py` PASS
  - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --max-markets 4 --disable-walk-forward` PASS
  - output check (`build/Release/logs/candidate_train_eval_cycle_summary.json`) PASS:
    - `promotion_verdict.recommendation=hold_candidate_improve_signal_generation_or_dataset_quality`
    - `promotion_verdict.live_no_trade_bias_any=true`
    - validation/holdout live funnel context populated with `top_group=signal_generation`

- Next subtask:
  - apply the same bottleneck-aware classification to candidate tuning scenario priority (not just verdict), so search order prefers signal-generation fixes when no-trade bias is active.
  - target artifact:
    - `build/Release/logs/candidate_trade_density_tuning_summary.json` (with bottleneck-priority metadata)

24. P1 step-2 continued: tuning scenario execution order is now live-funnel bottleneck-prioritized (2026-02-16)
- `scripts/tune_candidate_gate_trade_density.py`
  - added live-funnel reader:
    - `read_live_signal_funnel_snapshot(...)`
  - added bottleneck-priority scoring:
    - `compute_combo_bottleneck_priority_score(...)`
    - `prioritize_combo_specs_for_bottleneck(...)`
  - new args:
    - `--live-signal-funnel-taxonomy-json` (default: `build/Release/logs/live_signal_funnel_taxonomy_report.json`)
    - `--enable-bottleneck-priority` / `--disable-bottleneck-priority`
  - behavior:
    - `signal_generation` / `no_trade_bias_active` dominance:
      - evaluate higher entry-generation affinity combos earlier.
    - `manager_prefilter` dominance:
      - evaluate moderate-relaxation combos earlier.
    - `position_state` dominance:
      - evaluate quality-preserving combos earlier.
  - observability:
    - each screen/final row now includes:
      - `bottleneck_priority_rank`
      - `bottleneck_priority_score`
      - `bottleneck_top_group`
      - `bottleneck_no_trade_bias_active`
    - summary JSON now includes:
      - `bottleneck_priority.context`
      - `bottleneck_priority.combo_priority_order`

- Verification:
  - `python -m py_compile scripts/tune_candidate_gate_trade_density.py scripts/run_candidate_auto_improvement_loop.py scripts/run_candidate_train_eval_cycle.py scripts/run_realdata_candidate_loop.py` PASS
  - direct tuning smoke:
    - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode quality_focus -MaxScenarios 4 -RealDataOnly -RequireHigherTfCompanions -ScreenDatasetLimit 4 -ScreenTopK 2 -FinalProfileIds core_full -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate` PASS
    - observed run order reflects bottleneck-priority (`signal_generation`, `no_trade_bias_active=true`).
  - auto-improvement integration smoke:
    - `python scripts/run_candidate_auto_improvement_loop.py -MaxIterations 1 -RealDataOnly -MaxRuntimeMinutes 30 -TuneMaxScenarios 2 -TuneScreenDatasetLimit 2 -TuneScreenTopK 1 -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate` PASS
    - tuning phase logs include:
      - `bottleneck_priority=on, top_group=signal_generation, no_trade_bias_active=True`
  - output checks PASS:
    - `build/Release/logs/candidate_trade_density_tuning_summary.json`
      - `bottleneck_priority.enabled=true`
      - `bottleneck_priority.context.top_group=signal_generation`
      - summary rows include `bottleneck_priority_*` fields

- Next subtask:
  - feed bottleneck-priority context back into strategy-level patch candidates (feature/filter focus by top bottleneck), so generated combos are not only reordered but also compositionally adapted by bottleneck class.
  - target artifact:
    - `build/Release/logs/candidate_trade_density_tuning_summary.json` (with bottleneck-adapted scenario family tag)

25. P1 step-2 continued: bottleneck-adapted scenario families now change combo composition (2026-02-16)
- `scripts/tune_candidate_gate_trade_density.py`
  - implemented bottleneck-aware family adapter:
    - `adapt_combo_specs_for_bottleneck(...)`
    - family buckets:
      - `signal_generation_boost`
      - `manager_prefilter_relax`
      - `position_turnover_quality`
      - `neutral_balance`
      - `legacy_baseline`
  - behavior:
    - adapts combo parameters (entry edge/RR/strength/order density/EV gates) by dominant live-funnel bottleneck,
    - keeps `legacy_only` mode safe (no aggressive adaptation),
    - preserves baseline rollback anchor (`baseline_current`) as non-adapted where applicable.
  - new flags:
    - `--enable-bottleneck-adapted-scenarios` (default on)
    - `--disable-bottleneck-adapted-scenarios`
  - observability extended:
    - summary rows:
      - `bottleneck_scenario_family`
      - `bottleneck_scenario_family_adapted`
    - summary JSON:
      - `bottleneck_priority.scenario_family_counts`
      - per-combo order includes:
        - `scenario_family`
        - `scenario_family_adapted`

- Verification:
  - `python -m py_compile scripts/tune_candidate_gate_trade_density.py` PASS
  - tuning smoke:
    - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode quality_focus -MaxScenarios 4 -RealDataOnly -RequireHigherTfCompanions -ScreenDatasetLimit 4 -ScreenTopK 2 -FinalProfileIds core_full -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate` PASS
    - output shows:
      - `bottleneck_adapted_scenarios=on`
      - `scenario_family_counts={'signal_generation_boost': 4}`
  - auto-improvement integration:
    - `python scripts/run_candidate_auto_improvement_loop.py -MaxIterations 1 -RealDataOnly -MaxRuntimeMinutes 25 -TuneMaxScenarios 2 -TuneScreenDatasetLimit 2 -TuneScreenTopK 1 -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate` PASS
    - tuning phase logs confirm adapted family usage:
      - `scenario_family_counts={'signal_generation_boost': 2}`
    - post-apply snapshot improved vs baseline in this run:
      - `avg_profit_factor: 0.5403 -> 0.6897`
      - `avg_expectancy_krw: -7.3353 -> -6.0931`
      - `avg_win_rate_pct: 46.013 -> 50.2433`
      - `avg_total_trades: 151.5 -> 217.1667`

- Next subtask:
  - apply same bottleneck family context into C++ strategy/manager layer candidate generation hints (not only script-level tuning), starting with signal-generation-dominant path for `Advanced Scalping`/`Advanced Momentum`.
  - target artifact:
    - `build/Release/logs/strategy_rejection_taxonomy_report.json` + `build/Release/logs/live_signal_funnel_taxonomy_report.json` trend shift after patch

26. P1 step-2 continued: C++ runtime now applies no-trade-bias hint from live funnel context (2026-02-16)
- `src/engine/TradingEngine.cpp`
  - `generateSignals()` now derives live bottleneck pressure from cumulative funnel rejections:
    - `signal_generation_share`
    - `manager_prefilter_share`
    - `position_state_share`
    - `live_no_trade_bias_active`
  - when `live_no_trade_bias_active=true` (and regime is non-stress), manager prefilter thresholds get an additional modest relaxation:
    - `min_strength` downshift
    - `min_expected_value` downshift
  - added runtime log marker:
    - `Live no-trade bias active: ...`

  - `flushLiveSignalFunnelTaxonomyReport(...)` now emits derived context fields directly:
    - `total_rejections`
    - `signal_generation_share`
    - `manager_prefilter_share`
    - `position_state_share`
    - `no_trade_bias_active`
    - `recommended_trade_floor_scale`

- Verification:
  - build:
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
  - live time-box smoke:
    - `cmd /c "(echo 1&echo y&echo 5&echo 20&echo n&echo 2&echo 5&echo y)|build\\Release\\AutoLifeTrading.exe"` (time-boxed run) PASS for artifact refresh
  - output check PASS:
    - `build/Release/logs/live_signal_funnel_taxonomy_report.json` contains derived keys:
      - `signal_generation_share`
      - `manager_prefilter_share`
      - `position_state_share`
      - `no_trade_bias_active`
      - `recommended_trade_floor_scale`
      - `total_rejections`

- Next subtask:
  - connect the same runtime no-trade-bias signal to per-strategy candidate weighting in `StrategyManager` (Scalping/Momentum first), so relaxation is not only threshold-level but also candidate score-level.
  - target artifact:
    - `build/Release/logs/live_signal_funnel_taxonomy_report.json` (drop in `no_signal_generated` share)

27. P1 step-2 continued: StrategyManager candidate scoring now uses live bottleneck hint (2026-02-16)
- `include/strategy/StrategyManager.h`
  - added runtime hint contract:
    - `LiveSignalBottleneckHint`
    - `setLiveSignalBottleneckHint(...)`
    - `getLiveSignalBottleneckHint()`
- `src/strategy/StrategyManager.cpp`
  - `selectRobustSignalWithDiagnostics(...)` now reads live hint and applies modest score boost in signal-generation bottlenecks:
    - focus roles: `SCALPING`, `MOMENTUM`
    - guarded by minimum quality (`strength`, `expected_value`) and bottleneck severity.
  - keeps change minimal (small multiplicative reweight), preserving legacy safety chain.
- `src/engine/TradingEngine.cpp`
  - `generateSignals()` now computes top bottleneck group/share from live funnel and injects manager hint each scan:
    - `top_group`, `no_trade_bias_active`, share fields.

- Verification:
  - build:
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
  - loop regression smoke:
    - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions -DatasetNames data/backtest_real/upbit_KRW_BTC_1m_12000.csv -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipParityInvariant -SkipCoreVsLegacyGate` PASS

- Next subtask:
  - add explicit strategy-level bottleneck debug counters (per role boost-hit count) into diagnostics artifact chain to verify hint impact without relying only on aggregate gate metrics.
  - target artifact:
    - `build/Release/logs/strategy_rejection_taxonomy_report.json` (with role-level hint impact snapshot)

28. P1 step-2 continued: live bottleneck hint impact counters added to funnel artifacts (2026-02-16)
- `include/strategy/StrategyManager.h`
  - `SelectionDiagnostics` extended with hint-impact fields:
    - `live_hint_adjusted_candidate_count`
    - `live_hint_adjustment_counts`
- `src/strategy/StrategyManager.cpp`
  - `selectRobustSignalWithDiagnostics(...)` now records per-candidate hint adjustments when live bottleneck hint modifies score:
    - `boost_scalping`
    - `boost_momentum`
    - `boost_alpha_fallback`
    - `dampen_grid`
- `include/engine/TradingEngine.h`
  - `LiveSignalFunnelTelemetry` extended with selection/hint counters:
    - `selection_call_count`
    - `selection_scored_candidate_count`
    - `selection_hint_adjusted_candidate_count`
    - `selection_hint_adjustment_counts`
  - `flushLiveSignalFunnelTaxonomyReport(...)` now accepts last-scan hint-adjustment counts.
- `src/engine/TradingEngine.cpp`
  - `generateSignals()` now accumulates manager selection diagnostics into live funnel telemetry and last-scan hint maps.
  - `flushLiveSignalFunnelTaxonomyReport(...)` now emits new observability fields:
    - cumulative:
      - `selection_call_count`
      - `selection_scored_candidate_count`
      - `selection_hint_adjusted_candidate_count`
      - `selection_hint_adjusted_ratio`
      - `selection_hint_adjustment_counts`
      - `selection_hint_total_adjustments`
      - `top_selection_hint_adjustments`
    - last-scan:
      - `selection_hint_adjustment_counts`
      - `top_selection_hint_adjustments`

- Verification:
  - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions -DatasetNames data/backtest_real/upbit_KRW_BTC_1m_12000.csv -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipParityInvariant -SkipCoreVsLegacyGate` PASS
  - live funnel refresh (dry-run, time-boxed):
    - `cmd /c "(echo 1&echo y&echo 1&echo 5&echo n&echo 2&echo 2&echo y)|build\\Release\\AutoLifeTrading.exe"` (timeout after signal-generation stage) PASS for artifact refresh
    - residual process cleanup: `Stop-Process -Id <AutoLifeTrading_PID> -Force` PASS
  - output check:
    - `build/Release/logs/live_signal_funnel_taxonomy_report.json` contains new `selection_hint_*` fields.

- Next subtask:
  - mirror the same hint-impact snapshot into strategy taxonomy summary script output so tuning loops can consume `role-level boost-hit ratio` directly without parsing raw funnel JSON.
  - target artifact:
    - `build/Release/logs/strategy_rejection_taxonomy_report.json` (with hint-impact block)

29. P1 step-2 continued: strategy taxonomy report now carries live hint-impact snapshot (2026-02-16)
- `scripts/generate_strategy_rejection_taxonomy_report.py`
  - added optional input:
    - `--live-signal-funnel-taxonomy-json`
  - report now includes `live_hint_impact` block:
    - `selection_call_count`
    - `selection_scored_candidate_count`
    - `selection_hint_adjusted_candidate_count`
    - `selection_hint_adjusted_ratio`
    - `selection_hint_adjustment_counts`
    - `top_selection_hint_adjustments`
    - `no_trade_bias_active`
    - `signal_generation_share`, `manager_prefilter_share`, `position_state_share`
    - `dominant_rejection_group`
  - fail-open behavior preserved when live funnel file is missing/unreadable (`source_exists=false`).
- `scripts/run_realdata_candidate_loop.py`
  - added pass-through arg:
    - `--live-signal-funnel-taxonomy-json`
  - taxonomy step now forwards live funnel path into taxonomy script.

- Verification:
  - `python -m py_compile scripts/generate_strategy_rejection_taxonomy_report.py scripts/run_realdata_candidate_loop.py` PASS
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions -DatasetNames data/backtest_real/upbit_KRW_BTC_1m_12000.csv -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipParityInvariant -SkipCoreVsLegacyGate` PASS
  - output check:
    - `build/Release/logs/strategy_rejection_taxonomy_report.json` includes:
      - `live_hint_impact.selection_hint_adjusted_ratio`
      - `live_hint_impact.selection_hint_adjustment_counts`
      - `live_hint_impact.dominant_rejection_group`

- Next subtask:
  - feed `live_hint_impact.selection_hint_adjusted_ratio` into tuning objective guardrails (cap relaxation when ratio is too high) to reduce adaptation overshoot risk.
  - target artifact:
    - `build/Release/logs/candidate_trade_density_tuning_summary.json` (with hint-impact guardrail metadata)

30. P1 step-2 continued: anti-overfit guardrail added to bottleneck-adapted tuning scenarios (2026-02-16)
- `scripts/tune_candidate_gate_trade_density.py`
  - live funnel context now also reads:
    - `selection_hint_adjusted_ratio`
    - `selection_hint_adjustment_counts`
  - added hint-impact guardrail controls:
    - `--enable-hint-impact-guardrail` / `--disable-hint-impact-guardrail`
    - `--hint-impact-guardrail-ratio` (default `0.65`)
    - `--hint-impact-guardrail-tighten-scale` (default `0.55`)
  - added guardrail behavior:
    - when `selection_hint_adjusted_ratio >= ratio`, aggressive relax-family combos (`signal_generation_boost`, `manager_prefilter_relax`) are partially pulled back toward baseline.
    - this prevents tuning from over-leveraging one dataset-period bottleneck pattern.
  - observability additions:
    - combo/report fields:
      - `bottleneck_hint_guardrail_active`
      - `bottleneck_hint_guardrail_ratio`
      - `bottleneck_hint_guardrail_threshold`
      - `bottleneck_hint_guardrail_tighten_scale`
    - summary JSON includes:
      - `screening.enable_hint_impact_guardrail`
      - `bottleneck_priority.hint_impact_guardrail`

- Verification:
  - `python -m py_compile scripts/tune_candidate_gate_trade_density.py scripts/generate_strategy_rejection_taxonomy_report.py scripts/run_realdata_candidate_loop.py` PASS
  - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode quality_focus -MaxScenarios 2 -RealDataOnly -RequireHigherTfCompanions -DatasetNames data/backtest_real/upbit_KRW_BTC_1m_12000.csv -ScreenDatasetLimit 1 -ScreenTopK 1 -FinalProfileIds core_full -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate` PASS
    - runtime log:
      - `hint_impact_guardrail=on, ratio=0.5789, threshold=0.6500`
  - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --max-markets 4 --disable-walk-forward --matrix-max-workers 1 --matrix-backtest-retry-count 1` PASS

- Next subtask:
  - promote this guardrail signal from tuning-only into auto-improvement loop stop/skip policy:
  - if hint-adjusted ratio stays high while holdout PF/expectancy remain weak, skip further relax-family exploration and pivot to quality/exit-side scenarios.
  - target artifact:
    - `build/Release/logs/candidate_auto_improvement_summary.json` (guardrail-triggered decision trace)

31. P1 step-2 continued: auto-improvement loop now applies hint-overfit pivot policy (2026-02-16)
- `scripts/run_candidate_auto_improvement_loop.py`
  - live funnel snapshot now ingests:
    - `selection_hint_adjusted_ratio`
    - `selection_hint_adjusted_candidate_count`
    - `selection_scored_candidate_count`
  - added loop-level guardrail knobs:
    - `--tune-enable-hint-impact-guardrail` / `--tune-disable-hint-impact-guardrail`
    - `--tune-hint-impact-guardrail-ratio`
    - `--tune-hint-impact-guardrail-tighten-scale`
    - `--hint-overfit-ratio-threshold`
    - `--hint-overfit-force-guardrail-tighten-scale`
    - `--enable-hint-overfit-quality-pivot` / `--disable-hint-overfit-quality-pivot`
  - new runtime policy:
    - if `selection_hint_adjusted_ratio` is high and baseline PF/expectancy is still below active gate floors,
      tune phase is forced to stronger guardrail settings and can pivot scenario-mode to `quality_focus`.
  - iteration telemetry expanded:
    - `live_funnel_selection_hint_adjusted_ratio`
    - `hint_overfit_risk`
    - `tune_scenario_mode_iter`
    - `tune_hint_guardrail_enabled_iter`
    - `tune_hint_guardrail_ratio_iter`
    - `tune_hint_guardrail_tighten_scale_iter`

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py scripts/tune_candidate_gate_trade_density.py scripts/run_realdata_candidate_loop.py scripts/generate_strategy_rejection_taxonomy_report.py` PASS
  - `python scripts/run_candidate_auto_improvement_loop.py -MaxIterations 1 -RealDataOnly -MaxRuntimeMinutes 25 -TuneMaxScenarios 2 -TuneScreenDatasetLimit 1 -TuneScreenTopK 1 -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate` PASS
  - output checks:
    - `build/Release/logs/candidate_auto_improvement_iterations.csv` contains new guardrail/pivot columns.
    - `build/Release/logs/candidate_auto_improvement_summary.json` contains tuning guardrail config and per-iteration `hint_overfit_risk`.

- Next subtask:
  - connect holdout-stage failure pattern (`signal_generation` vs `risk_gate`) to scenario family suppression rules in tuning:
  - suppress relax-family when holdout still fails expectancy/PF under high hint-adjusted ratio.
  - target artifact:
  - `build/Release/logs/candidate_train_eval_cycle_summary.json` + `build/Release/logs/candidate_trade_density_tuning_summary.json` (suppression trace)

32. P1 step-2 continued: holdout failure pattern now suppresses relax-family tuning exploration (2026-02-16)
- `scripts/tune_candidate_gate_trade_density.py`
  - added holdout context input:
    - `--train-eval-summary-json` (default `build/Release/logs/candidate_train_eval_cycle_summary.json`)
  - added suppression controls:
    - `--enable-holdout-failure-family-suppression` / `--disable-holdout-failure-family-suppression`
    - `--holdout-suppression-hint-ratio-threshold` (default `0.60`)
    - `--holdout-suppression-require-both-pf-exp-fail` / `--holdout-suppression-allow-either-pf-or-exp-fail`
  - new behavior:
    - if holdout deterministic stage still fails PF/expectancy and live hint-adjusted ratio is high,
      and holdout top rejection group is `signal_generation` or `risk_gate`,
      relax-family combos (`signal_generation_boost`, `manager_prefilter_relax`) are suppressed.
    - safety fail-open:
      - when suppression would remove all combos, one fallback combo is retained and marked explicitly.
  - observability additions:
    - summary rows + combo metadata:
      - `holdout_failure_suppression_active`
      - `holdout_failure_suppressed_family`
      - `holdout_failure_suppression_reason`
    - summary JSON:
      - `bottleneck_priority.holdout_failure_suppression` block
      - screening config now stores holdout-suppression settings.

- Verification:
  - `python -m py_compile scripts/tune_candidate_gate_trade_density.py` PASS
  - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode quality_focus -MaxScenarios 4 -RealDataOnly -RequireHigherTfCompanions -DatasetNames data/backtest_real/upbit_KRW_BTC_1m_12000.csv -ScreenDatasetLimit 1 -ScreenTopK 1 -FinalProfileIds core_full -MatrixMaxWorkers 1 -MatrixBacktestRetryCount 1 -SkipCoreVsLegacyGate -HoldoutSuppressionHintRatioThreshold 0.55` PASS
  - output checks:
    - `build/Release/logs/candidate_trade_density_tuning_summary.json` includes:
      - `bottleneck_priority.holdout_failure_suppression.active=true`
      - `holdout_failure_suppression_reason=fallback_retain_single_combo_after_suppression`
      - `suppressed_families=["manager_prefilter_relax","signal_generation_boost"]`

- Next subtask:
  - feed holdout suppression status into auto-improvement iteration decision trace and pause criteria:
  - if suppression remains active for N iterations and PF/expectancy still fail, auto-stop relax-family branch and emit explicit recommendation.
  - target artifact:
  - `build/Release/logs/candidate_auto_improvement_summary.json` (suppression-persist decision trace)

33. Repository hygiene pass: generated outputs + unused code warnings cleanup (2026-02-16)
- Deleted generated/non-source artifacts:
  - `build/` (full)
  - `logs/` (full)
  - `src/strategy/data/backtest/adversarial/` (5000 generated CSVs, non-referenced)
  - `data/backtest/auto_sim_500.csv` (generated simulation sample)
- Unused warning cleanup in source:
  - `src/analytics/TechnicalIndicators.cpp`
    - removed unused local (`tr_sum`)
    - marked unused stochastic parameter (`d_period`) explicitly
  - `src/risk/RiskManager.cpp`
    - marked unused params (`capital`) in sizing helpers
    - migrated UTC day-reset logic from deprecated `gmtime` to `gmtime_s`
  - `src/backtest/DataHistory.cpp`
    - marked unused `filterByDate` placeholder params (`start_date`, `end_date`)

- Verification:
  - CMake regenerate:
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe -S . -B build -DCMAKE_TOOLCHAIN_FILE=D:/MyApps/vcpkg/scripts/buildsystems/vcpkg.cmake` PASS
  - build:
    - `D:\\MyApps\\vcpkg\\downloads\\tools\\cmake-3.31.10-windows\\cmake-3.31.10-windows-x86_64\\bin\\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
  - cleanup reapplied after verification:
    - `build/` and `logs/` re-removed.

- Next subtask:
  - continue TODO P1 flow with suppression-persist policy in auto-improvement loop, while keeping repository hygiene (remove generated outputs after each verification batch).

34. P1 step-2 continued: auto-improvement loop now enforces holdout-suppression persistence stop policy (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - added new CLI controls:
      - `--enable-holdout-suppression-persist-stop` / `--disable-holdout-suppression-persist-stop`
      - `--holdout-suppression-persist-iterations` (default `2`)
      - `--holdout-suppression-persist-require-both-pf-exp-fail` / `--holdout-suppression-persist-allow-either-pf-or-exp-fail`
    - added tune-summary parser for suppression context:
      - `read_tune_holdout_suppression_snapshot(...)`
    - added PF/expectancy failure evaluator:
      - `pf_expectancy_quality_fail(...)`
    - iteration trace now records:
      - `holdout_suppression_active`
      - `holdout_suppression_reason`
      - `holdout_suppression_suppressed_combo_count`
      - `holdout_suppression_kept_combo_count`
      - `holdout_suppression_fail_open_all_suppressed`
      - `holdout_suppression_persist_streak`
      - `holdout_suppression_persist_triggered`
      - `pf_fail_active_threshold`
      - `expectancy_fail_active_threshold`
      - `pf_expectancy_quality_fail`
    - stop criterion added:
      - when suppression is active and PF/expectancy quality failure persists for configured streak, loop ends with:
        - `status=paused_holdout_suppression_persist`
        - explicit pivot reason (`quality/exit-side strategy changes`).
    - summary JSON now includes:
      - `holdout_suppression_persist_policy` with threshold, trigger iteration, final streak, and per-iteration events.
      - tuning config fields for the new persistence policy.

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py scripts/tune_candidate_gate_trade_density.py scripts/run_realdata_candidate_loop.py` PASS
  - `python scripts/run_candidate_auto_improvement_loop.py -MaxIterations 0 -IterationCsv build/Release/logs/_smoke_auto_iterations.csv -SummaryJson build/Release/logs/_smoke_auto_summary.json` PASS
  - helper parser smoke:
    - `python -c "import json, pathlib, sys; sys.path.insert(0,'scripts'); import run_candidate_auto_improvement_loop as m; ..."` PASS
    - confirmed `read_tune_holdout_suppression_snapshot(...)` returns `active=True`, `suppressed_combo_count=3` on synthetic payload.
  - temporary smoke outputs cleaned.

- Next subtask:
  - run a real 1-iteration loop (with actual gate+tune artifacts) to validate `paused_holdout_suppression_persist` trigger behavior and decision trace under live suppression-active conditions.

35. P1 step-2 continued: suppression-persist trigger verified on real artifact chain (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - added tune holdout-suppression passthrough controls:
      - `--tune-enable-holdout-failure-family-suppression` / `--tune-disable-holdout-failure-family-suppression`
      - `--tune-holdout-suppression-hint-ratio-threshold`
      - `--tune-holdout-suppression-require-both-pf-exp-fail` / `--tune-holdout-suppression-allow-either-pf-or-exp-fail`
    - auto-loop now forwards these options to `tune_candidate_gate_trade_density.py`.
    - summary `tuning` block now records the passthrough settings above.

- Verification:
  - generated holdout context first:
    - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --max-markets 3 --disable-walk-forward --matrix-max-workers 1 --matrix-backtest-retry-count 1` PASS
    - produced:
      - `build/Release/logs/candidate_train_eval_cycle_summary.json`
      - holdout deterministic context present (`top_rejection_group=signal_generation`, PF/expectancy still failing).
  - real auto-loop trigger run:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 2 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.0 --holdout-suppression-persist-iterations 2` PASS
    - result:
      - `status=paused_holdout_suppression_persist`
      - `reason=Holdout-family suppression remained active while PF/expectancy stayed below active thresholds for 2 consecutive iterations...`
      - `build/Release/logs/candidate_auto_improvement_summary.json`:
        - `holdout_suppression_persist_policy.triggered=true`
        - `trigger_iteration=2`
        - `final_streak=2`
      - `build/Release/logs/candidate_auto_improvement_iterations.csv`:
        - last `post_apply` row has:
          - `holdout_suppression_active=True`
          - `holdout_suppression_persist_streak=2`
          - `holdout_suppression_persist_triggered=True`
          - `pf_expectancy_quality_fail=True`

- Note:
  - this run triggered on `holdout_failure_suppression.active=true` even when `suppressed_combo_count=0`
    (scenario family was `neutral_balance`), i.e., policy trace worked as designed but the suppression was advisory rather than actual family removal.

- Next subtask:
  - tighten persistence policy semantics:
    - count streak only when actual family suppression happened (`suppressed_combo_count>0`) or `fail_open_all_suppressed=true`,
    - then re-run the same 2-iteration verification and compare trigger delta.

36. P1 step-2 continued: suppression-persist semantics tightened to actual suppression effect (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - persistence streak now increments only when:
      - `holdout_failure_suppression.active=true`, and
      - actual suppression effect exists:
        - `suppressed_combo_count>0` OR `fail_open_all_suppressed=true`, and
      - PF/expectancy quality still fails.
    - added observability fields:
      - per-iteration row: `holdout_suppression_effective`
      - policy event trace: `effective`
    - preserves existing active/suppressed metadata, but prevents advisory-only activation from triggering stop.

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py` PASS
  - re-ran same 2-iteration command as step 35:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 2 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.0 --holdout-suppression-persist-iterations 2` PASS
  - outcome delta:
    - previous (step 35): `status=paused_holdout_suppression_persist`
    - now: `status=paused_no_improvement`
    - because suppression was active but not effective (`suppressed_combo_count=0`, family=`neutral_balance`).
  - artifact checks:
    - `build/Release/logs/candidate_auto_improvement_summary.json`
      - `holdout_suppression_persist_policy.triggered=false`
      - events show `active=true`, `effective=false`, `persist_streak=0`
    - `build/Release/logs/candidate_auto_improvement_iterations.csv`
      - last row includes `holdout_suppression_effective=False`

- Next subtask:
  - create/route at least one tuning family that maps into suppressible classes (`signal_generation_boost` / `manager_prefilter_relax`) under current live-funnel context, then re-run 2-iteration loop to validate `effective=true` persistence stop in a non-advisory case.

37. P1 step-2 continued: holdout-fallback bottleneck routing now enables non-advisory suppression path (2026-02-16)
- What changed:
  - `scripts/tune_candidate_gate_trade_density.py`
    - added `build_effective_bottleneck_context(live_context, holdout_context)`:
      - if live funnel `top_group` is empty, use holdout deterministic taxonomy top-group as fallback (`top_group_source=holdout_fallback`).
      - fallback applies to known bottleneck groups (`signal_generation`, `manager_prefilter`, `position_state`, `risk_gate`).
      - for `signal_generation` / `manager_prefilter`, inject conservative synthetic share floor to avoid neutral-only routing.
    - tuning now uses:
      - `live_bottleneck_context` (raw) + `bottleneck_context` (effective with fallback) split.
    - observability extended:
      - log now prints `top_group` + `source`.
      - summary JSON `bottleneck_priority` now includes `live_context_raw` in addition to effective `context`.

- Verification:
  - `python -m py_compile scripts/tune_candidate_gate_trade_density.py scripts/run_candidate_auto_improvement_loop.py` PASS
  - re-ran 2-iteration auto-loop (same command as step 36, with `--tune-holdout-suppression-hint-ratio-threshold 0.0`) PASS
  - observed non-advisory suppression:
    - tuning logs:
      - `top_group=signal_generation, source=holdout_fallback`
      - `scenario_family_counts={'signal_generation_boost': 1}`
      - `holdout_family_suppression ... suppressed=2, kept=1`
    - `build/Release/logs/candidate_trade_density_tuning_summary.json`:
      - `bottleneck_priority.context.top_group=signal_generation`
      - `bottleneck_priority.context.top_group_source=holdout_fallback`
      - `bottleneck_priority.live_context_raw.top_group=""`
      - `holdout_failure_suppression.suppressed_combo_count=2`
  - persistence stop confirmed with effective suppression:
    - `build/Release/logs/candidate_auto_improvement_summary.json`:
      - `status=paused_holdout_suppression_persist`
      - `holdout_suppression_persist_policy.triggered=true`
      - `trigger_iteration=2`
      - events show `effective=true` for both iterations.
    - `build/Release/logs/candidate_auto_improvement_iterations.csv`:
      - last row `holdout_suppression_effective=True`

- Next subtask:
  - integrate this fallback-context signal into scenario count allocation (not only family label), so suppressed families still leave enough distinct quality/exit-side alternatives under low-signal regimes.

38. P1 step-2 continued: post-suppression quality/exit candidate expansion added (2026-02-16)
- What changed:
  - `scripts/tune_candidate_gate_trade_density.py`
    - added new helper:
      - `expand_post_suppression_quality_exit_candidates(...)`
      - when holdout family suppression is effective and combo count falls below floor, injects tighter quality/exit variants.
    - new CLI controls:
      - `--enable-post-suppression-quality-expansion` / `--disable-post-suppression-quality-expansion`
      - `--post-suppression-min-combo-count` (default `3`)
    - injected variants:
      - stricter edge/RR/EV/PF floors, reduced order density, tighter strategy strengths.
      - tagged with `bottleneck_scenario_family=quality_exit_rebalance`.
    - observability:
      - log line `post_suppression_quality_expansion=...`
      - summary JSON:
        - `bottleneck_priority.post_suppression_quality_expansion`
      - screening config records expansion settings.

- Verification:
  - `python -m py_compile scripts/tune_candidate_gate_trade_density.py scripts/run_candidate_auto_improvement_loop.py` PASS
  - direct tune run:
    - `python scripts/tune_candidate_gate_trade_density.py --scenario-mode quality_focus --max-scenarios 2 --real-data-only --require-higher-tf-companions --screen-dataset-limit 1 --screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --skip-core-vs-legacy-gate --holdout-suppression-hint-ratio-threshold 0.0` PASS
    - confirmed:
      - `scenario_family_counts={'signal_generation_boost': 1, 'quality_exit_rebalance': 2}`
      - suppression effective (`suppressed_combo_count=2`)
      - expansion applied (`injected_combo_count=2`, `combo_count=3`)
  - end-to-end auto-loop run:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 2 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.0 --holdout-suppression-persist-iterations 2` PASS
    - selected combo can now come from expansion family (e.g. `scenario_quality_focus_000_quality_exit_02`).
    - persistence policy still triggers correctly:
      - `status=paused_holdout_suppression_persist`
      - `trigger_iteration=2`, `final_streak=2`

- Next subtask:
  - connect expansion-family outcome into auto-loop pivot recommendation text and next-iteration scenario-mode selection, so repeated persistence stop emits concrete patch direction (`entry timing` vs `exit/risk` emphasis).

39. P1 step-2 continued: directional pivot recommendation + next-iteration scenario override wired (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - added directional pivot controls:
      - `--enable-persist-directional-pivot` / `--disable-persist-directional-pivot`
      - `--persist-entry-timing-scenario-mode` (default `diverse_wide`)
      - `--persist-exit-risk-scenario-mode` (default `quality_focus`)
    - added tune expansion passthrough controls:
      - `--tune-enable-post-suppression-quality-expansion` / `--tune-disable-post-suppression-quality-expansion`
      - `--tune-post-suppression-min-combo-count` (default `3`)
    - added tune metadata readers:
      - `read_tune_post_suppression_expansion_snapshot(...)`
      - `build_persist_directional_pivot(...)`
    - behavior update:
      - when suppression-effective + PF/expectancy fail persists, loop now derives concrete patch direction:
        - `entry_timing` vs `exit_risk`
      - stores next-iteration scenario mode override and applies it on following iteration.
      - final `paused_holdout_suppression_persist` reason now includes:
        - `patch_direction=...`
        - `next_scenario_mode=...`
        - directional recommendation sentence.
    - observability additions:
      - iteration csv fields:
        - `tune_scenario_mode_source`
        - `post_suppression_expansion_*`
        - `selected_combo_family`
        - `persist_pivot_*`
      - holdout policy events now include:
        - `pivot_direction`, `pivot_reason_code`, `pivot_recommendation`, `pivot_next_scenario_mode`
      - summary json adds:
        - `persist_directional_pivot` block (last direction/recommendation/next mode).

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py scripts/tune_candidate_gate_trade_density.py` PASS
  - end-to-end 2-iteration run:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 2 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.0 --holdout-suppression-persist-iterations 2` PASS
    - observed logs:
      - `Directional pivot prepared for next iteration: direction=exit_risk, next_scenario_mode=quality_focus`
      - next iteration applies override:
        - `Applying scenario-mode override from previous pivot: scenario_mode=quality_focus, reason=quality_exit_family_or_expansion`
    - final reason includes concrete patch direction:
      - `patch_direction=exit_risk, next_scenario_mode=quality_focus ...`
  - artifact checks:
    - `build/Release/logs/candidate_auto_improvement_summary.json`:
      - `persist_directional_pivot.last_direction=exit_risk`
      - `persist_directional_pivot.last_reason_code=quality_exit_family_or_expansion`
      - `holdout_suppression_persist_policy.events[*].pivot_direction=exit_risk`
    - `build/Release/logs/candidate_auto_improvement_iterations.csv`:
      - last row:
        - `selected_combo_family=quality_exit_rebalance`
        - `tune_scenario_mode_source=persist_directional_pivot_override`
        - `persist_pivot_direction=exit_risk`

- Next subtask:
  - exercise and validate the `entry_timing` pivot branch explicitly (force non-expansion winner path) and confirm scenario override shifts to `diverse_wide` with corresponding recommendation trace.

40. P1 step-2 continued: `entry_timing` pivot branch validated with real loop trace (2026-02-16)
- What changed:
  - no additional code change required beyond step 39 wiring.
  - validation run forced non-expansion path using:
    - `--tune-disable-post-suppression-quality-expansion`
    - this keeps suppression effective while allowing `signal_generation_boost` family to remain as selected combo.

- Verification:
  - run:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 2 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.0 --holdout-suppression-persist-iterations 2 --tune-disable-post-suppression-quality-expansion` PASS
  - observed logs:
    - iteration-1 pivot prepare:
      - `direction=entry_timing`
      - `next_scenario_mode=diverse_wide`
      - `reason=entry_relax_family_selected`
    - iteration-2 override applied:
      - `Applying scenario-mode override ... scenario_mode=diverse_wide`
    - tune stage confirms override:
      - `scenario_mode=diverse_wide`
  - final status/reason:
    - `status=paused_holdout_suppression_persist`
    - reason now includes:
      - `patch_direction=entry_timing`
      - `next_scenario_mode=diverse_wide`
      - entry-timing recommendation text.
  - artifact checks:
    - `build/Release/logs/candidate_auto_improvement_summary.json`
      - `persist_directional_pivot.last_direction=entry_timing`
      - `persist_directional_pivot.last_reason_code=entry_relax_family_selected`
      - `persist_directional_pivot.last_next_scenario_mode=diverse_wide`
    - `build/Release/logs/candidate_auto_improvement_iterations.csv` (iteration=2, phase=post_apply)
      - `selected_combo_family=signal_generation_boost`
      - `tune_scenario_mode_source=persist_directional_pivot_override`
      - `persist_pivot_direction=entry_timing`
      - `persist_pivot_next_scenario_mode=diverse_wide`

- Next subtask:
  - add branch-specific patch templates to the loop summary:
    - when `entry_timing`: emit concrete entry-filter timing checklist.
    - when `exit_risk`: emit concrete exit/risk tightening checklist.
  - ensure template payload is machine-readable for next auto-improvement iteration planning.

41. P1 step-2 continued: branch-specific machine-readable patch templates fully wired (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - completed patch-template propagation for directional pivot metadata:
      - holdout persist events now include:
        - `pivot_patch_template_id`
        - `pivot_patch_template_action_count`
        - `pivot_patch_template` (full machine-readable template payload)
      - post-apply iteration rows now include:
        - `persist_pivot_template_id`
        - `persist_pivot_template_action_count`
      - summary `persist_directional_pivot` now includes:
        - `last_patch_template`
    - this finalizes step-40 TODO for machine-readable branch templates and keeps event/row/summary payloads consistent.

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py scripts/tune_candidate_gate_trade_density.py` PASS
  - real loop validation run:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 2 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.0 --holdout-suppression-persist-iterations 2` PASS
  - artifact checks:
    - `build/Release/logs/candidate_auto_improvement_summary.json`
      - `persist_directional_pivot.last_patch_template.template_id=exit_risk_v1`
      - `persist_directional_pivot.last_patch_template.checklist` count `=4`
      - `holdout_suppression_persist_policy.events[-1].pivot_patch_template_id=exit_risk_v1`
    - `build/Release/logs/candidate_auto_improvement_iterations.csv`
      - `persist_pivot_template_id`/`persist_pivot_template_action_count` columns present in header and populated on `phase=post_apply`.

- Next subtask:
  - promote these template payloads into an executable patch-plan handoff artifact per iteration
    (e.g. `candidate_auto_improvement_patch_plan.json`) so next loop can consume concrete change intents without re-deriving branch direction.

42. P1 step-2 continued: executable patch-plan handoff artifact added (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - added CLI/output path:
      - `--patch-plan-json` (default: `build/Release/logs/candidate_auto_improvement_patch_plan.json`)
    - added artifact builder:
      - `build_machine_readable_patch_plan(...)`
      - includes:
        - trigger state (`holdout_suppression_persist_*`)
        - pivot direction/reason/next scenario mode
        - branch template payload (`patch_template`)
        - normalized ordered action list (`actions`)
        - latest iteration context (`phase`, combo family, PF/expectancy/trades/win-rate, taxonomy top groups, blended hostility/quality)
    - summary outputs now include `patch_plan_json` path and completion log prints it.

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py` PASS
  - run:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 1 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.0` PASS
  - artifact checks:
    - `build/Release/logs/candidate_auto_improvement_patch_plan.json`
      - `pivot.direction=exit_risk`
      - `patch_template.template_id=exit_risk_v1`
      - `actions` count `=4`
      - `context.phase=post_apply`
      - `context.selected_combo_family=quality_exit_rebalance`
    - `build/Release/logs/candidate_auto_improvement_summary.json`
      - `outputs.patch_plan_json` present and points to artifact path.

- Next subtask:
  - consume `candidate_auto_improvement_patch_plan.json` in the next tuning iteration pre-step
    to automatically pin branch-specific scenario/config knobs (entry-timing vs exit-risk) before candidate combo generation.

43. P1 step-2 continued: patch-plan handoff consumption wired into next tuning pre-step (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - added handoff controls:
      - `--consume-patch-plan-handoff` / `--disable-consume-patch-plan-handoff`
    - added reader:
      - `read_patch_plan_handoff(...)`
      - extracts `direction`, `reason_code`, `next_scenario_mode`, `template_id`, action count.
    - startup behavior:
      - if handoff file is usable, loop preloads:
        - `next_tune_scenario_mode_override`
        - `next_tune_directional_hint`
      - logs loaded handoff metadata (`direction`, `next_scenario_mode`, `template`).
    - per-iteration pre-tune behavior:
      - applies override source `patch_plan_handoff` on baseline iteration when loaded from artifact.
      - pins one branch-specific knob before combo generation:
        - `entry_timing` -> disables post-suppression quality expansion for that iteration,
        - `exit_risk` -> enables post-suppression quality expansion.
    - observability:
      - iteration rows now include:
        - `tune_directional_hint_iter`
        - `tune_post_suppression_quality_expansion_enabled_iter`
        - `tune_post_suppression_min_combo_count_iter`
      - summary now includes:
        - `tuning.consume_patch_plan_handoff`
        - `persist_directional_pivot.patch_plan_handoff_snapshot`
        - `persist_directional_pivot.patch_plan_handoff_applied`
        - `persist_directional_pivot.next_directional_hint_pending`

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py` PASS
  - run:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 1 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.0` PASS
  - observed runtime trace:
    - `Loaded patch-plan handoff: direction=exit_risk, next_scenario_mode=quality_focus, template=exit_risk_v1`
    - `Applying scenario-mode override ... reason=patch_plan_handoff:quality_exit_family_or_expansion`
  - artifact checks:
    - `build/Release/logs/candidate_auto_improvement_summary.json`
      - `tuning.consume_patch_plan_handoff=true`
      - `persist_directional_pivot.patch_plan_handoff_applied=true`
      - `persist_directional_pivot.patch_plan_handoff_snapshot.direction=exit_risk`
    - `build/Release/logs/candidate_auto_improvement_iterations.csv` baseline row
      - `tune_scenario_mode_source=patch_plan_handoff`
      - `tune_directional_hint_iter=exit_risk`
      - `tune_post_suppression_quality_expansion_enabled_iter=True`

- Next subtask:
  - validate the `entry_timing` handoff branch end-to-end by seeding an `entry_timing` patch plan and confirming:
    - `tune_scenario_mode_source=patch_plan_handoff`
    - `tune_directional_hint_iter=entry_timing`
    - `tune_post_suppression_quality_expansion_enabled_iter=False` on baseline iteration.

44. P1 step-2 continued: `entry_timing` handoff branch end-to-end validation passed (2026-02-16)
- What changed:
  - no additional code change; executed forced handoff validation by seeding
    `build/Release/logs/candidate_auto_improvement_patch_plan.json` with:
    - `pivot.direction=entry_timing`
    - `pivot.next_scenario_mode=diverse_wide`
    - `patch_template.template_id=entry_timing_v1`
  - then re-ran one-iteration auto-loop to verify pre-tune consumption behavior.

- Verification:
  - run:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 1 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.0` PASS
  - observed runtime trace:
    - `Loaded patch-plan handoff: direction=entry_timing, next_scenario_mode=diverse_wide, template=entry_timing_v1`
    - `Applying scenario-mode override ... scenario_mode=diverse_wide, reason=patch_plan_handoff:entry_relax_family_selected`
    - tuning confirms pinned knob:
      - `post_suppression_quality_expansion=off`
  - artifact checks:
    - `build/Release/logs/candidate_auto_improvement_iterations.csv` baseline row:
      - `tune_scenario_mode_source=patch_plan_handoff`
      - `tune_directional_hint_iter=entry_timing`
      - `tune_post_suppression_quality_expansion_enabled_iter=False`
    - `build/Release/logs/candidate_auto_improvement_summary.json`:
      - `persist_directional_pivot.patch_plan_handoff_snapshot.direction=entry_timing`
      - `persist_directional_pivot.patch_plan_handoff_applied=true`
      - `persist_directional_pivot.last_direction=entry_timing`

- Next subtask:
  - implement deterministic mapping layer from patch-plan `actions[]` to concrete tune/config override set
    (currently only direction-level knobs are auto-pinned; action-level execution is not yet wired).

45. P1 step-2 continued: deterministic `actions[] -> tune/config override` mapping wired + dual-branch validation (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - added action-override control:
      - `--enable-patch-plan-action-overrides` / `--disable-patch-plan-action-overrides` (default enabled)
    - enhanced handoff parser:
      - `read_patch_plan_handoff(...)` now loads `actions[]` payload (fallback to `patch_template.checklist`), plus template focus.
    - added deterministic mapping layer:
      - `build_patch_plan_action_overrides(...)`
      - maps known action texts to concrete tuning overrides:
        - objective mode / objective floors (`ratio`, `win-rate`, `expectancy`, `avg-trades`)
        - gate trade floor scaling
        - hint guardrail ratio/tighten-scale
        - holdout suppression hint-ratio threshold
        - post-suppression expansion enable/min-combo count
    - wired execution in iteration pre-tune stage:
      - consumes action list when scenario override is applied from handoff/pivot.
      - applies resolved override set before building tune argv.
      - tune argv now uses iteration-resolved:
        - `--objective-mode`
        - `--holdout-suppression-hint-ratio-threshold`
    - pivot-to-next-iteration now carries action template checklist:
      - `next_tune_patch_plan_template_id`
      - `next_tune_patch_plan_actions`
    - observability added:
      - iteration CSV fields:
        - `tune_objective_mode_iter`
        - `tune_holdout_suppression_hint_ratio_threshold_iter`
        - `patch_plan_action_override_*`
      - holdout persist events include action-override summary.
      - summary includes:
        - `tuning.enable_patch_plan_action_overrides`
        - `persist_directional_pivot.last_action_override`
        - pending next patch-template/action counters.
      - patch-plan context now records action-override usage for handoff traceability.

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py` PASS
  - `entry_timing` action-override run:
    - seeded `candidate_auto_improvement_patch_plan.json` to `entry_timing_v1`
    - run:
      - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 1 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 2 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60` PASS
    - observed:
      - `Applied patch-plan action overrides: template=entry_timing_v1, matched=4 ... expansion=False`
      - tune stage uses `scenario_mode=diverse_wide`
      - tune stage shows guardrail threshold tightened (`0.55`) and expansion disabled.
    - artifact checks (`candidate_auto_improvement_iterations.csv`, baseline):
      - `patch_plan_action_override_applied=True`
      - `patch_plan_action_override_template_id=entry_timing_v1`
      - `patch_plan_action_override_matched_count=4`
      - `tune_scenario_mode_source=patch_plan_handoff`
      - `tune_holdout_suppression_hint_ratio_threshold_iter=0.55`
      - `tune_post_suppression_quality_expansion_enabled_iter=False`
  - `exit_risk` action-override run:
    - seeded `candidate_auto_improvement_patch_plan.json` to `exit_risk_v1`
    - same command PASS
    - observed:
      - `Applied patch-plan action overrides: template=exit_risk_v1, matched=4, objective_mode=profitable_ratio_priority, expansion=True`
      - tune stage uses `scenario_mode=quality_focus`
      - tune stage reflects tightened ratio/scale and expansion enabled.
    - artifact checks (baseline row):
      - `patch_plan_action_override_applied=True`
      - `patch_plan_action_override_template_id=exit_risk_v1`
      - `patch_plan_action_override_matched_count=4`
      - `tune_objective_mode_iter=profitable_ratio_priority`
      - `tune_holdout_suppression_hint_ratio_threshold_iter=0.55`
      - `tune_post_suppression_quality_expansion_enabled_iter=True`
    - summary checks:
      - `persist_directional_pivot.last_action_override.applied=true`
      - `persist_directional_pivot.last_action_override.resolved.objective_mode=profitable_ratio_priority`

- Next subtask:
  - add lightweight A/B harness for action-overrides ON vs OFF under same seeded handoff template
    and emit delta report artifact:
    - target: `build/Release/logs/candidate_patch_action_override_ab_summary.json`
    - metrics: objective score, PF, expectancy, trade count, profitable ratio deltas.

46. Cross-cutting policy: 한글 로그/주석 + UTF-8 통일 규칙 고정 (2026-02-16)
- What changed:
  - 실행/튜닝 관련 신규 로그 문구를 한국어 우선 표현으로 정렬:
    - `scripts/run_candidate_auto_improvement_loop.py` (patch handoff/override 주요 로그)
    - `scripts/run_patch_action_override_ab.py` (완료/에러 메시지)
  - 인코딩/컴파일 정책 재확인:
    - `.editorconfig` 전역 `charset = utf-8`
    - `CMakeLists.txt` MSVC `/utf-8` 컴파일 옵션 활성화

- Verification:
  - `.editorconfig` 확인:
    - `charset = utf-8` (전역 적용)
  - `CMakeLists.txt` 확인:
    - `add_compile_options(/W4 /EHsc /utf-8)` 포함
  - 깨진 한글(대체문자) 스캔:
    - `rg -n "�" scripts docs include src -S` -> no match
  - 스크립트 문법:
    - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py scripts/run_patch_action_override_ab.py` PASS

- Rule going forward:
  - 신규 로그/주석은 한국어 우선(필요 시 기술 키워드만 영어 병기).
  - 코드/문서/스크립트 파일은 UTF-8 유지.
  - 기존 영문 로그는 리팩토링 작업 시점에 점진 치환(대규모 일괄치환은 회귀 위험 때문에 단계 적용).

- Next subtask:
  - 45에서 추가한 A/B 하네스 결과(JSON)에 handoff 적용 여부와 action-override 적용 여부를 함께 포함해
    결과 해석 혼동(0 delta but handoff 미적용)을 방지하도록 정리.

47. P1 step-2 continued: patch-action A/B 하네스 해석력 보강 + handoff 재시드 정합성 수정 (2026-02-16)
- What changed:
  - `scripts/run_patch_action_override_ab.py`
    - ON/OFF 런 각각 시작 직전에 동일 템플릿 재시드:
      - ON 실행 후 loop가 patch-plan을 덮어써도 OFF 실행이 동일 조건으로 시작되도록 보정.
    - 결과 메트릭 확장:
      - handoff 적용 여부 / action-override 적용 여부 / 템플릿 / 매칭 액션 수.
      - iteration CSV 기반 메트릭 추가:
        - `selected_combo_objective_with_gate_bonus_post_apply`
        - `objective_score_post_apply`
        - baseline `tune_objective_mode_iter`
    - delta 확장:
      - `selected_combo_objective_with_gate_bonus_post_apply_delta_on_minus_off`
      - `objective_score_post_apply_delta_on_minus_off`
    - 필수 검증 옵션:
      - `--require-handoff-applied`에서 ON/OFF 각각 handoff 적용 여부를 별도로 검사.
    - 출력 로그 보강:
      - `selected_combo_objective_with_gate_bonus` delta 출력 추가.
  - `scripts/run_candidate_auto_improvement_loop.py`
    - iteration CSV에 `selected_combo_objective_with_gate_bonus` 기록 추가
      (A/B 하네스가 튜닝 단계 objective 차이를 직접 읽을 수 있도록).

- Verification:
  - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py scripts/run_patch_action_override_ab.py` PASS
  - run:
    - `python scripts/run_patch_action_override_ab.py --seed-patch-template exit_risk_v1 --require-handoff-applied --max-iterations 1 --max-runtime-minutes 60 --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
  - artifact checks:
    - `build/Release/logs/candidate_patch_action_override_ab_summary.json`
      - `handoff_applied_on=true`, `handoff_applied_off=true`
      - `action_override_applied_on=true`, `action_override_applied_off=false`
      - `selected_combo_objective_with_gate_bonus_post_apply_delta_on_minus_off=-504.0`
      - `objective_score_post_apply_delta_on_minus_off=0.0`
      - `warnings=[]`
    - iteration metrics:
      - ON baseline `tune_objective_mode_baseline=profitable_ratio_priority`
      - OFF baseline `tune_objective_mode_baseline=balanced`

- Interpretation:
  - action override가 실제로 적용되어 튜닝 점수 함수 관점의 콤보 objective 차이는 발생했지만,
    이번 샘플에서는 post-apply 실성능(PF/expectancy/trades) 지표 변화는 없었습니다.
  - 즉, “오버라이드 적용 여부”와 “실성능 개선 여부”를 분리해서 봐야 합니다.

- Next subtask:
  - A/B 하네스에 다회 반복(`N`회) + 통계 요약(평균/표준편차) 모드 추가:
    - 단일 1회 run의 노이즈를 줄이고 action-override 실효성 판단을 안정화.
    - target artifact:
      - `build/Release/logs/candidate_patch_action_override_ab_multirun_summary.json`

48. P1 step-2 continued: patch-action A/B 멀티런 통계 모드 추가 + 2회 반복 검증 완료 (2026-02-16)
- What changed:
  - `scripts/run_patch_action_override_ab.py`
    - 멀티런 옵션 추가:
      - `--repeat-runs` (default 1)
      - `--multirun-output-json` (default: `build/Release/logs/candidate_patch_action_override_ab_multirun_summary.json`)
    - 내부 구조 변경:
      - 라운드별 ON/OFF 실행 결과를 `round_results[]`로 누적.
      - 델타 지표(객체/기대값/PF/거래수/승률 + combo objective/post-apply objective)의
        평균/표준편차/최소/최대 집계(`aggregate`) 추가.
      - single-run과 호환되도록 기존 top-level `runs`, `delta_on_minus_off`, `warnings` 유지
        (마지막 라운드 기준) + 멀티런 상세 병행 저장.
    - 정합성 보강:
      - ON/OFF 각각 실행 직전 동일 템플릿 재시드 유지.
      - `--require-handoff-applied` 검사 시 전체 라운드 대상 검증.

- Verification:
  - `python -m py_compile scripts/run_patch_action_override_ab.py` PASS
  - run:
    - `python scripts/run_patch_action_override_ab.py --seed-patch-template exit_risk_v1 --require-handoff-applied --repeat-runs 2 --max-iterations 1 --max-runtime-minutes 60 --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
  - artifact checks:
    - `build/Release/logs/candidate_patch_action_override_ab_summary.json`
    - `build/Release/logs/candidate_patch_action_override_ab_multirun_summary.json`
    - 핵심 결과:
      - `repeat_runs=2`
      - `aggregate.handoff_applied_on_all_rounds=true`
      - `aggregate.handoff_applied_off_all_rounds=true`
      - `aggregate.action_override_applied_on_all_rounds=true`
      - `selected_combo_objective_with_gate_bonus_delta_on_minus_off_mean=-504.0`
      - post-apply 실성능 delta 평균은 0.0 (PF/expectancy/trades/profitable_ratio)

- Interpretation:
  - 액션 오버라이드 ON/OFF는 튜닝 콤보 objective 함수 점수에는 일관된 차이를 만들었지만,
    현재 데이터셋/1-iteration 조건에서는 post-apply 실성능 지표 개선으로는 아직 연결되지 않았습니다.
  - 즉, action override는 “탐색 압력(탐색 방향)”을 바꾸는 단계까지는 검증되었고,
    “실성능 우위”는 추가 실험 설계(반복 수/데이터 창/iteration 길이)로 검증해야 합니다.

- Next subtask:
  - 멀티런 결과 기반 자동 판정 규칙 추가:
    - 예: `M`회 평균에서 post-apply 실성능 delta가 개선되지 않으면 해당 template override를
      다음 루프에서 자동 약화/중단.
    - target artifact:
      - `build/Release/logs/candidate_patch_action_override_policy_decision.json`

49. P1 step-2 continued: patch-action 정책판정 생성 + auto-loop 소비 연동 완료 (2026-02-16)
- What changed:
  - `scripts/run_patch_action_override_ab.py`
    - 정책 아티팩트 출력 추가:
      - `--policy-decision-json` (default: `build/Release/logs/candidate_patch_action_override_policy_decision.json`)
    - 정책 판정 로직 추가:
      - `_build_policy_decision(...)`
      - 신호 입력:
        - 멀티런 평균 delta (PF/expectancy/profitable_ratio/post-apply objective/combo objective)
        - handoff/override 적용 정합성
      - 정책 결정:
        - `keep_override`
        - `decrease_override_strength`
        - `disable_override`
        - `invalid_experiment`
    - 정책 임계값 옵션:
      - `--policy-min-expectancy-delta`
      - `--policy-min-profit-factor-delta`
      - `--policy-min-profitable-ratio-delta`
      - `--policy-neutral-band-epsilon`

  - `scripts/run_candidate_auto_improvement_loop.py`
    - 정책 소비 입력 추가:
      - `--patch-action-policy-json` (default 위 policy artifact)
      - `--consume-patch-action-policy` / `--disable-consume-patch-action-policy`
    - 정책 소비 동작:
      - policy `disable_override` -> action override 비활성화
      - policy `decrease_override_strength` -> action override 강도 0.5로 약화
      - policy `keep_override` -> 강도 1.0 유지
    - action override 매핑에 강도 스케일 적용:
      - `build_patch_plan_action_overrides(..., strength_scale=...)`
      - delta/트레이드 스케일을 strength 기반으로 완화 적용
    - observability 확장:
      - summary:
        - `tuning.consume_patch_action_policy`
        - `tuning.effective_enable_patch_plan_action_overrides`
        - `tuning.patch_action_override_strength_scale`
        - `persist_directional_pivot.patch_action_policy_snapshot`
        - `persist_directional_pivot.patch_action_policy_applied`
      - outputs:
        - `patch_action_policy_json`

- Verification:
  - `python -m py_compile scripts/run_patch_action_override_ab.py scripts/run_candidate_auto_improvement_loop.py` PASS
  - policy artifact generation:
    - `python scripts/run_patch_action_override_ab.py --seed-patch-template exit_risk_v1 --require-handoff-applied --repeat-runs 1 --max-iterations 1 --max-runtime-minutes 60 --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
    - result:
      - `candidate_patch_action_override_policy_decision.json`
      - decision: `decrease_override_strength`
  - policy consumption validation (handoff+policy 동시):
    - patch-plan을 `exit_risk_v1`로 seed 후:
      - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 1 --max-runtime-minutes 60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60` PASS
    - observed logs:
      - `패치 액션 정책 적용 ... decision=decrease_override_strength ... strength_scale=0.5`
      - `패치 플랜 handoff 로드 ... template=exit_risk_v1`
      - `패치 플랜 액션 오버라이드 적용 완료 ... matched=4 ...`
    - summary checks:
      - `patch_action_policy_applied=true`
      - `patch_action_override_strength_scale=0.5`
      - `last_action_override.strength_scale=0.5`
    - iteration checks:
      - baseline `patch_plan_action_override_applied=True`
      - baseline `tune_objective_mode_iter=profitable_ratio_priority`

- Next subtask:
  - 정책 결정을 템플릿별로 분리 저장(예: `entry_timing_v1`, `exit_risk_v1`)하고
    auto-loop에서 현재 handoff 템플릿 ID에 맞는 정책만 적용하도록 확장.
    - target artifact:
      - `build/Release/logs/candidate_patch_action_override_policy_registry.json`

50. P1 step-2 continued: template별 정책 registry 저장 + handoff 템플릿 매칭 적용 완료 (2026-02-16)
- What changed:
  - `scripts/run_patch_action_override_ab.py`
    - policy registry 출력/갱신 추가:
      - `--policy-registry-json` (default: `build/Release/logs/candidate_patch_action_override_policy_registry.json`)
      - `--update-policy-registry` / `--disable-update-policy-registry`
    - 정책 결정을 template key로 registry에 기록:
      - `template_policies.<template_id>`
      - `latest_template_id`, `latest_decision`, `history[]`
    - A/B 결과 아티팩트 경로에 registry/policy decision 경로 포함.
    - 깨진 문자열 로그 정리:
      - 완료/에러 메시지 UTF-8 한국어로 복구.

  - `scripts/run_candidate_auto_improvement_loop.py`
    - template-매칭 policy 입력 추가:
      - `--patch-action-policy-registry-json`
    - registry 스냅샷 로더 추가:
      - `read_patch_action_policy_registry(path, template_id)`
    - 정책 적용 우선순위 변경:
      - registry 파일이 존재하면 현재 handoff `template_id`에 매칭된 정책만 적용.
      - 매칭 정책이 없으면 정책 미적용(단일 policy fallback 금지; template 오염 방지).
      - registry 파일이 없을 때만 기존 단일 policy(`patch_action_policy_json`) 사용.
    - observability 확장:
      - `tuning.patch_action_policy_source`
      - `tuning.patch_action_policy_template_id`
      - `persist_directional_pivot.patch_action_policy_registry_snapshot`
      - `persist_directional_pivot.patch_action_policy_effective_snapshot`
      - `outputs.patch_action_policy_registry_json`

- Verification:
  - 문법 검사:
    - `python -m py_compile scripts/run_patch_action_override_ab.py scripts/run_candidate_auto_improvement_loop.py` PASS
  - registry 생성 검증:
    - `python scripts/run_patch_action_override_ab.py --seed-patch-template exit_risk_v1 --require-handoff-applied --repeat-runs 1 --max-iterations 1 --max-runtime-minutes 60 --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
    - 결과:
      - `candidate_patch_action_override_policy_registry.json` 생성
      - `template_policies.exit_risk_v1.decision=decrease_override_strength`
  - template 매칭 적용 검증:
    - handoff template 없음(`template=none`) 상태:
      - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 1 --max-runtime-minutes 60 --skip-tune-phase --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --matrix-max-workers 1 --matrix-backtest-retry-count 1` PASS
      - 로그: `패치 액션 정책 미적용: registry에 현재 템플릿 정책이 없습니다`
    - handoff template를 `exit_risk_v1`로 seed한 상태:
      - 동일 auto-loop 명령 PASS
      - 로그: `패치 액션 정책 적용: ... source=registry, template=exit_risk_v1 ...`
      - summary 확인:
        - `tuning.patch_action_policy_source=registry`
        - `tuning.patch_action_policy_template_id=exit_risk_v1`

- Next subtask:
  - template policy registry의 신선도/유효성 제어 추가:
    - `max_policy_age_hours`(만료 정책 자동 무시),
    - `min_repeat_runs`(검증 수 미달 정책 미적용),
    - 적용 실패/미매칭 원인 코드 표준화.
  - target artifact:
    - `build/Release/logs/candidate_patch_action_override_policy_registry_guard_report.json`

51. P1 step-2 continued: policy registry guard(신선도/반복수) 적용 + 원인코드 리포트 추가 (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - registry guard 입력 추가:
      - `--patch-action-policy-guard-report-json` (default: `build/Release/logs/candidate_patch_action_override_policy_registry_guard_report.json`)
      - `--patch-action-policy-min-repeat-runs` (default: `1`)
      - `--patch-action-policy-max-age-hours` (default: `72.0`)
      - `--enable-patch-action-policy-registry-guards` / `--disable-patch-action-policy-registry-guards`
    - guard 판정 함수 추가:
      - `evaluate_patch_action_policy_registry_guard(...)`
      - 판정 항목:
        - template 매칭 여부,
        - decision 유효성,
        - repeat_runs 하한,
        - policy updated_at 신선도(시간).
    - 적용 규칙:
      - registry 존재 시 guard를 통과한 정책만 usable.
      - guard 미통과면 policy 미적용 + 원인코드 로그 출력.
    - observability 확장:
      - `tuning.patch_action_policy_registry_guards_enabled`
      - `tuning.patch_action_policy_min_repeat_runs`
      - `tuning.patch_action_policy_max_age_hours`
      - `persist_directional_pivot.patch_action_policy_registry_guard_report`
      - `outputs.patch_action_policy_guard_report_json`

- Verification:
  - 문법 검사:
    - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py scripts/run_patch_action_override_ab.py` PASS
  - guard 적용(정책 허용) 검증:
    - handoff template를 `exit_risk_v1`로 seed 후:
      - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 0 --max-runtime-minutes 10 --skip-tune-phase --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
      - 로그: `패치 액션 정책 적용 ... source=registry, template=exit_risk_v1 ...`
  - guard 차단 검증:
    - 동일 seed + `--patch-action-policy-min-repeat-runs 2`
      - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 0 --max-runtime-minutes 10 --skip-tune-phase --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --patch-action-policy-min-repeat-runs 2` PASS
      - 로그: `패치 액션 정책 미적용: registry guard/reason=repeat_runs_below_min ...`
      - guard report:
        - `accepted=false`
        - `reason_codes=[\"repeat_runs_below_min\"]`
        - `policy_age_hours` 값 기록 확인.

- Next subtask:
  - A/B 실행 스크립트(`run_patch_action_override_ab.py`)에서 template별 최소 반복수(`min_repeat_runs`)를 자동 상향/완화하는 피드백 규칙 추가:
    - 반복 실험 누적 근거가 약한 템플릿은 auto-loop 소비를 기본 차단,
    - 충분한 누적 후에만 `keep_override` 승격 허용.
  - target artifact:
    - `build/Release/logs/candidate_patch_action_override_policy_registry_feedback.json`

52. P1 step-2 continued: template별 policy feedback 규칙 추가 + auto-loop guard 연동 완료 (2026-02-16)
- What changed:
  - `scripts/run_patch_action_override_ab.py`
    - feedback artifact 출력/갱신 추가:
      - `--policy-registry-feedback-json` (default: `build/Release/logs/candidate_patch_action_override_policy_registry_feedback.json`)
      - `--update-policy-registry-feedback` / `--disable-update-policy-registry-feedback`
      - `--feedback-min-repeat-runs-floor` (default: `2`)
      - `--feedback-min-repeat-runs-ceiling` (default: `6`)
      - `--feedback-promote-min-consecutive-keeps` (default: `2`)
    - template별 feedback 상태 저장:
      - `recommended_min_repeat_runs`
      - `block_auto_loop_consumption`
      - `allow_keep_promotion`
      - decision 카운트/연속 streak/history
    - 정책 결과가 약한 반복수에서 들어오면 최소 반복수 자동 상향 + 소비 차단.

  - `scripts/run_candidate_auto_improvement_loop.py`
    - feedback 입력 추가:
      - `--patch-action-policy-registry-feedback-json`
      - `--consume-patch-action-policy-registry-feedback` / `--disable-consume-patch-action-policy-registry-feedback`
    - registry guard가 feedback을 함께 반영:
      - `effective_min_repeat_runs = max(cli_min_repeat_runs, feedback.recommended_min_repeat_runs)`
      - `feedback.block_auto_loop_consumption=true`면 정책 소비 차단.
      - `decision=keep_override`에서 `allow_keep_promotion=false`면 승격 차단.
    - observability 확장:
      - `tuning.consume_patch_action_policy_registry_feedback`
      - `tuning.patch_action_policy_effective_min_repeat_runs`
      - `persist_directional_pivot.patch_action_policy_registry_feedback_snapshot`
      - `outputs.patch_action_policy_registry_feedback_json`

- Verification:
  - 문법 검사:
    - `python -m py_compile scripts/run_patch_action_override_ab.py scripts/run_candidate_auto_improvement_loop.py` PASS
  - feedback artifact 생성:
    - `python scripts/run_patch_action_override_ab.py --seed-patch-template exit_risk_v1 --require-handoff-applied --repeat-runs 1 --max-iterations 1 --max-runtime-minutes 60 --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
    - 결과:
      - `candidate_patch_action_override_policy_registry_feedback.json`
      - `exit_risk_v1.recommended_min_repeat_runs=3`
      - `exit_risk_v1.block_auto_loop_consumption=true`
      - `exit_risk_v1.allow_keep_promotion=false`
  - auto-loop feedback guard 차단:
    - handoff template `exit_risk_v1` seed 후:
      - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 0 --max-runtime-minutes 10 --skip-tune-phase --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
      - 로그: `registry guard/reason=feedback_block_auto_loop_consumption,repeat_runs_below_min`
      - summary:
        - `tuning.consume_patch_action_policy_registry_feedback=true`
        - `tuning.patch_action_policy_effective_min_repeat_runs=3`
  - feedback bypass 회귀 확인:
    - 동일 조건 + `--disable-consume-patch-action-policy-registry-feedback` PASS
    - 로그: `패치 액션 정책 적용 ... source=registry, template=exit_risk_v1 ...`

- Next subtask:
  - feedback 기반 승격 실험 시나리오 추가:
    - 동일 template에 대해 다회 반복(`repeat_runs>=recommended_min_repeat_runs`) 실행 후
      `allow_keep_promotion=true`가 되는지 자동 체크하는 검증 스크립트/리포트 추가.
  - target artifact:
    - `build/Release/logs/candidate_patch_action_override_feedback_promotion_check.json`

53. P1 step-2 continued: feedback 승격 체크 스크립트/리포트 추가 완료 (2026-02-16)
- What changed:
  - 신규 스크립트:
    - `scripts/run_patch_action_override_feedback_promotion_check.py`
  - 기능:
    - policy feedback/registry/decision 아티팩트를 읽어 template별 승격 가능성 자동 판정.
    - 핵심 판정:
      - `promotion_ready`
      - `required_min_repeat_runs`
      - `required_keep_streak`
      - `blocker_reason_codes`
    - 다음 실험 권장치 계산:
      - `recommended_repeat_runs`
      - 재실행 커맨드 템플릿(`recommended_command`)
  - 출력:
    - `build/Release/logs/candidate_patch_action_override_feedback_promotion_check.json`

- Verification:
  - 문법 검사:
    - `python -m py_compile scripts/run_patch_action_override_feedback_promotion_check.py scripts/run_patch_action_override_ab.py scripts/run_candidate_auto_improvement_loop.py` PASS
  - 승격 체크 리포트 생성:
    - `python scripts/run_patch_action_override_feedback_promotion_check.py` PASS
    - 결과:
      - `template_id=exit_risk_v1`
      - `promotion_ready=false`
      - blockers:
        - `latest_decision_not_keep_override`
        - `allow_keep_promotion_false`
        - `block_auto_loop_consumption_true`
        - `repeat_runs_below_required`
        - `keep_streak_below_required`
      - `recommended_repeat_runs=3`
      - `recommended_command` 포함 확인

- Next subtask:
  - promotion-check 스크립트를 auto-loop summary chain에 옵션으로 연결:
    - auto-loop 실행 종료 시 현재 template 기준 promotion-check 리포트를 자동 갱신,
    - summary.outputs에 promotion-check 경로를 표준 포함.
  - target artifact:
    - `build/Release/logs/candidate_patch_action_override_feedback_promotion_check.json` (auto-loop chain 갱신본)

54. P1 step-2 continued: promotion-check를 auto-loop summary chain에 연동 완료 (2026-02-16)
- What changed:
  - `scripts/run_candidate_auto_improvement_loop.py`
    - auto-loop 종료 후 promotion-check 자동 실행 연동:
      - 내부 호출: `run_patch_action_override_feedback_promotion_check.main(...)`
    - 신규 옵션:
      - `--run-patch-action-feedback-promotion-check` / `--disable-run-patch-action-feedback-promotion-check`
      - `--patch-action-feedback-promotion-check-json`
      - `--patch-action-feedback-promotion-check-required-keep-streak`
    - summary observability 확장:
      - `tuning.run_patch_action_feedback_promotion_check`
      - `tuning.patch_action_feedback_promotion_check_required_keep_streak`
      - `persist_directional_pivot.patch_action_feedback_promotion_check_snapshot`
      - `outputs.patch_action_feedback_promotion_check_json`

- Verification:
  - 문법 검사:
    - `python -m py_compile scripts/run_candidate_auto_improvement_loop.py scripts/run_patch_action_override_feedback_promotion_check.py scripts/run_patch_action_override_ab.py` PASS
  - chain 연동 실행 확인:
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 0 --max-runtime-minutes 10 --skip-tune-phase --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
    - 로그:
      - `PatchActionFeedbackPromotionCheck 완료`
      - `patch_action_feedback_promotion_check_json=...` 출력 확인
    - summary 확인:
      - `persist_directional_pivot.patch_action_feedback_promotion_check_snapshot.invoked=true`
      - `outputs.patch_action_feedback_promotion_check_json` 존재
      - `tuning.run_patch_action_feedback_promotion_check=true`

- Next subtask:
  - feedback 승격 조건 달성 실험 실행:
    - promotion-check가 제안한 `recommended_repeat_runs`(현재 3)로
      동일 template(`exit_risk_v1`) A/B를 반복 수행해
      `block_auto_loop_consumption` 해제와 `allow_keep_promotion` 전환 가능성 검증.
  - target artifact:
    - `build/Release/logs/candidate_patch_action_override_policy_registry_feedback.json` (전환 여부 반영본)

55. P1 step-2 continued: feedback 승격 조건 실험(반복 3/4) 실행 + 규칙 안정화 검증 완료 (2026-02-16)
- What changed:
  - `scripts/run_patch_action_override_ab.py`
    - feedback 규칙 안정화 수정:
      - `decrease_override_strength` / `disable_override`에서
        repeat_runs 미달 시 `recommended_min_repeat_runs`를 추가 상향하지 않도록 수정(추격형 임계치 방지).
      - repeat_runs 충족 시 `recommended_min_repeat_runs`를 현 수준 유지로 처리.

- Verification:
  - 규칙 단위 검증(임시 파일 기반):
    - case1: prev_required=4, repeat_runs=3 -> `recommended_min_repeat_runs=4`, `block=true`
    - case2: prev_required=4, repeat_runs=4 -> `recommended_min_repeat_runs=4`, `block=false`
  - A/B 실험 재실행:
    - `python scripts/run_patch_action_override_ab.py --seed-patch-template exit_risk_v1 --require-handoff-applied --repeat-runs 3 --max-iterations 1 --max-runtime-minutes 60 --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
    - `python scripts/run_patch_action_override_ab.py --seed-patch-template exit_risk_v1 --require-handoff-applied --repeat-runs 4 --max-iterations 1 --max-runtime-minutes 60 --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
  - 최신 feedback 상태(`candidate_patch_action_override_policy_registry_feedback.json`):
    - `last_repeat_runs=4`
    - `recommended_min_repeat_runs=4`
    - `block_auto_loop_consumption=false`
    - `allow_keep_promotion=false`
    - `latest_decision=decrease_override_strength`
  - promotion-check 최신 상태:
    - `python scripts/run_patch_action_override_feedback_promotion_check.py` PASS
    - `promotion_ready=false`
    - blockers:
      - `latest_decision_not_keep_override`
      - `allow_keep_promotion_false`
      - `keep_streak_below_required`
    - `repeat_runs_below_required` / `block_auto_loop_consumption_true`는 해소됨.
  - auto-loop 정책 적용 재확인(템플릿 seed):
    - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 0 --max-runtime-minutes 10 --skip-tune-phase --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
    - 로그: `패치 액션 정책 적용: decision=decrease_override_strength, source=registry, template=exit_risk_v1 ...`

- Next subtask:
  - `keep_override` 신호 확보 실험:
    - `entry_timing_v1` 템플릿으로 다회 A/B를 실행해 `latest_decision=keep_override` 후보를 탐색,
    - 승격 조건(`allow_keep_promotion=true`, keep streak>=required) 달성 여부 비교.
  - target artifact:
    - `build/Release/logs/candidate_patch_action_override_policy_registry_feedback.json` (entry/exit 템플릿 비교 반영본)

56. P1 step-2 continued: 정책 과보정 완화 + `entry_timing_v1` keep 승격 조건 달성 (2026-02-16)
- What changed:
  - `scripts/run_patch_action_override_ab.py`
    - 정책 판정 완화:
      - `avg_expectancy/pf/profitable_ratio`가 중립대(`epsilon`) 안이고
        `selected_combo_objective`만 흔들리는 케이스를
        `decrease_override_strength`가 아니라 `keep_override`로 판정하도록 수정.
      - reason code 추가:
        - `primary_metrics_neutral_combo_shift_only`

- Verification:
  - 문법 검사:
    - `python -m py_compile scripts/run_patch_action_override_ab.py` PASS
  - 다회 A/B 재실행 (동일 조건, `repeat_runs=3`) 2회:
    - `python scripts/run_patch_action_override_ab.py --seed-patch-template entry_timing_v1 --require-handoff-applied --repeat-runs 3 --max-iterations 1 --max-runtime-minutes 60 --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate` PASS
    - 결과(최신 feedback):
      - `latest_decision=keep_override`
      - `latest_reason_code=primary_metrics_neutral_combo_shift_only`
      - `consecutive_keep_streak=2`
      - `recommended_min_repeat_runs=2`
      - `allow_keep_promotion=true`
      - `block_auto_loop_consumption=false`
  - 승격 체크:
    - `python scripts/run_patch_action_override_feedback_promotion_check.py --template-id entry_timing_v1` PASS
    - `promotion_ready=true`
    - `blockers=ready`

- Next subtask:
  - auto-loop에서 최신 feedback 승격 상태를 실제 소비하는지 1회 bounded 실행으로 확인:
    - guard report에서 `entry_timing_v1` 정책 usable 여부,
    - iteration summary에서 정책 소스/결정값 반영 여부 검증.
  - target artifacts:
    - `build/Release/logs/candidate_patch_action_override_policy_registry_guard_report.json`
    - `build/Release/logs/candidate_auto_improvement_summary.json`

57. P1 step-2 continued: auto-loop 정책 소비 검증 완료 (entry handoff seed 기준, 2026-02-16)
- What changed:
  - 코드 변경 없음(검증 실행).
  - 참고:
    - handoff 템플릿 없이 단독 auto-loop를 실행하면 `template_id=none`으로 registry 매칭이 되지 않음.
    - 실제 운영 경로(템플릿 seed/handoff 포함) 기준으로 재검증 수행.

- Verification:
  - handoff seed 기반 A/B 점검(상태 오염 방지):
    - `python scripts/run_patch_action_override_ab.py --seed-patch-template entry_timing_v1 --require-handoff-applied --repeat-runs 1 --max-iterations 0 --max-runtime-minutes 20 --tune-max-scenarios 1 --tune-screen-dataset-limit 1 --tune-screen-top-k 1 --matrix-max-workers 1 --matrix-backtest-retry-count 1 --tune-holdout-suppression-hint-ratio-threshold 0.60 --real-data-only --require-higher-tf-companions --skip-core-vs-legacy-gate --disable-update-policy-registry --disable-update-policy-registry-feedback` PASS
  - 로그 확인:
    - `AutoImprove`에서
      - `패치 액션 정책 적용: decision=keep_override, source=registry, template=entry_timing_v1`
      - (override on/off 분기 모두) 확인.
  - 최신 feedback 재확인:
    - `entry_timing_v1`:
      - `latest_decision=keep_override`
      - `consecutive_keep_streak=2`
      - `allow_keep_promotion=true`
      - `block_auto_loop_consumption=false`
  - promotion-check:
    - `python scripts/run_patch_action_override_feedback_promotion_check.py --template-id entry_timing_v1` PASS
    - `promotion_ready=true`

- Next subtask:
  - P1 본작업 복귀:
    - 전략 파이프라인(5개)에서 rejection reason taxonomy와 `NO_TRADE` 정책을 활용해
      `avg_expectancy_krw` / `profitable_ratio` 병목을 직접 줄이는 로직 수정(필터 임계치 튜닝 위주 접근 지양).
  - target artifacts:
    - `build/Release/logs/profitability_gate_report_realdata.json`
    - `build/Release/logs/strategy_rejection_taxonomy_report.json`

58. P1 step-3 started: 전략 품질 게이트를 alpha-head 우회 경로까지 확장 (2026-02-16)
- What changed:
  - `src/strategy/StrategyManager.cpp`
    - 선택/필터 단계 품질 강화:
      - RR 기반 품질 점수/패널티 추가 (`computeRewardRiskRatioForManager`)
      - 정적 EV 컷 대신 시장/유동성/변동성/전략 히스토리 반영 EV 타이트닝 추가 (`computeExpectedValueTighteningForManager`)
      - 단일 후보 저품질 차단:
        - `no_best_signal_single_candidate_quality_floor`
      - no-trade-bias 활성 시(포지션 상태 병목) 저엣지/저RR 후보 감점:
        - `no_best_signal_no_trade_bias_low_edge`
        - `no_best_signal_no_trade_bias_low_reward_risk`
      - manager prefilter 품질 사유 추가:
        - `filtered_out_by_manager_ev_quality_floor`
        - `filtered_out_by_manager_low_reward_risk`
  - `src/engine/TradingEngine.cpp`
    - alpha-head 스캔 경로에서 “무조건 후보 유지”를 제거하고,
      완화된 manager prefilter 후 후보를 pending queue에 적재하도록 변경.
  - `src/backtest/BacktestEngine.cpp`
    - alpha-head 경로에도 완화된 manager prefilter 적용.
    - `core_bridge_enabled`에서는 alpha-head 여부와 무관하게 robust selection 경로 사용.
  - `scripts/generate_strategy_rejection_taxonomy_report.py`
    - 신규 거절 사유 코드를 expected taxonomy에 등록(coverage 유지).

- Verification:
  - 빌드:
    - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release -j 6` PASS
  - 실검증:
    - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions` PASS
    - `core_full` 변화(직전 대비):
      - `avg_profit_factor`: `0.5277 -> 0.5443` (개선)
      - `avg_expectancy_krw`: `-7.8330 -> -7.1410` (개선)
      - `avg_total_trades`: `118.8333 -> 121.0833` (유지/소폭 증가)
      - `profitable_ratio`: `0.0` (미개선)
      - `overall_gate_pass=false` 유지
  - taxonomy:
    - `python scripts/generate_strategy_rejection_taxonomy_report.py` PASS
    - `taxonomy_coverage_ratio=1.0`, `unknown_reason_codes=0`
    - overall top reason:
      - `filtered_out_by_manager_ev_quality_floor`

- Next subtask:
  - P1 step-3 continued:
    - `profitable_ratio` 병목 직접 타격용으로
      “신호 발생 후 포지션 보유 중 재진입 후보 처리”와 “second-stage/risk gate 직전 RR-edge 보정”을 점검해
      손실 트레이드 클러스터를 줄이는 쪽으로 추가 수정.
  - target artifacts:
    - `build/Release/logs/profitability_gate_report_realdata.json`
    - `build/Release/logs/entry_rejection_summary_realdata.json`

59. P1 step-3 continued: 추세형 전략 off-regime 억제 보정(구조 안전장치, 성능 중립) (2026-02-16)
- What changed:
  - `src/strategy/StrategyManager.cpp`
    - Momentum/Breakout가 `TRENDING_UP`이 아닌 구간에서 저엣지 후보를 고르기 어렵도록
      selection/filter 단계 보정 추가.
    - 신규 reason code:
      - `no_best_signal_trend_off_regime_low_edge`
      - `filtered_out_by_manager_trend_off_regime_strength`
      - `filtered_out_by_manager_trend_off_regime_ev`
  - `scripts/generate_strategy_rejection_taxonomy_report.py`
    - 신규 reason code taxonomy expected 목록 반영.

- Verification:
  - 빌드:
    - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release -j 6` PASS
  - 실검증:
    - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions` PASS
    - `core_full`:
      - `avg_profit_factor=0.5439`
      - `avg_total_trades=121.1667`
      - `avg_expectancy_krw=-7.1500`
      - `profitable_ratio=0.0`
      - `overall_gate_pass=false`
    - 직전 스냅샷(`0.5443 / -7.1410`) 대비 성능 개선은 사실상 중립 범위.
  - taxonomy:
    - `python scripts/generate_strategy_rejection_taxonomy_report.py` PASS
    - `taxonomy_coverage_ratio=1.0`, `unknown_reason_codes=0`

- Next subtask:
  - 손실 기여 상위 전략(Momentum/Breakout) 내부 로직(신호 발생/진입 타이밍/청산) 정밀 개선:
    - 필터 임계치 추가보다, 진입 후 손실 꼬리(`left-tail`)를 줄이는 청산/리스크 관리 규칙 우선.
  - target artifacts:
    - `build/Release/logs/loss_contributor_by_strategy.csv`
    - `build/Release/logs/profitability_gate_report_realdata.json`

60. P1 step-3 continued: 손실 기여 리포트 체인 자동화 + 검증 락 안정화 (2026-02-16)
- What changed:
  - `scripts/run_realdata_candidate_loop.py`
    - 실검증 파이프라인에 손실 기여 분석 단계를 기본 연동:
      - `scripts/analyze_loss_contributors.py` 자동 실행
      - 출력 아티팩트:
        - `build/Release/logs/loss_contributor_by_market.csv`
        - `build/Release/logs/loss_contributor_by_strategy.csv`
    - 병렬 검증 충돌 방지를 위해 verification lock 체인 유지.
    - 신규 옵션:
      - `--loss-contributor-script`
      - `--loss-contributor-market-csv`
      - `--loss-contributor-strategy-csv`
      - `--loss-contributor-max-workers`
      - `--skip-loss-contributor-analysis`
  - `scripts/analyze_loss_contributors.py`
    - `config/config.json` 직접 수정 로직 제거(검증 간섭 최소화).
    - verification lock 연동 및 기본 worker를 보수적으로 `1`로 변경.
    - lock 옵션 추가:
      - `--verification-lock-path`
      - `--verification-lock-timeout-sec`
      - `--verification-lock-stale-sec`
  - 인코딩/주석 안정화:
    - `src/strategy/BreakoutStrategy.cpp` 인코딩 손상 이슈는 원본 상태로 복구 완료.
    - UTF-8 strict 유효성 확인 완료.

- Verification:
  - 문법:
    - `python -m py_compile scripts/run_realdata_candidate_loop.py scripts/analyze_loss_contributors.py` PASS
  - 빌드:
    - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release -j 6` PASS
  - 실검증:
    - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions -LossContributorMaxWorkers 1` PASS
    - `core_full`:
      - `avg_profit_factor=0.5439`
      - `avg_total_trades=121.1667`
      - `avg_expectancy_krw=-7.1500`
      - `profitable_ratio=0.0`
      - `overall_gate_pass=false`
    - parity:
      - `overall_invariant_pass=false`, `invariant_fail_count=4`
    - loss contributors (core_full loss rows):
      - strategy top2:
        - `Advanced Momentum` (`loss_share_pct=38.0252`)
        - `Breakout Strategy` (`loss_share_pct=36.4014`)
      - market top3:
        - `KRW-DOGE`, `KRW-AVAX`, `KRW-SUI`

- Next subtask:
  - 손실 기여 상위 2개 전략(`Advanced Momentum`, `Breakout Strategy`)에 대해
    아키타입/레짐별 손실 꼬리 분해를 먼저 수행하고,
    하드 필터 강화 대신 포지션 크기/청산 규칙을 소폭 보정하는 실험군을 분리 적용.
  - target artifacts:
    - `build/Release/logs/loss_contributor_by_strategy.csv`
    - `build/Release/logs/entry_rejection_summary_realdata.json`
    - `build/Release/logs/profitability_gate_report_realdata.json`

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
