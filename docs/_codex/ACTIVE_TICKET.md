# Active Ticket Snapshot
Last updated: 2026-02-24

## Header
- Ticket ID: `SO4-13-UPTREND-TAIL-GUARD-20260224`
- Master Ticket Number (`0`-`7` or `N/A`): `N/A`
- Sub-ticket / Experiment ID: `Strict-Order-4-13`
- Title: Loss-tail guard rebalance for dominant OOS failure cells
- Owner: Codex
- Date: 2026-02-24
- Status: `in_progress`
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
  - `verification_report_global_full_5set_refresh_20260224_step7z_recheck_v1.json`
- Extra tests:
  - daily OOS report diff vs prior maintained candidate (`step7w`)

## Current result snapshot
- Verification (`step7z`):
  - `overall_gate_pass=true`
  - `adaptive_verdict=pass`
  - `avg_profit_factor=2.9577`
  - `avg_expectancy_krw=14.7159`
  - `avg_total_trades=10.2`
  - `candidate_generation.no_signal_generated share=0.6374`
- Daily OOS (`step7z`):
  - `status=fail`
  - `evaluated_day_count=14`
  - `nonpositive_day_ratio=0.642857` (threshold 0.45 fail)
  - `total_profit_sum=-896.321805` (fail)
  - `peak_day_drawdown_pct=1.658036` (pass)
  - dominant loss cell: `TRENDING_UP|CORE_RESCUE_SHOULD_ENTER`
  - delta vs `step7w`: `nonpositive_day_ratio 0.785714 -> 0.642857`, `total_profit_sum -983.396745 -> -896.321805`
  - delta vs `step7f`: `total_profit_sum -2760.512552 -> -896.321805`
  - note: daily OOS aggregation bugfix applied in script.
    - trade-history day metrics now avoid summary fallback contamination.
    - `--exclude-backtest-eod-trades` added (default keeps legacy include behavior).
  - discarded follow-up probe (`step7v`):
    - verification improved (`avg_profit_factor=2.9795`, `avg_expectancy_krw=15.3616`)
    - daily OOS worsened (`total_profit_sum=-1505.699592`, `peak_day_drawdown_pct=2.80857`)
  - discarded follow-up probe (`step8a`):
    - verification degraded (`adaptive_verdict=inconclusive`, `overall_gate_pass=false`)
    - fail-closed rollback to `step7z`

## DoD
- [x] runtime guards are live/backtest isomorphic.
- [x] verification gate rerun completed with artifact.
- [x] daily OOS rerun completed with artifact.
- [ ] daily OOS fail reasons reduced to within thresholds.

## Risks and rollback
- Risks:
  - over-tightening can drop sample-size guard in verification.
- Rollback strategy:
  - keep latest passing candidate (`step7z`) and iterate only guard thresholds incrementally.
