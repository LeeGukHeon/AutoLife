# Baseline Implemented (v1)
Last updated: 2026-02-23
Status: Active baseline

## Purpose
Define the current production-relevant probabilistic pipeline behavior that must remain stable unless explicitly versioned.

## Pipeline entrypoint
- `scripts/run_probabilistic_hybrid_cycle.py`
- Ordered steps:
  1. Fetch bundle
  2. Build features
  3. Validate features (`--strict`)
  4. Generate split manifest
  5. Generate baseline metrics
  6. Train global model
  7. Export runtime bundle
  8. Validate runtime bundle parity

## Data collection baseline
- `scripts/fetch_probabilistic_training_bundle.py`
- Candle fetch worker: `scripts/fetch_upbit_historical_candles.py`
- Optional universe builder: `scripts/build_dynamic_universe.py`
- Canonical fetch manifest:
  - `data/backtest_probabilistic/probabilistic_bundle_manifest.json`
- Default timeframes:
  - Anchor: `1m`
  - Context: `5m, 15m, 60m, 240m`
- Scope behavior:
  - default: `markets_scope=all`
  - with `--universe-file`: `markets_scope=universe` and `1m` follows `final_1m_markets`

## Feature dataset baseline
- Builder: `scripts/build_probabilistic_feature_dataset.py`
- Validator: `scripts/validate_probabilistic_feature_dataset.py`
- Contract:
  - `config/model/probabilistic_feature_contract_v1.json`
- Labels:
  - `label_up_h1`
  - `label_up_h5`
  - `label_edge_bps_h5`
- Optional labels (default OFF):
  - Triple-barrier labels

## Training baseline
- Global model training:
  - `scripts/train_probabilistic_pattern_model_global.py`
- Per-market variant:
  - `scripts/train_probabilistic_pattern_model.py`
- Artifacts:
  - Joblib model files
  - Training summary JSON

## Runtime bundle baseline
- Export:
  - `scripts/export_probabilistic_runtime_bundle.py`
- Parity:
  - `scripts/validate_runtime_bundle_parity.py`
- Contract:
  - `config/model/probabilistic_runtime_bundle_v1.json`
- Export modes:
  - `global_only` (default)
  - `hybrid`
  - `per_market`

## Execution runtime baseline
- Live:
  - `src/runtime/LiveTradingRuntime.cpp`
- Backtest:
  - `src/runtime/BacktestRuntime.cpp`
- Model bridge:
  - `src/analytics/ProbabilisticRuntimeModel.cpp`
- High-level decision flow:
  1. Scan
  2. Probabilistic prefilter
  3. Enrichment
  4. Ranking
  5. Sizing
  6. Post-entry control

## Baseline invariants
- `allow_live_orders=false` by default.
- Fail-closed behavior on unknown or mismatched state.
- Reproducibility metadata (hashes/manifests/args/git sha) required per dataset/model/bundle.
- Extensions must not alter baseline behavior when disabled.
