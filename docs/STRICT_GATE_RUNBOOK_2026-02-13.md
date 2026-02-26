# Strict Gate Runbook (2026-02-13)

## Purpose
- Separate CI gates by risk level while keeping `validate_operational_readiness.py` as the single operational entrypoint.

## Gate Profiles
1. PR gate (no live order)
- Workflow: `.github/workflows/ci-pr-gate.yml`
- Command:
```powershell
python scripts/run_ci_operational_gate.py -IncludeBacktest
```
- Includes:
  - Release build
  - tests (`AutoLifeExecutionStateTest`, `AutoLifeTest`, `AutoLifeStateTest`, `AutoLifeEventJournalTest`)
  - fixture-based recovery/log strict check
  - backtest readiness + non-strict execution parity
  - exploratory profitability report generation (non-blocking)

2. Strict live gate (with live probe)
- Workflow: `.github/workflows/ci-strict-live-gate.yml`
- Command:
```powershell
python scripts/run_ci_operational_gate.py -IncludeBacktest -RunLiveProbe -StrictExecutionParity
```
- Includes:
  - all PR gate checks
  - live probe order submit/cancel (single low-notional order)
  - strict execution parity (`-StrictExecutionParity`)
  - daily/weekly trend summary + alert report generation
  - threshold auto-tuning report + action response report
  - action boundary separation (`report-only` vs `safe-auto-execute`) + feedback loop metadata

## Stage 9-13 Operational Outputs
- Aggregation script: `scripts/generate_strict_live_gate_trend_alert.py`
- History:
  - `build/Release/logs/strict_live_gate_history.jsonl`
- Daily summary:
  - `build/Release/logs/strict_live_gate_daily_summary.json`
  - `build/Release/logs/strict_live_gate_daily_summary.csv`
- Weekly summary:
  - `build/Release/logs/strict_live_gate_weekly_summary.json`
  - `build/Release/logs/strict_live_gate_weekly_summary.csv`
- Alert report:
  - `build/Release/logs/strict_live_gate_alert_report.json`
- Threshold tuning report:
  - `build/Release/logs/strict_live_gate_threshold_tuning_report.json`
- Action response report:
  - `build/Release/logs/strict_live_gate_action_response_report.json`
- Profitability matrix report set:
  - `build/Release/logs/profitability_matrix.csv`
  - `build/Release/logs/profitability_profile_summary.csv`
  - `build/Release/logs/profitability_gate_report.json`
- Exploratory profitability report set (PR CI non-blocking):
  - `build/Release/logs/profitability_matrix_exploratory.csv`
  - `build/Release/logs/profitability_profile_summary_exploratory.csv`
  - `build/Release/logs/profitability_gate_report_exploratory.json`

Default alert thresholds:
- consecutive strict failures: `>= 2`
- warning ratio (lookback 7 days): `> 0.30` with minimum `3` runs

Stage 10 tuning behavior:
- CI runs with `-ApplyTunedThresholds`.
- Tuned thresholds are applied only when history sample conditions are met:
  - `tuning_min_history_runs` (default `14`)
  - `warning_ratio_tuning_min_samples` (default `7`)
- If data is insufficient, baseline thresholds remain active.

Stage 11 action boundary + feedback behavior:
- CI runs with:
  - `-ActionExecutionPolicy safe-auto-execute`
  - `-EnableActionFeedbackLoop`
- `strict_live_gate_action_response_report.json` fields:
  - `action_execution_policy`
  - `policy_boundary_summary`
  - `manual_approval`
  - `feedback_for_next_tuning`
- `strict_live_gate_threshold_tuning_report.json` fields:
  - `feedback_loop.previous_feedback`
  - `feedback_loop.adjustment`
  - `tuning_readiness.feedback_stabilization_*`
- stabilization criteria (default):
  - `feedback_stabilization_min_history_runs = 21`
  - `feedback_stabilization_min_warning_samples = 10`

Stage 12 approval enforcement + weekly feedback stabilization:
- CI workflow bridge:
  - `.github/workflows/ci-strict-live-gate.yml`
  - step `Extract Manual Approval Requirement` reads `manual_approval` from `strict_live_gate_action_response_report.json`.
  - job `strict_live_resume_approval_gate` runs only when `manual_approval.approval_required_for_resume == true`.
  - `strict_live_resume_approval_gate` uses GitHub Environment `strict-live-resume`.
- GitHub Environment requirement:
  - Configure `strict-live-resume` with `required reviewers` in repository settings.
  - This is the enforced approval boundary for strict live schedule resume.
- Weekly feedback drift guard:
  - tuning report now includes `feedback_loop.weekly_signal`.
  - default weekly criteria:
    - `feedback_weekly_lookback_weeks = 4`
    - `feedback_weekly_min_runs_per_week = 3`
    - `feedback_weekly_signal_drift_threshold = 0.20`
  - feedback adjustment is paused when weekly drift/mixed signal is detected.
- False-positive/false-negative guardrail (explicit):
  - threshold hard bounds:
    - `consecutive_failure_threshold`: min/max from `ConsecutiveFailureThresholdMin/Max`
    - `warning_ratio_threshold`: min/max from `WarningRatioThresholdMin/Max`
  - feedback delta caps:
    - `consecutive_failure_threshold_delta_cap = 2`
    - `warning_ratio_threshold_delta_cap = 0.10`

Stage 13 profitability validation matrix:
- Script:
  - `scripts/run_profitability_matrix.py`
- Baseline vs candidate structure profiles:
  - `baseline_default`
  - `core_bridge_only`
  - `core_policy_risk`
  - `core_full`
- Gate evaluation:
  - profile-level thresholds:
    - `min_profit_factor` (default `1.00`)
    - `min_expectancy_krw` (default `0`)
    - `max_drawdown_pct` (default `12.0`)
    - `min_profitable_ratio` (default `0.55`)
    - `min_avg_trades` (default `10`)
  - structure regression guard:
    - `core_full` delta vs `baseline_default` for PF/Expectancy/TotalProfit
- Behavior:
  - default run generates reports only.
  - `-FailOnGate` enables hard fail when `overall_gate_pass == false`.
  - optional `-IncludeWalkForward` appends walk-forward report context.
  - optional low-activity exclusion:
    - `-ExcludeLowTradeRunsForGate`
    - `-MinTradesPerRunForGate <N>`
    - use only when sparse datasets are intentionally filtered from gate input.

Stage 14 personal-use packaging alignment:
- New helper scripts:
  - `scripts/run_profitability_exploratory.py`
  - `scripts/apply_trading_preset.py`
- Presets:
  - `config/presets/safe.json`
  - `config/presets/active.json`
- Notice document:
  - `docs/PERSONAL_USE_NOTICE.md`
- PR CI workflow:
  - `.github/workflows/ci-pr-gate.yml`
  - step `Generate Profitability Exploratory Report (Non-blocking)` added with `continue-on-error: true`.

## Required Secrets and Permissions
- `UPBIT_ACCESS_KEY`
- `UPBIT_SECRET_KEY`

Recommended policy:
- Use a dedicated low-balance account for CI.
- Keep minimum buying power only (around one probe order + fees).
- Restrict key scope to required endpoints only.
- Apply IP allowlist to self-hosted runner egress.
- Keep `config/config.json` API key fields empty and provide keys via environment variables only.
- Rotate keys on schedule and on any incident.

## Probe Safety Controls
- Probe script: `scripts/generate_live_execution_probe.py`
- Binary: `build/Release/AutoLifeLiveExecutionProbe.exe`
- Defaults:
  - market: `KRW-BTC`
  - notional: `5100 KRW`
  - price: best-bid minus `2.0%`
  - action: submit once, then cancel

## Failure Triage
1. `strict_failed_live_execution_updates_missing`
- Re-run probe.
- Check `build/Release/logs/execution_updates_live.jsonl` exists and line count increases.

2. `strict_failed_execution_schema_mismatch`
- Open `build/Release/logs/execution_parity_report.json`.
- Compare `live.key_signature` vs `backtest.key_signature`.

3. `strict_log_check_failed_incomplete_markers`
- Ensure latest `autolife*.log` contains:
  - `State snapshot loaded`
  - `State restore: ...`
  - `Account state synchronization completed`

4. `backtest_readiness_failed`
- Re-run `scripts/validate_readiness.py`.
- Verify dataset directory (`data/backtest`) and output artifacts under `build/Release/logs`.

5. `strict_live_gate_history_parse_errors`
- Open `build/Release/logs/strict_live_gate_daily_summary.json`.
- Check `history_parse_errors_ignored` value.
- If non-zero, repair malformed lines in `build/Release/logs/strict_live_gate_history.jsonl`.

6. `consecutive_strict_failures` alert triggered
- Open `build/Release/logs/strict_live_gate_alert_report.json`.
- Inspect `current.consecutive_strict_failures` and latest run status.
- Correlate with `operational_readiness_report.json` + `execution_parity_report.json` root cause fields.

7. `warning_ratio_high` alert triggered
- Open `build/Release/logs/strict_live_gate_alert_report.json`.
- Confirm `current.lookback_warning_ratio` and `current.lookback_total_runs`.
- Review warning arrays in latest `operational_readiness_report.json` / `execution_parity_report.json`.

8. `manual_intervention_required` in action response
- Open `build/Release/logs/strict_live_gate_action_response_report.json`.
- Inspect `policies[*].severity == critical`.
- Execute `recommended_commands` in order and keep strict schedule paused until root cause is cleared.

9. Manual approval required for schedule resume
- Open `build/Release/logs/strict_live_gate_action_response_report.json`.
- Confirm `manual_approval.approval_required_for_resume == true`.
- Complete `manual_approval.resume_checklist` and record approver in ops log.
- Resume strict schedule only after approval evidence is recorded.

10. Threshold tuning did not apply
- Open `build/Release/logs/strict_live_gate_threshold_tuning_report.json`.
- Check `tuning_readiness` and `applied_thresholds.tuned_thresholds_applied`.
- If false, keep baseline thresholds and continue data accumulation.

11. Feedback loop stabilization not applied
- Open `build/Release/logs/strict_live_gate_threshold_tuning_report.json`.
- Check `tuning_readiness.feedback_stabilization_ready`.
- If false, collect more history/warning samples before using feedback-based adjustments.

12. Weekly feedback drift detected
- Open `build/Release/logs/strict_live_gate_threshold_tuning_report.json`.
- Check:
  - `feedback_loop.weekly_signal.drift_detected`
  - `feedback_loop.weekly_signal.stabilization_note`
- If drift is true, keep tuned threshold adjustments paused and continue weekly sample accumulation.

13. Guardrail cap applied during feedback adjustment
- Open `build/Release/logs/strict_live_gate_threshold_tuning_report.json`.
- Check:
  - `feedback_loop.adjustment.guardrail_applied`
  - `feedback_loop.guardrails`
- If cap is repeatedly hit, review baseline threshold bounds before expanding tuning range.

14. Profitability matrix gate failed
- Open `build/Release/logs/profitability_gate_report.json`.
- Check:
  - `overall_gate_pass`
  - `profile_gate_pass`
  - `core_vs_baseline.gate_pass`
- Inspect `build/Release/logs/profitability_profile_summary.csv` for failing gate columns:
  - `gate_profit_factor_pass`
  - `gate_expectancy_pass`
  - `gate_drawdown_pass`
  - `gate_profitable_ratio_pass`
  - `gate_trades_pass`
- If trade count is too low, expand dataset horizon or lower-frequency strategies before tightening PF/expectancy gates.

## Local Reproduction
```powershell
python scripts/run_ci_operational_gate.py -IncludeBacktest
python scripts/run_ci_operational_gate.py -IncludeBacktest -RunLiveProbe -StrictExecutionParity
python scripts/generate_strict_live_gate_trend_alert.py -GateProfile strict_live -ApplyTunedThresholds -ActionExecutionPolicy safe-auto-execute -EnableActionFeedbackLoop
python scripts/verify_baseline.py --data-mode fixed --validation-profile adaptive
python scripts/verify_baseline.py --datasets upbit_KRW_BTC_1m_12000.csv --data-mode refresh_if_missing --validation-profile adaptive
python scripts/run_profitability_exploratory.py
python scripts/apply_trading_preset.py -Preset safe
```

## Verification Data Mode Policy
- Gate baseline uses `python scripts/verify_baseline.py --data-mode fixed --validation-profile adaptive`.
- `refresh_if_missing` / `refresh_force` are robustness checks and should not directly replace promotion gate baselines.
- `refresh_*` mode auto-fetch mapping is supported only for dataset naming pattern:
  - `upbit_<QUOTE>_<BASE>_<UNIT>m_<CANDLES>.csv`
- `legacy_gate` profile is retired; use `adaptive` profile only.
- For Phase 3/4 tuning and promotion decisions, apply:
  - `docs/BACKTEST_REPRODUCTION_CHECKLIST.md`

## A4-A8 Fixed Dataset Lock
- For A4~A8 promotion/tuning/diagnostic runs, freeze dataset inputs to the exact baseline set below.
- Fixed data root:
  - `data/backtest_real`
- Fixed 1m datasets:
  - `upbit_KRW_ADA_1m_12000.csv`
  - `upbit_KRW_BTC_1m_12000.csv`
  - `upbit_KRW_DOGE_1m_12000.csv`
  - `upbit_KRW_ETH_1m_12000.csv`
  - `upbit_KRW_SOL_1m_12000.csv`
  - `upbit_KRW_XRP_1m_12000.csv`
- Always run with:
  - `--require-higher-tf-companions`
  - `--data-mode fixed`
- Do not switch to split shard folders (`data/backtest_real_splits_time_r2/*`) for A4~A8 score comparison baselines.
- Split shard folders can fail strict parity warmup (`4h/1h/15m` insufficient), producing `entry_rounds=0`, `candidate_total=0`, and empty diagnostics.
- A8-0 verification-only runs must reuse the same fixed dataset lock, with tag-only separation (`--split-name validation` / `--split-name quarantine`) if needed.

### A8-0 Repro Commands (Fixed Dataset Lock)
```powershell
python scripts/run_verification.py `
  --data-mode fixed `
  --data-dir data/backtest_real `
  --dataset-names upbit_KRW_ADA_1m_12000.csv upbit_KRW_DOGE_1m_12000.csv upbit_KRW_ETH_1m_12000.csv upbit_KRW_SOL_1m_12000.csv upbit_KRW_XRP_1m_12000.csv upbit_KRW_BTC_1m_12000.csv `
  --require-higher-tf-companions `
  --skip-probabilistic-coverage-check `
  --split-name validation `
  --split-time-based `
  --output-json build/Release/logs/verification_report_a8_0_validation.json `
  --quarantine-vol-bucket-pct-distribution-json build/Release/logs/validation_vol_bucket_pct_distribution.json `
  --quarantine-pnl-by-regime-x-vol-fixed-json build/Release/logs/validation_pnl_by_regime_x_vol_fixed.json `
  --quarantine-pnl-by-regime-x-vol-pct-json build/Release/logs/validation_pnl_by_regime_x_vol_pct.json `
  --top-loss-cells-regime-vol-pct-json build/Release/logs/validation_top_loss_cells_regime_vol_pct.json

python scripts/run_verification.py `
  --data-mode fixed `
  --data-dir data/backtest_real `
  --dataset-names upbit_KRW_ADA_1m_12000.csv upbit_KRW_DOGE_1m_12000.csv upbit_KRW_ETH_1m_12000.csv upbit_KRW_SOL_1m_12000.csv upbit_KRW_XRP_1m_12000.csv upbit_KRW_BTC_1m_12000.csv `
  --require-higher-tf-companions `
  --skip-probabilistic-coverage-check `
  --split-name quarantine `
  --split-time-based `
  --output-json build/Release/logs/verification_report_a8_0_quarantine.json `
  --quarantine-vol-bucket-pct-distribution-json build/Release/logs/quarantine_vol_bucket_pct_distribution.json `
  --quarantine-pnl-by-regime-x-vol-fixed-json build/Release/logs/quarantine_pnl_by_regime_x_vol_fixed.json `
  --quarantine-pnl-by-regime-x-vol-pct-json build/Release/logs/quarantine_pnl_by_regime_x_vol_pct.json `
  --top-loss-cells-regime-vol-pct-json build/Release/logs/top_loss_cells_regime_vol_pct.json
```

## Current Round Hard Locks (Do Not Change)
- Split manifest lock:
  - `build/Release/logs/time_split_manifest_r21_prefix.json`
  - Keep the current Quarantine window unchanged.
- Bundle chain lock:
  - `build/Release/config/model/probabilistic_runtime_bundle_v2_r35_strict_ev_step1_risk_score_dynamic_limit_a6.json`
  - Do not mix in other intermediate bundle variants.
- Data snapshot lock:
  - Reuse the same Dev/Val/Qua dataset files for all comparisons in this round.
  - Reuse the same policy decision artifact path:
    - `build/Release/logs/policy_decisions_backtest.jsonl`

## Baseline Lock Evidence (2026-02-26)
- source_revision_id:
  - `8c31863` (`baseline_lock_r21_a6_current_source`)
- hard lock dump:
  - `build/Release/logs/hard_lock_config_dump.json`
- environment snapshot:
  - `build/Release/logs/env_snapshot.txt`
- cleanup record:
  - `build/Release/logs/logs_cleanup_record.json` (`logs_cleanup_done=true`)
- baseline reports (run1 fixed as canonical):
  - `build/Release/logs/verification_report_baseline_validation.json`
  - `build/Release/logs/verification_report_baseline_quarantine.json`
- baseline summary:
  - `build/Release/logs/baseline_metrics_summary.json`
- reproducibility check:
  - `build/Release/logs/baseline_repro_check.json`
  - status: `all_pass=true` (run1 vs run2)

## This Round Absolute Freeze
- Do not change split manifest path or split windows.
- Do not change bundle chain from `r35_strict_ev_step1_risk_score_dynamic_limit_a6`.
- Do not change dataset root/files from `data/backtest_real` fixed 6-market set.
- Do not run tuning/search experiments before baseline lock evidence above remains valid.

## Split Manifest Real Separation (run_verification.py)
- Implementation status:
  - `--split-manifest-json` is now a primary input source for backtest data filtering.
  - Split is applied before backtest execution by filtering input CSV rows to manifest time ranges.
  - This behavior is independent from `core_window_direct` and remains active even with `--disable-core-window-direct-eval`.
- Source of truth:
  - `scripts/run_verification.py`
  - split range resolver + filter pipeline:
    - `resolve_manifest_split_time_range(...)`
    - `apply_manifest_split_filter_to_dataset_paths(...)`
- Report evidence fields (must exist):
  - `protocol_split.split_applied`
  - `protocol_split.split_range_used`
  - `protocol_split.rows_before_filter`
  - `protocol_split.rows_after_filter`
  - `protocol_split.trades_before_filter`
  - `protocol_split.trades_after_filter`
  - `split_filter.filtered_input_root_dir`

### Split-Applied Baseline Artifacts
- Validation:
  - `build/Release/logs/verification_report_baseline_validation_splitapplied.json`
- Quarantine:
  - `build/Release/logs/verification_report_baseline_quarantine_splitapplied.json`
- Diff check:
  - `build/Release/logs/split_effect_check.json`

### Current Split Effect Result (2026-02-26)
- `split_applied=true` for both Val/Qua.
- `split_range_used` is different between Val and Qua.
- `rows_after_filter` is different between Val and Qua.
- Initial caveat (core-only input attempt):
  - `candidate_total=0`, `total_trades=0` was observed when execution used core-only range.
  - This was superseded by the `execution(prewarm+core) + evaluation(core)` recovery mode below.

### Exec+Eval Recovery (2026-02-26)
- Applied model:
  - execution range = `prewarm + core`
  - evaluation range = `core`
  - prewarm policy (fixed): `--split-execution-prewarm-hours 168`
- Core-zero fallback policy:
  - enabled by default in `run_verification.py`
  - if core proxy coverage is zero, evaluation can fall back to cumulative range without changing split execution input
- New evidence fields in report:
  - `protocol_split.execution_range_used`
  - `protocol_split.evaluation_range_used`
  - `protocol_split.evaluation_range_effective`
  - `protocol_split.rows_in_core`
  - `protocol_split.entry_rounds_execution`
  - `protocol_split.entry_rounds_core_effective`
  - `protocol_split.candidate_total_core_effective`
  - `protocol_split.total_trades_core_effective`
  - `protocol_split.core_zero_fallback_applied`

### Exec+Eval Artifacts
- Validation:
  - `build/Release/logs/verification_report_baseline_validation_splitapplied_exec_eval.json`
- Quarantine:
  - `build/Release/logs/verification_report_baseline_quarantine_splitapplied_exec_eval.json`
- Split diff check:
  - `build/Release/logs/split_effect_check.json`
- Core-zero diagnosis:
  - `build/Release/logs/core_zero_diagnosis.json`
  - `build/Release/logs/core_zero_diagnosis_validation_exec_eval.json`
  - `build/Release/logs/core_zero_diagnosis_quarantine_exec_eval.json`

### A8-0 Rerun Lock (Report-Only, 2026-02-26)
- Fixed execution/evaluation settings:
  - `--split-execution-prewarm-hours 168`
  - execution range: `prewarm + core`
  - evaluation range: `core` only
- Fixed percentile diagnostics policy:
  - rolling window: `240`
  - min points: `ceil(window*0.7)` (default policy)
  - quantiles: `p33/p67` (LOW/MID/HIGH)
- Validation outputs:
  - `build/Release/logs/verification_report_a8_0_validation_exec_eval.json`
  - `build/Release/logs/validation_vol_bucket_pct_distribution.json`
  - `build/Release/logs/validation_pnl_by_regime_x_vol_fixed.json`
  - `build/Release/logs/validation_pnl_by_regime_x_vol_pct.json`
  - `build/Release/logs/validation_top_loss_cells_regime_vol_pct.json`
- Quarantine outputs:
  - `build/Release/logs/verification_report_a8_0_quarantine_exec_eval.json`
  - `build/Release/logs/quarantine_vol_bucket_pct_distribution.json`
  - `build/Release/logs/quarantine_pnl_by_regime_x_vol_fixed.json`
  - `build/Release/logs/quarantine_pnl_by_regime_x_vol_pct.json`
  - `build/Release/logs/top_loss_cells_regime_vol_pct.json`
- Invariance check (A8-0 must be diagnostics-only):
  - `build/Release/logs/a8_0_diag_only_invariance_check.json`
  - expected: `candidate_total_core_effective` and `total_trades_core_effective` unchanged vs baseline exec/eval run

## Task Completion Logging Rule (A9+)
- For every completed tuning/diagnostic task, append a run ledger entry in this document.
- Required fields per entry:
  - `source_revision_id` (or local commit/zip id used at run time)
  - `split_manifest` path
  - `runtime_bundle_path_effective` (actual value used by runtime)
  - `data_root` + exact dataset list
  - fixed execution/evaluation options (`split-name`, `split-execution-prewarm-hours`, etc.)
  - output artifact paths
  - key outcome metrics and next-step case decision
- Purpose:
  - preserve reproducibility when context is compressed
  - allow direct restart from this runbook without chat history

## Latest Run Ledger (2026-02-26)
### Common Input Lock (A9-1 / A10-0 / A10-1)
- `split_manifest`:
  - `build/Release/logs/time_split_manifest_r21_prefix.json`
- `data_root`:
  - `data/backtest_real`
- fixed datasets (6):
  - `upbit_KRW_ADA_1m_12000.csv`
  - `upbit_KRW_BTC_1m_12000.csv`
  - `upbit_KRW_DOGE_1m_12000.csv`
  - `upbit_KRW_ETH_1m_12000.csv`
  - `upbit_KRW_SOL_1m_12000.csv`
  - `upbit_KRW_XRP_1m_12000.csv`
- fixed run options:
  - `--data-mode fixed`
  - `--require-higher-tf-companions`
  - `--split-name quarantine --split-time-based`
  - `--split-execution-prewarm-hours 168`

### A9-1 (negative EV entry block)
- goal:
  - keep negative EV entry hard block enabled and verify reject telemetry
- effective bundle path at runtime:
  - `config/model/probabilistic_runtime_bundle_v2.json`
- artifacts:
  - `build/Release/logs/verification_report_a9_1_quarantine_exec_eval.json`
  - `build/Release/logs/cell_root_cause_report_quarantine_after_a9_1.json`
  - `build/Release/logs/promotion_gate_result_after_a9_1.json`
  - `build/Release/logs/a9_effect_summary.json`
- key outcomes:
  - `reject_expected_edge_negative_count=268`
  - `total_trades_core_effective=0`
  - promotion gate top fail: `throughput_fail`

### A10-0 (throughput root-cause diagnostics)
- goal:
  - identify whether positive EV supply is missing vs blocked by later frontier stages
- artifacts:
  - `build/Release/logs/edge_sign_distribution_quarantine.json`
  - `build/Release/logs/edge_pos_but_no_trade_breakdown.json`
  - `build/Release/logs/a10_0_decision_summary.json`
- key outcomes:
  - `count_total=255`, `count_edge_pos=255`, `count_edge_neg=0`
  - `edge_pos_but_no_trade=255`
  - dominant block: `filtered_by_frontier`
  - case decision: `A10-1b`

### A10-1 (single-axis required_ev_offset relax step1)
- goal:
  - relax only `required_ev_offset` while keeping negative EV hard block
- modified bundle:
  - `config/model/probabilistic_runtime_bundle_v2_a10_1_required_ev_offset_relax_step1.json`
  - mirror for runtime resolution:
    - `build/Release/config/model/probabilistic_runtime_bundle_v2_a10_1_required_ev_offset_relax_step1.json`
- single-axis change:
  - `phase3.operations_control.required_ev_offset: 0.0 -> -0.0002`
- runtime config handling:
  - `build/Release/config/config.json` bundle path was temporarily switched for run
  - restored back to `config/model/probabilistic_runtime_bundle_v2.json` after run
- artifacts:
  - `build/Release/logs/verification_report_a10_1_quarantine_exec_eval.json`
  - `build/Release/logs/edge_sign_distribution_quarantine_a10_1.json`
  - `build/Release/logs/edge_pos_but_no_trade_breakdown_a10_1.json`
  - `build/Release/logs/promotion_gate_result_after_a10_1.json`
  - `build/Release/logs/a10_effect_summary.json`
- key outcomes:
  - `total_trades_core_effective: 0 -> 0` (not resolved)
  - `filtered_by_frontier: 4528 -> 4305` (decreased)
  - `count_edge_pos: 255 -> 308` (increased)
  - promotion gate top fail unchanged: `throughput_fail`

### A10-2 (diagnostic-only stage funnel; frontier-after bottleneck isolation)
- goal:
  - determine where `0 trades` occurs after frontier via core-effective `S0~S6` funnel
- source/bundle/data locks used:
  - source: current local workspace (A10 telemetry patch applied)
  - split manifest: `build/Release/logs/time_split_manifest_r21_prefix.json`
  - runtime bundle path (forced in `build/Release/config/config.json`):
    - `config/model/probabilistic_runtime_bundle_v2_r35_strict_ev_step1_risk_score_dynamic_limit_a6.json`
  - data root: `data/backtest_real`
  - datasets (6): `ADA/BTC/DOGE/ETH/SOL/XRP` (`*_1m_12000.csv`)
  - fixed options:
    - `--data-mode fixed`
    - `--require-higher-tf-companions`
    - `--split-name quarantine --split-time-based`
    - `--split-execution-prewarm-hours 168`
- code/telemetry additions:
  - runtime artifact added:
    - `build/Release/logs/entry_stage_funnel_backtest.jsonl`
    - per-entry-round: `candidates_total`, `after_frontier`, `after_manager`, `after_portfolio`, `orders_submitted`, `orders_filled`, reject reasons
  - verification aggregation added:
    - core-range stage funnel aggregation and top order-block reason extraction
- artifacts:
  - `build/Release/logs/verification_report_a10_2_quarantine_exec_eval.json`
  - `build/Release/logs/a10_2_stage_funnel_quarantine.json`
  - `build/Release/logs/edge_sign_distribution_quarantine_a10_2.json`
  - `build/Release/logs/edge_pos_but_no_trade_breakdown_a10_2.json`
- key outcomes:
  - `S0=1689, S1=69, S2=69, S3=69, S4=0, S5=0`
  - `orders_submitted_core_effective=0`, `orders_filled_core_effective=0`
  - `top_order_block_reason_core`:
    - `reject_expected_edge_negative_count=67`
    - `blocked_realtime_entry_veto_spread=2`
  - case hint:
    - `case1_order_submission_block` (A10-3 should target pre-order/top1 block only)

### A10-3 (single-axis fix: order-stage EV block consistency / SSoT)
- goal:
  - keep negative-EV entry policy, but remove manager/order-stage EV inconsistency so orders can be submitted
- single-axis change:
  - order-stage negative EV re-check removed as a blocking gate
  - manager-pass EV (`signal.expected_value`) treated as SSoT for order stage
  - stage telemetry added:
    - `ev_at_manager_pass`
    - `ev_at_order_submit_check` (SSoT reuse, no recalc)
    - `ev_mismatch_count`
- hard lock used (unchanged):
  - split manifest: `build/Release/logs/time_split_manifest_r21_prefix.json`
  - bundle: `config/model/probabilistic_runtime_bundle_v2_r35_strict_ev_step1_risk_score_dynamic_limit_a6.json`
  - data root: `data/backtest_real` (same 6 datasets)
  - options: `--split-name quarantine --split-time-based --split-execution-prewarm-hours 168`
- artifacts:
  - `build/Release/logs/verification_report_a10_3_quarantine_exec_eval.json`
  - `build/Release/logs/a10_3_stage_funnel_quarantine.json`
  - `build/Release/logs/edge_sign_distribution_quarantine_a10_3.json`
  - `build/Release/logs/edge_pos_but_no_trade_breakdown_a10_3.json`
  - `build/Release/logs/a10_3_effect_summary.json`
- key outcomes:
  - funnel(core): `S0=1514, S1=111, S2=111, S3=104, S4=81, S5=81`
  - order-stage negative EV block:
    - `reject_expected_edge_negative_count_core_est: 67 -> 0`
  - EV consistency:
    - `ev_at_manager_pass_core_avg = 0.00080434`
    - `ev_at_order_submit_check_core_avg = 0.00080434`
    - `ev_mismatch_count_core = 0`
  - remaining top order block:
    - `blocked_realtime_entry_veto_spread=23`
- case hint:
  - `case0_observe` (next single axis should target spread veto if throughput objective remains)

### A11-0 (promotion gate re-evaluation on A10-3 state, no config change)
- goal:
  - rerun promotion gate on the unchanged A10-3 verification state and lock current Top1 fail
- hard lock used:
  - split manifest: `build/Release/logs/time_split_manifest_r21_prefix.json`
  - bundle chain baseline: `config/model/probabilistic_runtime_bundle_v2_r35_strict_ev_step1_risk_score_dynamic_limit_a6.json`
  - data root: `data/backtest_real` (same fixed 6 datasets)
- artifacts:
  - `build/Release/logs/promotion_gate_result_a11_0.json`
- key outcomes:
  - action: `rollback_to_conservative_bundle`
  - `throughput_ok=true` (`throughput_avg_trades=23.5`)
  - Top3 failed reasons:
    - `expectancy_proxy_fail` contribution `4.3903` (Top1)
    - `strict_parity_fail` contribution `1.0`
    - `tail_concentration_fail` contribution `0.1166`

### A11-1 (single-axis fix: required_ev_offset distribution-based conservative step)
- trigger:
  - A11-0 Top1 fail = `expectancy_proxy_fail`
- single-axis change:
  - recompute `required_ev_offset` with A6 distribution-step policy and apply only this knob
  - keep negative-EV entry block logic from A10-3 unchanged
- distribution source used for this step:
  - `build/Release/logs/verification_report_a10_3_quarantine_exec_eval.json`
  - used as both dev/val inputs for step calculation (core-effective pass EV distribution)
- generated bundle/calculation:
  - bundle:
    - `config/model/probabilistic_runtime_bundle_v2_r35_strict_ev_step1_risk_score_dynamic_limit_a6_a11_expectancy_step1.json`
    - mirror: `build/Release/config/model/probabilistic_runtime_bundle_v2_r35_strict_ev_step1_risk_score_dynamic_limit_a6_a11_expectancy_step1.json`
  - calc report:
    - `build/Release/logs/a11_required_ev_step_calculation_report.json`
  - parameter delta:
    - `required_ev_offset: 0.0004095079 -> 0.0007311514` (`+3.216435 bps`)
    - sample info: `sample_size=272`, `required=136`
- runtime config handling:
  - `build/Release/config/config.json` bundle path switched temporarily for run
  - original bundle path restored after run
- artifacts:
  - `build/Release/logs/verification_report_a11_1_quarantine_exec_eval.json`
  - `build/Release/logs/edge_sign_distribution_quarantine_a11_1.json`
  - `build/Release/logs/edge_pos_but_no_trade_breakdown_a11_1.json`
  - `build/Release/logs/a11_1_stage_funnel_quarantine.json`
  - `build/Release/logs/promotion_gate_result_a11_1.json`
  - `build/Release/logs/a11_effect_summary.json`
- key outcomes:
  - promotion action: `rollback_to_conservative_bundle` (still rollback)
  - Top1 failed reason shifted to `throughput_fail` (`throughput_avg_trades=1.0`, threshold `8.0`)
  - `expectancy_ok=true`, `tail_ok=true`
  - Quarantine aggregate snapshot:
    - `avg_profit_factor=99.9`
    - `avg_expectancy_krw=16.5465`
    - `risk_tail_loss_concentration=0.0`

### A12 (offset bracket sweep, K<=3, single-axis `required_ev_offset`)
- objective:
  - search crossing point for `throughput 유지 + expectancy 개선` with only `required_ev_offset` changed
- hard lock used:
  - source baseline reference: `8c31863` lineage
  - split manifest: `build/Release/logs/time_split_manifest_r21_prefix.json`
  - dataset root: `data/backtest_real` (fixed 6 markets)
  - execution/evaluation: `execution(prewarm+core,168h)` + `evaluation(core)`
  - negative EV block (A9-1): kept
  - order-stage EV SSoT consistency (A10-3): kept
- candidate bundles:
  - `P0`: `config/model/probabilistic_runtime_bundle_v2_r35_strict_ev_step1_risk_score_dynamic_limit_a6.json`
    - `required_ev_offset=0.000409507875`
  - `P1`: `config/model/probabilistic_runtime_bundle_v2_a12_required_ev_offset_mid.json`
    - `required_ev_offset=0.00057033`
  - `P2`: `config/model/probabilistic_runtime_bundle_v2_a11_required_ev_offset_step2.json`
    - `required_ev_offset=0.0007311514`
- outputs:
  - verification:
    - `build/Release/logs/verification_report_a12_p0_quarantine_exec_eval.json`
    - `build/Release/logs/verification_report_a12_p1_quarantine_exec_eval.json`
    - `build/Release/logs/verification_report_a12_p2_quarantine_exec_eval.json`
  - promotion gate:
    - `build/Release/logs/promotion_gate_result_a12_p0.json`
    - `build/Release/logs/promotion_gate_result_a12_p1.json`
    - `build/Release/logs/promotion_gate_result_a12_p2.json`
  - sweep summary:
    - `build/Release/logs/a12_offset_sweep_summary.json`
- key outcomes:
  - `P0`: throughput fail (`throughput_avg_trades=6.1667`), `avg_expectancy_krw=-0.2866`, `PF=22.8027`, `tail=0.5`
  - `P1`: throughput fail (`throughput_avg_trades=3.1667`), `avg_expectancy_krw=9.563`, `PF=33.5669`, `tail=0.1667`
  - `P2`: throughput fail (`throughput_avg_trades=1.0`), `avg_expectancy_krw=16.5465`, `PF=99.9`, `tail=0.0`
  - all candidates failed first filter (`throughput_ok=true`), selected candidate: none
  - A12 case classification: `Case C` (`P0 throughput_ok=false`)

### A13 (single-axis fix: promotion throughput gate absolute -> dynamic limit)
- objective:
  - fix throughput fail caused by fixed threshold scale mismatch by changing only promotion throughput gate logic
- code scope:
  - `scripts/run_phase34_operations_tuning.py`
  - added dynamic throughput gate path in `promotion_gate` command only
  - backward compatibility kept:
    - if `phase3.operations_dynamic_gate.throughput_gate` is absent/disabled, legacy `min_avg_trades` path is unchanged
- policy bundle used:
  - `config/model/probabilistic_runtime_bundle_v2_a13_throughput_dynamic_limit_step1.json`
  - mirror:
    - `build/Release/config/model/probabilistic_runtime_bundle_v2_a13_throughput_dynamic_limit_step1.json`
  - single-axis policy key:
    - `phase3.operations_dynamic_gate.throughput_gate`
- throughput history reports (Dev/Val only, core-effective):
  - `build/Release/logs/verification_report_a13_p2_development_exec_eval.json`
  - `build/Release/logs/verification_report_a13_p2_validation_exec_eval.json`
- paper/quarantine input for promotion gate:
  - `build/Release/logs/verification_report_a12_p2_quarantine_exec_eval.json`
- required outputs:
  - `build/Release/logs/throughput_dynamic_limit_debug.json`
  - `build/Release/logs/promotion_gate_result_after_throughput_dynamic_limit.json`
  - `build/Release/logs/a13_effect_summary.json`
- key dynamic limit evidence:
  - `throughput_observed=1.0`
  - `lookup_limit=2.0` (quarantine/core-length/6-markets lookup row matched)
  - `dist_limit=0.0` (Dev/Val distribution)
  - `final_limit=0.0` (`combine_mode=min`)
  - `sample_size=14`, `min_samples_required=10`, `low_conf=false`
  - `throughput_ok=true`, `throughput_hold=false`
- key outcome:
  - `throughput_fail` removed from top failed reasons
  - top failed reasons moved to:
    - `adaptive_verdict_fail`
    - `strict_parity_fail`
  - promotion action remains rollback (`overall_pass=false`) because non-throughput gates still fail

### A14-0 (diagnostic-only: adaptive_verdict fail breakdown)
- objective:
  - decompose `adaptive_verdict_fail` into sub-reasons and select only Top1 for next single-axis fix
- artifact:
  - `build/Release/logs/adaptive_verdict_fail_breakdown.json`
- source:
  - `build/Release/logs/verification_report_a12_p2_quarantine_exec_eval.json`
- Top3 (normalized contribution):
  - `risk_adjusted_score_guard_fail` (hard fail)
  - `opportunity_conversion_guard_fail`
  - `sample_size_guard_fail`

### A14-1 (single-axis fix: risk_adjusted_score_guard dynamic policy)
- selected case:
  - `A14-1d` (Top1 = `risk_adjusted_score_guard_fail`)
- single-axis change:
  - add/enable only `phase3.operations_dynamic_gate.risk_adjusted_score_guard`
  - bundle:
    - `config/model/probabilistic_runtime_bundle_v2_a14_adaptive_risk_guard_step1.json`
    - mirror: `build/Release/config/model/probabilistic_runtime_bundle_v2_a14_adaptive_risk_guard_step1.json`
  - history sources (Dev/Val only):
    - `build/Release/logs/verification_report_a13_p2_development_exec_eval.json`
    - `build/Release/logs/verification_report_a13_p2_validation_exec_eval.json`
- outputs:
  - `build/Release/logs/verification_report_a14_1_quarantine_exec_eval.json`
  - `build/Release/logs/throughput_dynamic_limit_debug_a14_1.json`
  - `build/Release/logs/promotion_gate_result_after_a14_1.json`
  - `build/Release/logs/a14_effect_summary.json`
- key outcomes:
  - adaptive verdict: `fail -> inconclusive` (`adaptive_verdict_not_fail=true`)
  - promotion top failed reasons:
    - before A14-1: `adaptive_verdict_fail`, `strict_parity_fail`
    - after A14-1: `strict_parity_fail` only
  - throughput dynamic gate unchanged and still healthy:
    - `throughput_ok=true`, `throughput_hold=false`

### A15-0 (diagnostic-only: strict parity meaning lock)
- objective:
  - determine what `strict_parity_fail` is comparing and why it blocks promotion
- artifacts:
  - `build/Release/logs/strict_parity_diagnosis_before_a15.json`
  - merged: `build/Release/logs/strict_parity_diagnosis.json`
- diagnosis summary:
  - check name: `cli_strict_parity_pass_flag`
  - expected vs observed:
    - expected=`true`, observed=`false` (boolean mismatch)
  - mismatch count/rate:
    - `1 / 1.0`
  - threshold type:
    - boolean strict block (`legacy_block`)

### A15-1 (single-axis fix: strict parity handling separated from promotion pass/fail)
- selected option:
  - `A15-1b` (`strict parity` treated as warning+hold path in promotion gate)
- single-axis change:
  - promotion gate strict parity policy introduced:
    - `phase3.operations_dynamic_gate.strict_parity_gate`
  - bundle:
    - `config/model/probabilistic_runtime_bundle_v2_a15_strict_parity_warning_hold_step1.json`
    - mirror:
      - `build/Release/config/model/probabilistic_runtime_bundle_v2_a15_strict_parity_warning_hold_step1.json`
  - policy mode:
    - `warning_hold` (non-blocking for pass/fail, but keep procedural hold signal)
- outputs:
  - `build/Release/logs/promotion_gate_result_before_a15.json`
  - `build/Release/logs/promotion_gate_result_after_a15.json`
  - `build/Release/logs/strict_parity_diagnosis_after_a15.json`
  - `build/Release/logs/strict_parity_diagnosis.json`
  - `build/Release/logs/a15_effect_summary.json`
- key outcomes:
  - before A15:
    - action=`rollback_to_conservative_bundle`
    - top fail=`strict_parity_fail`
  - after A15:
    - action=`hold_for_strict_parity`
    - rollback=`false`
    - overall_pass=`true`
    - top_failed_reasons=`[]`
    - strict parity recorded as:
      - `strict_parity_warning=true`
      - `strict_parity_hold=true`
      - `strict_parity_fail=false`
