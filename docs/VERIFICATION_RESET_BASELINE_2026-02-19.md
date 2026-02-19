# Verification Reset Baseline (2026-02-19)

## Decision
- We restart verification from a minimal baseline.
- New default entrypoint: `scripts/run_verification.py`.
- Design constraints:
  - sequential only
  - adaptive validation profile by default (`validation_profile=adaptive`)
  - legacy threshold gate retained only as compatibility mode
  - fixed dataset mode for gate decisions
  - refresh dataset modes for robustness checks only
  - single matrix + single report output
  - no adaptive threshold relaxation
  - no profile-coupled gate in baseline mode

## Why
- Existing verification code became too large for reliable debugging and context handoff.
- Baseline first enables deterministic validation before advanced policy layers.

## Active Artifacts
- Report: `build/Release/logs/verification_report.json`
- Matrix: `build/Release/logs/verification_matrix.csv`
- Report diagnostics (engine/strategy bottleneck):
  - `diagnostics.aggregate`
  - `diagnostics.per_dataset`
  - `diagnostics.failure_attribution`
  - `diagnostics.per_dataset[*].strategy_collection`
- Report diagnostics (post-entry lifecycle):
  - `adaptive_validation.per_dataset[*].post_entry_risk_telemetry`
  - `adaptive_validation.aggregates.avg_adaptive_*`
  - `adaptive_validation.aggregates.adaptive_partial_ratio_histogram`

## User Command (minimal)
- `python scripts/verify_baseline.py`
- Gate baseline:
  - `python scripts/verify_baseline.py --data-mode fixed --validation-profile adaptive`
- Robustness check (optional):
  - `python scripts/verify_baseline.py --realdata-only --datasets upbit_KRW_BTC_1m_12000.csv --data-mode refresh_if_missing --validation-profile adaptive`
 - Legacy compatibility check (optional):
  - `python scripts/verify_baseline.py --data-mode fixed --validation-profile legacy_gate`

## Data Mode Contract
- `fixed`: no data refresh, deterministic gate baseline.
- `refresh_if_missing`: refresh only when dataset file is missing.
- `refresh_force`: refresh every run.
- `refresh_*` requires dataset naming contract:
  - `upbit_<QUOTE>_<BASE>_<UNIT>m_<CANDLES>.csv`

## Legacy Freeze Scope (Do not extend)
- `scripts/run_profitability_matrix.py`
- `scripts/run_realdata_candidate_loop.py`
- `scripts/run_candidate_train_eval_cycle.py`
- `scripts/run_candidate_auto_improvement_loop.py`
- `scripts/tune_candidate_gate_trade_density.py`

These remain for backward compatibility while baseline proves stability.

## Phase Plan
1. Phase A: baseline adoption
   - run baseline command in README/runbook
   - collect reproducibility samples
2. Phase B: compatibility wrappers
   - route selected old entrypoints to baseline mode
3. Phase C: legacy archive
   - move frozen scripts to `legacy_archive/` after parity checks

## Success Criteria
- Same datasets + same config => same gate outcome.
- Investigations can be done from one report without extra side artifacts.
- Root-cause direction must be visible from report diagnostics before parameter tuning.
- Adaptive validation verdict (`pass|fail|inconclusive`) should be explainable from regime/risk metrics.
- Low sample size should default to `inconclusive` unless hard-risk guards fail.
- Sample-size thresholds should be regime-aware (hostile market allows lower trade count).
- Opportunity conversion (`entries_executed / generated_signals`) must be tracked with trade count.

