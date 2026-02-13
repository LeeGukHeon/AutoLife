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
  - `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_realdata_candidate_loop.ps1 -SkipFetch -SkipTune`
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
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_realdata_candidate_loop.ps1`

2. 수집 생략 matrix만
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_realdata_candidate_loop.ps1 -SkipFetch -SkipTune`

3. exploratory 점검
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_profitability_exploratory.ps1`

4. preset 적용 점검
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/apply_trading_preset.ps1 -Preset safe`
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/apply_trading_preset.ps1 -Preset active`

