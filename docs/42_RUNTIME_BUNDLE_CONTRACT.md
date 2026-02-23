# Runtime Bundle Contract (v1)
Last updated: 2026-02-23

## Canonical bundle
- `config/model/probabilistic_runtime_bundle_v1.json`

## Export tool
- `scripts/export_probabilistic_runtime_bundle.py`

## Parity validation
- `scripts/validate_runtime_bundle_parity.py`

## Modes
- `global_only` (default baseline)
- `hybrid`
- `per_market`

## Contract expectations
- Include model reference(s) and feature metadata needed by runtime.
- Include training provenance:
  - source summary path
  - created timestamp
  - version identifiers
- For extension mode (future):
  - include optional ensemble artifacts/config only when enabled:
    - `default_model.ensemble_members[]`
    - `default_model.ensemble_model_artifacts[]`
    - top-level `ensemble`

## Baseline invariants
- Runtime prediction path must match training/export semantics.
- Parity test is mandatory before any live consideration.
- Bundle format changes require explicit versioning and migration notes.
