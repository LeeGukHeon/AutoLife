# File Usage Map

Last updated: 2026-02-21

## Purpose
Keep only files that are directly tied to current runtime, CI gate, and probabilistic model workflow.

## runtime-critical
- `CMakeLists.txt`
- `src/**/*.cpp` used by `AutoLifeTrading` target
- `include/**/*.h` used by runtime compile contracts
- `config/config.json`, `config/model/*`, `config/presets/active.json`, `config/presets/safe.json`
- `tests/*.cpp` (gate/unit test binaries)

Rule:
- Delete only after replacement path is implemented and verified.

## ops-critical
- `.github/workflows/ci-pr-gate.yml`
- `.github/workflows/ci-strict-live-gate.yml`
- `scripts/run_ci_operational_gate.py`
- `scripts/validate_operational_readiness.py`
- `scripts/validate_execution_parity.py`
- `scripts/validate_readiness.py`
- `scripts/validate_recovery_e2e.py`
- `scripts/validate_recovery_state.py`
- `scripts/validate_replay_reconcile_diff.py`
- `scripts/generate_live_execution_probe.py`
- `scripts/generate_strict_live_gate_trend_alert.py`
- `scripts/run_verification.py`
- `scripts/verify_baseline.py`
- `scripts/run_profitability_matrix.py`
- `scripts/generate_probabilistic_baseline.py`
- `scripts/build_probabilistic_feature_dataset.py`
- `scripts/train_probabilistic_pattern_model.py`
- `scripts/export_probabilistic_runtime_bundle.py`
- `scripts/validate_probabilistic_feature_dataset.py`
- `scripts/validate_probabilistic_inference_parity.py`
- `scripts/validate_runtime_bundle_parity.py`
- `scripts/fetch_probabilistic_training_bundle.py`
- `scripts/fetch_upbit_historical_candles.py`
- `scripts/run_profitability_exploratory.py`
- `scripts/walk_forward_validate.py`

Rule:
- If removed, CI/workflow command and docs must be updated in same change.

## removed-as-legacy (2026-02-21)
- Legacy candidate tuning / patch override chain:
  - `scripts/run_candidate_auto_improvement_loop.py`
  - `scripts/run_candidate_train_eval_cycle.py`
  - `scripts/run_realdata_candidate_loop.py`
  - `scripts/tune_candidate_gate_trade_density.py`
  - `scripts/run_patch_action_override_ab.py`
  - `scripts/run_patch_action_override_feedback_promotion_check.py`
  - `scripts/analyze_loss_contributors.py`
  - `scripts/analyze_entry_rejections.py`
  - `scripts/validate_context_stability_guard.py`
  - `scripts/verify_cleanup_wave_a.py`
- Unreferenced parity/taxonomy helper scripts (archived):
  - `scripts/generate_parity_invariant_report.py`
  - `scripts/generate_strategy_rejection_taxonomy_report.py`
- Obsolete large archive docs:
  - `docs/archive/CHAPTER_CURRENT_FULLLOG_2026-02-20.md`
  - `docs/archive/TODO_STAGE15_EXECUTION_PLAN_2026-02-13_FULLLOG_2026-02-19.md`
  - `docs/archive/SCRIPT_READING_TUNE_CANDIDATE_GATE_2026-02-19.md`
  - `docs/archive/TUNE_CANDIDATE_GATE_LINE_AUDIT_2026-02-19.md`
  - `docs/archive/V2_INCLUDE_README_LEGACY.md`
  - `docs/archive/V2_SRC_README_LEGACY.md`
- Obsolete review docs tied to removed chain:
  - `docs/FOUNDATION_ENGINE_STRATEGY_REVIEW_2026-02-19.md`
  - `docs/VALIDATION_METHOD_REVIEW_2026-02-19.md`
  - `docs/VERIFICATION_RESET_BASELINE_2026-02-19.md`

## safety policy
- Deletion rule: verify no runtime/CI reference -> delete -> build/smoke validate.
- Keep sequential verification; no parallel verification/tuning flow reintroduction.
