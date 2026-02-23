# EXT-52: Event Sampling
Last updated: 2026-02-23
Default: OFF

## Goal
Keep existing candle cache and feature contract while selecting more informative training timestamps.

## Implementation target
- `scripts/build_probabilistic_feature_dataset.py`
- New options:
  - `--sample-mode|--sample_mode time|dollar|volatility` (default `time`)
  - `--sample-threshold|--sample_threshold`
  - `--sample-lookback-minutes|--sample_lookback_minutes`

## Placement
- Compute full features first.
- Apply row selection after feature computation to keep schema stable.

## Current behavior
- `time`:
  - baseline mode; keep every computed training row.
- `dollar`:
  - build rolling sum of `close * volume` over `sample_lookback_minutes`.
  - keep row when rolling notional `>= sample_threshold`.
- `volatility`:
  - build rolling std of 1m returns over `sample_lookback_minutes`.
  - keep row when rolling volatility `>= sample_threshold`.
- warmup:
  - for `dollar/volatility`, rows before lookback window is full are dropped.
- safety:
  - `dollar/volatility` require `sample_threshold > 0`, otherwise fail-fast.

## Logging / reproducibility
- Feature manifest stores:
  - global config: `sample_mode`, `sample_threshold`, `sample_lookback_minutes`
  - totals: considered/selected/dropped rows and selection ratio
  - per-market sampling stats in `jobs[]`
- Hybrid pipeline passthrough:
  - `scripts/run_probabilistic_hybrid_cycle.py` supports same sampling flags and records them in cycle summary.

## Tests
- `scripts/test_probabilistic_event_sampling.py`
  - deterministic decisions for repeated runs
  - threshold-density relation checks
  - mode normalization checks

## Validation
- OFF: identical row set as baseline
- ON: deterministic sampled rows with logged parameters

## DoD
- Training-only sampling works reproducibly without changing baseline schema.
