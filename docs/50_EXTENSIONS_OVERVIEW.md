# Extensions Overview
Last updated: 2026-02-23
Default state: OFF for all extensions

## Core rule
- Extensions must be opt-in and reproducible.
- With all extension flags OFF, baseline outputs must remain unchanged.

## Extension set
- `EXT-51`: Purged walk-forward + embargo
- `EXT-52`: Event sampling (`time|dollar|volatility`)
- `EXT-53`: Uncertainty via ensemble-K
- `EXT-54`: Conditional cost model
- `EXT-55`: Regime spec hardening

## Required implementation standards
- A dedicated config flag per extension.
- Deterministic behavior (fixed seeds/inputs produce identical outputs).
- Summary/manifests record extension settings and hashable parameters.
- Unit and integration tests per extension.

## Rollout principle
- Introduce one extension at a time.
- Re-run strict gates after each extension enablement.
- Keep rollback path to baseline.
