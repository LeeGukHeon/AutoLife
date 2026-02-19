# Validation Method Review (2026-02-19)

## Scope
- Reviewed files:
  - `scripts/run_realdata_candidate_loop.py`
  - `scripts/run_profitability_matrix.py`
  - `scripts/run_candidate_train_eval_cycle.py`
  - `scripts/walk_forward_validate.py`
- Question: is the validation method fundamentally flawed?

## Verdict
- Yes. There are likely structural consistency defects in the validation pipeline.
- Impact: strategy improvements can be hidden or distorted by the validation flow.

## Findings (Root Causes)
1. No final matrix re-run after tuning
- Evidence:
  - `scripts/run_realdata_candidate_loop.py:847`
  - `scripts/run_realdata_candidate_loop.py:874`
  - `scripts/run_realdata_candidate_loop.py:953`
- Risk:
  - final `output_report_json` may not represent tuned state.

2. Ambiguous config source during matrix validation
- Evidence:
  - `scripts/run_realdata_candidate_loop.py:240` matrix command has no explicit source config flag.
  - `scripts/run_profitability_matrix.py:983` default source config is `.\config\config.json`.
- Risk:
  - tuned build config and evaluated source config can diverge.

3. Data-dependent gate relaxation
- Evidence:
  - `scripts/run_profitability_matrix.py:893` (`compute_effective_thresholds`)
  - `scripts/run_profitability_matrix.py:1126` (hostility/quality based adjustments)
- Risk:
  - gate comparability weakens across runs and datasets.

4. `overall_gate_pass` is tightly coupled beyond core profile performance
- Evidence:
  - `scripts/run_profitability_matrix.py:1214`
  - `scripts/run_profitability_matrix.py:1627`
  - `scripts/run_candidate_train_eval_cycle.py:813`
- Risk:
  - `core_full` improvement can still be blocked by profile coupling and legacy delta rules.

5. Holdout split is not strongly time-aware by default
- Evidence:
  - `scripts/run_candidate_train_eval_cycle.py:577`
- Risk:
  - out-of-time overfitting detection can be delayed.

6. Walk-forward state I/O mode is not explicitly controlled
- Evidence:
  - `scripts/run_profitability_matrix.py:69` controls `AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO`
  - `scripts/walk_forward_validate.py` has no matching mode switch
- Risk:
  - state contamination between validation stages.

## Mandatory Fix Order
1. Re-run matrix after tuning in the same pipeline run.
2. Pass explicit source/build config paths.
3. Split strict gate and adaptive gate modes.
4. Decouple `core_full` promotion logic from combined overall gate when needed.
5. Enforce latest-time holdout split.
6. Add explicit walk-forward adaptive state I/O mode.

## Operating Policy After Reset
- No large tuning batches before P0 integrity tasks are complete.
- Analyze failures by data pattern/regime, not by coin id.
- Do not promote based on single-dataset gains.
