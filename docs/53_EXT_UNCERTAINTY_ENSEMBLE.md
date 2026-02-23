# EXT-53: Uncertainty via Ensemble-K
Last updated: 2026-02-23
Default: OFF

## Goal
Use prediction disagreement (`prob_std`) as uncertainty and reduce position size when uncertainty is high.

## Implementation targets
- Training:
  - `scripts/train_probabilistic_pattern_model_global.py --ensemble_k K`
- Export:
  - Bundle stores `model_artifacts[]` for ensemble members
- Runtime:
  - infer `prob_mean`, `prob_std`
  - apply uncertainty-aware size multiplier

## Sizing examples
- `exp(-k*u)`
- `clamp(1 - u/u_max, 0, 1)`
- if `u > u_max`, skip or minimum size tier

## Validation
- Unit: uncertainty increases with model disagreement
- Integration: mean parity remains stable; ensemble config persisted in bundle

## DoD
- Ensemble-aware bundle export and runtime sizing are deterministic and gated.
