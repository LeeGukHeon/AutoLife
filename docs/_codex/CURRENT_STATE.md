# Current State
Last updated: 2026-02-24

## Repository
- Branch: `main`
- Commit snapshot (pushed): `d6390bf`

## Active ticket
- Source of truth: `docs/_codex/ACTIVE_TICKET.md`
- Current: `SO4-15-NEGATIVE-DAY-TAIL-ANALYSIS-20260224` (`Strict-Order-4-15`, Mode `A`, status `in_progress`)

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
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8f_runtime_tail_guard_rollback.json`
- Result:
  - `status=pass`
  - `evaluated_day_count=10`
  - `nonpositive_day_ratio=0.4`
  - `total_profit_sum=195.2653`
  - `peak_day_drawdown_pct=1.225896`
- Analysis artifacts:
  - `build/Release/logs/daily_oos_negative_day_cell_summary_step8e.json`
  - `build/Release/logs/daily_oos_negative_trade_detail_step8e_postgate4_targetday.json`
  - `build/Release/logs/verification_loss_tail_focus_step8e.json`
- Failed experiment (rolled back):
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8f_runtime_tail_guard_v2.json`
  - outcome: `status=fail`, `nonpositive_day_ratio=0.6`, `total_profit_sum=-271.050584`

## Gate4 shadow flow snapshot
- Flow report:
  - `build/Release/logs/probabilistic_shadow_gate_flow_step8e_live_enable_v14_asc.json`
- Result:
  - `status=pass`
  - step1 `build_aligned_backtest_log`: `pass`
    - aligned log: `build/Release/logs/policy_decisions_backtest.jsonl`
    - summary: `build/Release/logs/policy_decisions_backtest_shadow_aligned_summary_gate4_v14_asc.json`
  - generate step:
    - `build/Release/logs/probabilistic_shadow_report_v14_asc.json`
    - `same_candles=true`, `compared_decision_count=8`, `mismatch_count=0`
  - validate step:
    - `build/Release/logs/probabilistic_shadow_report_validation_v14_asc.json`
    - `status=pass`
  - promotion step:
    - `build/Release/logs/probabilistic_promotion_readiness_v14_asc.json`
    - `status=pass`, `promotion_ready=true`
- Interpretation:
  - canonical live shadow log exists (`build/Release/logs/policy_decisions.jsonl`).
  - aligned backtest builder now enforces market-aware scan alignment + live-selected hint priority.
  - Gate4 decision-parity blocker is closed under canonical v2 artifacts.

## Canonical v2 gate artifact snapshot
- Gate1:
  - `build/Release/logs/probabilistic_feature_validation_summary.json`
  - `status=pass`, `pipeline_version=v2`, `gate_profile=v2_strict`
- Gate2:
  - `build/Release/logs/probabilistic_runtime_bundle_parity.json`
  - `status=pass`, `pipeline_version=v2`, `gate_profile=v2_strict`
- Gate3:
  - `build/Release/logs/verification_report.json`
  - `overall_gate_pass=true`, `pipeline_version=v2`, `gate_profile=v2_strict`
  - run note: `--min-profitable-ratio 0.4`, baseline=`verification_report_global_full_5set_refresh_20260224_step8e_highcal_shallowmargin_tail_v1.json`

## Live safety status
- `allow_live_orders=false` maintained.
- Gate4 shadow is now `pass`; staged live enable remains Gate5-controlled and still requires manual rollout.

## Known gaps
- Residual loss-tail focus remains on:
  - `TRENDING_UP|PROBABILISTIC_PRIMARY_RUNTIME` (ETH, BacktestEOD exits)
  - `RANGING|CORE_RESCUE_SHOULD_ENTER` (ETH)
  - `TRENDING_UP|CORE_RESCUE_SHOULD_ENTER` (XRP)
- Gate5 staged enable execution and monitoring evidence are not started yet.

## Detailed history references
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- `docs/PROBABILISTIC_EXECUTION_ROADMAP_2026-02-21.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
