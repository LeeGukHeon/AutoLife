# Active Ticket Snapshot
Last updated: 2026-02-24

## Header
- Ticket ID: `SO4-12-DAILY-OOS-20260224`
- Master Ticket Number (`0`-`7` or `N/A`): `N/A`
- Sub-ticket / Experiment ID: `Strict-Order-4-12`
- Title: Day-sliced OOS stability guardrail (anti-overfit evidence)
- Owner: Codex
- Date: 2026-02-24
- Status: `done`
- Mode: `A`

## Scope
- In scope:
  - Add reproducible day-sliced OOS verification helper script.
  - Wire command into gate/runbook docs.
  - Keep baseline logic unchanged.
- Out of scope:
  - Strategy/runtime threshold tuning.
  - Live enable changes.
- Baseline impact:
  - `none` (new verification helper only)

## Inputs
- Relevant spec section(s):
  - `docs/_codex/MASTER_SPEC.md` section 7 (Gate 3 OOS-focused evaluation)
  - `docs/30_VALIDATION_GATES.md`
- Contracts touched:
  - none
- Flags added/changed:
  - none (new script arguments only)

## Implementation plan
1. Add `scripts/run_daily_oos_stability.py` (daily slice backtests + fail-closed gate checks).
2. Add unit tests for deterministic helper logic.
3. Update gate/runbook docs with command and pass criteria.

## Validation plan
- Strict feature validation:
  - N/A
- Parity:
  - N/A
- Verification:
  - run daily OOS script on a small dataset subset
- Extra tests:
  - `python scripts/test_daily_oos_stability.py`

## DoD
- [x] Daily OOS report JSON/CSV generated with reproducible gate checks.
- [x] Gate fail reasons emitted explicitly.
- [x] Gate/Runbook docs include command and expected pass interpretation.
- [x] Smoke run completed on target datasets and artifact path recorded.

## Smoke artifact
- `build/Release/logs/daily_oos_stability_report_smoke_20260224.json`
- `build/Release/logs/daily_oos_stability_windows_smoke_20260224.csv`
- Smoke verdict: `fail` (expected fail-closed; blocker=`max_nonpositive_day_ratio`)
- Multi-market run:
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_fix2.json`
  - `build/Release/logs/daily_oos_stability_windows_3m_7d_20260224_fix2.csv`
  - verdict: `fail` (blockers=`max_nonpositive_day_ratio`, `positive_profit_sum`)

## Risks and rollback
- Risks:
  - Runtime cost increases when many datasets/days are requested.
- Rollback strategy:
  - Revert added script/docs and keep existing `run_verification.py` gate only.
