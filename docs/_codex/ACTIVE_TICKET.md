# Active Ticket Snapshot
Last updated: 2026-02-24

## Header
- Ticket ID: `SO4-14-SHADOW-EVIDENCE-HARDENING-20260224`
- Master Ticket Number (`0`-`7` or `N/A`): `N/A`
- Sub-ticket / Experiment ID: `Strict-Order-4-14`
- Title: Gate4 shadow evidence fail-closed hardening
- Owner: Codex
- Date: 2026-02-24
- Status: `in_progress`
- Mode: `A`

## Scope
- In scope:
  - enforce distinct live/backtest decision-log evidence for Gate4.
  - keep shadow validation and promotion readiness strictly fail-closed.
  - verify Gate4 flow behavior against current artifact set.
- Out of scope:
  - runtime model/feature retraining.
  - actual live order enable.
- Baseline impact:
  - `no runtime policy change` (scripts + gate semantics only)

## Inputs
- Relevant spec section(s):
  - `docs/_codex/MASTER_SPEC.md` section 7
  - `docs/30_VALIDATION_GATES.md`
- Contracts touched:
  - none
- Flags added/changed:
  - none

## Implementation plan
1. reject shadow report generation when live/backtest decision log paths are identical.
2. tighten `shadow_report_pass` semantics in shadow validation/readiness evaluators.
3. rerun Gate4 flow and capture fail-closed blocker evidence.

## Validation plan
- Strict feature validation:
  - N/A
- Parity:
  - N/A
- Verification:
  - N/A (runtime unchanged; latest maintained evidence kept)
- Extra tests:
  - `python scripts/test_probabilistic_shadow_report_generation.py`
  - `python scripts/test_probabilistic_shadow_report_validation.py`
  - `python scripts/test_probabilistic_promotion_readiness.py`
  - `python scripts/test_probabilistic_shadow_gate_flow.py`
  - `build/Release/logs/probabilistic_shadow_gate_flow_step8e_live_enable_v3.json`

## Current result snapshot
- Shadow generation hardening:
  - identical log-path input now emits `shadow_live_backtest_log_path_identical`.
  - shadow `overall_gate_pass`/`shadow_pass` now require `distinct_log_paths=true`.
- Shadow validation/readiness hardening:
  - `shadow_report_pass` now requires `status=pass`, no report errors, and strict gate booleans.
- Gate4 flow rerun (`step8e` artifacts):
  - `build/Release/logs/probabilistic_shadow_gate_flow_step8e_live_enable_v3.json`
  - `status=fail` (expected fail-closed)
  - blocker chain:
    - generate: `shadow_live_backtest_log_path_identical`
    - validate: `shadow_report_status_not_pass`
    - promotion: `gate4_shadow_validation_failed_or_missing`, `gate4_shadow_failed_or_missing`
  - interpretation:
    - true live shadow evidence is currently missing (`policy_decisions.jsonl` absent).

## DoD
- [x] identical live/backtest log-path false positive removed.
- [x] shadow validation and readiness semantics aligned to fail-closed.
- [x] Gate4 flow rerun and blocker evidence recorded.
- [ ] collect real live dry-run decision log and pass Gate4 with distinct paths.

## Risks and rollback
- Risks:
  - if no live decision log is produced, Gate4 remains blocked regardless of Gate3 quality.
- Rollback strategy:
  - retain script hardening; rerun flow with explicit `--live-decision-log-jsonl` once shadow log exists.
