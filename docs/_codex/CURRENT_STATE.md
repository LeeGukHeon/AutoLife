# Current State
Last updated: 2026-02-25

## Repository
- Branch: `main`
- HEAD (pushed): `3ee443f`

## Baseline Status
- Phase 2 parity-readiness: complete
- Phase 3 runtime policy: default ON (`probabilistic_runtime_bundle_v2.json`)
- Runtime bundle contract: `v2_draft` only
- strict gate baseline: passing

## Active Work
- Source of truth: `docs/_codex/ACTIVE_TICKET.md`
- Current ticket: `CH01-PHASE4-PORTFOLIO-ENGINE-20260225`
- Current phase: `Phase 4 step7 verification delta hardening complete (OFF/ON comparison report path active)`

## Mandatory Boundaries
- Preserve Live/Backtest parity
- Deterministic + bundle-first policy
- Fail-closed behavior (unknown/mismatch => no trade)
- Phase 4 flags default OFF

## Quick Links
- Spec: `docs/_codex/MASTER_SPEC.md`
- Ticket: `docs/_codex/ACTIVE_TICKET.md`
- Phase 4 context: `docs/_codex/CH01_PHASE4_CONTEXT.md`
- Parity boundary: `docs/_codex/PARITY_BOUNDARY.md`
- Verification script audit: `docs/_codex/VERIFICATION_SCRIPT_AUDIT_2026-02-25.md`

## Gate Command (locked baseline)
```powershell
python scripts/run_ci_operational_gate.py --include-backtest --strict-execution-parity --run-should-exit-parity-analysis --refresh-should-exit-audit-from-logs --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" --should-exit-audit-live-runtime-log-mode-filter exclude_backtest --strict-should-exit-parity
```\n
