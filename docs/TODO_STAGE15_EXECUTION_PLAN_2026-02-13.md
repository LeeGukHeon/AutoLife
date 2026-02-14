# Stage 15 Execution TODO (2026-02-13)

## Scope
- 목적: candidate 승격 가능한 수익성(Expectancy/Profitable Ratio) 회복.
- 기준일: 2026-02-13.
- 원칙: 최소 침습, legacy 기본 동작 유지, strict 복구/승인 체계 유지.

## Current Snapshot
- 실데이터 matrix(`build/Release/logs/profitability_gate_report_realdata.json`) 기준:
  - `core_full.avg_profit_factor = 3.1007` (PASS)
  - `core_full.avg_total_trades = 11.2667` (PASS)
  - `core_full.avg_expectancy_krw = -13.1301` (FAIL)
  - `core_full.profitable_ratio = 0.3333` (FAIL)
  - `overall_gate_pass = false`
- backtest MTF/plane parity:
  - companion TF(5m/60m/240m) 로딩 반영 완료
  - profile 분리 확인(`profiles_identical_by_dataset=false`) 완료

## P0 (바로 실행)
1. 손실 기여 상위 마켓 분해
- 목표: expectancy 음수 주범을 전략/레짐/체결비용 단위로 분해.
- 산출물:
  - `build/Release/logs/loss_contributor_by_market.csv`
  - `build/Release/logs/loss_contributor_by_strategy.csv`
- 완료 조건:
  - 상위 5개 마켓과 상위 2개 전략의 손실 기여율(%) 명시.

2. 전략 내부 필터 1차 보정 (Breakout/Scalping 우선)
- 목표: trade count 유지하면서 expectancy/profitable ratio 개선.
- 변경 우선순위:
  - Breakout: 약한 신호 구간 강도/유동성 하한 상향
  - Scalping: 변동성/유동성 불리 구간 진입 억제 강화
- 완료 조건:
  - `core_full.avg_total_trades >= 10` 유지
  - `core_full.avg_expectancy_krw` 절대값 개선(기존 -13.1301 대비 상승)

3. realdata matrix 재실행
- 명령:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
- 완료 조건:
  - 최신 `profitability_gate_report_realdata.json` 수치 갱신 및 비교표 작성.

## P1 (P0 다음)
1. candidate tuning objective 전환
- 목표: trade-density 중심에서 edge-quality 중심으로 평가 순서 변경.
- 작업:
  - tuning summary 정렬 기준에 `avg_expectancy_krw`, `profitable_ratio` 가중치 상향.
- 완료 조건:
  - top combo가 PF/거래수만 높은 조합이 아닌 expectancy 개선 조합으로 선택.

2. profile parity 회귀 체크 자동화
- 목표: 다시 profile 동일화 회귀 방지.
- 작업:
  - CI 또는 스크립트 단계에서 `profiles_identical_by_dataset=false` 검증 추가.
- 완료 조건:
  - 회귀 시 hard-fail 또는 명시 warning 발생.

## P2 (후속)
1. preloaded TF 활용 메트릭 주간 리포트화
- 작업:
  - `used_preloaded_tf_5m`, `used_preloaded_tf_1h`, `used_resampled_tf_fallback` 집계.
- 완료 조건:
  - 주간 요약에 fallback ratio 포함.

2. 운영 패키지 최종화
- 작업:
  - 릴리즈 단위(압축/버전 태그/체크섬/노트) 정리.
- 완료 조건:
  - 개인 운영 재현 절차 1회 end-to-end 통과.

## Quick Runbook
1. 수집 포함 전체 루프
- `python scripts/run_realdata_candidate_loop.py`

2. 수집 생략 matrix만
- `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`

3. exploratory 점검
- `python scripts/run_profitability_exploratory.py`

4. preset 적용 점검
- `python scripts/apply_trading_preset.py -Preset safe`
- `python scripts/apply_trading_preset.py -Preset active`


## Stage 15 Progress Update (2026-02-13, late session)

### Completed in this context
- Added loss-contributor analysis script:
  - `scripts/analyze_loss_contributors.py`
  - outputs:
    - `build/Release/logs/loss_contributor_by_market.csv`
    - `build/Release/logs/loss_contributor_by_strategy.csv`
- Re-ran realdata candidate matrix loop without refetch/tune:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
- Implemented first-pass strategy filter tightening (internal logic only):
  - `src/strategy/BreakoutStrategy.cpp`
  - `src/strategy/ScalpingStrategy.cpp`

### Latest candidate snapshot (core_full)
- avg_profit_factor: `1.7709`
- avg_total_trades: `12.4167`
- avg_expectancy_krw: `-5.9749`
- profitable_ratio: `0.4167`
- gate_pass: `false`

### Delta vs earlier snapshot in this stage
- avg_expectancy_krw improved from `-13.1301` to `-5.9749`.
- avg_total_trades stayed above minimum (`>= 10`).
- Remaining blockers: profitable ratio and expectancy still below gate thresholds.

### Current loss concentration (core_full)
- Top markets by loss share:
  - KRW-LINK: `24.4740%`
  - KRW-DOGE: `22.8084%`
  - KRW-XRP: `19.0481%`
  - KRW-ADA: `13.5947%`
  - KRW-AVAX: `9.0489%`
- Top strategies by loss share:
  - Breakout Strategy: `58.6922%`
  - Advanced Scalping: `36.2952%`

### Next P0/P1 focus
- P0-2 continuation: convert at least two loss-heavy real markets to non-negative expectancy while keeping avg_total_trades >= 10.
- P1-1: adjust tuning objective weighting to prioritize expectancy/profitable_ratio over trade density.

### New automation loop (added)
- Script: `scripts/run_candidate_auto_improvement_loop.py`
- Loop flow:
  - realdata matrix run -> tuning combo search -> best combo apply -> re-validation
  - optional loss-contributor analysis per iteration
- Temporary pause conditions:
  - max iteration reached
  - max runtime minutes reached
  - no objective improvement for N consecutive iterations
- Main outputs:
  - `build/Release/logs/candidate_auto_improvement_iterations.csv`
  - `build/Release/logs/candidate_auto_improvement_summary.json`
- Quick run:
  - `python scripts/run_candidate_auto_improvement_loop.py -MaxIterations 3 -MaxRuntimeMinutes 90 -RunLossAnalysis`

## Stage 15 Strategy Refactor Update (2026-02-13, current)

### Code changes completed
- `src/strategy/ScalpingStrategy.cpp`
  - Added higher-timeframe trend alignment gate (`5m/1h` bias) before entry.
  - Connected trend bias to score/strength-floor/edge gate/RR floor/position scaling.
  - Added candle-order normalization guard for trend-bias calculations (descending timestamp fallback).
- `src/strategy/BreakoutStrategy.cpp`
  - Added candle-order normalization guard for directional-bias calculations.
  - Normalized `5m` candles before trend-bias and breakout analysis in both `generateSignal` and `shouldEnter`.

### Build and script verification
- Build PASS:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release -j 6`
- Exploratory script PASS (execution):
  - `python scripts/run_profitability_exploratory.py`
- Preset script PASS (execution):
  - `python scripts/apply_trading_preset.py -Preset safe -ConfigPath .\build\Release\logs\verify_preset_safe.json -SourceConfigPath .\config\config.json`
  - `python scripts/apply_trading_preset.py -Preset active -ConfigPath .\build\Release\logs\verify_preset_active.json -SourceConfigPath .\config\config.json`

### Latest realdata candidate re-check (core_full)
- Command:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
- Result:
  - `overall_gate_pass=false`
  - `avg_profit_factor=1.1106` (PF gate pass)
  - `avg_total_trades=16.3333` (trade-count gate pass)
  - `avg_expectancy_krw=-13.5864` (expectancy gate fail)
  - `profitable_ratio=0.2222` (profitable-ratio gate fail)
  - `core_vs_legacy.gate_pass=false` (expectancy delta fail: `-5.6087 < -5.0`)

### Immediate next action
- Keep this strategy refactor baseline and run wider tuning scenarios to recover:
  - expectancy (`>= 0`)
  - profitable ratio (`>= 0.55`)
  - core-vs-legacy expectancy delta (`>= -5.0`)

## Stage 15 Wide-Loop Pass #1 (2026-02-13, diverse_wide)

### Run setup
- Command:
  - `python .\scripts\run_candidate_auto_improvement_loop.py -MaxIterations 1 -TuneScenarioMode diverse_wide -TuneMaxScenarios 12 -MaxRuntimeMinutes 180`
- Notes:
  - realdata baseline/post-apply validation included (`-SkipFetch` path via loop default).
  - tune phase evaluated 12 diverse-wide scenarios.

### Result summary
- Loop status: `paused_max_iterations` (configured 1 iteration complete)
- Selected combo by loop objective: `scenario_diverse_wide_002`
- Post-apply core_full:
  - `avg_profit_factor=1.1054` (pass)
  - `avg_total_trades=16.5556` (pass)
  - `avg_expectancy_krw=-13.4633` (fail)
  - `profitable_ratio=0.2778` (fail)
  - `overall_gate_pass=false`
- core-vs-legacy:
  - `delta_avg_profit_factor=-0.0561` (threshold `>= -0.05`, fail)
  - `delta_avg_expectancy_krw=-6.2160` (threshold `>= -5.0`, fail)

### Interpretation
- Trade-density and PF are stable, but expectancy/profitable-ratio remain structural bottlenecks.
- Candidate improvement from this pass is marginal versus baseline:
  - expectancy: `-13.5864 -> -13.4633`
  - profitable ratio: `0.2222 -> 0.2778`
- Next step should prioritize strategy-level loss suppression on recurring negative regimes/markets rather than further broad gate relaxation.

## Stage 15 Pattern-Driven Entry Refactor (2026-02-14)

### What changed (approach shift)
- Entry criteria were changed from static threshold-only tuning to pattern-aware gating using realized trade outcomes.
- Implemented strategy+regime loss-pattern reinforcement in both live/backtest entry paths:
  - `src/engine/TradingEngine.cpp`
  - `src/backtest/BacktestEngine.cpp`
- Added backtest JSON pattern summaries so winning/losing entry patterns are measurable directly from historical runs:
  - `include/backtest/BacktestEngine.h`
  - `src/backtest/BacktestEngine.cpp`
  - `src/main.cpp` (`--backtest --json` payload extended with `pattern_summaries`)

### New analysis tool
- Added:
  - `scripts/analyze_entry_pattern_bias.py`
- Output artifacts:
  - `build/Release/logs/entry_patterns_winning.csv`
  - `build/Release/logs/entry_patterns_losing.csv`
  - `build/Release/logs/entry_pattern_recommendations.json`

### Pattern snapshot (12 datasets)
- Losing examples:
  - `Advanced Scalping / RANGING / strength_high / ev_high / rr_high`:
    - trades `33`, win_rate `0.1515`, avg_profit `-24.0948`
  - `Advanced Scalping / TRENDING_UP / strength_high / ev_high / rr_high`:
    - trades `17`, win_rate `0.1765`, avg_profit `-22.0091`
  - `Breakout Strategy / RANGING / strength_mid / ev_high / rr_high`:
    - trades `17`, win_rate `0.1765`, avg_profit `-30.4338`
- Winning example:
  - `Breakout Strategy / TRENDING_UP / strength_low / ev_high / rr_high`:
    - trades `48`, win_rate `0.5833`, avg_profit `37.3067`
- Recommendation highlights:
  - `Advanced Scalping / RANGING`: `recommended_min_strength=0.62`, `recommend_block=true` (medium confidence)
  - `Advanced Scalping / TRENDING_UP`: `recommended_min_strength=0.62`, `recommend_block=true` (medium confidence)
  - `Breakout Strategy / RANGING`: `recommended_min_strength=0.62`, `recommend_block=true` (low confidence)

### Re-check after applying pattern-aware gate
- Command:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
- Core result:
  - `avg_profit_factor=1.1346` (pass)
  - `avg_total_trades=15.9444` (pass)
  - `avg_expectancy_krw=-11.0209` (fail)
  - `profitable_ratio=0.2778` (fail)
  - `overall_gate_pass=false`
- vs pre-refactor snapshot in this stage:
  - expectancy improved: `-13.4633 -> -11.0209`
  - PF improved: `1.1054 -> 1.1346`
  - core-vs-legacy gate recovered to pass:
    - `delta_avg_profit_factor=+0.0031` (pass)
    - `delta_avg_expectancy_krw=-3.0131` (pass)

## Stage 15 Pattern Loop Follow-up (2026-02-14, current)

### Sequence executed in this context
1. Root-cause rerun:
   - `scripts/analyze_loss_contributors.py`
   - `scripts/analyze_entry_pattern_bias.py -MaxDatasets 18 -MinPatternTrades 6`
2. Strategy filter update:
   - `src/strategy/BreakoutStrategy.cpp`
   - `src/strategy/ScalpingStrategy.cpp`
3. Validation loop:
   - `scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
   - `scripts/tune_candidate_gate_trade_density.py -ScenarioMode diverse_wide -MaxScenarios 4`

### Key observations
- Loss concentration after first strict block was:
  - `Breakout Strategy` loss share `85.7444%`
  - `Advanced Momentum` loss share `14.2556%`
  - (`Scalping` loss share effectively removed by over-blocking, but trade count dropped too far)
- Pattern bias stayed concentrated in:
  - `Breakout Strategy / TRENDING_UP / strength_mid` (negative)
  - `Breakout Strategy / TRENDING_UP / strength_low` (positive)

### Latest baseline snapshot (after second filter adjustment)
- Command:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
- `core_full`:
  - `avg_profit_factor = 1.1049` (PASS)
  - `avg_total_trades = 10.7778` (PASS)
  - `avg_expectancy_krw = -10.9259` (FAIL)
  - `profitable_ratio = 0.4444` (FAIL)
  - `overall_gate_pass = false`

### Latest short wide tuning (4 scenarios)
- Command:
  - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode diverse_wide -MaxScenarios 4`
- Best-by-gap candidate from summary:
  - `scenario_diverse_wide_003`
  - `avg_profit_factor = 1.1049`
  - `avg_total_trades = 10.7778`
  - `avg_expectancy_krw = -10.9259`
  - `profitable_ratio = 0.4444`
- Current blockers remain:
  - expectancy gap to gate: `+10.9259 KRW/trade`
  - profitable ratio gap to gate: `+0.1056`

## Stage 15 Core-Lifecycle Alignment Update (2026-02-14)

### Completed in this pass
- Integrated strategy lifecycle hooks across live/backtest for core and legacy paths:
  - `onSignalAccepted` is now called for non-grid live entries (`src/engine/TradingEngine.cpp`).
  - `onSignalAccepted` is now called for backtest entries (`src/backtest/BacktestEngine.cpp`).
  - `updateStatistics` is now called on backtest full exits with fee-inclusive net PnL (`src/backtest/BacktestEngine.cpp`).
- Unified partial-exit accounting to TradeHistory path:
  - `RiskManager::partialExit` now routes through `applyPartialSellFill` and keeps TP1 breakeven behavior (`src/risk/RiskManager.cpp`).
  - Live/paper partial sells now apply actual filled quantity accounting (`src/engine/TradingEngine.cpp`).
- Unified strategy stat PnL basis in live full exits:
  - `updateStatistics` now receives fee-inclusive net PnL on full close (`src/engine/TradingEngine.cpp`).
  - Partial-fill close paths no longer trigger premature strategy close-stat updates.

### Verification run
- Build: PASS
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release --target AutoLifeTrading`
- Spot backtest smoke: PASS
  - `build/Release/AutoLifeTrading.exe --backtest .\data\backtest\sample_trend_1m.csv --json`
  - `build/Release/AutoLifeTrading.exe --backtest .\data\backtest\auto_sim_500.csv --json`
- Exploratory matrix rerun: PASS (script), gate currently FAIL
  - `python .\scripts\run_profitability_exploratory.py`
  - report: `build/Release/logs/profitability_gate_report_exploratory.json`

### Current exploratory note
- `overall_gate_pass = false`
- `core_full` in latest exploratory report:
  - `avg_profit_factor = 0`
  - `avg_expectancy_krw = 31.6378`
  - `avg_total_trades = 4.3333`
  - `profitable_ratio = 0.6667`
- This indicates metric semantics changed after lifecycle/accounting fixes and gate thresholds should be re-baselined before candidate promotion tuning.

### Post-alignment candidate re-check (realdata, no fetch/tune)
- Command:
  - `python .\scripts\run_realdata_candidate_loop.py -SkipFetch -SkipTune`
- `core_full` snapshot:
  - `avg_profit_factor = 0.4242` (FAIL)
  - `avg_total_trades = 4.2778` (FAIL)
  - `avg_expectancy_krw = -11.8951` (FAIL)
  - `profitable_ratio = 0.2778` (FAIL)
  - `runs_used_for_gate = 18`
  - `excluded_low_trade_runs = 8`
  - `overall_gate_pass = false`
- Artifact:
  - `build/Release/logs/profitability_gate_report_realdata.json`

## Core Migration Readiness TODO (2026-02-14)

### Goal
- Prove core path is consistently better (or at least not worse) than legacy, then execute staged full migration and legacy cleanup.

### Phase A: Measurement reliability (in progress)
1. Align metric semantics across live/backtest/core/legacy.
   - status: in progress
   - done now: profit-factor no-loss handling aligned to `99.9` semantics in backtest/live edge stats.
2. Keep strategy lifecycle hooks consistent.
   - status: done (entry accept + full-exit stats wiring added)
3. Re-run baseline reports after semantic alignment.
   - status: pending

### Phase B: Core-vs-legacy proof criteria
1. Repeatable superiority window.
   - criteria: `core_full` >= `legacy_default` for at least 5 consecutive comparable runs.
2. Gate pass criteria.
   - criteria: pass `profit_factor`, `expectancy`, `profitable_ratio`, `avg_total_trades`.
3. CI stability criteria.
   - criteria: `CI PR Gate` and strict chains stable with no regression trend.

### Phase C: Progressive cutover
1. Set core as default profile/preset.
2. Keep legacy as rollback-only for a short observation window.
3. Remove legacy-only branches/flags/scripts after observation.
4. Finalize docs/runbook to core-only operation.

### Immediate next execution
1. Re-run exploratory and realdata candidate loops with current fixes.
2. Refresh deltas (`core_full` vs `legacy_default`) using updated metric semantics.
3. If core still behind, continue strategy-side loss suppression and re-tune.

### Execution update (2026-02-14, after TODO activation)
- Completed now:
  1. PF semantic alignment patch applied (no-loss -> `99.9`) in:
     - `src/backtest/BacktestEngine.cpp`
     - `src/engine/TradingEngine.cpp`
  2. Rebuild and rerun:
     - `scripts/run_profitability_exploratory.py`
     - `scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
- Latest checkpoints:
  - exploratory:
    - `core_full.avg_profit_factor = 66.6`
    - `core_full.avg_expectancy_krw = 31.6378`
    - `core_full.avg_total_trades = 4.3333`
    - `core_full.profitable_ratio = 0.6667`
    - `core_vs_legacy.gate_pass = true`
  - realdata candidate:
    - `core_full.avg_profit_factor = 22.6242` (PASS)
    - `core_full.avg_expectancy_krw = -11.8951` (FAIL)
    - `core_full.avg_total_trades = 4.2778` (FAIL)
    - `core_full.profitable_ratio = 0.2778` (FAIL)
    - `core_vs_legacy.gate_pass = true`
- Immediate blocker remains:
  - trade density + profitable ratio + expectancy (not PF anymore).

### Tune kick-off result (2026-02-14, diverse_light x4)
- Command:
  - `python .\scripts\tune_candidate_gate_trade_density.py -ScenarioMode diverse_light -MaxScenarios 4`
- Output:
  - `build/Release/logs/candidate_trade_density_tuning_summary.csv`
  - `build/Release/logs/candidate_trade_density_tuning_summary.json`
- Best-by-current-sort:
  - `scenario_diverse_light_002`
  - `avg_profit_factor = 18.1398`
  - `avg_expectancy_krw = -11.3083`
  - `avg_total_trades = 4.4118`
  - `profitable_ratio = 0.2941`
  - `overall_gate_pass = false`
- Note:
  - `scenario_diverse_light_001` had better expectancy (`-9.103`) but lower profitable ratio/trade density, so objective/sort weighting still needs adjustment for migration criteria.

### Data parity update (2026-02-14)
- Validation/tuning scripts now support live-parity dataset mode:
  - `-RealDataOnly`
  - `-RequireHigherTfCompanions`
- Applied scripts:
  - `scripts/run_realdata_candidate_loop.py`
  - `scripts/tune_candidate_gate_trade_density.py`
- Behavior:
  - only `backtest_real` `*_1m_*` primary datasets are used
  - each primary dataset must have matching `5m/60m/240m` companion CSV in same folder
- Verified commands:
  - `python .\scripts\run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions`
  - `python .\scripts\tune_candidate_gate_trade_density.py -ScenarioMode legacy_only -MaxScenarios 1 -RealDataOnly -RequireHigherTfCompanions`

### Backtest UX update (2026-02-14)
- `src/main.cpp` updated for backtest usability and live-parity enforcement.
- CLI backtest new option:
  - `--require-higher-tf-companions`
  - behavior: only allows `upbit_*_1m_*` primary CSV that has matching `5m/60m/240m` companions.
- Interactive backtest updates:
  - data source now supports `3=실데이터 목록 선택`
  - optional live-parity MTF enforcement prompt (default: ON)
  - companion validation failure shows explicit missing TF guidance.
- Verified:
  - pass: `--backtest .\data\backtest_real\upbit_KRW_BTC_1m_12000.csv --require-higher-tf-companions`
  - fail (expected): non-upbit sample CSV with companion requirement

### Strategy patch + recheck (2026-02-14, latest)
- Strategy-side updates completed:
  - `src/strategy/BreakoutStrategy.cpp`
    - `generateSignal()` now enforces `shouldGenerateBreakoutSignal()` hard gate before score aggregation.
    - Added breakout quality floor (`computeBreakoutSignalQualityFloor`) to block weak setups.
    - Aligned `shouldEnter()` with same quality-floor logic.
  - `src/strategy/ScalpingStrategy.cpp`
    - Populated `used_preloaded_tf_5m`, `used_preloaded_tf_1h`, `used_resampled_tf_fallback`.
    - Added TRENDING_UP dual-quality gate (strict path + flow-confirmed alternate path).
    - Added preloaded `5m` alignment contribution to score.
    - Added anti-chase guard for overheated weak-flow entries.
- Build verification:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release --target AutoLifeTrading` PASS
- Realdata candidate recheck:
  - command:
    - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions`
  - core_full:
    - `avg_profit_factor = 11.2958` (PASS)
    - `avg_total_trades = 4.5556` (FAIL)
    - `avg_expectancy_krw = -17.2048` (FAIL)
    - `profitable_ratio = 0.1111` (FAIL)
    - `overall_gate_pass = false`
- Loss concentration after patch:
  - strategy loss share:
    - `Advanced Scalping = 44.1328%`
    - `Advanced Momentum = 43.7480%`
    - `Breakout Strategy = 12.1192%`
  - entry-pattern hotspots (`MinPatternTrades=4`):
    - `Advanced Scalping / TRENDING_UP / strength_high`: `25 trades`, `avg_profit_krw=-12.4043`
    - `Advanced Momentum / TRENDING_UP / strength_mid`: `4 trades`, `avg_profit_krw=-61.4433`
    - `Advanced Momentum / TRENDING_UP / strength_high`: `4 trades`, `avg_profit_krw=-9.9829`
  - top markets:
    - `KRW-ADA`, `KRW-AVAX`, `KRW-XRP`, `KRW-SUI`, `KRW-DOT`
- Next focus (P0):
  1. Reduce `Advanced Momentum` losing entries in TRENDING_UP (`strength_mid/high` buckets).
  2. Add market-level guardrails for repeated loss clusters (`ADA/AVAX/XRP/SUI/DOT`) without globally suppressing trades.
  3. Re-run realdata loop and check if `avg_total_trades >= 10` can be recovered with non-negative expectancy trend.
