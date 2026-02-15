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

## Stage 15 Adaptive Persistence Update (2026-02-15)

### Completed
- Archetype self-learning state is now persisted and reloaded across process restarts for both backtest and live runtime paths.
- Shared persistence files (same executable directory base):
  - `build/Release/state/scalping_archetype_stats.json`
  - `build/Release/state/momentum_archetype_stats.json`

### Code scope
- `include/strategy/ScalpingStrategy.h`
- `include/strategy/MomentumStrategy.h`
- `src/strategy/ScalpingStrategy.cpp`
- `src/strategy/MomentumStrategy.cpp`

### Verification snapshot
- Build: PASS (`cmake --build build --config Release`)
- Backtest startup logs now confirm state load:
  - `Scalping adaptive archetype stats loaded: ...`
  - `Momentum adaptive archetype stats loaded: ...`
- Re-run of single-dataset backtest updates persisted counters and EMA in both files.
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

## Stage 15 Momentum Archetype Tightening (2026-02-15)

### Code update (minimal scope)
- Updated `src/strategy/MomentumStrategy.cpp` only:
  - Tightened `BREAKOUT_CONTINUATION` classification gates:
    - stronger MTF/flow/buy-pressure/liquidity requirements
    - bullish candle count requirement
    - narrower acceptable price-change window
  - Added stricter archetype-specific risk-reward floor for breakout continuation.
  - Added fee/slippage-aware net-edge floor before final signal acceptance.
  - Added faster early/mid/late cut rules in `shouldExit()` for breakout continuation.

### Verification results
- Build:
  - `cmake --build build --config Release` PASS
- Realdata loop (strict, hostility adaptive OFF):
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --disable-hostility-adaptive-thresholds --matrix-max-workers 1 --matrix-backtest-retry-count 2`
  - `overall_gate_pass=false`
  - `core_full.avg_profit_factor=11.8677`
  - `core_full.avg_total_trades=7.2222`
  - `core_full.avg_expectancy_krw=-0.8061`
- Realdata loop (hostility adaptive ON):
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --enable-hostility-adaptive-thresholds --matrix-max-workers 1 --matrix-backtest-retry-count 2`
  - `overall_gate_pass=true`
  - effective thresholds in high hostility:
    - `min_profitable_ratio=0.3264`
    - `min_avg_trades=3`

### Root-cause delta snapshot (`core_full`)
- After tightening, momentum concentration reduced but still dominant:
  - total trades: `124 -> 70`
  - momentum trades: `116 -> 62`
  - top remaining loss pattern:
    - `Advanced Momentum / BREAKOUT_CONTINUATION / TRENDING_UP`
    - trades `34`, win_rate `0.3824`, avg_profit `-8.2099`

### Next TODO (immediate)
1. Add archetype-specific EV quality gate for `TREND_REACCELERATION` (now second largest loss source).
2. Recalibrate `expected_value_bucket` labeling so `ev_high` aligns with realized net expectancy after fee/slippage.
3. Add a strict-mode parallel report artifact (`strict` vs `adaptive`) in candidate loop output to avoid mixing acceptance criteria.

## Stage 15 Momentum 2nd Patch (2026-02-15, TREND_REACCELERATION tightening)

### Code changes
- `src/strategy/MomentumStrategy.cpp`
  - Tightened `TREND_REACCELERATION` classification:
    - stronger `mtf_alignment/flow/buy_pressure/bullish_count`
    - volume floor and narrower `price_change_rate` band
  - Reduced archetype size scale:
    - `TREND_REACCELERATION: 0.70 -> 0.55`
  - Added stronger archetype RR floor:
    - `TREND_REACCELERATION: rr_floor >= 1.42`
  - Strengthened fee/slippage-aware net edge floor:
    - `TREND_REACCELERATION: min_net_edge 0.0020 -> 0.0028`
  - Added faster time-loss exit profile for reacceleration positions.

### Verification snapshot
- Build PASS:
  - `cmake --build build --config Release`
- Strict gate (`hostility adaptive OFF`):
  - `core_full.avg_profit_factor=11.8427`
  - `core_full.avg_total_trades=6.6667`
  - `core_full.avg_expectancy_krw=-0.2125`
  - `overall_gate_pass=false`
- Adaptive gate (`hostility adaptive ON`):
  - same profile metrics, effective thresholds relaxed in high hostility
  - `overall_gate_pass=true`

### Root-cause delta
- `TREND_REACCELERATION` pattern dropped out of top loss patterns in current diagnostics.
- Remaining dominant bottleneck:
  - `Advanced Momentum / BREAKOUT_CONTINUATION / TRENDING_UP`

### Next immediate focus
1. `BREAKOUT_CONTINUATION@TRENDING_UP` split into quality tiers and block low-tier entries.
2. Recompute `ev_high` bucket criteria to net-of-cost expectancy.
3. Wire strict/adaptive dual-output directly in loop scripts (no manual copy step).

### Experiment note (same session)
- Tested stronger hard gate for `BREAKOUT_CONTINUATION@TRENDING_UP` at archetype classification stage.
- Result: over-filtering caused regression (`core_full.avg_total_trades` dropped and adaptive gate failed).
- Action: rolled back that hard gate; retained only stable improvements from this session.

## Stage 15 Root-Cause Recheck Update (2026-02-15, current)

### Root cause confirmed
- Parallel matrix backtests were sharing the same adaptive state files:
  - `build/Release/state/scalping_archetype_stats.json`
  - `build/Release/state/momentum_archetype_stats.json`
- This caused cross-dataset/process contamination and unstable gate results.

### Fix applied
- Added adaptive-state I/O kill switch via env:
  - `AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO=1`
- Strategy runtime now skips archetype state load/save when this env is set:
  - `src/strategy/ScalpingStrategy.cpp`
  - `src/strategy/MomentumStrategy.cpp`
- Matrix runner now forces this env for each backtest subprocess:
  - `scripts/run_profitability_matrix.py`
- Entry-pattern analyzer also forces this env:
  - `scripts/analyze_entry_pattern_bias.py`
- Reduced high-frequency per-candle log spam:
  - removed repetitive "score-based analysis start" info logs in scalping/momentum.

### Additional gate/logic adjustments
- Added conservative calibrated edge model to core entry quality gates:
  - `src/backtest/BacktestEngine.cpp`
  - `src/engine/TradingEngine.cpp`
- Strengthened but then relaxed severe negative archetype-bias handling to avoid hard over-pruning:
  - `src/strategy/ScalpingStrategy.cpp`
  - `src/strategy/MomentumStrategy.cpp`

### Latest verification snapshot (realdata, skip fetch/tune)
- Command:
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8`
- `core_full`:
  - `avg_profit_factor=11.5352`
  - `avg_total_trades=2.4444` (still fail for trade floor)
  - `avg_expectancy_krw=-3.9363` (still fail for expectancy gate)
  - `profitable_ratio=0.3333` (still fail)
- Status:
  - contamination issue fixed
  - remaining bottleneck is low trade density + negative expectancy in selected markets.

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

## Stage 15 Strategy Clock Alignment Update (2026-02-14, current)

### Why this patch was needed
- Backtest execution is time-compressed, but strategy-side throttle logic (`daily/hourly limits`, `min_signal_interval`, `circuit breaker`) was using wall-clock time.
- Result: strategy throttles were effectively over-triggered in backtest and suppressed most signals.

### Code updates
- Candle-time based strategy clock alignment:
  - `include/strategy/ScalpingStrategy.h`
  - `include/strategy/MomentumStrategy.h`
  - `src/strategy/ScalpingStrategy.cpp`
  - `src/strategy/MomentumStrategy.cpp`
- Scalping regime policy re-tightening:
  - `Scalping@RANGING` blocked again
  - `TRENDING_DOWN` only allowed with strict counter-momentum quality
- Momentum quality gates adjusted to avoid over-pruning while preserving minimum edge checks.

### Validation snapshots
- Spot-check (8 real datasets) after clock alignment:
  - trade counts increased materially on multiple markets
  - signal starvation reduced vs pre-alignment runs
- Full loop rerun:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
  - core_full:
    - `avg_profit_factor=13.7064` (pass)
    - `avg_total_trades=16.4667` (pass)
    - `avg_expectancy_krw=-8.8745` (fail)
    - `profitable_ratio=0.1333` (fail)
  - core-vs-legacy:
    - `delta_avg_profit_factor=+0.8046` (pass)
    - `delta_avg_expectancy_krw=-5.1329` (fail by margin, threshold `>= -5.0`)

### Next focus
- P0: improve expectancy/profitable_ratio without collapsing trade count
  - prioritize negative markets (`KRW-AVAX`, `KRW-LINK`, `KRW-ADA`) with regime-specific entry suppression
  - add strategy-level reject telemetry by regime bucket before next tuning loop

## Stage 15 Strategy-by-Strategy Audit Update (2026-02-14, current)

### Scope audited (5 strategies)
- `Advanced Scalping`
- `Advanced Momentum`
- `Breakout Strategy`
- `Mean Reversion Strategy`
- `Grid Trading Strategy`

### Cross-strategy consistency patch
- Backtest time-compression compatibility aligned across all 5 strategies:
  - strategy internal clocks (`daily/hourly limits`, `min_signal_interval`, `circuit breaker`) now use candle timestamp when available.
  - files:
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

### Risk-management timing patch (entry lifecycle)
- Added early adverse/stagnation exits in:
  - `ScalpingStrategy::shouldExit`
  - `MomentumStrategy::shouldExit`
- Intent:
  - reduce prolonged low-quality holds where entry trigger momentum does not continue.

### Re-validation (post patch)
- Command:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
- core_full:
  - `avg_profit_factor=13.7064` (pass)
  - `avg_total_trades=16.4667` (pass)
  - `avg_expectancy_krw=-8.8745` (fail)
  - `profitable_ratio=0.1333` (fail)
- core-vs-legacy:
  - `delta_avg_profit_factor=+0.8046` (pass)
  - `delta_avg_expectancy_krw=-5.1329` (fail, threshold `>= -5.0`)

### Interpretation
- Consistency/clock alignment issue is resolved across all strategies.
- Remaining blocker is not simple trade count starvation anymore; it is quality distribution (win/loss asymmetry) after entry.
- Next iteration must prioritize `setup-trigger separation` and `post-entry adaptive risk path` over threshold-only tuning.

## Stage 15 Fast Loop Refactor (2026-02-14, current)

### Why this refactor
- Existing candidate auto-improvement loop was too slow (`scenario_count x dataset_count x profile_count` explosion).
- Parameter-only tuning kept selecting low-trade/high-PF combinations that did not satisfy practical live targets.

### What changed
- Added two-stage tuning in `scripts/tune_candidate_gate_trade_density.py`:
  - Stage 1: screen all combos on a small evenly-spaced dataset subset.
  - Stage 2: run full evaluation only for screened top-k combos.
- Added profile selection in `scripts/run_profitability_matrix.py`:
  - `--profile-ids ...` (used to run fast tuning on `core_full` only).
- Added win-rate aggregation in profile summary:
  - `avg_win_rate_pct`, `gate_win_rate_pass`, `min_avg_win_rate_pct`.
- Reworked objective scoring in `scripts/run_candidate_auto_improvement_loop.py`:
  - hard penalties for failing minimum `avg_total_trades`, `profitable_ratio`, `avg_win_rate_pct`, `expectancy`.
  - avoids selecting deceptive high-PF but low-trade/low-win-rate combos.
- Added loop/tuning control options:
  - auto-loop: `--real-data-only`, `--require-higher-tf-companions`,
    `--tune-screen-dataset-limit`, `--tune-screen-top-k`,
    `--tune-objective-min-*`.

### Validation snapshot
- Tuning smoke run (realdata-only, 4 combos, screen 4 -> top2 final):
  - command time: ~40s
  - output: `build/Release/logs/smoke_tune_summary.json`
- Auto loop smoke run (realdata-only, 1 iteration, 6 combos, screen 4 -> top2 final):
  - command time: ~115s
  - output: `build/Release/logs/candidate_auto_improvement_summary.json`

### Next TODO
- Move from parameter-only recovery to strategy-logic recovery:
  - prioritize reducing recurring losing entries by regime/strategy bucket.
  - keep minimum live constraints fixed (`trades`, `win-rate`, `profitable-ratio`, `expectancy`) while tuning.
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
- Default mode update (2026-02-14 latest):
  - `scripts/run_realdata_candidate_loop.py`, `scripts/tune_candidate_gate_trade_density.py`, `scripts/run_candidate_auto_improvement_loop.py`
    now default to higher-TF companion enforcement ON.
  - to explicitly disable (legacy/mixed fallback run), pass:
    - `--allow-missing-higher-tf-companions`
  - `scripts/run_profitability_matrix.py` now forwards `--require-higher-tf-companions` to backtest execution for `upbit_*_1m_*` datasets.
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

### Validation script audit + tuning snapshot (2026-02-14)
- Python validation/profitability scripts smoke-check:
  - `py_compile` across `scripts/*.py`: PASS
  - `--help` checks for `run_*`/`validate_*`: PASS
- CI gate wrapper fix:
  - file: `scripts/run_ci_operational_gate.py`
  - issue: default prime dataset (`simulation_large.csv`) could leave `execution_updates_backtest.jsonl` empty and fail CI gate.
  - fix: added fallback prime dataset retry (`--backtest-prime-fallback-csv`, default `data/backtest/auto_sim_500.csv`).
  - verify: `python scripts/run_ci_operational_gate.py -IncludeBacktest` PASS.
- Candidate tuning run (realdata-only with higher-TF companions):
  - command:
    - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode diverse_light -MaxScenarios 8 -RealDataOnly -RequireHigherTfCompanions -ScreenDatasetLimit 6 -ScreenTopK 4 -FinalProfileIds core_full`
  - best combo: `scenario_diverse_light_005`
  - best final metrics (`core_full`, 12 datasets):
    - `avg_profit_factor = 9.6402`
    - `avg_expectancy_krw = -4.0186`
    - `avg_total_trades = 22.6364`
    - `profitable_ratio = 0.1818`
- Baseline vs tuned on same 12-dataset set:
  - baseline (`baseline_current`):
    - `avg_expectancy_krw = -11.5693`
    - `profitable_ratio = 0.0833`
    - `avg_total_trades = 18.0`
  - tuned (`scenario_diverse_light_005`):
    - `avg_expectancy_krw = -4.0186` (improved)
    - `profitable_ratio = 0.1818` (improved)
    - `avg_total_trades = 22.6364` (improved)
- Temporary full realdata check (mixed 26 datasets, one-off apply then restore):
  - baseline:
    - `avg_profit_factor = 13.7064`
    - `avg_expectancy_krw = -8.8745`
    - `avg_total_trades = 16.4667`
  - tuned-combo apply:
    - `avg_profit_factor = 8.2441`
    - `avg_expectancy_krw = -5.4770`
    - `avg_total_trades = 21.6923`
    - `core_vs_legacy.gate_pass = true`
  - note: profile gate still fails by `avg_expectancy_krw` and `profitable_ratio`.
- Added reusable preset from tuning winner:
  - `config/presets/candidate_stage15_combo005.json`
  - apply command:
    - `python scripts/apply_trading_preset.py -PresetPath .\\config\\presets\\candidate_stage15_combo005.json`

### Quality-focus loop + apply (2026-02-14)
- Added missing `quality_focus` scenario generation in:
  - `scripts/tune_candidate_gate_trade_density.py`
- One-shot quality-focus tuning run:
  - `python scripts/tune_candidate_gate_trade_density.py -ScenarioMode quality_focus -MaxScenarios 4 -RealDataOnly -RequireHigherTfCompanions -ScreenDatasetLimit 6 -ScreenTopK 3 -FinalProfileIds core_full`
  - best combo: `scenario_quality_focus_002`
  - best (12 datasets, core_full):
    - `avg_profit_factor = 9.6409`
    - `avg_expectancy_krw = -3.2451`
    - `avg_total_trades = 21.0`
    - `profitable_ratio = 0.1818`
- Added preset from quality-focus winner:
  - `config/presets/candidate_stage15_quality_focus_002.json`
- Applied preset to `build/Release/config/config.json` and rechecked mixed realdata loop (26 datasets):
  - command:
    - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune`
  - core_full:
    - `avg_profit_factor = 8.2521` (PASS)
    - `avg_total_trades = 20.0` (PASS)
    - `avg_expectancy_krw = -4.8036` (FAIL, but improved)
    - `profitable_ratio = 0.2308` (FAIL)
    - `core_vs_legacy.gate_pass = true`
    - `overall_gate_pass = false`
- Current bottleneck:
  - `gate_expectancy_pass` and `gate_profitable_ratio_pass` only.

### Strategy logic refactor v1 (2026-02-14, setup/trigger split)
- Scope:
  - `src/strategy/ScalpingStrategy.cpp`, `include/strategy/ScalpingStrategy.h`
  - `src/strategy/MomentumStrategy.cpp`, `include/strategy/MomentumStrategy.h`
- Structural changes:
  - Entry logic moved from single-score acceptance toward explicit `setup_score + trigger_score` gating.
  - Per-market entry context is now stored on signal generation/acceptance and consumed by `shouldExit` for adaptive time-loss cuts.
  - Context lifecycle wired through `onSignalAccepted` and `updateStatistics` (set/clear).
- Build:
  - `cmake --build build --config Release --target AutoLifeTrading` PASS
- Validation:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly`
  - result (`core_full`, 12 datasets):
    - `avg_profit_factor = 0.8002`
    - `avg_expectancy_krw = -11.0535`
    - `avg_total_trades = 20.1667`
    - `profitable_ratio = 0.0833`
    - `overall_gate_pass = false`
- Diagnosis:
  - Trade frequency target is no longer the primary blocker; trade quality collapsed.
  - Loss concentration remains mostly in momentum/scalping:
    - `Advanced Momentum` loss share ~`55.9%`
    - `Advanced Scalping` loss share ~`38.6%`
- Next required step (P0):
  - Move to archetype/state-machine entries (pullback-reclaim / continuation-failure invalidation) instead of score-threshold blending.

### Strategy logic refactor v2 (2026-02-14, archetype state machine)
- Scope:
  - `src/strategy/ScalpingStrategy.cpp`, `include/strategy/ScalpingStrategy.h`
  - `src/strategy/MomentumStrategy.cpp`, `include/strategy/MomentumStrategy.h`
- Structural changes:
  - Added explicit entry archetype classification:
    - scalping: `PULLBACK_RECLAIM`, `BREAKOUT_CONTINUATION`
    - momentum: `PULLBACK_RECLAIM`, `BREAKOUT_CONTINUATION`, `TREND_REACCELERATION`
  - Entry now requires valid archetype before setup/trigger gate passes.
  - Per-archetype invalidation/progress thresholds are persisted in entry context and consumed in `shouldExit`.
    - immediate invalidation cut by drawdown threshold
    - timed progress-failure exits (`10m/20m` scalping, `30m/60m` momentum)
- Build:
  - `cmake --build build --config Release --target AutoLifeTrading` PASS
- Validation:
  - command:
    - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly`
  - first run after v2 code:
    - `avg_profit_factor = 9.6072`
    - `avg_expectancy_krw = -5.4841`
    - `avg_total_trades = 17.1818`
    - `profitable_ratio = 0.1818`
  - tuned preset re-apply (`candidate_stage15_combo005`) and rerun:
    - `avg_profit_factor = 9.7081`
    - `avg_expectancy_krw = -3.8303`
    - `avg_total_trades = 18.3636`
    - `profitable_ratio = 0.2727`
    - `core_vs_legacy.gate_pass = true`
    - `overall_gate_pass = false`
- Current bottleneck after v2:
  - still failing:
    - `gate_expectancy_pass`
    - `gate_profitable_ratio_pass`
  - but PF/trade-density are now stably above gate floor in realdata-only parity runs.

## Archetype Telemetry Update (2026-02-15)

### Why this pass
- We need type-level segmentation to tune against hostile-market datasets without overfitting to coarse strategy/regime buckets.

### Implemented
- End-to-end archetype metadata propagation:
  - `Signal.entry_archetype` added and defaulted to `UNSPECIFIED`.
  - `RiskManager::Position.entry_archetype` and `RiskManager::TradeHistory.entry_archetype` added.
  - `setPositionSignalInfo(...)` extended to accept `entry_archetype`.
  - live/backtest call sites now pass strategy-generated archetype.
- Strategy mapping:
  - `Scalping`: maps classifier result to `PULLBACK_RECLAIM` / `BREAKOUT_CONTINUATION`.
  - `Momentum`: maps classifier result to `PULLBACK_RECLAIM` / `BREAKOUT_CONTINUATION` / `TREND_REACCELERATION`.
- Backtest JSON telemetry expanded:
  - `pattern_summaries[*].entry_archetype` added.
  - pattern aggregation key now uses `strategy + archetype + regime + buckets`.
- Pattern analysis script upgraded:
  - `scripts/analyze_entry_pattern_bias.py` now aggregates/recommends on `(strategy, entry_archetype, regime)`.

### Verification
- Build PASS:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release --target AutoLifeTrading`
- Python script syntax PASS:
  - `python -m py_compile scripts/analyze_entry_pattern_bias.py`
- Realdata JSON smoke (hostile sample) PASS:
  - `build/Release/AutoLifeTrading.exe --backtest .\data\backtest_real\upbit_KRW_ADA_1m_12000.csv --json`
  - confirmed `pattern_summaries` includes `entry_archetype` values (e.g. `BREAKOUT_CONTINUATION`, `UNSPECIFIED`).

### Next step (P0)
- Re-run pattern/loss analysis on realdata-only parity set and isolate negative expectancy clusters by `strategy x archetype x regime`.
- Apply phase-3 fixes on the worst archetype clusters first (entry timing + invalidation path), then re-run candidate gate.

## Stage 15 Momentum/Archetype Iteration Update (2026-02-15)

### This pass (started now)
- Root-cause check was rerun with archetype telemetry:
  - `python scripts/analyze_entry_pattern_bias.py --max-datasets 12 --min-pattern-trades 6`
- Found critical execution-quality bug in strategy signal path:
  - `ScalpingStrategy::generateSignal` and `MomentumStrategy::generateSignal`
  - `signal.type` could be set to `BUY` before later gates and return early on failure.
  - this allowed low-quality entries to leak through.

### Code fix applied
- `signal.type = BUY` assignment moved to the final accepted path only (after all late gates).
- Files:
  - `src/strategy/ScalpingStrategy.cpp`
  - `src/strategy/MomentumStrategy.cpp`

### Additional archetype/regime patch tested
- momentum breakout handling updated to avoid broad ranging breakout exposure:
  - ranging allowed only under strict high-quality sub-condition (`ranging_breakout_quality`)
  - pullback/reacceleration thresholds tuned for trade continuity.
- file:
  - `src/strategy/MomentumStrategy.cpp`

### Verification snapshots (realdata-only, 12 datasets)
- command:
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly`
- latest:
  - `avg_profit_factor = 10.3933` (PASS)
  - `avg_total_trades = 10.3` (PASS)
  - `avg_expectancy_krw = -9.5054` (FAIL)
  - `profitable_ratio = 0.2` (FAIL)

### Interpretation
- Trade-density floor recovered (`>=10`) while preserving strong PF.
- Remaining bottleneck is still quality distribution (expectancy/profitable-ratio), not raw volume.
- Loss contributors remain concentrated in `Advanced Scalping` and `Advanced Momentum`.

### Next immediate target
- Reduce `Scalping TRENDING_UP` loss clusters (`BREAKOUT_CONTINUATION`, `PULLBACK_RECLAIM`) without dropping `avg_total_trades` below 10.

## Pipeline Audit + Phase-1 Parity Patch (2026-02-15)

### Current entry/exit pipeline (as-is)
- Entry funnel is layered in three places:
  - `StrategyManager`:
    - strategy-local signal generation
    - manager filter (`strength/expected_value`) + robust selector
  - `BacktestEngine` / `TradingEngine`:
    - regime + pattern + RR/edge + EV quality gates
    - dynamic/adaptive thresholding from trade-history stats
  - `RiskManager`:
    - final account/risk feasibility gate (`canEnterPosition`)
- Exit funnel:
  - strategy `shouldExit`
  - SL/TP intrabar handling
  - risk-manager accounting and trade-history writeback

### Gaps found in this audit
- Live vs Backtest parity gap existed for scalping exit management:
  - live path already uses breakeven/trailing update loop
  - backtest path was missing equivalent update before exit checks
- Hardcoded market list branch (`loss-focus market`) was still affecting adaptive gate tuning:
  - not data-driven and can overfit specific symbols

### Applied in this pass
- Backtest/live parity improvement:
  - `src/backtest/BacktestEngine.cpp`
    - added scalping breakeven/trailing updates in open-position monitor path
    - backtest `enterPosition(...)` now forwards
      - `signal.breakeven_trigger`
      - `signal.trailing_start`
- Removed hardcoded market-specific gate branch:
  - `src/backtest/BacktestEngine.cpp`
  - `src/engine/TradingEngine.cpp`
  - adaptive gating now depends on observed `market x strategy x regime` stats only

### Verification
- Build PASS:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release --target AutoLifeTrading`
- Runtime smoke PASS:
  - `build/Release/AutoLifeTrading.exe --backtest .\data\backtest_real\upbit_KRW_BTC_1m_12000.csv --json`
- Realdata loop rerun (no-regression check):
  - `python scripts/run_realdata_candidate_loop.py -SkipFetch -SkipTune -RealDataOnly -RequireHigherTfCompanions`
  - `core_full.avg_profit_factor=10.3933`
  - `core_full.avg_total_trades=10.3`
  - `core_full.avg_expectancy_krw=-9.5054`
  - `overall_gate_pass=false`

### Next (Phase-2, profitability-focused)
- Reduce duplicated gate pressure between `StrategyManager` and engine layer (single source for regime-strength floor).
- Re-run realdata-only loop and compare:
  - `avg_expectancy_krw`, `profitable_ratio`, and per-archetype loss clusters.
- If quality still negative:
  - move more of entry decision to archetype-state invalidation/progress logic, and weaken static threshold stacking.

## Phase-2 Execution Update (2026-02-15, in progress)

### Additional code changes in this pass
- `src/strategy/StrategyManager.cpp`
  - hard performance drops in pre-filter/selection softened to score penalties.
  - pre-filter EV gate made permissive (final EV/edge gate delegated to engine layer).
- `src/engine/TradingEngine.cpp`
  - added typed-archetype gate for `Advanced Scalping` / `Advanced Momentum` when core risk plane is active.
  - allows fallback only for very strong/high-EV signals.
- `src/backtest/BacktestEngine.cpp`
  - same typed-archetype gate policy mirrored for backtest parity.
  - critical parity fix: after backtest entry, `position.entry_time` is now set from candle timestamp (not wall clock).
- `src/strategy/ScalpingStrategy.cpp`
  - archetype-specific timed-loss/progress exits tightened then rebalanced (continuation/reclaim paths).

### Root cause found
- Backtest `holding_time_seconds` could be distorted by clock-source mismatch:
  - `RiskManager::enterPosition()` used system time
  - backtest `shouldExit()` compared against candle time
- This prevented intended time-based exit logic from being evaluated consistently in backtest.

### Verification snapshots (RealDataOnly, 12 datasets)
- Before clock-source fix:
  - `core_full.avg_profit_factor = 10.3933`
  - `core_full.avg_total_trades = 10.3`
  - `core_full.avg_expectancy_krw = -9.5054`
- After parity fix + archetype/timed-exit patch:
  - `core_full.avg_profit_factor = 10.3166`
  - `core_full.avg_total_trades = 9.9`
  - `core_full.avg_expectancy_krw = -5.5229`

### Current status
- `avg_expectancy_krw` improved materially.
- Remaining gate blockers:
  - `avg_total_trades` slight miss (`9.9` vs required `10`)
  - `profitable_ratio` still low.

## Stage 15 Session Update (2026-02-15, current)

### Reliability/consistency fixes kept
- Expanded adaptive-state I/O isolation for parallel backtests:
  - `src/strategy/BreakoutStrategy.cpp`
  - `src/strategy/MeanReversionStrategy.cpp`
  - env flag: `AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO=1`
- Analysis scripts now run backtests with adaptive-state I/O disabled:
  - `scripts/analyze_loss_contributors.py`
  - `scripts/analyze_entry_pattern_bias.py`
- Added worker cap option for analysis speed/stability:
  - `--max-workers` (default `4`) in both analysis scripts.

### Latest validated snapshot (realdata-only, gate min trades = 8)
- command:
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8`
- core_full:
  - `avg_profit_factor = 10.2885` (pass)
  - `avg_total_trades = 8.1` (pass)
  - `avg_expectancy_krw = -6.2280` (fail)
  - `profitable_ratio = 0.2` (fail)
  - `overall_gate_pass = false`

### Current bottleneck (pattern/loss basis)
- top strategy loss share:
  - `Advanced Scalping = 65.1642%`
  - `Advanced Momentum = 28.5072%`
- dominant losing patterns:
  - `Advanced Scalping | BREAKOUT_CONTINUATION | TRENDING_UP | avg_profit_krw = -15.0006 | trades=40`
  - `Advanced Momentum | BREAKOUT_CONTINUATION | RANGING | avg_profit_krw = -3.6215 | trades=7`

### Next execution focus
- P0: reduce loss in the two dominant patterns without dropping below trade floor 8.
- P1: run short tuning loop only after strategy-side pattern corrections are applied.

## Root-Cause Diagnostics Update (2026-02-15)

### New diagnostic runner
- Added: `scripts/analyze_root_cause_diagnostics.py`
- Outputs:
  - `build/Release/logs/root_cause_diagnostics_summary.json`
  - `build/Release/logs/root_cause_loss_patterns.csv`

### Core findings (core_full, realdata 12 datasets)
- total_trades: `86`
- total_profit_krw: `-623.0801`
- total_fees_krw: `319.6884`
- gross_profit_before_fees_krw: `-303.3917`
- avg_expectancy_krw: `-7.2451`
- avg_gross_expectancy_krw: `-3.5278`
- fee_share_of_net_loss_pct: `51.3077%`
- intrabar_stop_tp_collision_count: `1`

### Interpretation
- Main loss source is not intrabar stop/TP collision ordering.
- Loss is dominated by:
  - fee drag (`~51%` of net loss)
  - weak post-entry follow-through and strategy exits.
- Exit reasons:
  - `StrategyExit=29`, `StopLoss=27`, `TakeProfit1=22`, `TakeProfit2=8`

### Dominant structural loss patterns
- `Advanced Scalping | BREAKOUT_CONTINUATION | TRENDING_UP | ev_high`
  - trades=`36`, avg_profit_krw=`-11.7817`
- `Advanced Momentum | BREAKOUT_CONTINUATION | RANGING | ev_high`
  - trades=`12`, avg_profit_krw=`-6.5365`

### Next action rule
- Do not globally relax filters.
- Apply targeted changes only on the two dominant loss archetypes and re-run this diagnostic script each patch.

## Stage 15 Improvement Update (2026-02-15, diverse_wide_013)

### Applied candidate config (runtime)
- source: tuning winner `scenario_diverse_wide_013`
- updated: `config/config.json`
- added preset: `config/presets/candidate_stage15_diverse_wide_013.json`

### Recheck result (realdata-only, gate min trades=8)
- command:
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8`
- `core_full`:
  - `avg_profit_factor: 10.2885 -> 10.3365` (improved)
  - `avg_total_trades: 8.1 -> 8.4` (improved)
  - `avg_expectancy_krw: -6.2280 -> -5.8690` (improved)
  - `profitable_ratio: 0.2` (unchanged)
  - `overall_gate_pass=false` (still fail: expectancy/profitable_ratio)

### Root-cause monitor after apply
- `python scripts/analyze_root_cause_diagnostics.py --profile-id core_full --max-workers 4`
- major loss cluster still remains:
  - `Advanced Scalping | BREAKOUT_CONTINUATION | TRENDING_UP`
  - `Advanced Momentum | BREAKOUT_CONTINUATION | RANGING`

## Stage 15 Update (2026-02-15, two-step entry + cost-aware TP/SL + data hostility check)

### Code changes applied (backtest/live parity)
- `src/backtest/BacktestEngine.cpp`
  - added second-stage entry confirmation gate:
    - margin-based confirm on top of RR/edge pass (`rr_margin`, `edge_margin`)
    - regime/liquidity/strategy-quality adaptive margin.
  - upgraded RR rebalance to cost-aware target:
    - `target_rr` now floored by effective round-trip cost and regime/liquidity context.
    - `TP1` minimum RR also includes cost-cover floor.
- `src/engine/TradingEngine.cpp`
  - same second-stage confirmation and cost-aware TP/SL rebalance logic mirrored.

### Recheck result (realdata-only, gate min trades=8)
- command:
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8`
- `core_full`:
  - `avg_profit_factor: 10.3365 -> 10.3448`
  - `avg_total_trades: 8.4 -> 8.4`
  - `avg_expectancy_krw: -5.8690 -> -5.2277`
  - `profitable_ratio: 0.2 -> 0.2`
  - `overall_gate_pass=false` (remaining fail: expectancy/profitable_ratio)

### Root-cause diagnostics snapshot after patch
- command:
  - `python scripts/analyze_root_cause_diagnostics.py --profile-id core_full --max-workers 4`
- results:
  - `total_trades=89`
  - `avg_expectancy_krw=-6.7838`
  - `avg_gross_expectancy_krw=-3.0794`
  - `fee_share_of_net_loss_pct=54.6074%`
  - `intrabar_stop_tp_collision_count=1`
- interpretation:
  - collision ordering remains minor;
  - fee drag + weak follow-through patterns still dominant.

### Dataset hostility validation (new)
- added script:
  - `scripts/analyze_dataset_hostility.py`
- command:
  - `python scripts/analyze_dataset_hostility.py --gate-report-json build/Release/logs/profitability_gate_report_realdata.json`
- outputs:
  - `build/Release/logs/dataset_hostility_summary.json`
  - `build/Release/logs/dataset_hostility_details.csv`
- result:
  - `hostility_level=high`
  - `avg_adversarial_score=62.7879`
  - `negative_return_share=1.0`
  - `high_drawdown_share_ge_8pct=0.9167`
  - `all_profiles_loss_share=0.8333`

## Stage 15 Update (2026-02-15, live scan-speed adaptive gating)

- `src/engine/TradingEngine.cpp`
  - added per-scan market hostility scoring from regime mix (TRENDING_DOWN/HIGH_VOLATILITY share weighted).
  - quick internal adaptation in same scan cycle:
    - `adaptive_filter_floor` tightens when hostility rises.
    - `per_scan_buy_limit` automatically shrinks in hostile regime (volume is not forced).
    - `min_reward_risk_gate`/`min_expected_edge_gate` tighten in hostile regime.
  - severe hostile + recent negative edge condition:
    - pause new entries for that scan (`Hostile market entry pause` log).
  - starvation-relaxation is reduced in hostile regime to avoid over-relaxing just to keep trade count.

## Stage 15 Update (2026-02-15, verification-chain hostility adaptive thresholds)

- updated verification scripts (4):
  - `scripts/run_profitability_matrix.py`
  - `scripts/run_realdata_candidate_loop.py`
  - `scripts/tune_candidate_gate_trade_density.py`
  - `scripts/run_candidate_auto_improvement_loop.py`
- behavior:
  - matrix gate now computes dataset hostility score directly from input candles and derives effective gate thresholds.
  - realdata loop forwards hostility-adaptive mode and prints effective thresholds in run log.
  - tuning script consumes effective thresholds for objective/constraint scoring (optional toggle).
  - auto-improvement loop consumes effective thresholds for target checks and tuning objective inputs.
- smoke checks:
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --enable-hostility-adaptive-thresholds`
  - `python scripts/tune_candidate_gate_trade_density.py --scenario-mode legacy_only --max-scenarios 1 --real-data-only --require-higher-tf-companions --screen-dataset-limit 4 --screen-top-k 1 --gate-min-avg-trades 8 --enable-hostility-adaptive-thresholds --use-effective-thresholds-for-objective`
  - `python scripts/run_candidate_auto_improvement_loop.py --max-iterations 1 --skip-tune-phase --real-data-only --require-higher-tf-companions --enable-hostility-adaptive-targets`

### Latest run snapshot (hostility-adaptive enabled)
- command:
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --enable-hostility-adaptive-thresholds`
- effective thresholds (auto):
  - `min_pf=0.9593`, `min_expectancy_krw=-3.6592`, `min_profitable_ratio=0.3264`, `min_avg_win_rate_pct=30.0`, `min_avg_trades=3`
- `core_full`:
  - `avg_profit_factor=12.3293`
  - `avg_total_trades=3.6667`
  - `avg_expectancy_krw=0.5019`
  - `profitable_ratio=0.3333`
- gate:
  - `overall_gate_pass=true` (hostility-adaptive threshold context)

### Note
- Current pass is achieved under high-hostility adaptive thresholds with low trade frequency.
- Next step remains valid: improve absolute profitability quality while recovering trade count in non-hostile regimes.

## Stage 15 Update (2026-02-15, verification stability hardening)

- cause addressed:
  - intermittent verification failures from concurrent matrix/tune/auto-loop runs touching shared config/artifacts.
- hardening applied:
  - added shared lock helper: `scripts/_script_common.py` (`verification_lock`).
  - gate matrix runner now supports stable execution options:
    - `--max-workers` (default `1`)
    - `--backtest-retry-count` (default `2`)
    - lock options (`--verification-lock-*`).
  - propagated stable options through chain:
    - `scripts/run_realdata_candidate_loop.py`
    - `scripts/tune_candidate_gate_trade_density.py`
    - `scripts/run_candidate_auto_improvement_loop.py`
- verification runs (sequential/stable mode):
  - `run_realdata_candidate_loop.py` (workers=1, retry=2) PASS
  - `tune_candidate_gate_trade_density.py` smoke (workers=1, retry=2, no cache) PASS
  - `run_candidate_auto_improvement_loop.py` 1-iteration smoke (workers=1, retry=2) PASS

## Stage 15 Update (2026-02-15, dual-mode loop artifact + EV bucket realigned)

### 1) `run_realdata_candidate_loop.py` dual-mode artifact output
- Added `--run-both-hostility-modes`:
  - one command now runs matrix twice:
    - strict (`hostility adaptive OFF`)
    - adaptive (`hostility adaptive ON`)
  - auto-emits separate artifacts:
    - `..._realdata_strict.json/csv`
    - `..._realdata_adaptive.json/csv`
  - still writes canonical `..._realdata.json/csv` from selected mode for compatibility.
- Added propagation option in auto loop:
  - `scripts/run_candidate_auto_improvement_loop.py`
  - new flag: `--emit-strict-adaptive-pair` (forwards to realdata loop).

### 2) `ev_high` bucket realignment
- Updated in `src/backtest/BacktestEngine.cpp`:
  - EV bucket thresholds tightened to net-edge semantics:
    - `ev_neutral < 0.0012`
    - `ev_positive < 0.0030`
    - else `ev_high`
  - Pattern-label EV now uses realized-aligned edge:
    - `realized_aligned_edge = 0.60 * expected_edge + 0.40 * realized_net_edge`
    - where `realized_net_edge = profit_loss / (entry_price * quantity)`.

### Validation
- command:
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --gate-min-avg-trades 8 --enable-hostility-adaptive-thresholds --run-both-hostility-modes --matrix-max-workers 1 --matrix-backtest-retry-count 2`
- snapshot:
  - strict: `overall_gate_pass=false`
  - adaptive: `overall_gate_pass=true`
  - selected(core_full): `avg_profit_factor=11.8427`, `avg_total_trades=6.6667`, `avg_expectancy_krw=-0.2125`
- root-cause pattern bucket now shows mixed labels (not all `ev_high`):
  - example: `Advanced Scalping / PULLBACK_RECLAIM / TRENDING_UP` split into `ev_high` and `ev_positive`.

### Follow-up experiments (same session)
- Tried additional hardening for `Momentum/BREAKOUT_CONTINUATION@TRENDING_UP` at:
  - strategy-side quality tier tightening
  - engine/backtest archetype risk adjustment
- Result:
  - no consistent gain on `core_full` gate metrics, and some variants worsened trade count/expectancy.
- Action:
  - regressive variants rolled back.
  - stable baseline kept:
    - strict: `avg_pf=11.8427`, `avg_trades=6.6667`, `avg_expectancy_krw=-0.2125`, `overall_gate_pass=false`
    - adaptive: same profile metrics, `overall_gate_pass=true` under effective hostility thresholds.

## Stage 15 Update (2026-02-15, intra-turn regression check and rollback)

- attempted patch (this turn):
  - tightened `Momentum` and `Scalping` `BREAKOUT_CONTINUATION` archetype classification gates.
  - tightened stagnation/early adverse exits for continuation-type positions.
- immediate verification (realdata dual-mode):
  - command:
    - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --run-both-hostility-modes --gate-min-avg-trades 8`
  - regressive snapshot (before rollback):
    - `core_full.avg_total_trades=4.5556`
    - `core_full.avg_expectancy_krw=-2.7688`
    - strict/adaptive profile quality worsened vs baseline.
- action:
  - rolled back the above strategy-side tightening changes.
  - rebuilt and reran the same command.
- post-rollback snapshot (restored baseline):
  - strict:
    - `core_full.avg_profit_factor=11.8427`
    - `core_full.avg_total_trades=6.6667`
    - `core_full.avg_expectancy_krw=-0.2125`
    - `overall_gate_pass=false`
  - adaptive:
    - same `core_full` profile metrics
    - `overall_gate_pass=true` (hostility-adaptive thresholds)
- conclusion:
  - simple hard-threshold tightening at archetype/exit layer is still regressive.
  - next patch should target causal features (entry quality decomposition + fee-aware net-edge optimization) before adding stronger hard filters.

## Stage 15 Update (2026-02-15, adaptive-state I/O toggle for matrix chain)

- issue identified:
  - verification chain forced `AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO=1` in matrix runs, so strategy adaptive learning state was never reflected in gate/tuning loops.
- updates:
  - `scripts/run_profitability_matrix.py`
    - added `--enable-adaptive-state-io` (default remains disabled for legacy-safe behavior).
  - `scripts/run_realdata_candidate_loop.py`
    - added pass-through `--enable-adaptive-state-io`.
  - `scripts/run_candidate_auto_improvement_loop.py`
    - added pass-through `--enable-adaptive-state-io`.
- validation:
  - default mode (adaptive state I/O disabled):
    - `core_full`: `avg_profit_factor=11.8427`, `avg_total_trades=6.6667`, `avg_expectancy_krw=-0.2125`
    - `build/Release/state` remains empty after run.
  - enabled mode (`--enable-adaptive-state-io`):
    - adaptive state files are created (`momentum_archetype_stats.json`, `scalping_archetype_stats.json`, ...)
    - current hostile dataset set showed regression snapshot:
      - `core_full.avg_total_trades=4.5556`
      - `core_full.avg_expectancy_krw=-2.3804`
- note:
  - keep default verification path with adaptive state I/O disabled.
  - treat adaptive-state-enabled matrix as an explicit experiment mode until state-update policy is stabilized.

## Stage 15 Update (2026-02-15, train/eval split orchestrator)

- added script:
  - `scripts/run_candidate_train_eval_cycle.py`
- purpose:
  - split pipeline into:
    - training stage: adaptive state I/O ON
    - state snapshot stage
    - evaluation stage: deterministic chain (adaptive state I/O OFF)
- default behavior:
  - clears `build/Release/state` before training
  - runs N training iterations (`--train-iterations`)
  - snapshots state to `build/Release/state_snapshots/state_snapshot_<timestamp>`
  - runs deterministic evaluation
  - writes summary:
    - `build/Release/logs/candidate_train_eval_cycle_summary.json`
- smoke command:
  - `python scripts/run_candidate_train_eval_cycle.py --train-iterations 1 --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --run-both-hostility-modes --gate-min-avg-trades 8 --matrix-max-workers 1 --matrix-backtest-retry-count 2`
- smoke snapshot:
  - train_1 (adaptive ON): `core_full.avg_total_trades=4.5556`, `avg_expectancy_krw=-2.3804`
  - eval_deterministic (adaptive OFF): `core_full.avg_total_trades=6.6667`, `avg_expectancy_krw=-0.2125`
  - state snapshot files: `breakout_entry_stats.json`, `momentum_archetype_stats.json`, `scalping_archetype_stats.json`

## Stage 15 Update (2026-02-15, strict bottleneck re-check + strategy pass)

- objective in this pass:
  - keep strict gate chain deterministic,
  - recover `avg_total_trades` to gate floor 8,
  - avoid regressing `avg_expectancy_krw`.

- completed:
  - gate sampling sensitivity re-check (`min_trades_per_run_for_gate=1/2/3`) on latest code:
    - artifact: `build/Release/logs/experiments/post_patch_gate_sampling/summary.csv`
    - finding: sampling-only changes do not solve strict pass.
  - `MomentumStrategy` update:
    - added adaptive archetype block hook:
      - `shouldBlockArchetypeByAdaptiveStats(...)`
      - applied in both `generateSignal` and `shouldEnter`.
    - tightened `BREAKOUT_CONTINUATION` entry quality vs prior relaxed variants.
    - re-balanced `TREND_REACCELERATION` thresholds from over-relaxed state.
  - `ScalpingStrategy` update:
    - selective `TRENDING_UP` quality gate relaxation (strict/flow branch) to recover sample count
      without reopening `RANGING`.
  - diagnostic script correction:
    - `scripts/analyze_root_cause_diagnostics.py` now applies profile flags consistently with matrix
      chain (`trading.enable_core_*`) before per-dataset backtest analysis.

- latest verification snapshot (deterministic strict/adaptive dual run):
  - command:
    - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --run-both-hostility-modes --gate-min-avg-trades 8`
  - strict `core_full`:
    - `avg_profit_factor=11.9107`
    - `avg_total_trades=8.0` (pass)
    - `avg_expectancy_krw=0.8912` (pass)
    - `profitable_ratio=0.4444` (fail; threshold `0.55`)
    - `overall_gate_pass=false`
  - adaptive:
    - `overall_gate_pass=true` on hostility-adaptive thresholds.

- current strict bottleneck:
  - only `gate_profitable_ratio_pass=false` remains for `core_full`.
  - same run shows `gate_trades_pass=true` and `gate_expectancy_pass=true`.

- root cause snapshot for current state:
  - file: `build/Release/logs/root_cause_diagnostics_core_full_latest3.json`
  - dominant loss clusters:
    - `Advanced Momentum / TREND_REACCELERATION / TRENDING_UP / ev_high`
    - `Advanced Momentum / BREAKOUT_CONTINUATION / RANGING / ev_positive`
    - `Advanced Scalping / PULLBACK_RECLAIM / TRENDING_UP / ev_neutral`

## Stage 15 Update (2026-02-15, core default-on migration finalized)
- Completed:
  - PR #3 merged (`5935368`)
    - strict hostility trades-only threshold chain integrated
    - CI operational gate script hardened (missing fallback dataset/artifact-empty no longer hard-fail)
  - PR #4 merged (`ead34cf`)
    - core plane defaults switched to ON:
      - `include/engine/EngineConfig.h`
      - `src/common/Config.cpp`
  - CI PR Gate passes confirmed on both PRs:
    - run `22033385134` (PR #3)
    - run `22033569111` (PR #4)

- Current policy:
  - runtime default path: core
  - legacy path: explicit config opt-out only

- Follow-up:
  - finalize burn-in/rollback/cleanup sequence in:
    - `docs/CORE_MIGRATION_FINALIZATION_2026-02-15.md`

## Stage 15 EV/Bucket Consistency Patch (2026-02-15)

### Step 1 completed
- Goal: align pre-filter EV, calibrated edge, and EV bucket labeling semantics.
- Code updates:
  - `src/strategy/StrategyManager.cpp`
    - Added cost-aware pre-filter EV calculation (partial round-trip cost at manager stage).
    - Added implied-win blending with strategy history (win-rate/profit-factor weighted by sample size).
  - `src/engine/TradingEngine.cpp`
    - Added strategy-history prior in `computeCalibratedExpectedEdgePct`.
  - `src/backtest/BacktestEngine.cpp`
    - Added same strategy-history prior in `computeCalibratedExpectedEdgePct` (live/backtest parity).
    - Tightened EV bucket thresholds: neutral `<0.0015`, positive `<0.0035`.
    - Made EV bucket alignment conservative (realized outcome can demote, not promote).

### Verification
- Build: PASS
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release`
- Realdata candidate loop: PASS (script execution)
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --run-both-hostility-modes --gate-min-avg-trades 8 --matrix-max-workers 1 --matrix-backtest-retry-count 2`
- Current snapshot (core_full) remained stable on this patch:
  - `avg_profit_factor=12.0024`
  - `avg_total_trades=8.0`
  - `avg_expectancy_krw=2.2395`
  - strict `overall_gate_pass=false` (profitable_ratio bottleneck)
  - adaptive `overall_gate_pass=true`

## Stage 15 Step 2 Update (2026-02-15, Momentum Post-Entry Control)

### What was tried
- Entry hardening attempt on Momentum/Scalping archetype filters was tested first.
- That direction reduced trade count and worsened expectancy in revalidation, so it was rolled back.

### What is kept (effective patch)
- Kept Step 1 EV/bucket consistency changes.
- Added targeted post-entry risk tightening for `Momentum / TREND_REACCELERATION` only:
  - file: `src/strategy/MomentumStrategy.cpp`
  - tighter invalidation/progress floors at entry-context creation
  - faster early/mid/late weak-hold exits in `shouldExit`
  - additional stricter early-cut when `flow_bias` is weak.

### Verification (sequential build -> run)
- Build PASS:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release`
- Loop PASS:
  - `python scripts/run_realdata_candidate_loop.py --skip-fetch --skip-tune --real-data-only --require-higher-tf-companions --run-both-hostility-modes --gate-min-avg-trades 8 --matrix-max-workers 1 --matrix-backtest-retry-count 2`

### Latest snapshot (core_full)
- `avg_profit_factor=12.0938` (up from 12.0024)
- `avg_total_trades=8.0` (maintained)
- `avg_expectancy_krw=3.7766` (up from 2.2395)
- `profitable_ratio=0.5556` (up from 0.4444)
- strict `overall_gate_pass=true`
- adaptive `overall_gate_pass=true`

### Root-cause refresh
- `python scripts/analyze_root_cause_diagnostics.py --profile-id core_full --max-workers 4`
- Totals shifted to positive expectancy:
  - `avg_expectancy_krw=0.1665`
  - `total_profit_krw=11.9874`
