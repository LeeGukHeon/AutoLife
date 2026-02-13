# Context Reset Handoff (2026-02-13)

## 현재 방향성 고정
- 목표: 실거래 안정성 우선 + 업비트 규칙 준수 + 전략 구조 재설계.
- 전략 목표 분리:
  - `scalping`: 고승률/저R
  - `breakout`: 저빈도/고R
  - `mean_reversion`: 횡보 전용
- 최적화 기준: 단일 승률이 아니라 `레짐별 승률 + PF 이중게이트`.

## 이번까지 완료된 핵심 변경
1. `StrategyManager` 구조 개편
- 레짐 정책: `ALLOW / HOLD / BLOCK`
- 역할 기반 라우팅(스캘핑/모멘텀/브레이크아웃/평균회귀/그리드)
- `filterSignals` + `selectRobustSignal`에 정책 반영
- 파일:
  - `include/strategy/StrategyManager.h`
  - `src/strategy/StrategyManager.cpp`

2. 선택기 재설계
- 단일 최고점 선택에서 합의 기반 선택(`selectRobustSignal`) 도입
- 스트레스 레짐 단일 신호 차단 규칙 추가
- 저성능 전략(저승률/저PF) 강한 디그레이드 추가

3. 전략 상태관리 일원화
- 엔진에 흩어져 있던 전략 ON/OFF 판단을 `StrategyManager`로 집약
- `refreshStrategyStatesFromHistory(...)` 추가
- 파일:
  - `src/strategy/StrategyManager.cpp`
  - `src/engine/TradingEngine.cpp`

4. 백테스트/검증 파이프라인 확장
- CLI 백테스트 전략 오버라이드 추가: `--strategies a,b,c`
- readiness 스크립트를 전략 단독/조합 매트릭스로 확장
- 파일:
  - `src/main.cpp`
  - `include/common/Config.h`
  - `scripts/validate_readiness.ps1`

5. 데이터 품질 진단 + 신규 테스트셋 생성
- 데이터 감사 스크립트 추가: `scripts/audit_backtest_data.ps1`
- 신규 장기 레짐 혼합 데이터셋 생성 스크립트 추가:
  - `scripts/generate_backtest_dataset.ps1`
  - 생성 파일: `data/backtest_curated/regime_mix_12000_1m.csv`
- 감사 결과(신규 파일):
  - 타임스탬프 단위 일관(ms), 길이 충분(12000), OHLC 무결성 정상

6. 전략 튜닝 1차
- `Scalping`: 스프레드/비용/엣지 하드게이트 재구성 + 빈도 바닥 보정
- `Momentum`: 강도/RR/엣지 컷 완화로 표본 생성 시작
- `MeanReversion`: 진입 완화 및 소액 계좌 최소주문(5000원) 충돌 보정
- 파일:
  - `src/strategy/ScalpingStrategy.cpp`
  - `src/strategy/MomentumStrategy.cpp`
  - `src/strategy/MeanReversionStrategy.cpp`

## 최신 관찰 결과 (regime_mix_12000_1m 기준)
- `breakout` 계열이 현재 주 수익원.
- `momentum`은 표본 0에서 1건으로 개선(아직 매우 부족).
- `mean_reversion`은 여전히 0건(신호 생성 로직 자체 추가 완화 필요).
- `scalping`은 현재 데이터셋에서 과보수 상태로 0건.
- 즉시 과제는 “전략별 최소 유효 표본 확보”이며, 아직 실전 준비 단계 아님.

## 추가 진행(이번 세션 후반)
- 신규 데이터셋 2개 추가 생성:
  - `data/backtest_curated/range_bias_12000_1m.csv`
  - `data/backtest_curated/trend_down_shock_12000_1m.csv`
- 생성 스크립트 프로파일 확장:
  - `scripts/generate_backtest_dataset.ps1` (`mixed`, `range_bias`, `trend_down_shock`)
- 최신 매트릭스 결과(3개 데이터셋 기준):
  - `breakout`는 거래는 발생하나 `range_bias`/`trend_down_shock`에서 손실.
  - `momentum`은 소량 표본만 생성.
  - `mean_reversion`은 여전히 0건 (코어 발화 로직 추가 수정 필요).
  - 전체 readiness는 악화(`profitable_ratio=0.3333`).
- 결과 파일:
  - `build/Release/logs/backtest_curated_matrix.csv`
  - `build/Release/logs/backtest_curated_strategy.csv`
  - `build/Release/logs/backtest_curated_profile.csv`
  - `build/Release/logs/backtest_curated_readiness.json`

## 다음 해야 할 일 (우선순위)
1. `MeanReversion` 신호 생성 코어 완화
- `analyzeMeanReversion` / `shouldGenerateMeanReversionSignal`의 유효성/확률/confidence 임계값을 직접 조정.
- 목표: `mean_reversion_only` 최소 거래 표본 생성.

2. `Scalping` 재활성 튜닝
- 현재 curated 데이터에서 0건인 원인 로그 기반으로 재조정.
- 목표: 과도한 차단 없이 손실성 진입만 차단.

3. 데이터셋 보강
- `range_bias_12000_1m.csv`(횡보 편향) 생성 완료.
- `trend_down_shock_12000_1m.csv` 생성 완료.
- 각 전략의 전문 레짐에서 표본을 먼저 확보.

4. 그 다음 `myOrder` 상태머신 복귀
- 전략 표본이 최소한 확보된 뒤, 주문 상태머신 구현으로 실거래 안정성 단계 진행.

## 실행 명령 메모
- 데이터 생성:
`powershell -ExecutionPolicy Bypass -File scripts/generate_backtest_dataset.ps1 -OutputPath data\backtest_curated\regime_mix_12000_1m.csv -Candles 12000 -StartPrice 1200000 -Seed 20260213`
- 데이터 감사:
`powershell -ExecutionPolicy Bypass -File scripts/audit_backtest_data.ps1 -DataDir data\backtest_curated -OutputCsv build\Release\logs\backtest_curated_audit.csv -OutputJson build\Release\logs\backtest_curated_audit.json`
- 전략 매트릭스 검증:
`powershell -ExecutionPolicy Bypass -File scripts/validate_readiness.ps1 -DataDir data\backtest_curated -OutputCsv build\Release\logs\backtest_curated_matrix.csv -OutputStrategyCsv build\Release\logs\backtest_curated_strategy.csv -OutputProfileCsv build\Release\logs\backtest_curated_profile.csv -OutputJson build\Release\logs\backtest_curated_readiness.json -MinTrades 10`

## 재시작 프롬프트(권장)
`이전 창에서 전략 구조개편(StrategyManager 정책화, 전략 매트릭스 백테스트, curated 데이터셋 생성)까지 완료됨. 지금은 mean_reversion/scalping 표본 확보가 병목이니 해당 전략 신호 생성 임계값부터 이어서 튜닝해줘. UTF-8 유지.`
