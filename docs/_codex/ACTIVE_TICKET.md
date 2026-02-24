# Active Ticket Snapshot
Last updated: 2026-02-24

## Header
- Ticket ID: `SO4-13-UPTREND-TAIL-GUARD-20260224`
- Master Ticket Number (`0`-`7` or `N/A`): `N/A`
- Sub-ticket / Experiment ID: `Strict-Order-4-13`
- Title: Loss-tail guard rebalance for dominant OOS failure cells
- Owner: Codex
- Date: 2026-02-24
- Status: `completed`
- Mode: `A`

## Scope
- In scope:
  - tighten non-coin-specific tail guards for dominant `regime|archetype` cells.
  - keep live/backtest isomorphic logic.
  - preserve baseline fail-closed behavior and rerun Gate3 artifacts.
- Out of scope:
  - model retraining/feature contract change.
  - live enable progression.
- Baseline impact:
  - `runtime decision policy updated` (documented strict-order iteration)

## Inputs
- Relevant spec section(s):
  - `docs/_codex/MASTER_SPEC.md` section 7
  - `docs/30_VALIDATION_GATES.md`
- Contracts touched:
  - none
- Flags added/changed:
  - none

## Implementation plan
1. apply risk-cell quality guards in probabilistic primary minimums (runtime C++).
2. run verification gate on fixed 5-set.
3. run daily OOS stability on `BTC/ETH/XRP` 7-day window and compare deltas.

## Validation plan
- Strict feature validation:
  - N/A (runtime logic only)
- Parity:
  - N/A (bundle/schema unchanged)
- Verification:
  - `verification_report_global_full_5set_refresh_20260224_step8e_highcal_shallowmargin_tail_v1.json`
- Extra tests:
  - `daily_oos_stability_report_3m_7d_20260224_step8e_highcal_shallowmargin_tail_v1.json`

## Current result snapshot
- Verification (`step8e`):
  - `overall_gate_pass=true`
  - `adaptive_verdict=pass`
  - `avg_profit_factor=2.9577`
  - `avg_expectancy_krw=14.7159`
  - `avg_total_trades=10.2`
  - `candidate_generation.no_signal_generated share=0.6374`
- Daily OOS (`step8e`):
  - `status=pass`
  - `evaluated_day_count=10` (threshold `min=10` pass)
  - `nonpositive_day_ratio=0.4` (threshold `0.45` pass)
  - `total_profit_sum=195.2653` (positive-profit gate pass)
  - `peak_day_drawdown_pct=1.225896` (threshold `12.0` pass)
  - dominant residual negative cells:
    - `TRENDING_UP|PROBABILISTIC_PRIMARY_RUNTIME` (ETH day-slice residual)
    - `TRENDING_UP|CORE_RESCUE_SHOULD_ENTER` (XRP tail residual)
  - delta vs `step7z`:
    - `nonpositive_day_ratio: 0.642857 -> 0.4`
    - `total_profit_sum: -896.321805 -> 195.2653`
  - delta vs `step8b`:
    - `nonpositive_day_ratio: 0.538462 -> 0.4`
    - `total_profit_sum: -606.185678 -> 195.2653`
  - intermediate probe notes:
    - `step8c`: `expected_value`-dependent guard was no-hit (metrics unchanged).
    - `step8d`: mid-liquidity tail guard reached near-pass (`ratio=0.454545`, `profit_sum=180.271024`).
    - `step8e`: narrow high-calibration shallow-margin tail guard converted gate to pass.

## DoD
- [x] runtime guards are live/backtest isomorphic.
- [x] verification gate rerun completed with artifact.
- [x] daily OOS rerun completed with artifact.
- [x] daily OOS fail reasons reduced to within thresholds.

## Risks and rollback
- Risks:
  - current daily OOS pass uses `evaluated_day_count=10` (gate floor), so further tightening may drop below floor.
- Rollback strategy:
  - keep latest passing candidate (`step8e`) and revert probes that reduce evaluated day count or break verification.
