# Next Context Direction (2026-02-13)

## Current Objective
- 목표: `TARGET_ARCHITECTURE` 기준으로 기존 코드베이스를 깨지 않고 단계적으로 이전.
- 운영 제약: 실거래 안정성/업비트 규칙 준수 우선, API 키 처리 방식은 현행 유지.
- 전략: `legacy 경로 유지 + core 경로 점진 활성화` (플래그 기반 전환).

## Latest Progress Snapshot (2026-02-13)
1. Stage 0 완료: baseline 고정
- `scripts/capture_baseline.ps1` 추가.
- baseline 산출물 생성 완료:
  - `build/Release/logs/small_seed_matrix_baseline_20260213_141137.csv`
  - `build/Release/logs/readiness_report_baseline_20260213_141137.json`
  - `build/Release/logs/walk_forward_report_baseline_20260213_141137.json`

2. Stage 1 완료: core 스캐폴딩 + 브리지
- 신규 구조:
  - `include/core/model/PlaneTypes.h`
  - `include/core/contracts/*`
  - `include/core/adapters/*`
  - `include/core/orchestration/TradingCycleCoordinator.h`
  - `src/core/adapters/*`
  - `src/core/orchestration/TradingCycleCoordinator.cpp`
- 엔진 플래그/연결:
  - `include/engine/EngineConfig.h`
  - `src/common/Config.cpp`
  - `include/engine/TradingEngine.h`
  - `src/engine/TradingEngine.cpp`

3. Stage 2 완료: LearningStateStore 연결
- 신규:
  - `include/core/state/LearningStateStoreJson.h`
  - `src/core/state/LearningStateStoreJson.cpp`
- 연동:
  - `TradingEngine::loadLearningState()`
  - `TradingEngine::saveLearningState()`
- 저장 파일:
  - `state/learning_state.json`

4. Stage 3(기초) 완료: Event Journal append 계층
- 신규:
  - `include/core/state/EventJournalJsonl.h`
  - `src/core/state/EventJournalJsonl.cpp`
- 연동 이벤트:
  - policy decision, order submit, fill applied, position opened/reduced/closed
- 저장 파일:
  - `state/event_journal.jsonl`

5. Stage 4 완료: `UpbitComplianceAdapter` 통합
- 신규:
  - `include/core/adapters/UpbitComplianceAdapter.h`
  - `src/core/adapters/UpbitComplianceAdapter.cpp`
- 연동:
  - `TradingEngine` core risk plane을 `UpbitComplianceAdapter`로 연결
  - 실주문 직전 `core_cycle_->validateEntry(...)` 호출로 compliance gate 강제
  - `UpbitHttpClient::post()`에도 `Remaining-Req` 파싱/429·418 처리 반영
- 핵심 기능:
  - `orders/chance` 캐시 기반 주문 가능/최소금액 검증
  - `orderbook/instruments` 기반 tick-size 검증(+ fallback)
  - `Remaining-Req` 기반 동적 압박 감지
  - 위반 징후 시 no-trade degrade 및 시간 기반 자동 복구

6. Stage 3 심화 1차 완료: snapshot 분리 + deterministic replay 기본 경로
- 연동:
  - `TradingEngine::saveState()`가 `state/snapshot_state.json`을 주 저장소로 기록
  - snapshot에 `snapshot_last_event_seq` 워터마크 저장
  - `TradingEngine::loadState()`가 snapshot 우선 로드 후 `event_journal` replay 적용
  - replay 대상: `POSITION_OPENED/FILL_APPLIED/POSITION_REDUCED/POSITION_CLOSED`
- 복구 강화:
  - replay 과정에서 open position 재구성 + trade history 보강(REDUCED/CLOSED)
  - 외부 청산 reconcile 시 `POSITION_CLOSED` 저널 기록 추가
- 검증 보강:
  - 신규 스크립트 `scripts/validate_recovery_state.ps1`
    - snapshot/journal 존재/파싱/seq 증가/워터마크 정합성 검사
    - replay 후 예측 open position 수 산출 리포트 생성

7. Stage 3 심화 2차(검증 자동화) 완료
- 신규:
  - `scripts/validate_recovery_e2e.ps1`
  - `scripts/validate_replay_reconcile_diff.ps1`
  - `scripts/validate_operational_readiness.ps1`
- 기능:
  - `validate_recovery_state.ps1` 선행 실행 및 결과 결합
  - 엔진 로그(`autolife*.log`)에서 복구 단계 마커 자동 스캔
    - `State snapshot loaded`
    - `State restore: journal replay applied` 또는 `no replay events applied`
    - `Account state synchronization completed`
  - 통합 리포트 생성:
    - `build/Release/logs/recovery_e2e_report.json`
    - `build/Release/logs/recovery_e2e_report_strict.json`
    - `build/Release/logs/replay_reconcile_diff_report.json`
    - `build/Release/logs/operational_readiness_report.json`
- 상태:
  - 2026-02-13 실제 재기동 1회 수행 후 복구 마커 3종 확인
  - 실제 경로(`build/Release/state/*`, `build/Release/logs/autolife.log`) 기준 `validate_recovery_e2e.ps1` 경고 0 PASS
  - `-StrictLogCheck` 모드 PASS
  - replay vs reconcile 차이 요약(`validate_replay_reconcile_diff.ps1 -Strict`) PASS
  - 운영/CI 진입점으로 `validate_operational_readiness.ps1` 추가

8. Stage 5 준비 1차 완료: 공통 실행 상태머신 스캐폴딩
- 신규:
  - `include/core/execution/OrderLifecycleStateMachine.h`
  - `src/core/execution/OrderLifecycleStateMachine.cpp`
  - `tests/TestExecutionStateMachine.cpp`
- 연동:
  - `src/execution/OrderStateMapper.cpp`가 코어 상태머신 위임 사용
  - `src/backtest/BacktestEngine.cpp`의 체결 처리(`executeOrder`)가 코어 상태머신 사용
- 검증:
  - `AutoLifeExecutionStateTest` PASS
  - 기존 `AutoLifeStateTest`/`AutoLifeTest`/`AutoLifeEventJournalTest` 회귀 PASS

9. Stage 5 준비 2차 완료: Backtest `checkOrders` 실행 경로 + 수명주기 로그 포맷 수렴
- 연동:
  - `BacktestEngine::executeOrder()`는 즉시 체결 대신 `pending_orders_` 큐에 `submitted` 이벤트 적재
  - `BacktestEngine::checkOrders()`에서 `filled` 전이 처리 및 거래 카운트 반영
  - `processCandle()`에서 `checkOrders()`를 실제 호출하도록 연결
  - `OrderManager`에도 동일 키 포맷의 `Execution lifecycle: source=..., event=..., ...` 로그 추가
- 효과:
  - live/backtest 모두 공통 상태머신 전이 규칙(`OrderLifecycleStateMachine`) + 공통 로그 키 사용
  - 기존 리스크/포지션 처리 순서는 유지(주문 수명주기 처리만 큐 경유로 정리)

10. Stage 6 완료: live/backtest execution update 모델 일치 + parity 자동 검증
- 공통 스키마:
  - `include/core/model/PlaneTypes.h`의 `ExecutionUpdate`를 `source/event/order_volume/terminal/ts_ms`까지 확장
  - `include/core/execution/ExecutionUpdateSchema.h`로 공통 빌더/문자열 변환/JSON 스키마 직렬화 통일
- live 경로:
  - `src/execution/OrderManager.cpp`의 lifecycle 로그 지점에서 공통 `ExecutionUpdate` 생성
  - JSONL 아티팩트 출력: `build/Release/logs/execution_updates_live.jsonl`
- backtest 경로:
  - `src/backtest/BacktestEngine.cpp`의 `executeOrder(submitted)`/`checkOrders(filled)`에서 동일 스키마 생성
  - JSONL 아티팩트 출력: `build/Release/logs/execution_updates_backtest.jsonl`
- 검증 자동화:
  - 신규 `scripts/validate_execution_parity.ps1` 추가(스키마/enum/양측 호환성 점검)
  - `scripts/validate_operational_readiness.ps1`에 parity 리포트 결합
    - 기본 모드: 산출물 누락 시 warning 기반(기존 strict 복구 체인 비파괴)
    - 옵션: `-StrictExecutionParity`로 누락/불일치 hard-fail 가능
- 신규 리포트:
  - `build/Release/logs/execution_parity_report.json`

11. Stage 7 완료: small-seed CI 게이트 고도화 + execution parity strict 닫기
- CI 진입점 정리:
  - `scripts/validate_operational_readiness.ps1`에 `-StrictLogCheck` 명시 옵션 추가
  - strict 조합 표준화: `-StrictLogCheck -StrictExecutionParity [-IncludeBacktest]`
  - legacy 호환 유지: `-NoStrictLogCheck` 옵션 유지
- live parity strict 닫기:
  - 신규 probe 실행 파일: `AutoLifeLiveExecutionProbe` (`src/tools/LiveExecutionProbe.cpp`)
  - 자동 실행 스크립트: `scripts/generate_live_execution_probe.ps1`
  - live 아티팩트 생성 확인: `build/Release/logs/execution_updates_live.jsonl`
- 최종 strict 검증:
  - `validate_execution_parity.ps1 -Strict` PASS
  - `validate_operational_readiness.ps1 -StrictLogCheck -StrictExecutionParity -IncludeBacktest` PASS

12. Stage 8 완료: CI 워크플로우 연동 + 운영 권한 분리(runbook)
- 워크플로우 분리:
  - PR 게이트: `.github/workflows/ci-pr-gate.yml`
  - strict live 게이트(스케줄/수동): `.github/workflows/ci-strict-live-gate.yml`
- CI 게이트 실행 래퍼/fixture:
  - `scripts/prepare_operational_readiness_fixture.ps1`
  - `scripts/run_ci_operational_gate.ps1`
- 운영 문서:
  - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md`
- strict live 게이트 표준 명령:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_ci_operational_gate.ps1 -IncludeBacktest -RunLiveProbe -StrictExecutionParity`

13. Stage 9 완료: strict live 결과 집계/경보 자동화
- 신규 집계/경보 스크립트:
  - `scripts/generate_strict_live_gate_trend_alert.ps1`
- 입력 리포트:
  - `build/Release/logs/execution_parity_report.json`
  - `build/Release/logs/operational_readiness_report.json`
- 누적/집계 산출물:
  - `build/Release/logs/strict_live_gate_history.jsonl`
  - `build/Release/logs/strict_live_gate_daily_summary.json`
  - `build/Release/logs/strict_live_gate_weekly_summary.json`
  - `build/Release/logs/strict_live_gate_daily_summary.csv`
  - `build/Release/logs/strict_live_gate_weekly_summary.csv`
- 경보 산출물:
  - `build/Release/logs/strict_live_gate_alert_report.json`
- 임계치 기본값:
  - 연속 strict 실패: `2`회 이상(`ConsecutiveFailureThreshold`)
  - 최근 `7`일 warning 비율: `0.30` 초과 + 최소 샘플 `3`회(`WarningRatioThreshold`, `WarningRatioMinSamples`)
- CI 연동:
  - strict live 워크플로우에서 `Generate Strict Live Trend + Alert Reports` 단계(항상 실행) 추가.

14. Stage 10 완료: 임계치 운영 튜닝 + 실패 패턴 자동 조치
- Stage 9 스크립트 확장:
  - `scripts/generate_strict_live_gate_trend_alert.ps1`
- 신규 산출물:
  - `build/Release/logs/strict_live_gate_threshold_tuning_report.json`
  - `build/Release/logs/strict_live_gate_action_response_report.json`
- 임계치 튜닝 자동화:
  - 연속 strict 실패 임계치: 최근 히스토리 alert-rate target 기반 추천값 계산
  - warning 비율 임계치: 일 단위 warning ratio quantile 기반 추천값 계산
  - 샘플 부족 시 baseline 임계치 유지(비파괴)
- 자동 조치 리포트:
  - 실패 패턴별 정책(`operational/parity missing`, `errors present`, `consecutive failures`, `warning ratio high`) 자동 분류
  - 정책별 권장 명령/에스컬레이션 레벨 산출
- CI 연동:
  - strict live 워크플로우에서 집계 스크립트 실행 시 `-ApplyTunedThresholds` 적용.

15. Stage 11 완료: 자동 조치 실행 경계 분리 + 경보-조치 피드백 루프
- 스크립트 확장:
  - `scripts/generate_strict_live_gate_trend_alert.ps1`
- 실행 경계 분리:
  - 신규 파라미터 `-ActionExecutionPolicy` (`report-only`, `safe-auto-execute`)
  - 정책별 `execution_boundary` 자동 분류(`critical => report-only`, 안전군 warning/info => safe-auto-execute 후보)
  - action response에 `manual_approval` 절차 필드 추가
- 피드백 루프:
  - 이전 `strict_live_gate_action_response_report.json`의 `feedback_for_next_tuning`를 다음 튜닝 주기에 반영
  - 안정화 조건(`feedback_stabilization_min_history_runs`, `feedback_stabilization_min_warning_samples`) 충족 시에만 보정 적용
  - 오탐/미탐 완화 신호(`reduce_false_positive_risk`, `reduce_false_negative_risk`, `stable`) 기반 임계치 보정
- CI 연동:
  - `.github/workflows/ci-strict-live-gate.yml`
  - `Generate Strict Live Trend + Alert Reports` 단계에서
    - `-ApplyTunedThresholds`
    - `-ActionExecutionPolicy safe-auto-execute`
    - `-EnableActionFeedbackLoop`
- 운영 문서:
  - `docs/STRICT_GATE_RUNBOOK_2026-02-13.md` 승인 기반 재개 절차/피드백 루프 기준 반영

## Verification Status
- Build:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release`
- Tests:
  - `AutoLifeExecutionStateTest` PASS
  - `AutoLifeTest` PASS
  - `AutoLifeStateTest` PASS
  - `AutoLifeEventJournalTest` PASS
- Recovery validation script:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_recovery_state.ps1` PASS
- Recovery E2E script:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_recovery_e2e.ps1` PASS (warnings 0)
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_recovery_e2e.ps1 -StrictLogCheck -OutputJson build/Release/logs/recovery_e2e_report_strict.json -StateValidationJson build/Release/logs/recovery_state_validation_strict.json` PASS
- Replay/Reconcile diff script:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_replay_reconcile_diff.ps1 -Strict` PASS
- Execution parity script:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_execution_parity.ps1 -Strict` PASS
- Live execution probe:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/generate_live_execution_probe.ps1 -Market KRW-BTC -NotionalKrw 5100 -DiscountPct 2.0 -CancelDelayMs 1500` PASS
- Operational readiness script (strict gate):
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/validate_operational_readiness.ps1 -StrictLogCheck -StrictExecutionParity -IncludeBacktest` PASS
- Stage 10 trend+tuning+action script:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/generate_strict_live_gate_trend_alert.ps1 -GateProfile strict_live -ApplyTunedThresholds` PASS
- Backtest sample run:
  - `AutoLifeTrading.exe --backtest <absolute_csv_path> --json` 실행 확인
- Baseline recapture:
  - `scripts/capture_baseline.ps1` PASS

## Stage 12 Update (2026-02-13)
- 승인 강제 연동:
  - `.github/workflows/ci-strict-live-gate.yml`에 `Extract Manual Approval Requirement` 단계 추가.
  - `strict_live_gate_action_response_report.json`의 `manual_approval.approval_required_for_resume`를 job output으로 추출.
  - `strict_live_resume_approval_gate` job이 `manual_approval_required == true`일 때만 실행되도록 연결.
  - `strict_live_resume_approval_gate`는 GitHub Environment(`strict-live-resume`)를 사용해 required reviewers 승인 경계와 직접 연결.
- 피드백 루프 운영 안정화:
  - `scripts/generate_strict_live_gate_trend_alert.ps1`에 주간 누적/드리프트 점검 규칙 추가.
  - tuning report 확장:
    - `tuning_readiness.feedback_weekly_*`
    - `feedback_loop.weekly_signal`
    - `feedback_loop.guardrails`
    - `feedback_loop.adjustment.*_requested`, `guardrail_applied`
  - action response 확장:
    - `manual_approval.github_environment`
    - `feedback_for_next_tuning.weekly_signal_context`
    - `feedback_for_next_tuning.guardrails`
  - false positive/false negative 보정 cap 명문화:
    - `consecutive_failure_threshold_delta_cap = 2`
    - `warning_ratio_threshold_delta_cap = 0.10`

## Stage 13 Update (2026-02-13)
- 수익성 검증 자동화 스크립트 추가:
  - `scripts/run_profitability_matrix.ps1`
- 검증 범위:
  - 구조 플래그 프로파일별 매트릭스 실행:
    - `legacy_default`
    - `core_bridge_only`
    - `core_policy_risk`
    - `core_full`
  - 데이터셋 기본값:
    - `data/backtest/simulation_2000.csv`
    - `data/backtest/simulation_large.csv`
- 신규 산출물:
  - `build/Release/logs/profitability_matrix.csv`
  - `build/Release/logs/profitability_profile_summary.csv`
  - `build/Release/logs/profitability_gate_report.json`
- 게이트 모델:
  - profile-level KPI gate + `core_full vs legacy_default` 상대 델타 gate
  - `-FailOnGate` 옵션으로 hard-fail 전환 가능
  - `-IncludeWalkForward` 옵션으로 OOS 컨텍스트 결합 가능
  - `-ExcludeLowTradeRunsForGate`, `-MinTradesPerRunForGate` 옵션으로 저활동 샘플 제외 가능
- 1차 실행 결과:
  - `overall_gate_pass = false`
  - 주요 원인: 평균 거래수 부족(`avg_total_trades`), PF 미충족(`avg_profit_factor`), 보수적 게이트 대비 샘플 부족

## Stage 14 Update (2026-02-13)
- 개인용 운영/배포 정렬 반영:
  - 신규 문서:
    - `README.md`
    - `docs/PERSONAL_USE_NOTICE.md`
  - 신규 스크립트:
    - `scripts/run_profitability_exploratory.ps1` (PR CI non-blocking 리포트용)
    - `scripts/apply_trading_preset.ps1` (safe/active 프리셋 적용)
  - 신규 프리셋:
    - `config/presets/safe.json`
    - `config/presets/active.json`
- CI 연동:
  - `.github/workflows/ci-pr-gate.yml`
  - `Generate Profitability Exploratory Report (Non-blocking)` 단계 추가 (`continue-on-error: true`).
- 수익성 matrix 스크립트 확장:
  - 저활동 샘플 제외 옵션:
    - `-ExcludeLowTradeRunsForGate`
    - `-MinTradesPerRunForGate`
  - 산출물 확장:
    - `runs_used_for_gate`
    - `excluded_low_trade_runs`
    - `gate_sample_pass`

## Stage 15 Update (2026-02-13)
- 보안 정리:
  - `config/config.json`의 `api.access_key`, `api.secret_key`를 빈 값으로 정리.
  - 신규 `.env` / `.env.example` 생성(키 이동용).
  - `src/common/Config.cpp`에서 config 파일의 API 키는 무시하고 env(`UPBIT_ACCESS_KEY`, `UPBIT_SECRET_KEY`)만 사용하도록 고정.
- candidate 승격 준비(확장 데이터셋 재실행):
  - `scripts/run_profitability_matrix.ps1`를 `data/backtest + data/backtest_curated` 전체(14개)로 재실행.
  - 결과:
    - `build/Release/logs/profitability_gate_report.json`
    - `overall_gate_pass = false`
    - 병목 지표:
      - `avg_profit_factor = 0.2686` (`min_profit_factor=1.0` 미충족)
      - `avg_total_trades = 1.8` (`min_avg_trades=10` 미충족)
    - pass 경계(현재 데이터 고정 시):
      - `min_profit_factor <= 0.2686`
      - `min_avg_trades <= 1.8`
- CI/운영 검증:
  - PR 워크플로 로컬 재현:
    - `scripts/run_ci_operational_gate.ps1 -IncludeBacktest` PASS
    - `scripts/run_profitability_exploratory.ps1` PASS
  - exploratory 산출물:
    - `build/Release/logs/profitability_gate_report_exploratory.json`
    - `overall_gate_pass = true`
    - `dataset_count = 14`
  - 업로드 대상 경로 존재 확인:
    - `build/Release/logs/*.json`, `*.csv`, `*.log`
    - `build/Release/state/*.json`, `*.jsonl`

## Remaining Gaps
1. GitHub Environment 운영 설정 점검 필요
- 코드/워크플로우 연동은 완료됨.
- 저장소 설정에서 `strict-live-resume` Environment의 `required reviewers`/보호 규칙이 runbook 기준과 일치하는지 운영 측 최종 점검이 필요.

2. Stage 13 수익성 gate 튜닝/표본 확대 필요
- 현재 기본 데이터셋 2개 기준으로 거래수와 PF 표본이 부족해 `overall_gate_pass=false`.
- 데이터셋 확장(기간/레짐) + gate 임계치 보정(운영 기준 유지)을 통해 재평가 필요.

3. 개인 배포용 최소 패키지 최종화 필요
- 현재 문서/프리셋/스크립트는 반영 완료.
- 배포 단위(압축/버전 태그/체크섬/릴리즈 노트)와 설치 검증 1회가 남음.

## Immediate Next Steps (Priority Order)
1. Stage 13 수익성 gate 통과 조건 재검증
- `scripts/run_profitability_matrix.ps1`를 dataset 확장/threshold 조정 케이스로 재실행.
- `core_full vs legacy_default` 델타가 음수 회귀가 아닌지 우선 확인.

2. exploratory -> candidate 승격 기준 정의
- exploratory non-blocking 리포트 추세를 주간으로 수집하고 candidate 기준 승격 시점을 수치로 정의.

3. 운영 자동화 고도화
- runbook 기준으로 CI 시크릿 로테이션/권한 점검을 배치화.

4. 승인 경계 운영 점검
- `strict-live-resume` Environment required reviewers 실제 승인 흐름(승인/거부/재시도)을 정기 리허설로 검증.

## Environment Memo
- CMake path:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe`
- Repo root:
  - `D:\MyApps\AutoLife`

## Next Context First Prompt (Copy/Paste)
`현재 기준일: 2026-02-13.

이전 컨텍스트 진행 상태:
- Stage 0~13 완료.
- Stage 12 완료 내용:
  - strict live 승인 강제 연동(GitHub Environment `strict-live-resume` + manual_approval bridge)
  - feedback loop 주간 drift 점검 + guardrail cap 반영
- Stage 13 완료 내용:
  - 신규 스크립트: scripts/run_profitability_matrix.ps1
  - 구조 비교 프로파일:
    - legacy_default
    - core_bridge_only
    - core_policy_risk
    - core_full
  - 신규 산출물:
    - build/Release/logs/profitability_matrix.csv
    - build/Release/logs/profitability_profile_summary.csv
    - build/Release/logs/profitability_gate_report.json
  - 게이트:
    - profile-level KPI gate + core_full vs legacy_default delta gate
    - -FailOnGate 지원
    - -IncludeWalkForward 지원
  - 1차 실행 결과:
    - overall_gate_pass=false
    - trade count/PF 표본 부족으로 gate 미통과

이번 컨텍스트 목표(Stage 14 후보):
1) 수익성 게이트 통과를 위한 표본 확장
- 데이터셋 기간/레짐 확장 후 profitability matrix 재실행.

2) 게이트 임계치 재교정
- 운영 안전성은 유지하면서 trade count/PF 임계치 현실화 검토.

3) CI 연동 전략 결정
- profitability matrix를 non-blocking 보고 단계로 먼저 붙인 뒤, 안정화 후 blocking gate 전환 여부 결정.

제약:
- 최소 침습(file 단위 최소 변경), legacy 기본 동작 유지
- 기존 strict 복구 검증 체인 깨지지 않게 유지

마지막 검증:
- scripts/run_profitability_matrix.ps1 PASS(실행 성공)
- build/Release/logs/profitability_gate_report.json 생성 확인
- overall_gate_pass 상태와 미통과 원인 요약 보고`
