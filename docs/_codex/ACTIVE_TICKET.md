# Active Ticket Snapshot
Last updated: 2026-02-24

## Header
- Ticket ID: `SO4-15-NEGATIVE-DAY-TAIL-ANALYSIS-20260224`
- Master Ticket Number (`0`-`7` or `N/A`): `N/A`
- Sub-ticket / Experiment ID: `Strict-Order-4-15`
- Title: Residual negative-day tail analysis and non-degrade guard candidates
- Owner: Codex
- Date: 2026-02-24
- Status: `in_progress`
- Mode: `A`

## Scope
- In scope:
  - analyze residual negative days after `step8e` baseline (`ETH/XRP` focus).
  - derive guard candidates from trade-level evidence (no coin hardcoding in logic).
  - keep fail-closed workflow and preserve Gate3/Gate4 pass state.
- Out of scope:
  - enabling live orders.
  - MODE B/v2 contract restructure.
- Baseline impact:
  - `no baseline behavior change accepted unless verification + daily OOS are non-degrade/pass`

## Inputs
- Relevant spec section(s):
  - `docs/_codex/MASTER_SPEC.md` section 7
  - `docs/30_VALIDATION_GATES.md`
  - `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- Contracts touched:
  - none
- Flags added/changed:
  - none

## Implementation plan
1. regenerate target-day negative-trade evidence from day-sliced OOS windows.
2. identify dominant loss cells and feature ranges (regime/archetype + calibrated/margin/EV/liquidity/strength).
3. test one narrow guard candidate (live/backtest symmetric) with strict non-degrade checks.
4. rollback immediately when daily OOS or verification degrades.
5. keep only candidates that pass both verification and day-sliced OOS.

## Validation plan
- Verification (5-set):
  - `python scripts/run_verification.py ... --output-json build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8f_*.json`
- Daily OOS (3m/7d):
  - `python scripts/run_daily_oos_stability.py ... --output-json build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8f_*.json`
- Gate safety invariants:
  - keep `allow_live_orders=false`
  - keep Gate4 pass artifacts intact (`v14_asc`)

## Current result snapshot
- Baseline reference remains:
  - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8e_highcal_shallowmargin_tail_v1.json`
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8e_highcal_shallowmargin_tail_v1.json`
- Analysis artifacts (new):
  - `build/Release/logs/daily_oos_negative_day_cell_summary_step8e.json`
  - `build/Release/logs/verification_loss_tail_focus_step8e.json`
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8e_postgate4.json`
  - `build/Release/logs/daily_oos_negative_trade_detail_step8e_postgate4_targetday.json`
- Target-day loss concentration:
  - `TRENDING_UP|PROBABILISTIC_PRIMARY_RUNTIME` (ETH, 2 trades, `profit_sum=-235.2360749483776`)
  - `RANGING|CORE_RESCUE_SHOULD_ENTER` (ETH, 2 trades, `profit_sum=-146.1536975705141`)
  - `TRENDING_UP|CORE_RESCUE_SHOULD_ENTER` (XRP, 1 trade, `profit_sum=-69.85687989698229`)
- Failed experiment (rolled back):
  - candidate: narrow runtime-tail guard for `TRENDING_UP|PROBABILISTIC_PRIMARY_RUNTIME`
  - failed report: `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8f_runtime_tail_guard_v2.json`
  - result: `status=fail`, `nonpositive_day_ratio=0.6`, `total_profit_sum=-271.050584`
  - rollback confirmed:
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8f_runtime_tail_guard_rollback.json`
    - `status=pass`, `nonpositive_day_ratio=0.4`, `total_profit_sum=195.2653`

## DoD
- [x] residual negative-day trade-level evidence captured.
- [x] first narrow candidate tested with strict metrics.
- [x] failed candidate rolled back to baseline-safe state.
- [ ] at least one candidate passes verification + daily OOS non-degrade.

## Risks and rollback
- Risks:
  - localized guard may shift position occupancy and cause second-order loss elsewhere.
  - day-sliced windows with `warmup=-1` can amplify path dependency from early entries.
- Rollback strategy:
  - immediate code rollback on any `daily_oos` fail or verification degradation.
  - keep `step8e` and `v14_asc` artifacts as immutable baseline references.
