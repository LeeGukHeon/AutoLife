# Core Migration Finalization (2026-02-15)

## Current Status
- Core path is now default-on:
  - `include/engine/EngineConfig.h`
  - `src/common/Config.cpp`
- Legacy path is still available as explicit opt-out via config flags:
  - `trading.enable_core_plane_bridge=false`
  - `trading.enable_core_policy_plane=false`
  - `trading.enable_core_risk_plane=false`
  - `trading.enable_core_execution_plane=false`

## Verified Milestones
- PR #3 merged (`5935368`):
  - strict hostility trades-only threshold wiring
  - CI operational gate robustness fixes
- PR #4 merged (`ead34cf`):
  - core plane defaults switched to ON
- CI PR Gate pass confirmed:
  - run `22033385134` (PR #3)
  - run `22033569111` (PR #4)

## Migration Completion Checklist
1. Runtime default path
- Done: core default-on in code and config fallback.

2. Verification chain
- Done: PR gate stable after operational gate script hardening.
- Done: strict/adaptive realdata loop rechecked during Stage 15.

3. Rollback readiness
- Done: legacy opt-out flags remain supported.
- Pending: add one-click rollback preset for emergency fallback.

4. Legacy cleanup readiness
- In progress: legacy-related branches/adapters remain in source.
- Pending: split cleanup into low-risk batches after burn-in window.

## Recommended Cleanup Order
1. Preserve operational safety first
- Keep rollback flags and legacy state file read-path during burn-in.

2. Remove legacy-only verification/tuning paths
- Candidate first targets:
  - legacy-only scenario defaults in tuning scripts
  - legacy-only profile reporting paths where no longer needed for gate policy

3. Remove legacy adapters/branching after burn-in
- Candidate targets:
  - `LegacyExecutionPlaneAdapter`
  - `LegacyPolicyLearningPlaneAdapter`
  - `LegacyRiskCompliancePlaneAdapter`

## Burn-in Policy (Suggested)
- Window: 7 days minimum on core default-on.
- Daily checks:
  - `CI PR Gate` status
  - strict/adaptive realdata gate snapshots
  - core-vs-legacy delta trends (until legacy comparison is retired)

## Notes
- During burn-in, do not remove explicit legacy opt-out.
- Perform legacy deletion in separate PRs with single-purpose scope.
