# Runtime Bundle Contract (v1)
Last updated: 2026-02-25

## Canonical bundle
- `config/model/probabilistic_runtime_bundle_v1.json`

## Optional draft bundle (not active)
- `config/model/probabilistic_runtime_bundle_v2.json` (MODE B kickoff draft)

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

## Strict version gate
- Runtime/parity must fail closed on:
  - unknown runtime bundle `version`
  - `version` and `pipeline_version` mismatch
  - mixed pipeline versions across bundle/train/split inputs

## v2 strict fields
- For `probabilistic_runtime_bundle_v2_draft`, these fields are mandatory:
  - `pipeline_version=v2`
  - `feature_contract_version=v2_draft`
  - `runtime_bundle_contract_version=v2_draft`
- Missing v2 strict fields are treated as fail-closed runtime load errors.

## Phase3 extension block (v1-compatible, default OFF)
- Optional top-level keys:
  - `phase3_frontier_enabled=false`
  - `phase3_ev_calibration_enabled=false`
  - `phase3_cost_tail_enabled=false`
  - `phase3_adaptive_ev_blend_enabled=false`
  - `phase3_diagnostics_v2_enabled=false`
- Optional nested object:
  - `phase3.frontier`
  - `phase3.ev_calibration`
  - `phase3.cost_model`
  - `phase3.adaptive_ev_blend`
  - `phase3.diagnostics_v2`
- Runtime rules:
  - Missing `phase3` block must behave as legacy/identity fallback.
  - Nested `*.enabled` and top-level `phase3_*_enabled` are synchronized at export/load.
  - OFF path must preserve baseline outputs.
