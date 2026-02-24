# Current State
Last updated: 2026-02-24

## Repository
- Branch: `main`
- Commit snapshot (pushed): `667237d`

## Active ticket
- Source of truth: `docs/_codex/ACTIVE_TICKET.md`
- Current: `SO4-14-SHADOW-EVIDENCE-HARDENING-20260224` (`Strict-Order-4-14`, Mode `A`, status `completed`)

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
- Residual day-sliced negatives remain (ETH/XRP), although Gate3 supplement aggregate now passes.
- Gate5 staged enable execution and monitoring evidence are not started yet.

## Detailed history references
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- `docs/PROBABILISTIC_EXECUTION_ROADMAP_2026-02-21.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
