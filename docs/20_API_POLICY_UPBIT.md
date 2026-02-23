# Upbit API Policy (Mandatory)
Last updated: 2026-02-23

## Scope
This policy applies to all Upbit REST and public WebSocket usage in this repository.

## Non-negotiable rules
- Do not bypass/evasion tactics for rate limits:
  - No proxy rotation
  - No multi-IP spraying
- Parse and obey `Remaining-Req` headers.
- On HTTP `429`, stop request pressure immediately and apply bounded backoff.
- Server-side requests must not include `Origin` header.
- Use only official Upbit endpoints and documented limits.
- Never log secrets (`API key`, `JWT`, `signature`, raw auth headers).

## REST throttling behavior
- Parse header example: `group=default; min=1800; sec=29`.
- Primary limiter input is `sec` (not `min`).
- If `sec == 0`:
  - wait until next second boundary
  - add jitter `<= 50ms`
- If `429`:
  - exponential backoff with upper bound
  - abort repeated immediate retries

## Candle endpoint constraints
- Minute units allowed: `1,3,5,10,15,30,60,240`.
- `count` max `200` per call.
- `to` is an exclusive boundary (`candles before to`).
- Missing candles can occur when no trades happened in interval.

## WebSocket guidance
- Prefer WS for live tick/orderbook/trade feeds.
- Apply rate limiting to WS subscribe/publish messages.
- Respect connection and message-per-time-window limits.

## Logging requirements
- Log:
  - endpoint
  - sanitized params
  - status
  - `Remaining-Req`
  - latency
  - retry/backoff actions
- Do not log:
  - keys/tokens/signatures
  - unredacted auth headers

## Current code touchpoints
- Python historical fetcher:
  - `scripts/fetch_upbit_historical_candles.py`
- C++ HTTP and rate limiter:
  - `src/network/UpbitHttpClient.cpp`
  - `src/execution/RateLimiter.cpp`
  - `src/risk/UpbitComplianceAdapter.cpp`

## Official references
- Rate limits:
  - https://docs.upbit.com/kr/reference/rate-limits
- User request guide:
  - https://docs.upbit.com/kr/v1.5.9/docs/user-request-guide
- REST best practice:
  - https://docs.upbit.com/kr/docs/rest-api-best-practice
- Minute candles:
  - https://docs.upbit.com/kr/reference/list-candles-minutes
- Origin policy changelog:
  - https://docs.upbit.com/kr/changelog/origin_rate_limit
- WS best practice:
  - https://global-docs.upbit.com/docs/websocket-best-practice
- WS trade format:
  - https://docs.upbit.com/kr/reference/websocket-trade
