# Verification Script Audit (2026-02-25)

## Scope
- scripts/run_ci_operational_gate.py
- scripts/validate_*.py (except deleted scripts)
- scripts/run_verification.py
- scripts/verify_baseline.py
- scripts/verify_script_suite.py
- scripts/collect_strategyless_exit_audit.py
- scripts/prepare_operational_readiness_fixture.py

## Decisions

### Deleted (unused from active workflow)
- scripts/capture_baseline.py
  - Reason: not referenced by CI, operational gate, hybrid cycle, or current runbooks.
- scripts/validate_small_seed.py
  - Reason: only used by deleted capture_baseline.py.

### Kept (active)
- scripts/run_ci_operational_gate.py
- scripts/validate_operational_readiness.py
- scripts/validate_execution_parity.py
- scripts/validate_recovery_e2e.py
- scripts/validate_recovery_state.py
- scripts/validate_replay_reconcile_diff.py
- scripts/validate_should_exit_parity.py
- scripts/run_verification.py
- scripts/verify_baseline.py
- scripts/verify_script_suite.py
- scripts/validate_probabilistic_feature_dataset.py
- scripts/validate_runtime_bundle_parity.py
- scripts/validate_probabilistic_shadow_report.py
- scripts/validate_probabilistic_inference_parity.py
- scripts/collect_strategyless_exit_audit.py
- scripts/prepare_operational_readiness_fixture.py

## Normalization Applied

### 1) Recovery state CLI naming cleanup
- Removed legacy naming from active CLI surface.
- Canonical argument is now `--state-path` (`-StatePath`).
- Updated scripts:
  - scripts/prepare_operational_readiness_fixture.py
  - scripts/validate_recovery_state.py
  - scripts/validate_recovery_e2e.py
  - scripts/validate_operational_readiness.py

### 2) Outdated fixed-date default paths cleanup
- scripts/validate_probabilistic_inference_parity.py
  - train summary default -> `probabilistic_model_train_summary_global_latest.json`
  - split manifest default -> `probabilistic_features_v2_draft_latest/probabilistic_split_manifest_v2_draft.json`
  - output default -> `probabilistic_inference_parity_latest.json`
- scripts/validate_runtime_bundle_parity.py
  - output default -> `probabilistic_runtime_bundle_parity_latest.json`
- scripts/run_ci_operational_gate.py
  - should-exit audit default -> `strategyless_exit_audit_latest.json`
- scripts/validate_should_exit_parity.py
  - audit default -> `strategyless_exit_audit_latest.json`
- scripts/collect_strategyless_exit_audit.py
  - output default -> `strategyless_exit_audit_latest.json`

### 3) Deprecated CLI flag surface cleanup
- scripts/verify_baseline.py
  - `--realdata-only` kept as hidden no-op compatibility flag (`argparse.SUPPRESS`).
  - User-visible messages normalized to current wording.

## Validation Results After Cleanup
- `python -m py_compile ...` on changed scripts: PASS
- `python scripts/verify_script_suite.py --skip-help`: PASS
- Recovery/operational smoke:
  - `python scripts/prepare_operational_readiness_fixture.py --log-path build/Release/logs/ci_fixture/autolife_ci_fixture.log`: PASS
  - `python scripts/validate_operational_readiness.py -NoStrictLogCheck -LogDir build/Release/logs/ci_fixture -OutputJson build/Release/logs/operational_readiness_report_tmp.json`: PASS

## Notes
- Historical docs may still reference deleted scripts or old artifact names. Those references are non-runtime and can be cleaned in a docs-only sweep.
