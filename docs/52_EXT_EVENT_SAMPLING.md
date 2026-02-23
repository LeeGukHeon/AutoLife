# EXT-52: Event Sampling
Last updated: 2026-02-23
Default: OFF

## Goal
Keep existing candle cache and feature contract while selecting more informative training timestamps.

## Implementation target
- `scripts/build_probabilistic_feature_dataset.py`
- New options:
  - `--sample_mode time|dollar|volatility` (default `time`)
  - `--sample_threshold`
  - `--sample_lookback_minutes`

## Placement
- Compute full features first.
- Apply row selection after feature computation to keep schema stable.

## Validation
- OFF: identical row set as baseline
- ON: deterministic sampled rows with logged parameters

## DoD
- Training-only sampling works reproducibly without changing baseline schema.
