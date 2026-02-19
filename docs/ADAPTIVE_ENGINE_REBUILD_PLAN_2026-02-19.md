# Adaptive Engine Rebuild Plan (2026-02-19)

## Objective
- Stop patch-style threshold tuning and rebuild around deterministic strategy/risk logic.
- Target behavior:
  - adaptive by market regime
  - conservative in hostile/downtrend markets
  - continuous post-entry risk management
  - prioritize risk-adjusted expectancy, not raw win-rate only

## Non-Negotiable Principles
- Legacy runtime path must be disconnected from active execution.
  - Early return is allowed as temporary protection.
  - Physical removal is required after shadow parity checks.
- No coin hardcoding. Use regime/pattern level logic only.
- Validation default must be adaptive (`--validation-profile adaptive`).
- Legacy threshold gate is compatibility-only (`--validation-profile legacy_gate`).
- Gate baseline must use `--data-mode fixed`; refresh modes are robustness checks only.
- Validation execution remains sequential (`--max-workers 1`).

## Ordered Execution (Current Batch)
1. Documentation sync first.
  - Update active chapter/todo/runbook baseline rules.
  - Keep active docs short; move details to archive docs only.
2. Runtime legacy disconnect hardening.
  - Remove unreachable legacy selection branch in `StrategyManager`.
  - Keep only foundation-first selection/prefilter path as active runtime.
3. Validation script alignment.
  - Keep adaptive as default.
  - Keep legacy mode only for compatibility checks and explicit calls.
4. Rebuild verification and smoke checks.
  - Release build
  - adaptive baseline smoke
  - optional legacy compatibility smoke
5. Chapter close update.
  - Record completed changes and exact remaining tasks in `docs/CHAPTER_CURRENT.md`.

## Phases
1. Phase A: Legacy disconnect
- Status: mostly complete (StrategyManager path)
- Scope:
  - Foundation prefilter path active
  - Foundation-first signal selection active
  - Unreachable legacy block in `selectRobustSignalWithDiagnostics(...)` physically pruned

2. Phase B: Position lifecycle risk rewrite
- Status: pending
- Scope:
  - dynamic TP/SL
  - trailing logic
  - post-entry risk re-evaluation state machine
  - downtrend loss containment and exposure throttling

3. Phase C: Verification adaptive baseline
- Status: baseline complete
- Scope:
  - adaptive verdict: `pass|monitor|fail`
  - regime-aware metrics:
    - downtrend loss/trade
    - downtrend trade share
    - uptrend expectancy
    - risk-adjusted score

4. Phase D: Legacy physical cleanup
- Status: in progress
- Scope:
  - remove dead/unreachable legacy blocks
  - archive legacy-only scripts after parity checks

## Success Criteria
- Runtime path executes only rebuilt strategy/risk logic by default.
- Validation result is reproducible and explainable from one report.
- Promotion logic is based on robust regime/risk evidence, not single-slice gains.
