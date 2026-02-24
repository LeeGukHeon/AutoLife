# Current State
Last updated: 2026-02-24

## Repository
- Branch: `main`
- Commit snapshot (pushed): `b402b67`

## Active ticket
- Source of truth: `docs/_codex/ACTIVE_TICKET.md`
- Current: `SO4-13-UPTREND-TAIL-GUARD-20260224` (`Strict-Order-4-13`, Mode `A`, status `in_progress`)

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
  - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step7r_final_v1.json`
- Result:
  - `overall_gate_pass=true`
  - `adaptive_verdict=pass`
  - `avg_profit_factor=2.9577`
  - `avg_expectancy_krw=14.7159`
  - `avg_total_trades=10.2`
  - `candidate_generation.no_signal_generated share=0.6374`

## Daily OOS snapshot (Gate3 supplement)
- Report:
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step7r_final.json`
- Result:
  - `status=fail`
  - `evaluated_day_count=15`
  - `nonpositive_day_ratio=0.8` (threshold `0.45` fail)
  - `total_profit_sum=-1369.08985` (fail)
  - `peak_day_drawdown_pct=2.092465` (pass)
  - dominant loss cell: `TRENDING_UP|CORE_RESCUE_SHOULD_ENTER`
  - improvement vs `step7f`:
    - `nonpositive_day_ratio: 0.933333 -> 0.8`
    - `total_profit_sum: -2760.512552 -> -1369.08985`
  - improvement vs `step7i_final`:
    - `total_profit_sum: -1850.427359 -> -1369.08985`
    - `peak_day_drawdown_pct: 2.598311 -> 2.092465`

## Live safety status
- `allow_live_orders=false` maintained.
- Gate4 shadow + Gate5 staged enable still mandatory and not yet eligible.

## Known gaps
- Daily OOS supplement is still fail-closed (`max_nonpositive_day_ratio`, `positive_profit_sum`).
- Must improve `TRENDING_UP` loss-tail cells without dropping verification sample-size guard.
- discarded probes (kept out of runtime): `step7g`, `step7j`, `step7k`, `step7l`.
- discarded probes (kept out of runtime): `step7g`, `step7j`, `step7k`, `step7l`, `step7m`, `step7q`, `step7s`.

## Detailed history references
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- `docs/PROBABILISTIC_EXECUTION_ROADMAP_2026-02-21.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
