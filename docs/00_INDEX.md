# Probabilistic Execution Docs Index
Last updated: 2026-03-02

## Core baseline
- `docs/STRICT_GATE_RUNBOOK_2026-02-13.md`: Locked full roadmap (hard locks, Stage B~E execution order, output format).
- `docs/RND_GATENEXT_FULL_REBUILD_2026-03-02.md`: Current top-priority RND roadmap (Gate vNext full rebuild, Fresh14-first validation).
- `docs/10_BASELINE_IMPLEMENTED.md`: Current implemented baseline (what runs today).
- `docs/11_BASELINE_RUNBOOK.md`: Step-by-step baseline operation runbook.
- `docs/20_API_POLICY_UPBIT.md`: Upbit API compliance and throttling policy.
- `docs/30_VALIDATION_GATES.md`: Fail-closed validation gates and acceptance criteria.

## Contracts
- `docs/40_DATA_MANIFEST_CONTRACT.md`: Bundle manifest contract including universe scope.
- `docs/41_FEATURE_CONTRACT.md`: Probabilistic feature contract v1.
- `docs/42_RUNTIME_BUNDLE_CONTRACT.md`: Runtime bundle contract v1.

## Extensions (default OFF)
- `docs/50_EXTENSIONS_OVERVIEW.md`: Extension principles and compatibility.
- `docs/51_EXT_PURGED_WALKFORWARD.md`: Purged walk-forward + embargo.
- `docs/52_EXT_EVENT_SAMPLING.md`: Event sampling for training rows.
- `docs/53_EXT_UNCERTAINTY_ENSEMBLE.md`: Ensemble uncertainty and confidence-aware sizing.
- `docs/54_EXT_CONDITIONAL_COST_MODEL.md`: Variable cost model in labels/runtime edge.
- `docs/55_EXT_REGIME_SPEC.md`: Explicit regime detector and action rules.

## Optional MODE B (v2)
- `docs/60_MODE_B_V2_RESTRUCTURE_PLAN.md`: Optional full-restructure migration plan and guardrails.

## Security and compliance
- `docs/90_SECURITY_COMPLIANCE.md`: Secrets, logging, ToS and live-safety requirements.

## Codex session reliability
- `docs/_codex/BOOTSTRAP.md`
- `docs/_codex/CURRENT_STATE.md`
- `docs/_codex/ACTIVE_TICKET.md`
- `docs/_codex/CH01_PHASE3_CONTEXT.md`
- `docs/_codex/CH01_PHASE3_FINAL_DELIVERY_2026-02-25.md`
- `docs/_codex/PARITY_BOUNDARY.md`
- `docs/_codex/CONTEXT_REFRESH_PROTOCOL.md`
- `docs/_codex/TICKET_TEMPLATE.md`
- `docs/_codex/PR_CHECKLIST.md`
- `docs/_codex/MASTER_SPEC.md`

## Single source of truth rule
- Baseline documents define current behavior and must not change without explicit versioning.
- Extensions are opt-in and must not change baseline outputs when OFF.
