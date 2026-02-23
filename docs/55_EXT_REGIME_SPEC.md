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

## Current implementation entrypoints
- Shared regime policy module (Live/Backtest common):
  - `include/common/ProbabilisticRegimeSpec.h`
  - `src/common/ProbabilisticRegimeSpec.cpp`
- Runtime integration:
  - `src/runtime/LiveTradingRuntime.cpp`
  - `src/runtime/BacktestRuntime.cpp`
- Engine config keys (all default OFF-safe):
  - `probabilistic_regime_spec_enabled`
  - `probabilistic_regime_volatility_window`
  - `probabilistic_regime_drawdown_window`
  - `probabilistic_regime_volatile_zscore`
  - `probabilistic_regime_hostile_zscore`
  - `probabilistic_regime_volatile_drawdown_speed_bps`
  - `probabilistic_regime_hostile_drawdown_speed_bps`
  - `probabilistic_regime_enable_btc_correlation_shock`
  - `probabilistic_regime_correlation_window`
  - `probabilistic_regime_correlation_shock_threshold`
  - `probabilistic_regime_hostile_block_new_entries`
  - `probabilistic_regime_volatile_threshold_add`
  - `probabilistic_regime_hostile_threshold_add`
  - `probabilistic_regime_volatile_size_multiplier`
  - `probabilistic_regime_hostile_size_multiplier`
- Python mirror + deterministic tests:
  - `scripts/probabilistic_regime_spec.py`
  - `scripts/test_probabilistic_regime_spec.py`

## Validation
- Unit: deterministic transitions from fixed input streams
- Integration: identical regime stream in live/backtest for same candles

## DoD
- Regime states and actions are deterministic, logged, and reproducible.
