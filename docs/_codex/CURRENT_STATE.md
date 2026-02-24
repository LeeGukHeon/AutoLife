# Current State
Last updated: 2026-02-24

## Repository
- Branch: `main`
- Commit snapshot (pushed): `32167f5`

## Active ticket
- Source of truth: `docs/_codex/ACTIVE_TICKET.md`
- Current: `SO4-13-UPTREND-TAIL-GUARD-20260224` (`Strict-Order-4-13`, Mode `A`, status `completed`)

## Master ticket progress (0-7)
- Ticket 0: implemented.
- Ticket 1: implemented.
- Ticket 2: implemented.
- Ticket 3: implemented.
- Ticket 4: implemented.
- Ticket 5: implemented.
- Ticket 6: implemented.
- Ticket 7 (MODE B/v2): partially implemented behind strict guards.

## Latest gate snapshot (runtime tuning path)
- Verification report:
  - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8e_highcal_shallowmargin_tail_v1.json`
- Result:
  - `overall_gate_pass=true`
  - `adaptive_verdict=pass`
  - `avg_profit_factor=2.9577`
  - `avg_expectancy_krw=14.7159`
  - `avg_total_trades=10.2`
  - `candidate_generation.no_signal_generated share=0.6374`

## Daily OOS snapshot (Gate3 supplement)
- Report:
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8e_highcal_shallowmargin_tail_v1.json`
- Result:
  - `status=pass`
  - `evaluated_day_count=10` (threshold `min=10` pass)
  - `nonpositive_day_ratio=0.4` (threshold `0.45` pass)
  - `total_profit_sum=195.2653` (positive-profit gate pass)
  - `peak_day_drawdown_pct=1.225896` (threshold `12.0` pass)
  - delta vs `step8b`:
    - `nonpositive_day_ratio: 0.538462 -> 0.4`
    - `total_profit_sum: -606.185678 -> 195.2653`
  - note:
    - `step8c` (`expected_value`-dependent tail guard) was no-hit.
    - `step8d` achieved near-pass (`ratio=0.454545`) before final narrow-tail fix.

## Live safety status
- `allow_live_orders=false` maintained.
- Gate4 shadow + Gate5 staged enable still mandatory and not yet eligible.

## Known gaps
- Daily OOS supplement now passes, but `evaluated_day_count=10` is exactly gate floor; further guard tightening may regress.
- Residual day-sliced negatives still exist in ETH/XRP tails; only aggregate gate is currently satisfied.
- discarded probes (kept out of runtime): `step7g`, `step7j`, `step7k`, `step7l`.
- discarded probes (kept out of runtime): `step7g`, `step7j`, `step7k`, `step7l`, `step7m`, `step7q`, `step7s`, `step7t`, `step7v`, `step8a`, `step8c`.
- latest discarded probe summary:
  - `step7v`: verification improved but daily OOS worsened (`total_profit_sum=-1505.699592`, `peak_day_drawdown_pct=2.80857`).
  - `step8a`: verification degraded to `adaptive_verdict=inconclusive` and was rolled back.
  - `step8c`: guard condition no-hit (verification/daily OOS unchanged from prior step).

## Detailed history references
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- `docs/PROBABILISTIC_EXECUTION_ROADMAP_2026-02-21.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
