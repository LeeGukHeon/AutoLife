# Probabilistic Execution Architecture (Current Implementation)

Last updated: 2026-02-23  
Status: `ACTIVE_IMPLEMENTED_BASELINE`

## 1. 문서 목적
- 현재 코드베이스의 실제 구현 상태를 기준으로, 확률형 실행 모델의 설계/운영 기준을 단일 문서로 고정한다.
- 실험 아이디어가 아니라, 지금 바로 운영/검증에 쓰는 구조를 명확히 정의한다.
- 라이브/백테스트 동형성, 데이터 무결성, 과적합 방지 원칙을 우선한다.

## 2. 목표와 범위
### 2.1 목표
- 확률형 우선 의사결정(`P(win)` + `expected edge`) 기반으로 진입/사이징/리스크를 통합한다.
- 하드게이트 남발을 줄이고, 운영상 필수 안전장치 중심으로 일관된 실행 경로를 유지한다.
- 소액 계좌부터 일반 계좌까지 동일 로직으로 안정 동작한다.

### 2.2 범위
- 포함:
  - 데이터 수집, 피처/라벨 생성, 학습, 런타임 번들 반영, 검증 게이트.
  - Live/Backtest 공통 확률 추론 경로와 리스크 연동.
- 제외:
  - 초저지연 HFT, 거래소 외부 데이터 의존 전략, 수익 보장 가정.

## 3. 고정 설계 원칙
1. 데이터/로직 동형성
- 라이브/백테스트는 동일 피처 계약과 동일 추론 번들을 사용한다.

2. 누수 방지
- 피처는 시점 `t`까지 캔들만 사용, 라벨은 `(t, t+h]`만 사용한다.
- 랜덤 분할 금지, 시간순 워크포워드만 허용한다.

3. 운영 필수 게이트 우선
- 최소주문, 수수료/슬리피지, 포지션 캡, 실시간 veto, 레짐 방어 등 필수 안전장치 유지.
- 비필수 품질 게이트는 확률형 신호를 방해하지 않도록 축소/재배치한다.

4. 점진 업데이트
- 풀 리프레시보다 증분 수집/재학습을 기본 운용 방식으로 삼는다.

## 4. 핵심 구성요소와 책임
## 4.1 데이터 수집 레이어
- 스크립트: `scripts/fetch_probabilistic_training_bundle.py`
- 하위 수집기: `scripts/fetch_upbit_historical_candles.py`
- 핵심 기능:
  - 마켓/TF 조합 순차 수집
  - 증분 갱신(`--incremental-update`, `--incremental-overlap-bars`)
  - 디스크 가드(`--min-free-gb`, `--max-output-gb`, `--disk-budget-policy`)
  - 매니페스트 실시간 기록

단일 진실원:
- `data/backtest_probabilistic/probabilistic_bundle_manifest.json`

## 4.2 피처/라벨 빌드 레이어
- 스크립트: `scripts/build_probabilistic_feature_dataset.py`
- 앵커 TF: `1m`
- 컨텍스트 TF: `5m, 15m, 60m, 240m`
- 기본 계약:
  - 피처 36개 + 메타/라벨 포함 총 43컬럼 계약
  - 라벨: `label_up_h1`, `label_up_h5`, `label_edge_bps_h5`
- 확장 옵션:
  - triple-barrier 라벨(`--enable-triple-barrier-labels`)은 선택 활성화

계약 파일:
- `config/model/probabilistic_feature_contract_v2.json`

## 4.3 학습 레이어
- 전역 학습: `scripts/train_probabilistic_pattern_model_global.py`
- 기반 학습기: `scripts/train_probabilistic_pattern_model.py`
- 출력:
  - 학습 요약 JSON
  - 모델 아티팩트(joblib)
- 모델 헤드:
  - 확률 분류 헤드(`h1/h5`)
  - 기대값 보강(`edge_profile`, optional `edge_regressor`)

## 4.4 런타임 번들 레이어
- 스크립트: `scripts/export_probabilistic_runtime_bundle.py`
- 모드:
  - `global_only`
  - `hybrid`
  - `per_market`
- 검증:
  - `scripts/validate_runtime_bundle_parity.py`

배포 번들:
- `config/model/probabilistic_runtime_bundle_v2.json`

## 4.5 실행 엔진 레이어
- Live: `src/runtime/LiveTradingRuntime.cpp`
- Backtest: `src/runtime/BacktestRuntime.cpp`
- 런타임 모델: `src/analytics/ProbabilisticRuntimeModel.cpp`
- 엔진 설정: `include/engine/EngineConfig.h`, `config/config.json`

핵심 흐름:
- 시장 스캔 -> 확률 prefilter -> 신호 보정 -> 순위화 -> 진입/사이징 -> 포지션 후행관리

## 5. 현재 운영 정책(고정)
## 5.1 수집 윈도우 정책
- 상위 TF(`5/15/60/240m`): 상장 이후 ~ 현재
- `1m`: 최근 2년 ~ 현재

## 5.2 스캔 정책
- 스캔 대상은 유동성 기준 상위 마켓 중심(현재 구현 상 top 25 상세 분석 경로).
- 멀티 TF 캔들 캐시를 유지하고, 스캔 주기마다 증분 반영한다.

## 5.3 라이브 안전 정책
- `allow_live_orders=false`를 기본으로 시작.
- 확정봉 기반 신호 + 실시간 진입 veto 동시 사용.
- 웜업 스캔/준비율(`live_cache_warmup_*`) 충족 전 신규 진입 차단.

## 5.4 소액 계좌 대응 정책
- 최소주문/수수료 reserve/계좌 구간별 최대 주문 비중 적용.
- 다중 시그널 상황에서도 주문 캡과 포지션 캡을 함께 적용.

## 6. 런타임 의사결정(상세)
1. Market prefilter
- 확률 마진 기반 마켓 사전 필터(`probabilistic_runtime_scan_prefilter_*`)
- 번들/피처 차원 불일치, 미지원 마켓은 strict mode에서 fail-closed 처리

2. Signal enrichment
- `prob_h1/h5`, threshold, margin, expected edge 계산
- online-learning bias(최근 결과 기반)로 margin/strength 미세 조정

3. Candidate ranking
- 확률/edge 중심 우선순위화
- 필수 운영 게이트 통과 후보만 진입 큐로 승격

4. Entry sizing
- 확률 신뢰도와 위험 상태를 반영해 포지션 크기 동적 조정
- 소액 계좌 tier cap + 최소주문 충돌 방지 적용

5. Post-entry risk control
- adaptive trailing
- adaptive partial ratio
- stagnation/recycle 관리
- hostile regime에서 방어적 축소

## 7. 검증 무결성 기준
## 7.1 데이터 무결성
- `validate_probabilistic_feature_dataset.py --strict`
- 통과 조건:
  - schema mismatch 0
  - timestamp order violation 0
  - leakage mismatch 0

## 7.2 추론 무결성
- runtime bundle parity `pass` 필수
- 학습 추론값과 번들 추론값의 허용 오차 내 일치 필수

## 7.3 전략 검증
- `scripts/run_verification.py`로 OOS/리스크/게이트 결과 확인
- baseline 대비 악화 여부를 PF/expectancy/DD로 동시 판정

## 8. 수집 완료 후 표준 실행 순서
1. 수집 완료 고정
- 매니페스트 `run_state=completed` 확인

2. 포맷 변환(권장)
- 원본 CSV 보존
- 학습/보관 경로는 `Parquet + ZSTD`(권장) 또는 `csv.zst` 전환

3. 피처 생성
- 1차 계약 피처 생성
- 필요 시 2차(미시구조/호가/체결압력) 피처 버전 추가

4. 학습
- global 재학습
- 권장: hybrid 번들 생성(global + per-market calibration)

5. 테스트
- bundle parity -> verification -> shadow run
- 통과 후만 live enable

## 9. 현재 구현 완성도(정리)
- 완료:
  - 확률형 런타임 모델 연결
  - global 학습 및 번들 export/parity 경로
  - 증분 수집과 디스크 가드
  - live/backtest 공통 확률 추론/보정
- 진행 중:
  - 대규모 전 마켓 `1m 최근 2년` 수집
- 다음 핵심:
  - 포맷 전환(압축 저장)
  - 2차 피처 추가 버전
  - hybrid 운용 고도화

## 10. 운영 가드레일
- 기본 실행은 순차 모드 유지(재현성 우선).
- 코인 하드코딩 금지(패턴/레짐 기반 일반화 유지).
- lookahead 금지.
- 라이브 전환은 shadow 통과 후 단계적 증액만 허용.

## 11. 실행 엔트리
원클릭 파이프라인:
- `scripts/run_probabilistic_hybrid_cycle.py`
- 수행 단계:
  - fetch -> feature build/validate -> split -> baseline -> global train -> export -> parity

참고:
- 현재 파이프라인 기본 export 모드는 `global_only`.
- 운영 전환 목표는 `hybrid` 기준으로 문서/스크립트 정렬한다.
