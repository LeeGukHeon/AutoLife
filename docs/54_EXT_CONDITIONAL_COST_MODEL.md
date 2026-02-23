# EXT-54: Conditional Cost Model
Last updated: 2026-02-23
Default: OFF

## Goal
Replace fixed cost assumption with a candle-conditioned variable cost estimate.

## Inputs
- rolling volatility
- traded value (`close * volume`)
- range/close proxy

## Output
- `cost_bps_estimate >= fee_floor`

## Integration
- Labeling side:
  - `label_edge_bps_h5 = future_ret_bps - cost_bps_estimate`
- Runtime side:
  - `expected_edge_bps = edge_profile - cost_bps_estimate`

## Current implementation entrypoints
- Python cost function module:
  - `scripts/probabilistic_cost_model.py`
- Feature builder flags:
  - `--enable-conditional-cost-model`
  - `--cost-fee-floor-bps`
  - `--cost-volatility-weight`
  - `--cost-range-weight`
  - `--cost-liquidity-weight`
  - `--cost-volatility-norm-bps`
  - `--cost-range-norm-bps`
  - `--cost-liquidity-ref-ratio`
  - `--cost-liquidity-penalty-cap`
  - `--cost-cap-bps`
- Runtime inference:
  - `src/analytics/ProbabilisticRuntimeModel.cpp` reads `cost_model` from runtime bundle.

## Isomorphism requirement
- Python and C++ must share the same functional form and parameters.
- Parameters must be written into both training summary and runtime bundle.

## Validation
- Unit monotonic tests:
  - vol up => cost up
  - liquidity down => cost up
- Integration consistency between training/runtime outputs

## DoD
- Cost model is mirrored, reproducible, and parity-safe.
