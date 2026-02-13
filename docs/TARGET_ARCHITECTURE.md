# Target Architecture (2026-02-13)

## 구조/근본 한계 재진단 (코드 근거, 2026-02-13)
1. 오케스트레이션 단일체 구조
- `src/engine/TradingEngine.cpp`의 `run()` 루프에 스캔/신호/정책/주문/모니터링/상태저장/동기화/메트릭이 밀집돼 있다.
- 장애 전파 범위가 넓고, 기능별 성능 병목을 분리 추적하기 어렵다.

2. 온라인 학습은 아직 "통계 기반 휴리스틱" 단계
- `dynamic_filter_value`, `position_scale_multiplier`, 전략 통계 가중치는 적용되어 있으나(`src/engine/TradingEngine.cpp`, `src/engine/AdaptivePolicyController.cpp`), 탐색-활용(uncertainty-aware) 모델은 부재하다.
- `PerformanceStore`는 재구축형 집계이며(`src/engine/PerformanceStore.cpp`), 학습 파라미터 자체의 버전/감쇠/롤백 포인트 저장소가 없다.

3. 라이브-백테스트 결정 경로 불일치
- 라이브는 `OrderManager + myOrder WS + REST stale fallback` 상태동기화를 쓰지만(`src/execution/OrderManager.cpp`), 백테스트는 단일 마켓/합성 오더북/단순 체결 경로다(`src/backtest/BacktestEngine.cpp`).
- 결과적으로 파라미터 스윕이 "불변"으로 나오는 구간이 생기며, 실거래 체결 품질/재시도 비용이 제대로 반영되지 않는다.

4. 재기동 동기화는 스냅샷 중심
- 현재 `state/state.json` 저장/로드는 존재하지만(`TradingEngine::saveState/loadState`), 이벤트 저널(WAL) 기반의 deterministic replay가 없다.
- 인플라이트 주문과 정책 변경 이력의 원자적 복구 모델이 없어, 장애 시 재현 가능성이 낮다.

5. 업비트 규칙 대응의 동적성 부족
- `RateLimiter`에 기본 제한이 하드코딩되어 있고(`src/execution/RateLimiter.cpp`), 엔드포인트별 최신 정책 반영 자동화가 충분하지 않다.
- `orders/chance`, 호가 정책 메타데이터를 중심으로 한 "사전검증 캐시 계층"이 아직 목표 상태에 못 미친다.

6. 소액시드(5~10만원)에서 구조적 비용 압박
- 최소 주문금액(5,000 KRW)과 수수료/슬리피지의 고정비 비중 때문에 과매매 시 기대값이 급격히 악화된다.
- 따라서 "고승률 단일 지표"보다 비용차감 후 `Expectancy`, `Profit Factor`, 거래당 순엣지, 거래빈도 통제가 우선이다.

## 목표 아키텍처 (Enterprise + Small Seed + Restart-safe)
1. 모듈 경계: "모듈러 모놀리스 우선, 서비스 분리 가능 구조"
- `MarketDataPlane`: WS 중심 수집 + REST 보강 + 품질상태(지연, 누락) 태깅.
- `SignalPlane`: 전략은 alpha 후보만 산출(실행/리스크 책임 분리).
- `PolicyLearningPlane`: 후보 스코어링, 컨텍스트 밴딧, 실시간 파라미터 튜닝.
- `RiskCompliancePlane`: 하드 리스크 한도 + 거래소 규칙 검증 + 비용 후 엣지 게이트.
- `ExecutionPlane`: 주문 상태머신, 부분체결/재호가/취소/대체주문, 체결동기화.
- `StateAuditPlane`: 이벤트 저널 + 스냅샷 + 학습 체크포인트 + 감사로그.

2. 실시간 자체 학습 (운영 안전형)
- 학습 단위: `(strategy, regime, liquidity_bucket, volatility_bucket, spread_bucket)`.
- 업데이트: 체결 이벤트마다 온라인 통계(감쇠 포함) + 신뢰구간 추정.
- 의사결정: 보수적 Thompson/UCB + 리스크 캡(일손실/연속손실/최대노출) 내에서만 탐색 허용.
- 안전장치: 성능 급락(드리프트) 탐지 시 "보수 프로파일" 자동 전환 + 직전 안정 버전 롤백.

3. 실시간 파라미터 튜닝 (Fast/Slow Loop 이원화)
- Fast loop (1~5분): 필터/엣지/RR/사이징 multiplier 미세조정.
- Slow loop (1시간~1일): 전략 on/off, 버킷 가중치 재학습, 정책 기준선 재설정.
- 모든 변경은 `policy_change_log`에 이유/전후값/영향구간을 기록하고, 기여도 미달 시 자동 원복.

4. 재기동 동기화 유지 (Restart-safe by design)
- 저장소 3종:
  - `state/event_journal.jsonl`: 주문/체결/포지션/정책변경 append-only 이벤트.
  - `state/snapshot_state.json`: 주기 스냅샷.
  - `state/learning_state.json`: 학습 파라미터/감쇠/버전/롤백포인트.
- 부팅 절차:
  - snapshot 로드 -> journal replay -> 업비트 `accounts + open orders + myOrder WS` reconcile -> 차이 정산 -> 거래 재개.

5. 소액시드 전용 정책 프로파일 (`SmallSeedPolicy`)
- 기본값: 동시 1포지션, 스캔당 신규 1건, 평균 보유시간/거래빈도 캡, 손실구간 자동 디레버리징.
- 주문단위: 5,000 KRW lot 기반 정수 lot 스케줄링(수수료 reserve 포함).
- 진입조건: 비용차감 후 기대엣지 하한 + 체결가능성(유동성/스프레드) 하한 동시 충족 시에만 진입.

6. Live/Backtest 경로 일치화
- 정책선택/리스크게이트/주문상태머신을 공통 코어로 추출하고, 데이터/체결만 어댑터로 분리.
- 백테스트에도 라이브와 동일한 "gate reject reason" 및 "policy decision log"를 강제 출력.

## 업비트 통신 규칙 준수 기준 (공식 문서 기반, 2026-02-13 확인)
1. 레이트리밋/차단 규칙
- Quotation REST: 그룹별 초당 최대 10회 (IP 단위).
- Exchange REST: 기본적으로 계정당 초당 최대 30회, 주문 관련 엔드포인트는 `order` 그룹(예: 주문 가능 정보 조회는 초당 최대 8회)로 별도 제한.
- WebSocket 연결 요청: 초당 5회, 분당 100회.
- WebSocket 데이터 요청: 초당 5회, 분당 100회.
- 429 반복 시 418 차단으로 강화될 수 있으므로 자동 no-trade degrade + 점진 재개가 필요하다.

2. 사전검증/메타데이터 기반 주문 검증
- 주문 직전 `orders/chance` 캐시로 마켓별 주문 가능 여부와 금액 제한을 검증한다.
- 호가 정책(`orderbook/instruments`) 기반 tick-size 검증을 주문 파이프라인에 강제한다.
- KRW 호가단위 정책은 2025-07-31 개정 이력이 있으므로 하드코딩 최소화가 필수다.

3. 구현 원칙
- `Remaining-Req` 기반 그룹별 동적 토큰버킷 + 429/418 지수 백오프.
- WS 우선, WS stale 시 REST 보조(자동 fallback) 정책 유지.
- 규칙 위반 가능성이 감지되면 즉시 신규 진입 차단(포지션 축소만 허용).

4. 공식 참조 링크
- https://docs.upbit.com/kr/reference/rate-limits
- https://docs.upbit.com/kr/reference/available-order-information
- https://docs.upbit.com/kr/reference/list-orderbook-instruments
- https://docs.upbit.com/kr/changelog/krw_tick_unit_change_250731

## 목표 지표 (현실적 운영 KPI, 승률 포함)
1. 포트폴리오 레벨 (30일 롤링)
- `Expectancy > 0` (비용 차감 후).
- `Profit Factor >= 1.15` (스트레스 구간 `>= 1.00` 방어).
- `Max Drawdown <= 8%`.

2. 전략 레벨 승률/품질 목표 (역할별)
- Scalping/Mean Reversion: 승률 58%+, PF 1.08+.
- Momentum: 승률 50%+, PF 1.15+.
- Breakout: 승률 42%+, PF 1.25+.
- 승률은 포트폴리오 단일 하드게이트가 아니라 전략 역할별 목표로 관리한다.

3. 소액시드 KPI (50k/100k KRW)
- 거래당 순엣지(비용 후) 양수 유지.
- 과매매 억제: 스캔당 신규 주문 cap 준수율 100%.
- 재기동 후 정합성 오류 0건(포지션/주문/학습 상태).

4. 운영 안정성 KPI
- 429 연속/418로 인한 주문중단 0건/일.
- 정책 결정 감사로그 누락률 0%.
- 정책 변경 자동 롤백 성공률 100%.

## 아키텍처 전환 우선순위 (Updated)
1. `LearningStateStore` 구현
- `state/learning_state.json` 스키마: 버전, 버킷 통계, 감쇠, 정책 가중치, 롤백 포인트.
- Fast/Slow tuner가 동일 스키마를 읽고 쓰도록 통일.

2. 이벤트 저널 + 결정적 복구 파이프라인
- 주문/체결/포지션/정책변경 이벤트 append-only 저장.
- 부팅 시 deterministic replay + 거래소 실상태 reconcile 자동화.

3. `UpbitComplianceAdapter` 고도화
- `orders/chance` 캐시 + 호가 정책 메타데이터 + Remaining-Req 동적 레이트리밋을 단일 어댑터로 통합.
- 규칙 위반 징후 시 no-trade degrade 및 자동 복구 절차 구현.

4. Live/Backtest 경로 단일화
- 정책/리스크/실행 상태머신 공통코어화.
- 백테스트 어댑터는 데이터/체결 시뮬레이터만 교체하도록 정리.

5. `SmallSeedPolicy` 프로덕션 승격
- lot 스케줄러, 비용 후 엣지 하한, 스트레스 레짐 자동 디레버리징 기본 적용.
- 50k/100k 전용 회귀 테스트(전략별/레짐별) CI 게이트화.

6. 배포 게이트
- Walk-forward OOS + dry-run shadow + 재기동 복구 테스트 3종 동시 통과 시에만 실거래 확장.

## 전환 실행 현황 (2026-02-13, Stage 0~8 완료)
1. 완료된 단계
- Stage 0 (기준선 고정): baseline 캡처 스크립트 추가 및 최신 baseline 산출물 생성 완료.
  - `scripts/capture_baseline.ps1`
  - `build/Release/logs/*baseline_20260213_141137*`
- Stage 1 (모듈 경계 스캐폴딩): `core/contracts + adapters + orchestration` 추가, legacy 브리지 연결 완료.
  - `include/core/contracts/*`
  - `include/core/adapters/*`
  - `include/core/orchestration/TradingCycleCoordinator.h`
  - `src/core/adapters/*`
  - `src/core/orchestration/TradingCycleCoordinator.cpp`
- Stage 2 (LearningStateStore): 학습 상태 저장/복원 경로 연결 완료.
  - `include/core/state/LearningStateStoreJson.h`
  - `src/core/state/LearningStateStoreJson.cpp`
  - 저장소: `state/learning_state.json`
- Stage 3 (Event Journal + Recovery 심화 1차): append-only 저널 계층 + snapshot/replay 기본 경로 완료.
  - `include/core/state/EventJournalJsonl.h`
  - `src/core/state/EventJournalJsonl.cpp`
  - `src/engine/TradingEngine.cpp` (`snapshot_state.json` 저장/로드 + journal replay)
  - 저장소:
    - `state/event_journal.jsonl`
    - `state/snapshot_state.json` (신규 주 저장소, `state/state.json` 호환 유지)
  - 기능:
    - snapshot 워터마크(`snapshot_last_event_seq`) 기록
    - 부팅 시 snapshot 우선 로드 후 replay(`POSITION_OPENED/FILL_APPLIED/POSITION_REDUCED/POSITION_CLOSED`)
    - replay 중 reduced/closed 이벤트의 trade history 보강
    - reconcile 외부청산 시 `POSITION_CLOSED` 저널 기록
- Stage 3 심화 2차(검증 자동화): 실제 재기동 기반 복구 검증 루프 완료.
  - `scripts/validate_recovery_state.ps1`
  - `scripts/validate_recovery_e2e.ps1`
  - `scripts/validate_replay_reconcile_diff.ps1`
  - `scripts/validate_operational_readiness.ps1`
  - 산출물:
    - `build/Release/logs/recovery_state_validation.json`
    - `build/Release/logs/recovery_e2e_report.json`
    - `build/Release/logs/recovery_state_validation_strict.json`
    - `build/Release/logs/recovery_e2e_report_strict.json`
    - `build/Release/logs/replay_reconcile_diff_report.json`
    - `build/Release/logs/operational_readiness_report.json`
  - 검증 포인트:
    - snapshot/journal 정합성
    - replay 가능 이벤트 수
    - 엔진 로그의 replay/reconcile 마커 유무
    - replay 예측 포지션 vs reconcile 결과 분해 일관성
  - 2026-02-13 실제 재기동 로그 마커 확인:
    - `State snapshot loaded`
    - `State restore: no replay events applied (start_seq=5, journal_last_seq=4)`
    - `Account state sync summary: wallet_markets=..., reconcile_candidates=..., restored_positions=..., external_closes=...`
    - `Account state synchronization completed`
- Stage 4 (`UpbitComplianceAdapter`): 업비트 규칙 기반 사전검증/동적 제한/디그레이드 경로 추가 완료.
  - `include/core/adapters/UpbitComplianceAdapter.h`
  - `src/core/adapters/UpbitComplianceAdapter.cpp`
  - 연동:
    - `TradingEngine`의 core risk plane에 `UpbitComplianceAdapter` 연결
    - 실주문 직전 `validateEntry` 강제 호출
    - `UpbitHttpClient::post`의 `Remaining-Req` 연동 보강
  - 핵심 기능:
    - `orders/chance` 캐시 검증
    - `orderbook/instruments` tick-size 검증
    - `Remaining-Req` 기반 동적 압박 감지
    - 위반 징후 시 no-trade degrade + 자동 복구
- Stage 5 준비 1차(공통 실행 상태머신): live/backtest 공통 전이 규칙 스캐폴딩 완료.
  - `include/core/execution/OrderLifecycleStateMachine.h`
  - `src/core/execution/OrderLifecycleStateMachine.cpp`
  - `tests/TestExecutionStateMachine.cpp`
  - 연동:
    - `src/execution/OrderStateMapper.cpp` -> 코어 상태머신 위임
    - `src/backtest/BacktestEngine.cpp` -> `executeOrder`에서 코어 상태머신 사용
- Stage 5 준비 2차(실행 경로 수렴): backtest 주문 수명주기 처리를 `checkOrders` 경로로 승격.
  - `BacktestEngine`:
    - `executeOrder`는 `submitted` 이벤트를 큐(`pending_orders_`)에 적재
    - `checkOrders`에서 `filled` 전이 처리 및 거래 카운트 반영
    - `processCandle`에서 `checkOrders`를 실제 호출
  - `OrderManager`:
    - live 경로에서도 `Execution lifecycle` 공통 키 로그 출력
- Stage 6 (execution update 모델 일치화): live/backtest 공통 `ExecutionUpdate` 스키마/아티팩트/검증 자동화 반영 완료.
  - 공통 스키마:
    - `include/core/model/PlaneTypes.h` (`source/event/order_volume/terminal/ts_ms` 확장)
    - `include/core/execution/ExecutionUpdateSchema.h` (공통 빌더 + JSON 직렬화)
  - live 출력:
    - `src/execution/OrderManager.cpp` lifecycle 이벤트에서 공통 스키마 생성
    - `build/Release/logs/execution_updates_live.jsonl`
  - backtest 출력:
    - `src/backtest/BacktestEngine.cpp`의 `submitted/filled` 이벤트에서 동일 스키마 생성
    - `build/Release/logs/execution_updates_backtest.jsonl`
  - 자동 검증:
    - 신규 `scripts/validate_execution_parity.ps1`
    - `scripts/validate_operational_readiness.ps1`에 parity 리포트(`build/Release/logs/execution_parity_report.json`) 통합
    - `-StrictExecutionParity` 옵션으로 hard-fail 가능(기본은 기존 체인 비파괴)
- Stage 7 (small-seed CI 게이트 강화 + parity strict 닫기) 완료.
  - `scripts/validate_operational_readiness.ps1` 인터페이스 정리:
    - strict 조합 명시 지원: `-StrictLogCheck -StrictExecutionParity [-IncludeBacktest]`
    - legacy 호환 유지: `-NoStrictLogCheck` 그대로 유지
    - 충돌 조합(`-StrictLogCheck` + `-NoStrictLogCheck`)은 즉시 실패
  - live parity 아티팩트 생성 경로 고정:
    - 신규 툴: `AutoLifeLiveExecutionProbe` (`src/tools/LiveExecutionProbe.cpp`)
    - 보조 스크립트: `scripts/generate_live_execution_probe.ps1`
    - 산출물: `build/Release/logs/execution_updates_live.jsonl`
  - 최종 strict 게이트 통과:
    - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_operational_readiness.ps1 -StrictLogCheck -StrictExecutionParity -IncludeBacktest`
- Stage 8 (CI 워크플로우 연동 + 운영 권한 분리) 완료.
  - CI 워크플로우 분리:
    - PR 게이트: `.github/workflows/ci-pr-gate.yml`
    - strict live 게이트(스케줄/수동): `.github/workflows/ci-strict-live-gate.yml`
  - CI 실행 래퍼/fixture 추가:
    - `scripts/prepare_operational_readiness_fixture.ps1`
    - `scripts/run_ci_operational_gate.ps1`
  - 운영 권한 분리 runbook 문서화:
    - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md`

2. 현재 코드 경계 상태
- 기본 동작은 legacy 경로 유지(기능 플래그 기본값 OFF).
- 브리지 플래그(`EngineConfig`)를 통해 `core` 경로를 점진 활성화 가능.
  - `enable_core_plane_bridge`
  - `enable_core_policy_plane`
  - `enable_core_risk_plane`
  - `enable_core_execution_plane`

3. 검증 상태
- Release 빌드 통과.
- 테스트 통과:
  - `AutoLifeExecutionStateTest`
  - `AutoLifeTest`
  - `AutoLifeStateTest`
  - `AutoLifeEventJournalTest`
- live parity probe 통과:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/generate_live_execution_probe.ps1`
- 복구 검증 통과:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_recovery_state.ps1`
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_recovery_e2e.ps1`
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_recovery_e2e.ps1 -StrictLogCheck -OutputJson build/Release/logs/recovery_e2e_report_strict.json -StateValidationJson build/Release/logs/recovery_state_validation_strict.json`
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_replay_reconcile_diff.ps1 -Strict`
- parity 검증 통과:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_execution_parity.ps1 -Strict`
- 운영 준비 검증(최종 strict 조합) 통과:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_operational_readiness.ps1 -StrictLogCheck -StrictExecutionParity -IncludeBacktest`
- baseline 캡처 재실행 통과(기존 성능 지표 수준 유지).

4. 미완료(다음 단계 핵심)
- Stage 9: strict live 게이트 산출물의 장기 추세 대시보드화 및 경보 임계치 운영 자동화.

5. strict gate 실행 절차 / 실패 대응(runbook)
- 권장 실행 순서:
  - PR 게이트:
    - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_ci_operational_gate.ps1 -IncludeBacktest`
  - strict live 게이트:
    - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_ci_operational_gate.ps1 -IncludeBacktest -RunLiveProbe -StrictExecutionParity`
- 실패 대응:
  - `strict_failed_live_execution_updates_missing`:
    - live probe 재실행 후 `build/Release/logs/execution_updates_live.jsonl` 생성 여부 확인.
  - `strict_failed_execution_schema_mismatch`:
    - `build/Release/logs/execution_parity_report.json`의 `live.key_signature`, `backtest.key_signature` 비교.
  - `strict_log_check_failed_incomplete_markers`:
    - `build/Release/logs/autolife*.log`에서 복구 마커 3종 존재 여부(`snapshot loaded`, `replay`, `synchronization completed`) 점검.
  - `backtest_readiness_failed`:
    - `build/Release/logs/readiness_report.json` 및 `backtest_*_summary.csv` 재생성 후 입력 데이터셋 경로/개수 확인.
- 운영 권한/보안 상세:
  - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md` 참조.

6. 다음 컨텍스트 시작 프롬프트 (복사/붙여넣기)
- `현재 기준일: 2026-02-13.

이전 컨텍스트 진행 상태:
- Stage 0~8 완료.
- Stage 8 완료 내용:
  - CI 워크플로우 분리: .github/workflows/ci-pr-gate.yml, .github/workflows/ci-strict-live-gate.yml
  - CI 실행 래퍼/fixture: scripts/prepare_operational_readiness_fixture.ps1, scripts/run_ci_operational_gate.ps1
  - 운영 runbook: docs/STRICT_GATE_RUNBOOK_2026-02-13.md

이번 컨텍스트 목표(Stage 9):
1) strict live 게이트 결과 집계 자동화
- 입력: build/Release/logs/execution_parity_report.json, build/Release/logs/operational_readiness_report.json
- 일/주 단위 집계 JSON(또는 CSV) 산출 스크립트 추가

2) 추세/경보 임계치 자동화
- 경보 규칙(예: strict 실패 연속 N회, warning 비율 임계치 초과) 정의
- CI strict live 게이트에서 경보 리포트까지 생성

3) 문서 마감
- docs/NEXT_CONTEXT_DIRECTION_2026-02-13.md
- docs/TARGET_ARCHITECTURE.md
- Stage 9 운영 절차/실패 대응(runbook) 반영

제약:
- 최소 침습(file 단위 최소 변경), legacy 기본 동작 유지
- 기존 strict 복구 검증 체인 깨지지 않게 유지

마지막 검증:
- Release 빌드
- 테스트 4종 PASS
- validate_operational_readiness.ps1 (strict 조합) PASS
- Stage 9 신규 집계/경보 스크립트 PASS
- 실행한 명령/결과/산출물 경로를 요약 보고`

## Stage 9 Update (2026-02-13)
- strict live gate trend/alert automation is now integrated without changing legacy default behaviors.
- Added script:
  - `scripts/generate_strict_live_gate_trend_alert.ps1`
- Input reports:
  - `build/Release/logs/execution_parity_report.json`
  - `build/Release/logs/operational_readiness_report.json`
- New outputs:
  - `build/Release/logs/strict_live_gate_history.jsonl`
  - `build/Release/logs/strict_live_gate_daily_summary.json`
  - `build/Release/logs/strict_live_gate_weekly_summary.json`
  - `build/Release/logs/strict_live_gate_daily_summary.csv`
  - `build/Release/logs/strict_live_gate_weekly_summary.csv`
  - `build/Release/logs/strict_live_gate_alert_report.json`
- Alert rules (default):
  - consecutive strict failures >= 2
  - warning ratio > 0.30 in last 7 days (min 3 runs)
- CI strict live workflow wiring:
  - `.github/workflows/ci-strict-live-gate.yml`
  - step `Generate Strict Live Trend + Alert Reports` runs with `if: always()`.
- Runbook coverage:
  - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md` includes Stage 9 operational procedure and failure triage.

## Stage 10 Update (2026-02-13)
- strict live gate threshold tuning and action-response automation is integrated with legacy-safe defaults.
- Updated script:
  - `scripts/generate_strict_live_gate_trend_alert.ps1`
- New outputs:
  - `build/Release/logs/strict_live_gate_threshold_tuning_report.json`
  - `build/Release/logs/strict_live_gate_action_response_report.json`
- Tuning logic:
  - consecutive failure threshold recommendation from historical alert-rate target.
  - warning ratio threshold recommendation from daily warning-ratio quantile.
  - baseline thresholds stay active when history samples are insufficient.
- Action-response logic:
  - auto classification for missing reports / parity errors / operational errors / sustained failures / high warning ratio.
  - per-policy command hints and escalation levels in action response report.
- CI strict live workflow:
  - `.github/workflows/ci-strict-live-gate.yml`
  - stage report step runs with `-ApplyTunedThresholds`.
- Runbook coverage:
  - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md` includes Stage 10 tuning and action-response procedures.

## Stage 11 Update (2026-02-13)
- action execution boundaries are now separated in strict live action policies:
  - `report-only` for critical and resume-gated policies.
  - `safe-auto-execute` for selected low-risk warning/info policies.
- approval-based resume contract is added in action response artifact:
  - `manual_approval.approval_required_for_resume`
  - `manual_approval.required_evidence_paths`
  - `manual_approval.resume_checklist`
- alert-to-action feedback loop is now wired into threshold tuning:
  - previous run `feedback_for_next_tuning` is consumed when `-EnableActionFeedbackLoop` is enabled.
  - stabilization criteria guard is required before applying feedback adjustment:
    - `feedback_stabilization_min_history_runs` (default `21`)
    - `feedback_stabilization_min_warning_samples` (default `10`)
  - false-positive/false-negative mitigation signals:
    - `reduce_false_positive_risk`
    - `reduce_false_negative_risk`
    - `stable`
- CI strict live workflow integration:
  - `.github/workflows/ci-strict-live-gate.yml`
  - stage report step runs with:
    - `-ApplyTunedThresholds`
    - `-ActionExecutionPolicy safe-auto-execute`
    - `-EnableActionFeedbackLoop`

## Stage 12 Update (2026-02-13)
- manual approval enforcement is now wired to GitHub Environment reviewers:
  - `.github/workflows/ci-strict-live-gate.yml`
  - `Extract Manual Approval Requirement` step reads:
    - `strict_live_gate_action_response_report.json`
    - `manual_approval.approval_required_for_resume`
  - conditional job `strict_live_resume_approval_gate` is triggered only when manual approval is required.
  - resume approval job is bound to Environment `strict-live-resume` (required reviewers boundary).
- feedback-loop operational stabilization is extended with weekly drift checks:
  - `scripts/generate_strict_live_gate_trend_alert.ps1`
  - weekly accumulation fields in tuning report:
    - `tuning_readiness.feedback_weekly_*`
    - `feedback_loop.weekly_signal`
  - drift/mixed-signal detection now pauses feedback-based threshold adjustments.
- false-positive/false-negative correction guardrails are explicitly formalized:
  - threshold bounds:
    - consecutive failure threshold min/max
    - warning ratio threshold min/max
  - feedback delta caps:
    - `consecutive_failure_threshold_delta_cap = 2`
    - `warning_ratio_threshold_delta_cap = 0.10`

## Stage 13 Update (2026-02-13)
- profitability validation matrix is now automated for architecture-path comparison:
  - `scripts/run_profitability_matrix.ps1`
  - profile set:
    - `legacy_default`
    - `core_bridge_only`
    - `core_policy_risk`
    - `core_full`
- generated artifacts:
  - `build/Release/logs/profitability_matrix.csv`
  - `build/Release/logs/profitability_profile_summary.csv`
  - `build/Release/logs/profitability_gate_report.json`
- gate model:
  - profile-level KPI gates:
    - profit factor / expectancy / drawdown / profitable ratio / average trade count
  - structural regression guard:
    - `core_full` deltas vs `legacy_default` for PF/Expectancy/Total Profit
  - optional hard-fail mode:
    - `-FailOnGate`
  - optional OOS context merge:
    - `-IncludeWalkForward`

## Stage 14 Update (2026-02-13)
- personal-use distribution baseline is now documented and scripted:
  - `README.md`
  - `docs/PERSONAL_USE_NOTICE.md`
  - `scripts/apply_trading_preset.ps1`
  - `config/presets/safe.json`
  - `config/presets/active.json`
- PR CI now includes exploratory profitability report as non-blocking telemetry:
  - `.github/workflows/ci-pr-gate.yml`
  - step `Generate Profitability Exploratory Report (Non-blocking)` with `continue-on-error: true`
- profitability matrix gating now supports sparse-sample filtering:
  - `-ExcludeLowTradeRunsForGate`
  - `-MinTradesPerRunForGate`
  - profile summaries now expose `runs_used_for_gate` and `excluded_low_trade_runs`.
