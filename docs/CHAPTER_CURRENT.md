# Current Chapter

Last updated: 2026-02-21
Chapter ID: `CH-01`
Title: `Probabilistic Hybrid Foundation`

## Objective
- Validation/tuning 중심 운영에서 확률형 학습 기반 구조로 전환.
- 라이브/백테스트 동형성 유지(데이터/피처/결정 규칙 동일).
- 과적합 방지 원칙 고정: 시계열 분리, 워크포워드, 순차 검증.
- 원천 데이터(OHLCV)와 파생 피처(보조지표/라벨) 생성 경로를 분리하고 계약 고정.

## New Direction (Fixed)
- 거래 스타일: 초단타 스캘핑이 아니라 품질형 진입/관리.
  - 익절 목표는 고정 퍼센트가 아니라 `먹을 수 있을 때까지 보유`를 기본으로 함.
  - 구현 원칙: 부분익절 + 트레일링 + 레짐 악화 시 축소/종료.
- 의사결정 구조:
  - 상위 TF(15/60/240m)로 장세/레짐 판별
  - 하위 TF(1/5m)로 진입 타이밍 정밀화
- 모델 구조:
  - 확률형 패턴 모델(`P(up/down | context)`) + 기존 adaptive 리스크 엔진 결합

## Data Policy (Fixed)
- 업비트 API에서 수집하는 것은 `OHLCV 원천 캔들`만 사용.
- 보조지표/레이블/피처는 로컬에서 결정론적으로 생성(같은 입력이면 같은 출력).
- 멀티 TF 기준은 `1/5/15/60/240m`를 공통 사용.
- 라이브/백테스트 모두 동일한 피처 계산 규칙을 사용해야 함(정합성 우선).

## Build Toolchain (Fixed)
- CMake binary path (vcpkg 하위 고정):
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe`
- CMake toolchain path:
  - `D:\MyApps\vcpkg\scripts\buildsystems\vcpkg.cmake`
- `cmake`가 PATH에 없을 수 있으므로 빌드/검증은 절대경로 사용을 기본으로 함.

## Data Foundation Progress
1. `60m,240m` 장기 번들 수집 완료(9마켓 기준, EOS 404 제외).
2. `5m,15m` 번들 수집 완료.
3. `1m` 최근 2년 번들 수집 완료(9마켓).
4. 현재 상태는 manifest 기준(`data/backtest_probabilistic/probabilistic_bundle_manifest.json`)을 단일 진실원으로 사용.
5. 진행 가시성 개선 완료:
   - `scripts/fetch_probabilistic_training_bundle.py` job 단위 manifest 즉시 갱신
   - `current_job`, `progress_pct`, `run_state` 실시간 기록
6. 정체 루프 방지 완료:
   - `scripts/fetch_upbit_historical_candles.py`에 stagnant cursor/unique break 추가
7. Phase-2 진행/완료:
   - `scripts/build_probabilistic_feature_dataset.py` 추가
   - 1m anchor + 5/15/60/240 context + leakage-safe labels 생성 파이프라인 구축
  - full build 완료(9 markets, `9,477,036` rows, `~3.48 GB`)
  - 메이저+알트 혼합 데이터로 일반화 케이스 확보(코인별 과적합 방지 목적)
8. Storage safety guard 추가(2026-02-21):
   - script: `scripts/fetch_probabilistic_training_bundle.py`
   - options:
     - `--min-free-gb` (최소 여유 디스크 유지)
     - `--max-output-gb` (output-dir 용량 상한)
     - `--disk-budget-policy {halt|skip}`
   - 목적: 대용량 수집 중 디스크 고갈/중단 리스크를 사전 차단.
9. Triple-barrier 라벨 옵션 추가(2026-02-21):
   - script: `scripts/build_probabilistic_feature_dataset.py`
   - options:
     - `--enable-triple-barrier-labels`
     - `--triple-barrier-horizon`
     - `--triple-barrier-take-profit-bps`
     - `--triple-barrier-stop-loss-bps`
   - 기본값은 OFF(기존 라벨 계약 유지), 필요 시 단계적으로 활성화 가능.
10. 학습 타깃 컬럼 선택 옵션 추가(2026-02-21):
   - script: `scripts/train_probabilistic_pattern_model.py`
   - options:
     - `--h1-target-column`
     - `--h5-target-column`
     - `--edge-column`
     - `--drop-neutral-target / --keep-neutral-target`
   - 목적: 기존 `label_up_h5` 고정에서 벗어나 triple-barrier 기반 타깃 실험 가능.
   - smoke:
     - `build/Release/logs/probabilistic_model_train_summary_smoke_targetflex_20260221.json`
     - `max-datasets=1`, status=`pass`
11. EV 브리지(확률->기대값) 1차 연결(2026-02-21):
   - `scripts/train_probabilistic_pattern_model.py`:
     - fold metric에 `h5 valid/test edge_profile` 추가.
     - joblib artifact(`h5`)에 `edge_profile` 저장.
   - `scripts/export_probabilistic_runtime_bundle.py`:
     - `h5_model.edge_profile` 출력.
   - C++ runtime:
     - `ProbabilisticInference.expected_edge_bps/pct` 계산 추가.
     - live/backtest `applyProbabilisticRuntimeAdjustment`에서 expected-value blend 반영.
   - smoke:
     - train: `build/Release/logs/probabilistic_model_train_summary_smoke_evprofile_20260221.json` (`pass`)
     - bundle: `build/Release/logs/probabilistic_runtime_bundle_smoke_evprofile_20260221.json` (`h5_model.edge_profile` 확인)
     - parity: `build/Release/logs/probabilistic_runtime_bundle_parity_smoke_evprofile_20260221.json` (`status=pass`)
12. EV 브리지 2차(회귀 헤드 우선 + profile fallback) 연결(2026-02-21):
   - `scripts/train_probabilistic_pattern_model.py`:
     - `--enable-edge-regressor`(default ON), `--edge-target-clip-bps` 옵션 반영.
     - fold metric에 `h5 valid/test edge_regression(mae/rmse/corr)` 반영.
     - `h5 artifact`에 `edge_regressor(linear coef/intercept + clip_abs_bps)` 저장.
   - `scripts/export_probabilistic_runtime_bundle.py`:
     - runtime bundle에 `h5_model.edge_regressor` 직렬화.
   - C++ runtime:
     - `ProbabilisticRuntimeModel`이 `edge_regressor`를 로드하면 EV 계산에 우선 사용.
     - regressor 미존재/차원불일치 시 `edge_profile` 기반 계산으로 자동 fallback.
   - smoke:
     - train: `build/Release/logs/probabilistic_model_train_summary_smoke_evbridge2_20260221.json` (`pass`)
     - bundle: `config/model/probabilistic_runtime_bundle_smoke_evbridge2_20260221.json` (`h5_model.edge_regressor` 확인)
     - parity: `build/Release/logs/probabilistic_runtime_bundle_parity_smoke_evbridge2_20260221.json` (`status=pass`, `worst=0`, `edge_parity_mode=checked`, `edge_max_abs_diff_bps=0`)
     - build: `AutoLifeTrading` Release 빌드 통과(절대경로 cmake 사용).
13. 증분 갱신 + 하이브리드 재학습 사이클 추가(2026-02-21):
   - `scripts/fetch_upbit_historical_candles.py`:
     - `--start-utc`, `--append-existing`, `--auto-start-from-output` 지원.
     - 기존 CSV 병합(중복 timestamp dedup) + 마지막 캔들 이후 증분 수집 가능.
   - `scripts/fetch_probabilistic_training_bundle.py`:
     - `--incremental-update`, `--incremental-overlap-bars` 지원.
     - 번들 수집 시 job별 `last_ts` 기준 증분 시작점 계산 후 append 갱신.
   - 신규 오케스트레이터:
     - `scripts/run_probabilistic_hybrid_cycle.py`
     - 단계: 증분 fetch -> feature build -> feature validate -> split manifest ->
       baseline -> global train -> runtime bundle export -> parity validate
   - smoke:
     - `build/Release/logs/probabilistic_hybrid_cycle_summary_smokehybrid3_20260221.json` (`status=pass`)
     - bundle parity: `build/Release/logs/probabilistic_runtime_bundle_parity_smokehybrid3_20260221.json` (`status=pass`)

## Post-Fetch Canonical Order
- 수집 완료 후 실행 순서 및 Stage B~E 실험 규칙은
  `docs/STRICT_GATE_RUNBOOK_2026-02-13.md`를 단일 기준으로 사용.
- 요약:
  1. fetch 완료/실패건 고정
  2. feature rebuild
  3. strict validation
  4. split 재생성
  5. baseline 재생성
  6. global 재학습
  7. hybrid 확장(권장)
  8. runtime bundle export
  9. bundle parity
  10. verification gate
  11. paper/shadow
  12. guarded live enable

## Current Snapshot (at document update)
- Active batch: `none`
- Status: `phase-4 probabilistic-primary strict/fastpass hardening + rank-first minimums + operational-gate-only path(1차) 적용`
- Artifacts:
  - `data/backtest_probabilistic/probabilistic_bundle_manifest.json`
  - `build/Release/logs/probabilistic_bundle_summary.json`
  - `data/model_input/probabilistic_features_v1_full_20260220_181345/feature_dataset_manifest.json`
  - `build/Release/logs/probabilistic_feature_build_summary_full_20260220_181345.json`
  - `build/Release/logs/probabilistic_feature_validation_summary_full_20260220.json`
  - `data/model_input/probabilistic_features_v1_full_20260220_181345/probabilistic_split_manifest_v1.json`
  - `build/Release/logs/probabilistic_baseline_summary_full_20260220.json`
  - `build/Release/logs/probabilistic_model_train_summary_full_20260220.json`
  - `build/Release/models/probabilistic_pattern_v1_full_20260220`
  - `build/Release/logs/probabilistic_walkforward_report_full_20260220.json`
  - `build/Release/logs/probabilistic_inference_parity_full_20260220.json`
  - `config/model/probabilistic_runtime_bundle_v1.json`
  - `build/Release/logs/probabilistic_runtime_bundle_parity_full_20260220.json`
  - `build/Release/logs/verification_report_runtime_risk_rescuefix_20260220.json`
  - `build/Release/logs/verification_report_runtime_risk_rescuefix_expA_20260220.json`
  - `build/Release/logs/verification_report_runtime_risk_rescuefix_expB_20260220.json`

## Runtime Structural Update (2026-02-21, v3)
- 적용 파일:
  - `src/strategy/StrategyManager.cpp`
  - `src/runtime/BacktestRuntime.cpp`
  - `src/runtime/LiveTradingRuntime.cpp`
- 변경 요약:
  - manager soft-queue/promotion/fastpass를 `확률 우선 + 안전 조건 유지` 형태로 재조정.
    - 비적대 구간 margin/calibrated/liquidity 조건 완화
    - RR/EV/변동성 하한은 유지
  - `passesProbabilisticPrimaryMinimums`를 hard-cut 위주에서
    `hard safety + soft composite` 구조로 재구성(백테스트/라이브 동형 반영).
    - 극단 저신뢰/저유동만 즉시 차단
    - 일반 구간은 composite/확률-지원 컨텍스트 경로로 통과 허용
- 검증:
  - `build/Release/logs/verification_report_global_full_5set_probfirst4_20260221.json` (패치 전)
    - `avg_profit_factor=0.0`, `avg_expectancy_krw=0.0`, `avg_total_trades=0.0`
  - `build/Release/logs/verification_report_global_full_5set_probfirst5_20260221.json` (패치 후)
    - `avg_profit_factor=0.2679`
    - `avg_expectancy_krw=-19.1211`
    - `avg_total_trades=262.2`
    - `avg_entries_executed=230.0`
    - `avg_opportunity_conversion=0.0525`
- 병목 이동:
  - 무거래(0 trades) 병목은 해소.
  - 현재 1차 병목은 `candidate_generation > no_signal_generated (share=0.6503)`.
  - manager 세부 병목은 `rr_guard_strength`, `history_guard_ev` 중심.
  - `blocked_probabilistic_primary_calibrated`는 대량(기존 23,398)에서 4,852로 감소.

## Runtime Structural Update (2026-02-21, v4)
- 적용 파일:
  - `src/strategy/FoundationAdaptiveStrategy.cpp`
  - `src/strategy/FoundationRiskPipeline.cpp`
  - `src/runtime/BacktestRuntime.cpp`
  - `src/runtime/LiveTradingRuntime.cpp`
- 변경 요약:
  - `foundation_no_signal_liquidity_volume_gate` 완화:
    - 스프레드/오더북/압력 동반 조건에서만 저유동 통과 범위 확장
    - RANGING/UPTREND 저유동 탐지 경로 확장
  - manager guard 완화:
    - `history_guard` 활성 표본 조건 상향(과도한 early-history 차단 완화)
    - `rr_guard` 발동 임계/가중치 완화(확률 신호 고신뢰 구간 우선)
  - no-signal fallback 완화:
    - 확률 fallback의 비적대 구간 margin/prob/liquidity/volume 하한 완화(안전조건 유지)
- 검증:
  - 이전(v3): `build/Release/logs/verification_report_global_full_5set_probfirst5_20260221.json`
    - `avg_profit_factor=0.2679`
    - `avg_expectancy_krw=-19.1211`
    - `avg_total_trades=262.2`
  - 현재(v4): `build/Release/logs/verification_report_global_full_5set_probfirst6_20260221.json`
    - `avg_profit_factor=0.4557`
    - `avg_expectancy_krw=-14.3720`
    - `avg_total_trades=15.2`
    - `peak_max_drawdown_pct=4.42`
    - `profitable_ratio=0.2`
- 현재 판정:
  - adaptive fail 원인은 `risk_adjusted_score_guard_pass` 단일 실패.
  - 나머지 guard(`sample_size`, `drawdown`, `downtrend`, `opportunity_conversion`)는 통과.

## Validation Snapshot
- 전수 대상: 9 markets, `9,477,036` rows
- 결과: `pass (9/9)`
- 핵심 무결성:
  - 스키마 누락 0
  - 타임스탬프 정렬 위반 0
  - 라벨 도메인 위반 0
  - h1/h5/edge 누수 정합성 mismatch 0

## Baseline Snapshot
- baseline 종류: `train label-prior` (fold별)
- 전체 결과: `pass (9/9 datasets)`
- weighted test:
  - `h5_logloss=0.6743`
  - `h5_brier=0.2406`
  - `h5_accuracy=0.6025`
  - `h5_mean_edge_bps=-12.1554`

## Initial Model Snapshot
- 실행: `py -3.10 scripts/train_probabilistic_pattern_model.py`
- 모델: streaming `SGDClassifier(log_loss)` + fold별 `Platt calibration`
- 결과(9/9 datasets pass):
  - `h5_test_calibrated_logloss=0.6606` (baseline 0.6743 대비 개선)
  - `h5_test_calibrated_brier=0.2342` (baseline 0.2406 대비 개선)
  - `h5_test_calibrated_accuracy=0.6025` (baseline와 동일)
  - 선택 트레이드(`~11.05%` coverage) `mean_edge_bps=-10.5596`
    (baseline unconditional `-12.1554` 대비 개선)

## Walk-Forward Decomposition Snapshot
- 리포트: `build/Release/logs/probabilistic_walkforward_report_full_20260220.json`
- overall test:
  - `logloss=0.6606`
  - `brier=0.2342`
  - `accuracy=0.6025`
  - selected coverage `~11.03%`
  - selected mean edge `-10.5565 bps`
- regime 관찰:
  - `trend_down_sync`: coverage 높음(`~23.37%`), edge `-10.76 bps`
  - `trend_up_sync`: coverage 낮음(`~2.77%`), edge `-12.52 bps`
  - 현재는 up-sync 품질 개선이 핵심 병목

## Inference Parity Snapshot
- 리포트: `build/Release/logs/probabilistic_inference_parity_full_20260220.json`
- 검증 범위: 9 markets x 3 folds = 27 비교
- 결과: `pass`
  - worst raw diff: `2.220e-16`
  - worst calibrated diff: `1.721e-15`
  - tolerance: `1e-9`
- 의미: sklearn 추론과 수동 로짓 계산이 사실상 동일(추후 C++ 이식 기준 충족)

## Runtime Bundle Snapshot
- 번들 생성: `scripts/export_probabilistic_runtime_bundle.py`
  - 산출물: `config/model/probabilistic_runtime_bundle_v1.json`
  - 정책: 마켓별 latest fold(3) 선택
- 번들 parity:
  - script: `scripts/validate_runtime_bundle_parity.py`
  - output: `build/Release/logs/probabilistic_runtime_bundle_parity_full_20260220.json`
  - 결과: `pass (9 markets)`, worst diff `1.110e-16` (tol `1e-9`)
- C++ 연동 진행:
  - `src/analytics/ProbabilisticRuntimeFeatures.cpp` 추가 (학습 스크립트와 동일 변환식)
  - `src/runtime/LiveTradingRuntime.cpp`, `src/runtime/BacktestRuntime.cpp`에 런타임 확률 보정 연결
  - 확률 피처 캐시 추가(2026-02-21):
    - key: `market + anchor_ts + TF candle sizes(1m/5m/15m/1h/4h)`
    - 동일 캔들 구간 재스캔 시 보조지표/피처 재계산 없이 cache hit
    - 새 캔들 유입 시 자동 재계산 후 cache 갱신(학습 변환식 동일 유지)
  - `signal -> runtime -> risk manager -> trade/state persistence` 경로에 확률 메타(`h1/h5 cal, threshold, margin`) 직결
  - 라이브 실주문 비동기 체결 경로에 메타 지연 적용(`pending metadata`) 추가
  - 리스크 후행 제어에서 확률 마진 기반 동적 조정 적용:
    - 트레일링 갭
    - stagnation/recycle 시간 가드
    - 부분익절 비율
  - 빌드 확인: `AutoLifeTrading` Release 성공

## Global Cross-Market Retrain Path (2026-02-21)
- 목적: 코인별 특화가 아닌 전 코인 공통 패턴/보조지표/멀티프레임 확률학습으로 전환.
- 신규 학습 스크립트:
  - `scripts/train_probabilistic_pattern_model_global.py`
  - 동작: 모든 market dataset을 fold 단위로 합쳐 `GLOBAL` 모델 학습/보정/threshold 선택.
- 번들 export 확장:
  - `scripts/export_probabilistic_runtime_bundle.py --export-mode {global_only|hybrid|per_market}`
  - `global_only`/`hybrid`에서 `default_model` + `global_fallback_enabled=true` 생성.
- 런타임 추론 확장:
  - `include/analytics/ProbabilisticRuntimeModel.h`
  - `src/analytics/ProbabilisticRuntimeModel.cpp`
  - `default_model`(global fallback) 로드 및 추론 지원.
  - `prefer_default_model=true`면 마켓별 엔트리가 있어도 전역 모델 우선 적용.
- Live/Backtest 추론 경로 반영:
  - `src/runtime/LiveTradingRuntime.cpp`
  - `src/runtime/BacktestRuntime.cpp`
  - `hasMarket` 대신 `supportsMarket`으로 판정(전역 fallback 고려).
- 검증 커버리지 규칙 보강:
  - `scripts/run_verification.py`
  - 번들에 `global_fallback_enabled/default_model`이 있으면 market coverage fail을 강제하지 않음.
- 스모크 검증 산출물:
  - `build/Release/logs/probabilistic_model_train_summary_global_smoke.json`
  - `build/Release/config/model/probabilistic_runtime_bundle_global_smoke.json`
  - `build/Release/logs/probabilistic_runtime_bundle_parity_global_smoke.json`
- full retrain/배포 산출물:
  - `build/Release/logs/probabilistic_model_train_summary_global_full_20260221.json`
  - `build/Release/models/probabilistic_pattern_global_v1_full_20260221`
  - `config/model/probabilistic_runtime_bundle_v1.json` (`export_mode=global_only`)
  - `build/Release/config/model/probabilistic_runtime_bundle_v1.json` (`export_mode=global_only`)
  - `build/Release/logs/probabilistic_runtime_bundle_parity_global_full_20260221.json`
    - status=`pass`, `comparison_count=9`, `worst=1.110e-16`
  - `build/Release/logs/verification_report_global_full_gatecheck_20260221.json`
    - 미지원 market(`KRW-ADA`) 포함 실행에서도 coverage precheck fail 없이 진행 확인
  - `build/Release/logs/verification_report_global_full_5set_20260221.json`
    - 5-set 검증 결과: `avg_total_trades=0`, `avg_expectancy_krw=0`, `overall_gate_pass=false`
    - non-exec 1차 그룹: `candidate_generation (share=1.0)`

## Runtime/Manager Regression Snapshot (2026-02-20)
- 변경점:
  - `StrategyManager` core-rescue 경로에서 Foundation 전략의 이중게이트 재검증을 제거.
  - 대신 `실패 사유 기반 rescue 허용` + `safety floor`(스프레드/유동성/압력/단기수익률 하한) 적용.
  - `FoundationAdaptiveStrategy` 내부 엔트리 판정을 공통 snapshot 함수로 통합(`generateSignal`/`shouldEnter` 드리프트 제거).
- 검증 결과(6 datasets):
  - 이전(`verification_report_runtime_risk_20260220.json`):
    - `avg_profit_factor=0.4392`, `avg_expectancy_krw=-25.0062`, `avg_total_trades=3.0`
    - candidate_generation 중 `no_signal_generated_share=0.977`
  - 이후(`verification_report_runtime_risk_rescuefix_20260220.json`):
    - `avg_profit_factor=0.4182`, `avg_expectancy_krw=-22.0043`, `avg_total_trades=3.3333`
    - candidate_generation 중 `no_signal_generated_share=0.5687`
    - `filtered_out_by_manager_share=0.4313`로 병목 이동
- 해석:
  - 후보 공급 부족(no-signal) 병목은 크게 완화됨.
  - 대신 매니저 prefilter(`strength`, `ev_quality_floor`)가 주 병목으로 전환됨.
  - 즉, 다음 단계는 게이트 완화가 아니라 `manager prefilter staging` 정밀 분해가 우선.

## Probabilistic-Primary Flow Refactor (2026-02-20)
- 목표 반영:
  - 기존 전략 필터 보조가 아니라, 확률모델이 스캔/신호/진입/리스크 전 구간에서 주도권을 갖도록 파이프라인 재배치.
- 코드 반영:
  - `src/runtime/LiveTradingRuntime.cpp`
  - `src/runtime/BacktestRuntime.cpp`
  - `include/engine/EngineConfig.h`
  - `src/common/Config.cpp`
- 핵심 변경:
  - 스캔 단계: `probabilistic_market_prefilter` 추가(마켓 단위 확률 마진 prefilter).
  - 신호 단계: 확률 snapshot을 시그널 score/strength/filter에 우선 반영(기존 전략값은 보조).
  - 매수 단계: 확률 마진 기반 `position_size` 동적 스케일(양수 여지 확대, 음수 여지 축소).
  - 리스크 단계: 기존 확률 메타 연동(`prob_h5_margin`) 경로 유지/강화.
- 신규 설정 키(기본값):
  - `probabilistic_runtime_primary_mode=true`
  - `probabilistic_runtime_scan_prefilter_enabled=true`
  - `probabilistic_runtime_scan_prefilter_margin=-0.10`
  - `probabilistic_runtime_strength_blend=0.45`
- 회귀 결과(6 datasets):
  - 리포트: `build/Release/logs/verification_report_prob_primary_refactor_final_20260220.json`
  - `avg_profit_factor=0.3941`
  - `avg_expectancy_krw=-29.7897`
  - `avg_total_trades=2.6667`
  - `overall_gate_pass=false`
  - 해석: 확률 주도 경로는 반영되었고, 현재는 `candidate_generation(no_signal) + manager strength prefilter` 동시 병목.

## Probabilistic-Primary Structural Follow-up (2026-02-20, v1)
- 변경 요약:
  - 실험용 env 토글 경로를 정식 config 기반으로 전환:
    - `foundation_signal_supply_fallback_enabled`
    - `manager_soft_queue_enabled`
    - `manager_soft_queue_position_scale`
    - `manager_soft_queue_max_strength_gap`
    - `manager_soft_queue_max_ev_gap`
  - `FoundationAdaptiveStrategy`의 `RANGING + 저유동` 신호 공급 경로 확장:
    - `signal_supply_fallback` 활성 조건 확장
    - `ranging minimal probe` 경로 추가(소형 포지션 경로)
  - `SignalPolicyShared::isAlphaHeadFallbackCandidate`에
    `foundation_adaptive_regime_entry_signal_supply_fallback` 연동 추가
    (probabilistic primary mode에서 fallback 완화 경로와 연결).
  - 백테스트/라이브 entry-quality에 확률 기반 near-miss(edge) 완화 추가
    (비우호 레짐 제외, 확률 마진/보정 확률 조건부).
- 주요 리포트:
  - `build/Release/logs/verification_report_prob_primary_structural_refactor_v1_20260220.json`
  - 2-dataset probe:
    - `build/Release/logs/verification_report_softqueue_supply_edge_relief_probe_v4_20260220.json`
- 관측 결과(6 datasets):
  - `avg_generated_signals: 5654.8333 -> 5810.8333`
  - `avg_primary_candidate_conversion: 0.2187 -> 0.2504`
  - `no_signal_generated_share: 0.5843 -> 0.5797`
  - `avg_profit_factor: 0.3941 (동일)`
  - `avg_expectancy_krw: -29.7897 (동일)`
- 해석:
  - 후보 공급/매니저 통과율은 개선됐지만, 실거래 진입으로 이어지는 구간에서
    `blocked_risk_gate_entry_quality_edge_base` 비중이 증가.
  - 다음 병목은 `entry-quality edge gate` 구조(현재 base floor 설계)로 확정.

## Probabilistic-Primary Hardening (2026-02-20, v2)
- 적용 파일:
  - `src/strategy/StrategyManager.cpp`
  - `src/strategy/FoundationRiskPipeline.cpp`
  - `src/runtime/LiveTradingRuntime.cpp`
  - `src/runtime/BacktestRuntime.cpp`
- 핵심 변경:
  - `StrategyManager`:
    - 확률 고신뢰 신호 `manager_probabilistic_primary_fastpass` 추가
      (안전조건: 유동성/변동성/RR/EV 하한 유지).
    - 신호 점수 계산에 `probabilistic_margin + calibrated_prob` 가중 반영 강화.
  - `FoundationRiskPipeline`:
    - 확률 신호에 대해 history/RR/no-trade-bias 패널티를 고정값 대신 confidence 기반 스케일로 완화.
    - hostile 레짐 안전장치는 유지.
  - `Live/Backtest inferProbabilisticRuntimeSnapshot`:
    - `probabilistic_runtime_primary_mode=true`에서 fail-open 제거(모델 우선 강제).
    - 실패 사유를 명시적으로 taxonomy에 기록:
      - `probabilistic_runtime_bundle_unavailable`
      - `probabilistic_market_not_supported`
      - `probabilistic_feature_build_failed`
      - `probabilistic_feature_dimension_mismatch`
      - `probabilistic_inference_failed`
- 검증:
  - Release build: `AutoLifeTrading` 성공.
  - 스모크 실행: `data/backtest/sample_trend_pullback_1m.csv` 기준 실행 정상(종료코드 0).
  - 주의: 샘플 데이터는 MTF context 부족으로 확률 피처 빌드 실패 로그가 발생하며,
    이는 v2 strict-primary 정책에서 의도된 차단 동작.

## Runtime Dead-Path Trim (2026-02-21)
- 적용 파일:
  - `src/runtime/BacktestRuntime.cpp`
  - `src/runtime/LiveTradingRuntime.cpp`
  - `include/engine/EngineConfig.h`
  - `src/common/Config.cpp`
- 변경:
  - `probabilistic_primary_rank_first` 토글/파싱 제거(확률 1순위 고정).
  - 실행 경로에서 더 이상 호출되지 않는 대형 보조 게이트 블록(EntryQuality/SecondStage/TwoHead 집계 helper) 제거.
  - 위 helper 전용 dead config key 파싱/필드 제거(`EngineConfig`, `Config.cpp`).
  - 운영 경로 분기 오류(`BacktestRuntime.cpp` entry block brace mismatch) 복구.
- 검증:
  - Release build: `AutoLifeTrading` 성공.
  - 스모크: `AutoLifeTrading --backtest data/backtest/sample_trend_pullback_1m.csv --json` 정상 종료(코드 0).

## Verification Baseline Refresh (2026-02-21)
- 실행:
  - `python scripts/run_verification.py --data-dir .\\data\\backtest_real --dataset-names upbit_*_1m_12000.csv(6개) --data-mode fixed --validation-profile adaptive --require-higher-tf-companions ...`
- 코드 정합성 보강:
  - `scripts/run_verification.py`에서 삭제된 dead telemetry(`blocked_second_stage_confirmation`, `two_head_aggregation_blocked`) 집계 제거.
  - `scripts/run_verification.py`에 `probabilistic bundle ↔ dataset market coverage` 선검증 추가(기본 엄격 모드).
    - 미지원 마켓 포함 시 즉시 실패:
      `Probabilistic market coverage check failed ... upbit_KRW_ADA_1m_12000.csv:KRW-ADA`
- 결과:
  - baseline은 번들 지원 마켓 5개(`BTC/DOGE/ETH/SOL/XRP`)로 재생성:
    - `build/Release/logs/verification_report_baseline_current.json`
  - `dataset_count=5`
  - `avg_profit_factor=0.0`
  - `avg_expectancy_krw=-36.2498`
  - `overall_gate_pass=false`, `adaptive_verdict=fail`
  - `primary_non_execution_group=candidate_generation`
  - `primary_candidate_generation_component=no_signal_generated (share=0.5775)`
  - no-signal 상위 원인:
    - `foundation_no_signal_liquidity_volume_gate: 13483`
    - `probabilistic_market_prefilter: 1900`
    - `foundation_no_signal_uptrend_structure: 1774`
  - manager prefilter 상위 원인:
    - `filtered_out_by_manager: 15828`
    - `filtered_out_by_manager_ev_quality_floor: 5635`
    - `filtered_out_by_manager_strength: 5245`
  - 공급 개선 A/B 실험(`A`, `B`, `A+B`)은 baseline 대비 변화 없음(동일 지표).

## What Was Trimmed
- 레거시 후보튜닝/패치오버라이드 체인 스크립트 삭제.
- 대용량 archive full-log 문서 삭제.
- 본 문서는 "현재 방향 + 지금 해야 할 일"만 유지.

## Next Required Steps
1. `entry-quality edge base floor`를 확률 마진 연동형 floor로 재설계(레짐별 상/하한, 비우호 레짐 분리).
2. `filtered_out_by_manager_strength / ev_quality_floor` hard/soft 분해를 정식화(현재 soft queue는 보조 단계).
3. `후보 증가 -> 진입 증가 -> 수익성` 연결 계측 추가(전이 구간별 conversion contract).
4. 재설계 후 동일 6-dataset 회귀 + baseline delta 계약 재평가.

## Non-Negotiables
- 순차 실행 유지(병렬 검증/병렬 튜닝 금지).
- 코인 하드코딩 금지, 패턴/레짐 단위 일반화.
- 라이브/백테스트 데이터량 및 피처 계산 규칙 동일 유지.
