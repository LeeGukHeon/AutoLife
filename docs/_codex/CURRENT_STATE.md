# Current State
Last updated: 2026-02-24

## Repository
- Branch: `main`
- Commit snapshot (pushed): `8ec3ac0`

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
  - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8ac_rollback_seq.json`
- Result:
  - `overall_gate_pass=true`
  - `adaptive_verdict=pass`
  - `avg_profit_factor=2.9577`
  - `avg_expectancy_krw=14.7159`
  - `avg_total_trades=10.2`

## Daily OOS snapshot (Gate3 supplement)
- Report:
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8ac_rollback_seq.json`
- Result:
  - `status=pass`
  - `evaluated_day_count=10`
  - `nonpositive_day_ratio=0.3`
  - `total_profit_sum=246.968137`
  - `peak_day_drawdown_pct=1.225673`
- Analysis artifacts:
  - `build/Release/logs/daily_oos_negative_day_cell_summary_step8e.json`
  - `build/Release/logs/daily_oos_negative_trade_detail_step8e_postgate4_targetday.json`
  - `build/Release/logs/verification_loss_tail_focus_step8e.json`
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8w_profile.json`
  - `build/Release/logs/so4_15_target_cell_trade_profile_step8w.json`
- Failed experiment (rolled back):
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8f_runtime_tail_guard_v2.json`
  - outcome: `status=fail`, `nonpositive_day_ratio=0.6`, `total_profit_sum=-271.050584`
- Additional failed attempts (rolled back):
  - runtime-tail scale (`step8g/step8h`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8g_fallback_tail_scale_v1.json`
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8h_runtime_tail_scale_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8g_fallback_tail_scale_v1.json`
    - outcome: baseline metrics unchanged (no-hit).
  - backtest runtime stop/tp parity fix (`step8i`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8i_backtest_runtime_stopfix_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8i_backtest_runtime_stopfix_v1.json`
    - outcome: both gates fail (`verification fail`, `daily_oos fail`), rolled back.
  - uptrend runtime narrow guard (`step8j`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8j_uptrend_runtime_tail_guard_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8j_uptrend_runtime_tail_guard_v1.json`
    - outcome: verification pass but daily OOS fail (`nonpositive_day_ratio=0.6`), rolled back.
  - runtime time-guard for strategy-less runtime positions (`step8k`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8k_runtime_time_guard_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8k_runtime_time_guard_v1.json`
    - outcome: verification/daily OOS both fail, rolled back.
  - ranging-rescue narrow hard block (`step8l`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8l_ranging_rescue_midvol_tail_v1.json`
    - outcome: sample-size guard fail (`avg_total_trades=9.8`), rolled back/discarded.
  - ranging-rescue risk/size scaling (`step8m`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8m_ranging_rescue_midvol_riskscale_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8m_ranging_rescue_midvol_riskscale_v1.json`
    - outcome: gate pass but daily OOS 핵심 지표 무개선 + expectancy 악화, rolled back.
  - uptrend primary narrow-tail risk/size scaling (`step8n`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8n_uptrend_primary_tail_riskscale_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8n_uptrend_primary_tail_riskscale_v1.json`
    - outcome: baseline 대비 무변화(no-hit), rolled back.
  - runtime post-entry tail hardening (`step8o`):
    - analysis:
      - `build/Release/logs/so4_15_target_cell_trade_profile_step8o.json`
    - reports:
      - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8o_runtime_tail_riskmanager_v1.json`
      - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8o_runtime_tail_riskmanager_v1.json`
    - outcome: baseline 대비 무변화(no-hit), rolled back.
  - exclude-BacktestEOD attribution diagnostic (`step8p`):
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8p_exclude_eod_diag.json`
    - outcome: ratio/profit 개선 신호는 있으나 `evaluated_day_count=6`으로 gate fail (diagnostic-only).
  - uptrend-rescue high-liq positive-EV tail scaling (`step8q`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8q_uptrend_rescue_highliq_ev_tail_scale_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8q_uptrend_rescue_highliq_ev_tail_scale_v1.json`
    - outcome: `nonpositive_day_ratio` 무변화, `total_profit_sum` 악화 -> rolled back.
  - uptrend-rescue high-liq margin-tail scaling v1 (`step8r`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8r_uptrend_rescue_highliq_margin_tail_scale_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8r_uptrend_rescue_highliq_margin_tail_scale_v1.json`
    - outcome: daily profit 개선 vs verification PF/expectancy 동반 하락 -> discarded.
  - uptrend-rescue high-liq margin-tail scaling v2 (`step8s`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8s_uptrend_rescue_highliq_margin_tail_scale_v2.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8s_uptrend_rescue_highliq_margin_tail_scale_v2.json`
    - outcome: daily 소폭 개선 vs verification PF/expectancy 동반 하락 잔존 -> rolled back.
  - runtime long-hold tail guard (`step8t`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8t_runtime_long_hold_tail_guard_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8t_runtime_long_hold_tail_guard_v1.json`
    - outcome: baseline 대비 무변화(no-hit), rolled back.
  - ranging-rescue bimodal-tail scaling (`step8u`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8u_ranging_rescue_bimodal_tail_scale_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8u_ranging_rescue_bimodal_tail_scale_v1.json`
    - outcome: verification 비열화 + daily profit/DD 개선(중간 기준점).
  - ranging-rescue bimodal-tail scaling v2 (`step8v`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8v_ranging_rescue_bimodal_tail_scale_v2.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8v_ranging_rescue_bimodal_tail_scale_v2.json`
    - outcome: daily profit 추가 개선, ratio는 0.4 유지.
  - ranging-rescue bimodal-tail scaling v3 (`step8w`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8w_ranging_rescue_bimodal_tail_scale_v3.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8w_ranging_rescue_bimodal_tail_scale_v3.json`
    - outcome: verification 비열화 유지 + `nonpositive_day_ratio 0.4 -> 0.3` 개선, 현재 코드에 유지.
  - uptrend tail dual guard v1 (`step8x`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8x_uptrend_tail_dual_guard_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8x_uptrend_tail_dual_guard_v1.json`
    - outcome: baseline(step8w) 대비 무변화(no-hit), rolled back.
    - rollback:
      - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8x_rollback_seq.json`
      - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8x_rollback_seq.json`
  - uptrend tail dual guard v2 (`step8y`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8y_uptrend_tail_dual_guard_v2.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8y_uptrend_tail_dual_guard_v2.json`
    - outcome: baseline(step8w) 대비 무변화(no-hit), rolled back.
  - riskmanager uptrend tail guard (`step8z`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8z_riskmanager_uptrend_tail_guard_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8z_riskmanager_uptrend_tail_guard_v1.json`
    - outcome: baseline(step8w) 대비 무변화(no-hit), rolled back.
  - riskmanager multiday tail guard v1 (`step8aa`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8aa_riskmanager_multiday_tail_guard_v1.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8aa_riskmanager_multiday_tail_guard_v1.json`
    - outcome: nonpositive ratio 유지 + total profit 개선.
  - riskmanager multiday tail guard v2 (`step8ab`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8ab_riskmanager_multiday_tail_guard_v2.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8ab_riskmanager_multiday_tail_guard_v2.json`
    - outcome: 현재 유지 코드. `nonpositive_day_ratio=0.3` 유지, `total_profit_sum=246.968137`.
  - riskmanager multiday tail guard v3 (`step8ac`):
    - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8ac_riskmanager_multiday_tail_guard_v3.json`
    - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8ac_riskmanager_multiday_tail_guard_v3.json`
    - outcome: step8ab 대비 열화로 rolled back.
    - rollback:
      - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step8ac_rollback_seq.json`
      - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_step8ac_rollback_seq.json`

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
  - `TRENDING_UP|PROBABILISTIC_PRIMARY_RUNTIME` (ETH `2026-02-18/2026-02-19`, BacktestEOD exits)
  - `TRENDING_UP|CORE_RESCUE_SHOULD_ENTER` (XRP `2026-02-19`, loss reduced to `-27.26890753867527`)
- Gate5 staged enable execution and monitoring evidence are not started yet.

## Detailed history references
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- `docs/PROBABILISTIC_EXECUTION_ROADMAP_2026-02-21.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
