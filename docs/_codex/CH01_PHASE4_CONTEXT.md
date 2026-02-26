# CH-01 Phase 4 Context
Last updated: 2026-02-25

## Purpose
Phase 4 upgrades from single-signal quality (Phase 3) to a portfolio engine that controls
selection, allocation, exposure, and risk in multi-market trading.

## Input Contract (from Phase 3)
Each market candidate must expose at least:
- `market/symbol`, `decision_time`
- `expected_edge_after_cost` (plus optional tail edge)
- `margin`
- `prob_confidence`, `ev_confidence`
- `regime`, `volatility_bucket`, `liquidity_bucket`
- cost breakdown summary
- execution proxy (spread/vol/liquidity)
- current position state

## Core Modules
1. Portfolio Candidate Snapshot
2. Portfolio Allocator
3. Exposure/Correlation Controller
4. Risk Budget + Drawdown Governor
5. Execution-aware Sizing
6. Portfolio Diagnostics + Verification

## Flags (all default OFF)
- `phase4_portfolio_allocator_enabled`
- `phase4_correlation_control_enabled`
- `phase4_risk_budget_enabled`
- `phase4_drawdown_governor_enabled`
- `phase4_execution_aware_sizing_enabled`
- `phase4_portfolio_diagnostics_enabled`

## Rollout Strategy
1. Diagnostics-first (measurement only)
2. Allocator core (score/rank/top-k)
3. Budget caps
4. Drawdown governor
5. Correlation cap
6. Execution-aware sizing

## Parity Rule
- Live and Backtest must use the same allocator/policy path.
- OFF results must match current baseline; ON results must be deterministic and reproducible.

## Verification Delta (required)
OFF vs ON comparison must include:
- trade count
- exposure distribution
- MDD/loss streak
- cost/slippage deltas
- selection reason breakdown

Current command path:
- `python scripts/run_verification.py --phase4-off-on-compare --pipeline-version v2 --data-dir data/backtest_recent_7d_20260225 --dataset-names upbit_KRW_BTC_1m_full.csv`
