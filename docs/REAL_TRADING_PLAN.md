# AutoLife Real Trading Plan

## Reality Check
- No strategy can guarantee "high win-rate + high return" in all market regimes.
- A practical target is: stable risk-adjusted return with strict drawdown control.
- For small accounts, execution quality (spread/slippage/fees) dominates outcome.

## Current Risk Gaps
- Backtest uses live HTTP client shape and does not fully model exchange microstructure.
- Strategy signals are mostly indicator-driven; robust walk-forward validation is missing.
- Fill model and partial fill behavior are still simplified compared to exchange reality.

## What Was Hardened
- API key handling moved to environment-variable fallback (`UPBIT_ACCESS_KEY`, `UPBIT_SECRET_KEY`).
- `grid`/`grid_trading` strategy key mismatch fixed by normalization.
- Order cancel/fill race handling improved (`done_order` sync flow).
- Market fallback order format aligned with Upbit rule (`price` buy, `market` sell).
- Position-entry checks now include pending capital.
- Strategy execution changed from parallel to sequential to reduce API burst/race.
- Added per-scan buy cap (`max_new_orders_per_scan`) to reduce rapid over-entry.
- Added spread/orderbook quality filter before candidate market selection.

## Next Refactor Priorities
1. Adaptive Decision Layer (Primary)
- Promote structural refactor as the main track.
- Treat each strategy as an alpha source; final entry decision is policy-driven.
- Replace static threshold tuning with context/regime-aware adaptive gating.
- Optimize for EV/PF/DD with win-rate as a soft target, not a single hard gate.

2. Event-Driven Execution
- Move order state tracking to WebSocket `myOrder` stream first, REST as fallback.
- Keep a strict order state machine (`NEW`, `PARTIAL`, `FILLED`, `CANCELLED`, `REJECTED`).

3. Backtest Integrity
- Replace network client in backtest with deterministic offline data adapter.
- Add fees, slippage, partial-fill, queue-priority assumptions explicitly.
- Enforce walk-forward split (train/validate/test by time).

4. Data and Features
- Persist per-signal feature snapshot (spread, depth, volatility regime, expected EV, fill quality).
- Build a supervised dataset from accepted/rejected signals and realized outcomes.
- Add market-regime gating (trend/range/high-vol regime dependent strategy allow-list).

5. Risk and Capital
- Add hard kill-switches: daily loss, max consecutive loss, abnormal spread spike.
- Add portfolio concentration constraints by correlated assets.
- Use volatility-targeted position sizing rather than fixed ratio only.

6. API Compliance
- Keep dynamic parsing of `Remaining-Req`.
- Validate per-market order capability via `orders/chance` cache.
- Fail-safe degrade to no-trade mode on repeated 429/418 or stale market data.
