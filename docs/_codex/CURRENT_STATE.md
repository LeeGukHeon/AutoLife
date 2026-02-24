# Current State
Last updated: 2026-02-24

## Repository
- Branch: `main`
- Commit snapshot (pushed): `667237d`

## Active ticket
- Source of truth: `docs/_codex/ACTIVE_TICKET.md`
- Current: `SO4-14-SHADOW-EVIDENCE-HARDENING-20260224` (`Strict-Order-4-14`, Mode `A`, status `in_progress`)

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

## Daily OOS snapshot (Gate3 supplement)
- Report:
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8e_highcal_shallowmargin_tail_v1.json`
- Result:
  - `status=pass`
  - `evaluated_day_count=10`
  - `nonpositive_day_ratio=0.4`
  - `total_profit_sum=195.2653`
  - `peak_day_drawdown_pct=1.225896`

## Gate4 shadow flow snapshot
- Flow report:
  - `build/Release/logs/probabilistic_shadow_gate_flow_step8e_live_enable_v6.json`
- Result:
  - `status=fail` (expected fail-closed)
  - generate step errors:
    - `shadow_candle_sequence_mismatch`
    - `shadow_decision_log_mismatch`
  - validate step error: `shadow_report_status_not_pass`
  - promotion step errors:
    - `gate4_shadow_validation_failed_or_missing`
    - `gate4_shadow_failed_or_missing`
- Interpretation:
  - canonical live shadow log now exists (`build/Release/logs/policy_decisions.jsonl`).
  - remaining blocker is evidence alignment: compared live/backtest logs do not share the same candle sequence/window.
  - current backtest decision log source is single-dataset replay (`policy_decisions_backtest.jsonl`), not a matched live multi-market window.

## Live safety status
- `allow_live_orders=false` maintained.
- Gate4 shadow + Gate5 staged enable still mandatory and not yet eligible.

## Known gaps
- Need same-window live/backtest decision evidence (not just distinct paths).
- Gate4 cannot pass until same-candle + decision-parity evidence is available.
- Residual day-sliced negatives remain (ETH/XRP), although Gate3 supplement aggregate now passes.

## Detailed history references
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- `docs/PROBABILISTIC_EXECUTION_ROADMAP_2026-02-21.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
