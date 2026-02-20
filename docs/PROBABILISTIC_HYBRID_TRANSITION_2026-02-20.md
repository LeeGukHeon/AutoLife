# Probabilistic Hybrid Transition Plan (2026-02-20)

Status: `PHASE_4_RUNTIME_RISK_COUPLED`

## Goal
- Threshold 튜닝 중심에서 `확률형 패턴 모델 + adaptive 리스크 엔진` 구조로 전환.
- 라이브/백테스트 동형성과 과적합 방지 규칙을 우선 보장.

## Data Source Contract
- Upbit API 수집 범위: `OHLCV 원천 캔들`만.
- 보조지표/컨텍스트/레이블은 로컬 파이프라인에서 생성.
- 동일 캔들 입력에 대해 라이브/백테스트 피처 출력이 일치해야 함.

## Fixed Direction
- 전략 운영: 초단타 스캘핑보다 품질형 진입/관리.
- 익절은 고정 퍼센트 목표가 아니라 `런너를 최대한 보유`하는 동적 구조를 채택.
- 기본 규칙: 부분익절 + 트레일링 + 레짐/모멘텀 악화 시 단계적 축소.
- TF 역할 분담:
  - 상위(15/60/240m): 레짐/컨텍스트
  - 하위(1/5m): 타이밍/실행

## Phase 1: Data Foundation
### Completed
- [x] Long-horizon `60m/240m` collected (EOS 404 제외)
- [x] Long-horizon `5m/15m` collected
- [x] `1m` recent-2y collected (`2024-02-20T00:00:00Z`~now, 9 markets)
- [x] Collector reliability upgrades
  - per-job manifest flush
  - `current_job/progress_pct/run_state` exposure
  - stagnant-loop break in candle fetch

### Artifacts
- `data/backtest_probabilistic/probabilistic_bundle_manifest.json`
- `build/Release/logs/probabilistic_bundle_summary.json`
- `data/backtest_probabilistic/probabilistic_split_manifest_v1.json`

## Phase 2: Feature/Label Contract
### Required
- [x] Build indicator + context feature pipeline base script
- [x] Run full feature build and freeze output manifest
- [x] Freeze schema/version using `config/model/probabilistic_feature_contract_v1.json`
- [x] Regenerate split manifest on final dataset

### Completed Snapshot
- Full feature build output:
  - `data/model_input/probabilistic_features_v1_full_20260220_181345/feature_dataset_manifest.json`
  - `build/Release/logs/probabilistic_feature_build_summary_full_20260220_181345.json`
- Full quality validation output:
  - `build/Release/logs/probabilistic_feature_validation_summary_full_20260220.json`
- Feature-split manifest (final dataset):
  - `data/model_input/probabilistic_features_v1_full_20260220_181345/probabilistic_split_manifest_v1.json`
- Scale:
  - markets: `9`
  - feature rows: `9,477,036`
  - output size: `3,736,769,423 bytes` (about `3.48 GB`)
- Validation:
  - status: `pass`
  - files: `9/9`
  - leakage consistency mismatch: `0`

### Feature Groups (v1)
- 선행 보조지표: RSI/ATR/EMA/MACD/BB width 등
- 컨텍스트: 상위TF 추세/변동성/유동성 레짐
- 실행 리스크: spread/slippage/fee-aware edge inputs

## Phase 3: Training
- [x] Baseline regeneration on frozen dataset
- [x] Initial model training + calibration
- [x] Initial walk-forward evaluation
- [x] Regime stability report (decomposition)
- [x] Inference parity validation (Python model vs manual logistic path)
- [x] Runtime bundle export/parity validation
- [ ] Non-degradation contract check before promotion

## Phase 4: Runtime Wiring (In Progress)
- [x] C++ runtime bundle loader in live/backtest runtime paths
- [x] C++ feature transform builder (36 features, Python contract-aligned)
- [x] Signal-level probabilistic score/edge adjustment hook (live/backtest 공통)
- [x] Runtime→Risk metadata coupling (`Signal/Position/TradeHistory`)
- [x] Live async fill path metadata defer/apply wiring
- [x] Risk post-entry controls probabilistic margin coupling
- [ ] runtime adjusted baseline re-freeze
- [ ] same-slice parity/regression report

### Baseline Snapshot
- `build/Release/logs/probabilistic_baseline_summary_full_20260220.json`
- weighted test:
  - `h5_logloss=0.6743`
  - `h5_brier=0.2406`
  - `h5_accuracy=0.6025`
  - `h5_mean_edge_bps=-12.1554`

### Initial Model Snapshot
- script:
  - `scripts/train_probabilistic_pattern_model.py`
- artifacts:
  - `build/Release/logs/probabilistic_model_train_summary_full_20260220.json`
  - `build/Release/models/probabilistic_pattern_v1_full_20260220`
- weighted test:
  - `h5_logloss=0.6606`
  - `h5_brier=0.2342`
  - `h5_accuracy=0.6025`
  - selected-edge (`~11.05%` coverage): `-10.5596 bps`
- baseline delta:
  - `logloss -0.0137`
  - `brier -0.0064`
  - `accuracy +0.0000`

### Walk-Forward / Parity Snapshot
- walk-forward decomposition:
  - `build/Release/logs/probabilistic_walkforward_report_full_20260220.json`
  - observed bottleneck: `trend_up_sync` bucket (low coverage, weaker selected-edge)
- inference parity:
  - `build/Release/logs/probabilistic_inference_parity_full_20260220.json`
  - status: `pass`
  - worst calibrated diff: `1.721e-15` (tol `1e-9`)

### Runtime Bundle Snapshot
- runtime bundle:
  - `config/model/probabilistic_runtime_bundle_v1.json`
  - generated via `scripts/export_probabilistic_runtime_bundle.py`
- runtime bundle parity:
  - `build/Release/logs/probabilistic_runtime_bundle_parity_full_20260220.json`
  - status: `pass`
  - worst diff: `1.110e-16` (tol `1e-9`)

## Non-Negotiables
- Sequential execution only.
- No lookahead leakage.
- Live/backtest feature parity mandatory.
- Single-dataset gain cannot be promoted.
