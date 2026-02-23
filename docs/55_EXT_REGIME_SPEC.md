# EXT-55: Regime Specification Hardening
Last updated: 2026-02-23
Default: OFF

## Goal
Make regime detection and hostile-market actions explicit, testable, and reproducible.

## States
- `normal`
- `volatile`
- `hostile`

## Detection signals (no lookahead)
- rolling volatility z-score
- drawdown speed
- optional correlation shock versus BTC

## Actions
- `volatile`:
  - higher threshold
  - size multiplier `0.5`
- `hostile`:
  - no new entries, or much higher threshold
  - size multiplier `0.2`

## Implementation
- C++ `RegimeDetector` module
- Optional mirrored Python logic for labeling/research

## Validation
- Unit: deterministic transitions from fixed input streams
- Integration: identical regime stream in live/backtest for same candles

## DoD
- Regime states and actions are deterministic, logged, and reproducible.
