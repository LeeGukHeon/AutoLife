# Strict Parity Fix Plan (A16-2)

## Scope
- Target check: `cli_strict_parity_pass_flag`
- Current mismatch: expected=`true`, observed=`false` (`mismatch_count=1`)
- Promotion behavior after A15:
  - `strict_parity_fail=false`
  - `strict_parity_warning=true`
  - `strict_parity_hold=true`
  - Action stays `hold_for_strict_parity` (Paper allowed, Live blocked)

## Root Cause Hypothesis
- The strict parity gate currently depends on a CLI flag (`--strict-parity-pass`) as observed input.
- Promotion runs do not pass that CLI flag in normal automation, so `observed_value` remains `false`.
- This creates a stable procedural hold unrelated to strategy quality.

## Single-Fix Track (no strategy tuning)
1. Move strict parity observed source from CLI-only to runtime evidence:
   - Prefer a persisted parity evidence field from verification/runtime artifacts.
   - Keep CLI flag as explicit override only.
2. Keep policy mode as `warning_hold` until parity evidence is wired:
   - Continue allowing Paper.
   - Continue blocking Live expansion.
3. After evidence wiring, re-run promotion gate:
   - expected and observed parity values must come from the same evidence family.
   - mismatch must be measurable and reproducible.

## Re-Validation Procedure
1. Run promotion gate with unchanged bundle:
   - `config/model/probabilistic_runtime_bundle_v2_a15_strict_parity_warning_hold_step1.json`
2. Save diagnostics:
   - `build/Release/logs/strict_parity_diagnosis_after_fix.json`
3. Acceptance checks:
   - `mismatch_count == 0` OR mismatch justified by real parity evidence
   - If mismatch resolved:
     - `strict_parity_hold=false`
     - promotion state may move from `S1` to `S2` (subject to other gates)
   - If mismatch persists:
     - keep `hold_for_strict_parity`; do not allow Live promotion

## Safety Rule
- Strict parity unresolved => Live promotion blocked by policy.
- Paper run remains allowed for evidence collection and operational validation.
