# MODE B / v2 Restructure Plan (Optional)
Last updated: 2026-02-23
Status: Phase 2 implemented + Phase 3 partial + Phase 4 partial (v2 gate profile)

## Goal
- Prepare optional MODE B migration with explicit v2 contracts.
- Keep baseline MODE A (`v1`) as the default active path until full parity + verification is redefined and passed.

## Added draft artifacts
- `config/model/probabilistic_feature_contract_v2.json`
- `config/model/probabilistic_runtime_bundle_v2.json`

## Guardrails
- v2 is not active by default.
- No existing v1 script/runtime path may switch behavior implicitly.
- Any v2 run must be explicit and produce separate manifests/bundles.

## Planned phases
1. Contract freeze:
   - finalize v2 column contract and transforms.
   - finalize v2 bundle fields and provenance.
2. Pipeline branching (implemented):
   - add explicit `v1|v2` switches in build/train/export scripts.
   - keep `v1` default.
   - fail closed on v1/v2 mixed inputs.
3. Runtime compatibility (partial implemented):
   - add safe v2 parsing with strict version checks.
   - fail closed on unknown/mixed contracts.
4. Gate redefinition (partial implemented):
   - define parity/verification criteria specifically for v2.
   - keep v1 gate rules unchanged.
5. Promotion:
   - run shadow + staged live with v2 bundle only after v2 gates pass.

## Exit criteria for full Ticket 7 completion
- v2 end-to-end pipeline passes strict validation/parity/verification.
- v1 remains fully supported and reproducible.

## Implemented in Phase 2
- `scripts/build_probabilistic_feature_dataset.py`
  - `--pipeline-version v1|v2` (default `v1`)
  - `v2` emits `prob_features_v2_draft` manifest version.
- `scripts/train_probabilistic_pattern_model_global.py`
  - `--pipeline-version v1|v2` (default `v1`)
  - verifies split-manifest pipeline/version compatibility before training.
- `scripts/export_probabilistic_runtime_bundle.py`
  - `--pipeline-version v1|v2` (default `v1`)
  - verifies train-summary pipeline/version compatibility before export.
  - `v2` emits `probabilistic_runtime_bundle_v2_draft`.
- `scripts/run_probabilistic_hybrid_cycle.py`
  - passes pipeline version to build/train/export.
  - when `v2` and defaults are used, output paths auto-switch to v2 draft paths.

## Implemented in Phase 3 (partial)
- `src/analytics/ProbabilisticRuntimeModel.cpp`
  - strict bundle version validation:
    - allowed: `probabilistic_runtime_bundle_v1`, `probabilistic_runtime_bundle_v2_draft`
  - fail-closed on:
    - unknown `version`
    - `version` vs `pipeline_version` mismatch
    - unsupported v2 draft contract tags
- `scripts/validate_runtime_bundle_parity.py`
  - fail-closed when bundle/train/split pipeline versions are mixed
  - parity output records `runtime_bundle_version` and `pipeline_version`

## Implemented in Phase 4 (partial)
- `scripts/validate_runtime_bundle_parity.py`
  - v2 requires explicit draft contract tags:
    - `feature_contract_version=v2_draft`
    - `runtime_bundle_contract_version=v2_draft`
  - parity output includes `gate_profile` (`v1` or `v2_strict`)
- `scripts/run_verification.py`
  - adds `--pipeline-version auto|v1|v2`
  - v2 strict contract requires all pass:
    - threshold gate
    - adaptive verdict
    - baseline comparison available
    - dataset-set comparability
    - baseline non-degradation contract pass
  - v2 gate fail returns non-zero exit code (fail-closed)
