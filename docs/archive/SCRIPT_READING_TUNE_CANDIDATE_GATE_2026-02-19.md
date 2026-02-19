# tune_candidate_gate_trade_density.py Code Reading (2026-02-19)

## Scope
- Target file: `scripts/tune_candidate_gate_trade_density.py`
- Size snapshot:
  - ~22,946 lines
  - top-level functions: 75

## High-Level Structure
- Entry and orchestration:
  - `main` (`scripts/tune_candidate_gate_trade_density.py:14487`) is extremely large (~9,013 lines).
  - This function currently handles argument parsing output, dataset gate checks, combo expansion, screening/final evaluation, selector application, and artifact writes in one flow.
- Adaptation core:
  - `adapt_combo_specs_for_bottleneck` (`scripts/tune_candidate_gate_trade_density.py:5739`) is the second major monolith (~3,434 lines).
  - Most bottleneck-family branching and adaptation policy lives here.
- Evaluation bridge:
  - `evaluate_combo` (`scripts/tune_candidate_gate_trade_density.py:13968`) shells out to `run_profitability_matrix.py` and aggregates per-combo metrics.
- Objective and selector:
  - `compute_combo_objective` (`scripts/tune_candidate_gate_trade_density.py:10499`) contains layered penalties/bonuses.
  - `select_best_row_with_selector` (`scripts/tune_candidate_gate_trade_density.py:13627`) handles selector mode resolution and final pick logic.
- Expansion families (representative):
  - `expand_oos_profitability_remediation_candidates`
  - `expand_holdout_expectancy_lift_candidates`
  - `expand_walk_forward_*` families
  - `expand_rr_adaptive_history_mixed_oos_recovery_candidates`

## Why It Is Hard To Reason About
- Multi-responsibility in `main`:
  - data quality gate, scenario generation, screening/final loops, selector, promotion, and reporting are tightly coupled.
- Hidden coupling via shared combo dict:
  - one combo object carries many unrelated feature flags and objective-only metadata.
- Objective policy drift risk:
  - layered penalties and guardrails are spread across many feature toggles and can interact nonlinearly.
- Debugging difficulty:
  - a config-source mismatch or artifact path mismatch can silently invalidate interpretation.

## Validation Parallelism Risk (Your Point)
- Confirmed risk pattern:
  - matrix evaluator writes/reads runtime config paths during validation runs.
  - if multiple validations run concurrently with shared config paths, overwrite race can occur.
- Action taken in this change set:
  - validation paths are now forced to sequential execution even when `--max-workers > 1`.
  - touched files:
    - `scripts/run_profitability_matrix.py`
    - `scripts/validate_readiness.py`
    - `scripts/validate_small_seed.py`
    - `scripts/analyze_loss_contributors.py`

## Recommended Refactor Order (Pragmatic)
1. Split `main` into explicit phases (pure functions):
   - load_context
   - generate_candidates
   - screen_candidates
   - final_evaluate
   - select_and_promote
   - emit_reports
2. Separate objective policy into independent module:
   - keep a typed `ObjectiveInput` and return `(score, penalty_breakdown)`.
3. Isolate bottleneck adaptation families:
   - one family per module, explicit input/output contract.
4. Freeze artifact schema contracts:
   - enforce typed read/write helpers for summary/gate/profile JSON/CSV.
5. Keep validation strictly sequential by default:
   - parallel mode only with isolated per-run temp config namespace.

## Immediate Guardrail
- Until phase-split refactor is done, avoid parallel validation in all verification scripts.
- Keep one-run-one-config-path discipline for reproducibility.
