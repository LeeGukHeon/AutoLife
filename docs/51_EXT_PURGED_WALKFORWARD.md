# EXT-51: Purged Walk-forward + Embargo
Last updated: 2026-02-23
Default: OFF

## Problem
Horizon labels overlap in time, so naive walk-forward may leak information and inflate evaluation.

## Implementation target
- Split tool:
  - `scripts/split_probabilistic_dataset.py`
  - implemented via `scripts/generate_probabilistic_split_manifest.py`
- Parameters:
  - `--h1_bars`
  - `--h5_bars`
  - `--purge_bars` (default `max(h1,h5)`)
  - `--embargo_bars` (default `ceil(purge*0.1)`)
- Enable flag:
  - `--enable-purged-walk-forward`

## Output
- `split_plan.json` with exact time ranges
- removed sample counts due to purge/embargo

## Validation
- Unit: overlap detection correctness
- Integration: training summary captures purge/embargo settings

## DoD
- OFF: baseline split unchanged
- ON: no overlap between train and eval label windows
