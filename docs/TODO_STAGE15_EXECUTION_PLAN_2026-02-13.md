# Stage 15 Execution TODO (Active)

Last updated: 2026-02-20
Status: `PROBABILISTIC_TRANSITION_ACTIVE`

## Purpose
- 장문 로그 중심 운영을 중단하고, 현재 실행 가능한 항목만 유지.
- 목표: 확률형 학습 기반 + adaptive runtime 결합 구조로 전환.

## Active Docs (Minimal)
- `docs/CHAPTER_CURRENT.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
- `docs/PROBABILISTIC_HYBRID_TRANSITION_2026-02-20.md`
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`

## Build Path (Fixed)
- CMake executable:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe`
- vcpkg toolchain:
  - `D:\MyApps\vcpkg\scripts\buildsystems\vcpkg.cmake`

## Archive Docs
- `docs/archive/CHAPTER_CURRENT_FULLLOG_2026-02-20.md`
- `docs/archive/TODO_STAGE15_EXECUTION_PLAN_2026-02-13_FULLLOG_2026-02-19.md`
- `docs/archive/SCRIPT_READING_TUNE_CANDIDATE_GATE_2026-02-19.md`
- `docs/archive/TUNE_CANDIDATE_GATE_LINE_AUDIT_2026-02-19.md`

## Current Progress
- [x] `60m/240m` long-horizon bundle collected (EOS 404 excluded).
- [x] `5m/15m` bundle collected.
- [x] `1m` recent-2y bundle finish (`2024-02-20`~now, 9 markets).
- [x] Fetch/manifest reliability hardening.
  - immediate progress flush (`run_state/progress_pct/current_job`)
  - stagnant pagination break in candle fetch
- [x] Feature/label base builder added.
  - `scripts/build_probabilistic_feature_dataset.py`
- [x] Full feature build (`prob_features_v1`) completion + manifest freeze.
  - output: `data/model_input/probabilistic_features_v1_full_20260220_181345`
  - manifest: `feature_dataset_manifest.json`
  - rows: `9,477,036`
- [x] Data source contract fixed.
  - Upbit: raw OHLCV only
  - Indicators/labels/features: local deterministic generation
- [x] Full feature dataset quality validation completed.
  - script: `scripts/validate_probabilistic_feature_dataset.py`
  - output: `build/Release/logs/probabilistic_feature_validation_summary_full_20260220.json`
  - result: `pass (9/9, 9,477,036 rows)`
- [x] Contract freeze completed.
  - `config/model/probabilistic_feature_contract_v1.json` updated with fixed 43-column schema
- [x] Split manifest regenerated on final feature dataset.
  - `data/model_input/probabilistic_features_v1_full_20260220_181345/probabilistic_split_manifest_v1.json`
- [x] Baseline regenerated on frozen split.
  - script: `scripts/generate_probabilistic_baseline.py`
  - output: `build/Release/logs/probabilistic_baseline_summary_full_20260220.json`
  - weighted test `h5_mean_edge_bps=-12.1554`
- [x] Initial probabilistic model training/calibration completed.
  - script: `scripts/train_probabilistic_pattern_model.py`
  - output: `build/Release/logs/probabilistic_model_train_summary_full_20260220.json`
  - model dir: `build/Release/models/probabilistic_pattern_v1_full_20260220`
  - weighted test:
    - `h5_logloss=0.6606` (vs baseline `0.6743`)
    - `h5_brier=0.2342` (vs baseline `0.2406`)
    - `h5_accuracy=0.6025` (same as baseline)
    - selected-edge (`~11.05%` coverage): `-10.5596 bps`
- [x] Walk-forward decomposition report completed.
  - script: `scripts/generate_probabilistic_walkforward_report.py`
  - output: `build/Release/logs/probabilistic_walkforward_report_full_20260220.json`
  - key finding: `trend_up_sync` quality bottleneck under low selected coverage
- [x] Inference parity validation completed.
  - script: `scripts/validate_probabilistic_inference_parity.py`
  - output: `build/Release/logs/probabilistic_inference_parity_full_20260220.json`
  - result: `pass` (`worst_cal_diff=1.721e-15`, tol `1e-9`)
- [x] Runtime deployment bundle exported.
  - script: `scripts/export_probabilistic_runtime_bundle.py`
  - output: `config/model/probabilistic_runtime_bundle_v1.json`
  - policy: latest fold per market
- [x] Runtime bundle parity validated.
  - script: `scripts/validate_runtime_bundle_parity.py`
  - output: `build/Release/logs/probabilistic_runtime_bundle_parity_full_20260220.json`
  - result: `pass` (`worst_diff=1.110e-16`, tol `1e-9`)
- [x] C++ runtime inference/feature path 연결 1차 완료.
  - `src/analytics/ProbabilisticRuntimeFeatures.cpp` 추가
  - `src/runtime/LiveTradingRuntime.cpp` / `src/runtime/BacktestRuntime.cpp` 보정 경로 연결
  - build: `AutoLifeTrading` Release 성공
- [x] Runtime↔Risk 결합 2차 완료 (확률 메타 직결).
  - `Signal/Position/TradeHistory`에 확률 메타(`h1/h5 cal, threshold, margin`) 추가
  - `RiskManager` 후행 제어에 확률 마진 반영
    - adaptive trailing gap
    - stagnation/recycle time guard
    - adaptive partial-exit ratio
  - 라이브 비동기 체결 경로에 pending metadata 적용(체결 시점 metadata 보존)
  - 상태 저장/복원 및 저널 리플레이(open/close/reduce) 확률 메타 직렬화 반영

## Next (Strict Order)
1. Runtime↔Risk 결합 반영 기준 baseline 재생성(현재 로직 스냅샷 고정).
2. Live/backtest same-slice regression(손익/공급량/후행리스크 telemetry) 검증.
3. Hard gate 비활성 유지 상태에서 score/expected_value + risk coupling 영향 분석.

## Guardrails
- Sequential only (`--max-workers 1` policy).
- No coin hardcoding (pattern/regime only).
- No lookahead leakage.
- Live/backtest data and feature parity required.
