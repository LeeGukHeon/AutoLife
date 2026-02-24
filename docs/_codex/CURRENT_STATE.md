# Current State
Last updated: 2026-02-24

## Repository
- Branch: `main`
- Commit snapshot (pushed): `986ee40`

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
  - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step7z_recheck_v1.json`
- Result:
  - `overall_gate_pass=true`
  - `adaptive_verdict=pass`
  - `avg_profit_factor=2.9577`
  - `avg_expectancy_krw=14.7159`
  - `avg_total_trades=10.2`
  - `candidate_generation.no_signal_generated share=0.6374`

## Daily OOS snapshot (Gate3 supplement)
- Report:
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step7z_recheck.json`
- Result:
  - `status=fail`
  - `evaluated_day_count=14`
  - `nonpositive_day_ratio=0.642857` (threshold `0.45` fail)
  - `total_profit_sum=-896.321805` (fail)
  - `peak_day_drawdown_pct=1.658036` (pass)
  - dominant loss cell: `TRENDING_UP|CORE_RESCUE_SHOULD_ENTER`
  - improvement vs `step7w`:
    - `nonpositive_day_ratio: 0.785714 -> 0.642857`
    - `total_profit_sum: -983.396745 -> -896.321805`
    - `peak_day_drawdown_pct: 1.786715 -> 1.658036`
  - improvement vs `step7f`:
    - `total_profit_sum: -2760.512552 -> -896.321805`
  - note:
    - `scripts/run_daily_oos_stability.py` corrected day-metric attribution fallback.
    - optional flag added: `--exclude-backtest-eod-trades` (default include).

## Live safety status
- `allow_live_orders=false` maintained.
- Gate4 shadow + Gate5 staged enable still mandatory and not yet eligible.

## Known gaps
- Daily OOS supplement is still fail-closed (`max_nonpositive_day_ratio`, `positive_profit_sum`).
- Must improve `TRENDING_UP` loss-tail cells without dropping verification sample-size guard.
- discarded probes (kept out of runtime): `step7g`, `step7j`, `step7k`, `step7l`.
- discarded probes (kept out of runtime): `step7g`, `step7j`, `step7k`, `step7l`, `step7m`, `step7q`, `step7s`, `step7t`, `step7v`.
- latest discarded probe summary:
  - `step7v`: verification improved but daily OOS worsened (`total_profit_sum=-1505.699592`, `peak_day_drawdown_pct=2.80857`).
  - `step8a`: verification degraded to `adaptive_verdict=inconclusive` and was rolled back.

## Detailed history references
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- `docs/PROBABILISTIC_EXECUTION_ROADMAP_2026-02-21.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
