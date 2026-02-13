# Live Validation Checklist

## 목적
- 실거래 안정성을 우선으로 `myOrder WebSocket + REST 보조` 주문 상태머신을 검증한다.

## 사전 점검
- `UPBIT_ACCESS_KEY`, `UPBIT_SECRET_KEY` 환경변수 또는 `config/config.json` 키 설정 확인
- 최소 주문금액(`min_order_krw`) 5,000 KRW 이상 확인
- `max_new_orders_per_scan`, `max_positions`, `max_daily_trades`를 보수적으로 설정
- `max_daily_loss_pct`, `max_daily_loss_krw` 안전 한도 설정

## 검증 시나리오
1. 연결 검증
- `myOrder WS connected` 로그 확인
- REST API 기본 호출(`getMarkets`) 성공 확인

2. 주문 상태 전이 검증
- 소액 주문 1건 발생
- 상태 전이 확인: `SUBMITTED -> PARTIALLY_FILLED(선택) -> FILLED` 또는 `CANCELLED/REJECTED`
- 체결 후 `RiskManager::enterPosition` 반영 확인

3. 장애 대응 검증
- 네트워크 단절 또는 WS 메시지 지연 상황에서 REST 보조 동작 확인
- WS 재연결 로그 확인

4. 종료/복구 검증
- 프로세스 정상 종료 후 재기동
- 상태 저장/복원(loadState/saveState) 동작 확인

## 통과 기준
- WS 수신이 정상이며, WS 지연 시 REST 보조로 상태 누락이 발생하지 않음
- 체결/취소/거부 상태가 일관되게 반영됨
- 일손실 한도/주문 한도 등 안전장치가 의도대로 작동함

## 운영 권고
- 실거래 초기 1~3일은 `max_positions=1`, `max_new_orders_per_scan=1` 유지
- 로그(`logs/autolife.log`, `logs/trades.log`)를 일 단위로 리뷰
- 전략 on/off 변경은 장중이 아닌 비활성 구간에서 수행
