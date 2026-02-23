# MODE B / v2 Restructure Plan (Optional)
Last updated: 2026-02-23
Status: Phase 1 transitional + Phase 2 implemented + Phase 3 expanded + Phase 4 expanded + Phase 5 expanded

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
3. Runtime compatibility (expanded):
  - add safe v2 parsing with strict version checks.
  - fail closed on unknown/mixed contracts.
4. Gate redefinition (expanded):
  - define parity/verification criteria specifically for v2.
  - keep v1 gate rules unchanged.
5. Promotion (expanded):
  - run shadow + staged live with v2 bundle only after v2 gates pass.

## Exit criteria for full Ticket 7 completion
- v2 end-to-end pipeline passes strict validation/parity/verification.
- v1 remains fully supported and reproducible.

## Implemented in Phase 1 (transitional)
- `config/model/probabilistic_feature_contract_v2.json`
  - `version=v2_draft` with explicit 43-column row schema freeze.
  - marked as transitional; baseline-equivalent row layout until full MODE B migration.

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

## Implemented in Phase 3 (expanded)
- `src/analytics/ProbabilisticRuntimeModel.cpp`
  - strict bundle version validation:
    - allowed: `probabilistic_runtime_bundle_v1`, `probabilistic_runtime_bundle_v2_draft`
  - fail-closed on:
    - unknown `version`
    - `version` vs `pipeline_version` mismatch
    - unsupported v2 draft contract tags
    - missing v2 strict fields:
      - `pipeline_version`
      - `feature_contract_version`
      - `runtime_bundle_contract_version`
- `tests/TestProbabilisticRuntimeBundleContract.cpp`
  - regression coverage for v2 runtime loader strictness:
    - missing `pipeline_version` => fail
    - missing `feature_contract_version` => fail
    - missing `runtime_bundle_contract_version` => fail
    - valid minimal v2 bundle => pass
- `scripts/validate_runtime_bundle_parity.py`
  - fail-closed when bundle/train/split pipeline versions are mixed
  - parity output records `runtime_bundle_version` and `pipeline_version`

## Implemented in Phase 4 (expanded)
- `scripts/validate_probabilistic_feature_dataset.py`
  - adds `--pipeline-version auto|v1|v2`.
  - fail-closed preflight alignment checks:
    - requested/manifest pipeline consistency
    - manifest dataset version tag consistency
    - manifest contract tag consistency
    - contract version and row-schema consistency
  - outputs `gate_profile` (`v1` or `v2_strict`) and `preflight_errors`.
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
- `scripts/run_probabilistic_hybrid_cycle.py`
  - optional `--run-verification` step added
  - passes `--pipeline-version` through to verification gate
  - passes pipeline-specific feature contract path to strict feature validation gate
  - allows v2 draft cycle to fail fast on strict verification gate failure

## Implemented in Phase 5 (expanded)
- `scripts/evaluate_probabilistic_promotion_readiness.py`
  - fail-closed promotion readiness evaluation for:
    - Gate1 feature validation
    - Gate2 parity
    - Gate3 verification
    - Gate4 shadow (required for `target-stage=live_enable`)
    - Gate5 pre-live safety (`allow_live_orders=false` required before live enable)
  - v2 checks:
    - feature/parity/verification `gate_profile` consistency (`v2_strict`)
    - feature validation preflight errors must be empty
    - shadow report pipeline consistency with promotion target pipeline
    - if shadow report publishes gate profile, v2 requires `v2_strict`
    - optional shadow validation summary can be wired as extra fail-closed evidence
    - pipeline consistency across gate artifacts
- `scripts/validate_probabilistic_shadow_report.py`
  - fail-closed validation for shadow report schema + decision parity evidence
  - strict checks include:
    - `decision_log_comparison_pass`
    - `same_bundle`
    - `same_candles`
    - `compared_decision_count > 0`
    - `mismatch_count == 0`
- `scripts/generate_probabilistic_shadow_report.py`
  - fail-closed shadow report generator from live/backtest policy decision logs
  - emits required evidence fields for Gate4:
    - `checks.decision_log_comparison_pass`
    - `checks.same_bundle`
    - `checks.same_candles`
    - `metadata.compared_decision_count`
    - `metadata.mismatch_count`
- `src/runtime/BacktestRuntime.cpp`
  - writes `logs/policy_decisions_backtest.jsonl` for deterministic shadow decision comparison
- `scripts/run_probabilistic_hybrid_cycle.py`
  - optional `--evaluate-promotion-readiness` step
  - supports `--promotion-target-stage prelive|live_enable`
  - supports `--promotion-shadow-report-json` for live-enable readiness
  - supports `--generate-shadow-report` with decision log inputs
  - supports integrated `--validate-shadow-report` step
  - in `v2 + live_enable`, shadow validation is automatically enforced fail-closed
