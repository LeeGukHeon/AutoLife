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
  - `legacy_default`
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
    - `core_full` delta vs `legacy_default` for PF/Expectancy/TotalProfit
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
  - `core_vs_legacy.gate_pass`
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
python scripts/run_profitability_matrix.py
python scripts/run_profitability_exploratory.py
python scripts/apply_trading_preset.py -Preset safe
```
