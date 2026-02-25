# CH-01 Phase 3 Final Delivery Summary
Last updated: 2026-02-25
Scope: Phase 3 only (Phase 4 intentionally excluded per user direction)

## 1) Changed / Added Files
- Runtime/model and policy:
  - `include/analytics/ProbabilisticRuntimeModel.h`
  - `src/analytics/ProbabilisticRuntimeModel.cpp`
  - `include/strategy/IStrategy.h`
  - `include/strategy/FoundationRiskPipeline.h`
  - `src/strategy/FoundationRiskPipeline.cpp`
  - `src/strategy/StrategyManager.cpp`
  - `src/runtime/LiveTradingRuntime.cpp`
  - `src/runtime/BacktestRuntime.cpp`
  - `src/risk/RiskManager.cpp`
- Bundle/export/train/verify scripts:
  - `scripts/export_probabilistic_runtime_bundle.py`
  - `scripts/train_probabilistic_pattern_model.py`
  - `scripts/train_probabilistic_pattern_model_global.py`
  - `scripts/run_verification.py`
  - `scripts/run_ci_operational_gate.py`
  - `scripts/validate_execution_parity.py`
  - `scripts/validate_should_exit_parity.py` (added)
  - `scripts/test_validate_should_exit_parity.py` (added)
  - `scripts/collect_strategyless_exit_audit.py`
  - `scripts/test_collect_strategyless_exit_audit.py`
- Documentation/context:
  - `docs/42_RUNTIME_BUNDLE_CONTRACT.md`
  - `docs/50_EXTENSIONS_OVERVIEW.md`
  - `docs/56_EXT_PHASE3_FRONTIER_EV_COST_DIAGNOSTICS.md` (added)
  - `docs/_codex/CH01_PHASE3_CONTEXT.md` (added)
  - `docs/_codex/PARITY_BOUNDARY.md` (added)
  - `docs/_codex/PHASE2_CLOSURE_2026-02-25.md` (added)
  - `docs/_codex/BOOTSTRAP.md`
  - `docs/_codex/CURRENT_STATE.md`
  - `docs/_codex/ACTIVE_TICKET.md`
  - `docs/00_INDEX.md`

## 2) Added Phase 3 Bundle Keys and Defaults
Source of truth: `scripts/export_probabilistic_runtime_bundle.py::default_phase3_bundle_config`

- Top-level feature flags (all default `false`):
  - `phase3_frontier_enabled`
  - `phase3_ev_calibration_enabled`
  - `phase3_cost_tail_enabled`
  - `phase3_adaptive_ev_blend_enabled`
  - `phase3_diagnostics_v2_enabled`

- `phase3.frontier` defaults:
  - `enabled=false`
  - `k_margin=0.0`
  - `k_uncertainty=0.0`
  - `k_cost_tail=0.0`
  - `min_required_ev=-0.0002`
  - `max_required_ev=0.0050`
  - `margin_floor=-1.0`
  - `ev_confidence_floor=0.0`
  - `ev_confidence_penalty=0.0`
  - `cost_tail_penalty=0.0`
  - `cost_tail_reject_threshold_pct=1.0`

- `phase3.ev_calibration` defaults:
  - `enabled=false`
  - `use_quantile_map=false`
  - `min_bucket_samples=64`
  - `default_confidence=1.0`
  - `min_confidence=0.10`
  - `ood_penalty=0.10`
  - `buckets=[]`

- `phase3.cost_model` defaults:
  - `enabled=false`
  - `mode="mean_mode"`
  - `entry_multiplier=0.50`
  - `exit_multiplier=0.50`
  - `entry_add_bps=0.0`
  - `exit_add_bps=0.0`
  - `tail_markup_ratio=0.35`
  - `tail_add_bps=0.0`
  - `hybrid_lambda=0.50`

- `phase3.adaptive_ev_blend` defaults:
  - `enabled=false`
  - `min=0.05`
  - `max=0.40`
  - `base=0.20`
  - `trend_bonus=0.08`
  - `ranging_penalty=0.06`
  - `hostile_penalty=0.08`
  - `high_confidence_bonus=0.05`
  - `low_confidence_penalty=0.10`
  - `cost_penalty=0.06`

- `phase3.diagnostics_v2` defaults:
  - `enabled=false`

## 3) New Diagnostics Counters / Bucket Aggregates
- Funnel counters:
  - `candidate_total`
  - `pass_total`
  - `reject_margin_insufficient`
  - `reject_strength_fail`
  - `reject_expected_value_fail`
  - `reject_frontier_fail`
  - `reject_ev_confidence_low`
  - `reject_cost_tail_fail`
- Bucket pass-rate slices:
  - `pass_rate_by_regime`
  - `pass_rate_by_vol_bucket`
  - `pass_rate_by_liquidity_bucket`
  - `pass_rate_edge_regressor_present_vs_fallback`
  - `pass_rate_by_cost_mode` (additional)
- Bottleneck summary:
  - `top3_bottlenecks` in verification report.
- Cost snapshot fields carried on signals:
  - `cost_entry_pct`, `cost_exit_pct`, `cost_tail_pct`, `cost_mode`
  - plus EV fields `expected_edge_raw_pct`, `expected_edge_calibrated_pct`, `ev_confidence`.

## 4) Phase3 OFF vs ON Behavior Summary
Comparison artifacts:
- OFF: `build/Release/logs/verification_report_phase3_off_btc1m_swapped_tmp.json`
- ON: `build/Release/logs/verification_report_phase3_on_btc1m_swapped_tmp.json`

Aggregate metric snapshot:
- OFF:
  - `overall_gate_pass=false`
  - `avg_total_trades=4.0`
  - `avg_expectancy_krw=-32.5714`
  - `total_profit_sum_krw=-130.2857`
  - `phase3_diagnostics_v2.enabled=false` (funnel counters all zero)
- ON:
  - `overall_gate_pass=false`
  - `avg_total_trades=2.0`
  - `avg_expectancy_krw=-20.9826`
  - `total_profit_sum_krw=-41.9652`
  - `phase3_diagnostics_v2.enabled=true`
  - funnel:
    - `candidate_total=706`
    - `pass_total=14` (pass-rate `0.0198`)
    - `reject_expected_value_fail=692`
    - `reject_frontier_fail=692`
    - `reject_strength_fail=140`
    - `reject_margin_insufficient=0`
    - `reject_ev_confidence_low=0`
    - `reject_cost_tail_fail=0`
  - top3 bottlenecks:
    - `reject_expected_value_fail (692)`
    - `reject_frontier_fail (692)`
    - `reject_strength_fail (140)`
  - pass-rate slices:
    - by regime:
      - `RANGING: 1/375 (0.0027)`
      - `TRENDING_DOWN: 13/270 (0.0481)`
      - `TRENDING_UP: 0/61 (0.0)`
    - by vol:
      - `vol_low: 14/706 (0.0198)`
    - by liquidity:
      - `liq_low: 9/667 (0.0135)`
      - `liq_mid: 5/39 (0.1282)`
    - edge source:
      - `edge_fallback: 14/706 (0.0198)`
    - cost mode:
      - `hybrid_mode: 14/706 (0.0198)`

## 5) Operational Gate Result
Command baseline:
- `python scripts/run_ci_operational_gate.py --include-backtest --strict-execution-parity --run-should-exit-parity-analysis --refresh-should-exit-audit-from-logs --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" --should-exit-audit-live-runtime-log-mode-filter exclude_backtest --strict-should-exit-parity`

Result artifact:
- `build/Release/logs/operational_readiness_report.json`
- `generated_at=2026-02-25T07:32:06.869282+00:00`
- checks:
  - `recovery_e2e_passed=true`
  - `replay_reconcile_diff_passed=true`
  - `execution_parity_passed=true`
  - `backtest_readiness_executed=true`
  - `backtest_readiness_passed=true`
- `errors=[]`

## 6) Strict Parity Result
- Execution parity artifact:
  - `build/Release/logs/execution_parity_report.json`
  - `generated_at=2026-02-25T07:32:03.516174+00:00`
  - schema/availability checks all `true`
  - `errors=[]`
- Should-exit parity artifact:
  - `build/Release/logs/should_exit_parity_report.json`
  - `generated_at_utc=2026-02-25T07:32:03.508188+00:00`
  - `verdict=pass`
  - `critical_findings=0`
  - allowed findings:
    - `allowed_reason_difference` (`BACKTEST_EOD`)
    - `allowed_tp1_collapsed_to_full_due_to_min_order`

## Notes
- Profitability optimization is intentionally deferred until post-Phase-3/4 + migration cleanup policy.
- This document closes the Phase 3 required 6-section delivery format.
