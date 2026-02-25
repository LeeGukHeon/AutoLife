# EXT-56: Phase 3 Frontier + EV Calibration + Cost Tail + Diagnostics v2
Last updated: 2026-02-25
Default state: OFF (all phase3 flags)

## Goal
- Upgrade CH-01 Phase 2 gate stack to a Phase 3 policy layer:
  - frontier-coupled decision threshold
  - EV calibration layer
  - entry/exit/tail-aware cost mode
  - adaptive EV blend
  - diagnostics v2 bottleneck decomposition

## Non-negotiables
- Keep live/backtest/runtime-manager/foundation-risk parity.
- Keep deterministic feature and inference rules.
- Keep baseline behavior unchanged when phase3 flags are OFF.
- New policy paths must be diagnosable with explicit counters.

## Decision Frontier Policy
- Definitions:
  - `margin = P_calibrated - selection_threshold`
  - `base_required_ev = f_context(...)` (existing dynamic EV requirement logic)
  - `required_ev = clamp(base_required_ev - k_margin * margin + k_uncertainty * uncertainty_term + k_cost_tail * cost_tail_term, min_required_ev, max_required_ev)`
  - `frontier_pass = (expected_edge_after_cost >= required_ev)`
- Notes:
  - Existing strength/regime/operational safety gates are preserved.
  - In phase3 frontier ON mode, EV pass semantics are aligned to `frontier_pass`.
  - OFF mode must preserve legacy pass/fail outputs.

## EV Calibration Layer
- Runtime order of operations:
  1. raw edge (regressor or fallback profile)
  2. optional raw clip (legacy-compatible)
  3. EV calibration (bias/monotonic mapping)
  4. cost deduction
  5. `expected_edge_pct` is final after-cost value
- Minimum capabilities:
  - bucketed bias correction (`slope/intercept`)
  - optional monotonic table mapping
  - `ev_confidence` (bucket sample support / out-of-range confidence)
- Fallback:
  - missing calibration block -> identity mapping (no behavior change)

## Cost Model Extension
- Add entry and exit estimates independently.
- Add tail-safe estimate (`p80/p90`-style conservative proxy).
- Runtime mode:
  - `mean_mode`
  - `tail_mode`
  - `hybrid_mode = mean + lambda * (tail - mean)`
- Diagnostics surface:
  - `entry_cost_estimate`
  - `exit_cost_estimate`
  - `tail_cost_estimate`
  - active cost mode

## Adaptive EV Blend
- Replace fixed blend with context policy:
  - `ev_blend = g(regime, volatility, liquidity, hostility, prob_confidence, ev_confidence, cost_state)`
- Clamp to bundle-provided `[ev_blend_min, ev_blend_max]`.
- Fallback:
  - no phase3 adaptive policy -> legacy `0.20`.

## Diagnostics v2
- Required counters (or equivalent):
  - `reject_margin_insufficient`
  - `reject_strength_fail`
  - `reject_expected_value_fail`
  - `reject_frontier_fail`
  - `reject_ev_confidence_low`
  - `reject_cost_tail_fail`
  - `pass_total`
  - `candidate_total`
- Required pass-rate slices:
  - by regime
  - by volatility bucket
  - by liquidity bucket
  - edge regressor present vs fallback
- Verification report must include top-3 bottleneck summary.

## Runtime Bundle: phase3 block
- Add `phase3` object (all OFF by default):
  - `phase3.frontier`
  - `phase3.ev_calibration`
  - `phase3.cost_model`
  - `phase3.adaptive_ev_blend`
  - `phase3.diagnostics_v2`
- Compatibility:
  - missing `phase3` -> strict fallback to legacy behavior.

## Feature Flags
- `phase3_frontier_enabled`
- `phase3_ev_calibration_enabled`
- `phase3_cost_tail_enabled`
- `phase3_adaptive_ev_blend_enabled`
- `phase3_diagnostics_v2_enabled`
- All defaults: `false`

## Validation
- Phase3 OFF:
  - operational gate pass
  - strict execution parity pass
  - no meaningful metric drift vs baseline
- Phase3 ON:
  - parity still pass
  - diagnostics v2 present
  - frontier coupling observable (margin-dependent required_ev)
  - EV calibration / cost mode / adaptive blend ON-OFF comparisons available

## Phase 4 Mention (scope only)
- Candidate scope:
  - portfolio-level capital allocator
  - correlation-aware exposure constraints
  - Kelly-capped sizing
  - multi-market allocator
- Not implemented in EXT-56 scope.
