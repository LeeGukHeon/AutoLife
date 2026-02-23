# EXT-53: Uncertainty via Ensemble-K
Last updated: 2026-02-23
Default: OFF

## Goal
Use prediction disagreement (`prob_std`) as uncertainty and reduce position size when uncertainty is high.

## Implementation targets
- Training:
  - `scripts/train_probabilistic_pattern_model_global.py --ensemble-k K`
- Export:
  - Bundle stores `model_artifacts[]` for ensemble members
- Runtime:
  - infer `prob_mean`, `prob_std`
  - apply uncertainty-aware size multiplier

## Current implementation entrypoints
- Training:
  - `scripts/train_probabilistic_pattern_model_global.py`
  - flags:
    - `--ensemble-k`
    - `--ensemble-seed-step`
- Export:
  - `scripts/export_probabilistic_runtime_bundle.py`
  - runtime bundle includes:
    - `default_model.ensemble_members[]`
    - `default_model.ensemble_model_artifacts[]`
    - top-level `ensemble` metadata
- Runtime inference/model:
  - `include/analytics/ProbabilisticRuntimeModel.h`
  - `src/analytics/ProbabilisticRuntimeModel.cpp`
  - exposes:
    - `prob_h5_mean`
    - `prob_h5_std`
    - `ensemble_member_count`
- Runtime sizing controls (config):
  - `probabilistic_uncertainty_ensemble_enabled`
  - `probabilistic_uncertainty_size_mode` (`linear|exp`)
  - `probabilistic_uncertainty_u_max`
  - `probabilistic_uncertainty_exp_k`
  - `probabilistic_uncertainty_min_scale`
  - `probabilistic_uncertainty_skip_when_high`
  - `probabilistic_uncertainty_skip_u`
- Unit tests:
  - `scripts/test_probabilistic_ensemble_uncertainty.py`

## Sizing examples
- `exp(-k*u)`
- `clamp(1 - u/u_max, 0, 1)`
- if `u > u_max`, skip or minimum size tier

## Validation
- Unit: uncertainty increases with model disagreement
- Integration: mean parity remains stable; ensemble config persisted in bundle

## DoD
- Ensemble-aware bundle export and runtime sizing are deterministic and gated.
