# Current State
Last updated: 2026-02-23

## Repository
- Branch: current working branch (check with `git branch --show-current`)
- Commit (snapshot): `7d28827974b851e30788efff2d06bcb4e2e55872`

## Contract hashes
- `config/model/probabilistic_feature_contract_v1.json`:
  - `c0c8f87df95be43ce6dd9526ed9c31b674e211bf187b365f2022439e68f1084e`
- `config/model/probabilistic_runtime_bundle_v1.json`:
  - `fddbfe3514e1e2d726c5c6cd58b6105fda50459c038ae283acb5af14aacdba84`
- `data/backtest_probabilistic/probabilistic_bundle_manifest.json`:
  - `6c6568cbfc5349332e1899fb9ac154d3f372de09ccd51ad8d24e91a75591f89a`

## Active direction
- Master spec baseline: `docs/_codex/MASTER_SPEC.md`
- Execution mode target: Mode A (baseline-preserving), extensions behind flags
- Current implementation focus:
  - Ticket 0: docs/bootstrap reliability pack (implemented in current working tree)
  - Ticket 1: dynamic universe + scope-aware 1m fetch/build strictness (implemented in current working tree)
  - Ticket 2: purged walk-forward + embargo split option (implemented)
  - Ticket 3: conditional cost model isomorphism (implemented in current working tree)
  - Ticket 4: explicit probabilistic regime spec + runtime actions (implemented in current working tree)
  - Ticket 5: uncertainty ensemble + confidence-aware sizing (implemented)
  - Ticket 6: event sampling (time|dollar|volatility) for training rows (implemented in current working tree)
  - Optional Ticket 7 (MODE B): v2 draft kickoff + Phase 2 pipeline branching (`v1|v2` switches) implemented
  - Optional Ticket 7 (MODE B): Phase 3 partial implemented (strict version/pipeline fail-closed checks)
  - Optional Ticket 7 (MODE B): Phase 4 partial implemented (v2 parity/verification strict gate profile)

## Last known gate status
- Strict feature validation: run required after any feature/build changes
- Bundle parity: run required after model/bundle/export changes
- Verification: run required after decision/runtime logic changes
- Shadow/live staged enable: not active by default

## Known issues / watchpoints
- 1m full-market fetch is expensive and operationally brittle for dynamic top-N usage.
- Universe-scoped 1m handling is now available; keep manifest scope semantics explicit and reproducible.
- Do not change baseline outputs when extension flags are OFF.
