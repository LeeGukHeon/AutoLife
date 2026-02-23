# Current State
Last updated: 2026-02-23

## Repository
- Branch: current working branch (check with `git branch --show-current`)
- Commit (snapshot): check with `git rev-parse HEAD`

## Contract hashes
- `config/model/probabilistic_feature_contract_v1.json`:
  - `c0c8f87df95be43ce6dd9526ed9c31b674e211bf187b365f2022439e68f1084e`
- `config/model/probabilistic_feature_contract_v2.json`:
  - `8dc014f9a52305dc26bce0b6c8cc7a10cb3047ddfeb725aad09f62a8140704f6`
- `config/model/probabilistic_runtime_bundle_v1.json`:
  - `fddbfe3514e1e2d726c5c6cd58b6105fda50459c038ae283acb5af14aacdba84`
- `config/model/probabilistic_runtime_bundle_v2.json`:
  - `d9f2749382e931488f4d0e2fa03f367db4effc180f846d232a83816c7fbfdc3e`
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
  - Optional Ticket 7 (MODE B): Phase 1 transitional contract freeze (`v2_draft` row schema explicit) implemented
  - Optional Ticket 7 (MODE B): v2 draft kickoff + Phase 2 pipeline branching (`v1|v2` switches) implemented
  - Optional Ticket 7 (MODE B): Phase 3 expanded (runtime v2 strict field presence + version/pipeline fail-closed checks)
  - Optional Ticket 7 (MODE B): Phase 4 expanded (Gate1 + Gate2 + Gate3 v2 strict profile alignment)
  - Optional Ticket 7 (MODE B): hybrid cycle optional verification-step wiring implemented
  - Optional Ticket 7 (MODE B): Phase 5 expanded (promotion readiness evaluator + hybrid wiring, includes Gate1 preflight/profile checks and Gate4 shadow pipeline/profile checks)
  - Optional Ticket 7 (MODE B): runtime strictness regression test added (`AutoLifeProbBundleContractTest`)
  - Optional Ticket 7 (MODE B): shadow report strict validator + hybrid cycle integration (`validate_probabilistic_shadow_report.py`, `--validate-shadow-report`) implemented
  - Optional Ticket 7 (MODE B): shadow report generator + decision-log evidence path implemented (`generate_probabilistic_shadow_report.py`, `--generate-shadow-report`, backtest policy decision artifact)
  - Optional Ticket 7 (MODE B): hybrid cycle arg guard hardened (shadow generate/validate flags are fail-closed outside promotion evaluation mode) + regression tests added
  - Optional Ticket 7 (MODE B): live policy decision audit reset on engine start (stale shadow comparison contamination guard)
  - Codex context refresh helper implemented (`scripts/run_codex_context_refresh_checks.py`) and wired into bootstrap/protocol docs
  - Ticket 1 universe-scope behavior regression tests added (`scripts/test_probabilistic_universe_scope.py`)
  - Optional Ticket 7 (MODE B): standalone Gate4 flow runner implemented (`scripts/run_probabilistic_shadow_gate_flow.py`) + regression tests
  - API policy hardening: `scripts/fetch_upbit_historical_candles.py` now enforces `Remaining-Req sec==0` boundary throttle+jitter, bounded 429/418 backoff cap, defensive Origin-header stripping, and richer compliance telemetry (endpoint/params/status/latency/retry).
  - Runtime API policy hardening: `src/execution/RateLimiter.cpp` + `src/network/UpbitHttpClient.cpp` now apply sec==0 throttle evidence, bounded 429 exponential backoff path, and defensive Origin-header stripping in request header assembly.
  - Codex context refresh checks now enforce gate-output fail-closed semantics (`scripts/run_codex_context_refresh_checks.py`): feature/parity require `status=pass`, runtime verification requires `overall_gate_pass=true`.

## Last known gate status
- Strict feature validation: run required after any feature/build changes
- Bundle parity: run required after model/bundle/export changes
- Verification: run required after decision/runtime logic changes
- Latest runtime context-refresh verification check (`pipeline=v1`) executed and failed fail-closed (`overall_gate_pass=false` in `build/Release/logs/context_refresh_verification.json`).
- Shadow/live staged enable: not active by default

## Known issues / watchpoints
- 1m full-market fetch is expensive and operationally brittle for dynamic top-N usage.
- Universe-scoped 1m handling is now available; keep manifest scope semantics explicit and reproducible.
- Do not change baseline outputs when extension flags are OFF.
