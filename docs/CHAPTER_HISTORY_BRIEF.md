# Chapter History (Brief)

This file stays short.
Detailed logs are archived.

## Entries
- 2026-02-19 | `CH-00` | Reset baseline and moved oversized logs to archive.
- 2026-02-19 | `CH-01` started | Main runtime split + legacy cleanup + validation baseline normalization.
- 2026-02-20 | `CH-01` | Probabilistic-hybrid transition activated.
  - Added sequential bundle collector: `scripts/fetch_probabilistic_training_bundle.py`
  - Added split generator: `scripts/generate_probabilistic_split_manifest.py`
  - Added feature contract seed: `config/model/probabilistic_feature_contract_v1.json`
- 2026-02-20 | `CH-01` | Data foundation expansion.
  - `60m/240m` completed (EOS 404 excluded)
  - `5m/15m` completed
  - `1m recent-2y` completed for MTF set (`1/5/15/60/240`)
- 2026-02-20 | `CH-01` | Fetch stability hardening.
  - Manifest immediate flush (`progress_pct`, `current_job`, `run_state`)
  - Candle fetch stagnant-loop break added to prevent excessive repeated paging
- 2026-02-20 | `CH-01` | Feature build pipeline bootstrapped.
  - Added `scripts/build_probabilistic_feature_dataset.py` (1m anchor + 5/15/60/240 context + leakage-safe labels).
  - Completed full v1 feature dataset build on mixed major/alt universe for regime generalization.
  - Final scale: 9 markets, 9,477,036 rows, about 3.48 GB output.
- 2026-02-20 | `CH-01` | Data source contract fixed.
  - Upbit API inputs are limited to raw OHLCV candles.
  - Indicators/labels/features are generated locally and shared by live/backtest logic.
- 2026-02-20 | `CH-01` | Full feature quality validation completed.
  - Added `scripts/validate_probabilistic_feature_dataset.py`.
  - Result: pass (9/9 files, 9,477,036 rows, leakage mismatch 0).
- 2026-02-20 | `CH-01` | Contract/split freeze completed on final feature dataset.
  - `config/model/probabilistic_feature_contract_v1.json` frozen with explicit 43-column schema.
  - Generated feature-based walk-forward split manifest:
    `data/model_input/probabilistic_features_v1_full_20260220_181345/probabilistic_split_manifest_v1.json`.
- 2026-02-20 | `CH-01` | Baseline regenerated on frozen feature split.
  - Added `scripts/generate_probabilistic_baseline.py`.
  - Weighted test snapshot: `h5_logloss=0.6743`, `h5_brier=0.2406`, `h5_accuracy=0.6025`, `h5_mean_edge_bps=-12.1554`.
- 2026-02-20 | `CH-01` | Initial probabilistic model training completed.
  - Added `scripts/train_probabilistic_pattern_model.py` (streaming SGD + Platt calibration).
  - Full run: 9/9 datasets pass.
  - Weighted test: `h5_logloss=0.6606`, `h5_brier=0.2342`, `h5_accuracy=0.6025`.
  - Selected-edge (`~11.05%` coverage): `-10.5596 bps`.
- 2026-02-20 | `CH-01` | Walk-forward decomposition + inference parity completed.
  - Added `scripts/generate_probabilistic_walkforward_report.py`.
  - Added `scripts/validate_probabilistic_inference_parity.py`.
  - Parity result: pass (`worst_cal_diff=1.721e-15`, tol `1e-9`).
- 2026-02-20 | `CH-01` | Runtime bundle prepared for C++ integration.
  - Added `scripts/export_probabilistic_runtime_bundle.py`.
  - Added `scripts/validate_runtime_bundle_parity.py`.
  - Bundle parity result: pass (`worst_diff=1.110e-16`, tol `1e-9`).
- 2026-02-20 | `CH-01` | Runtime wiring phase started.
  - Added `src/analytics/ProbabilisticRuntimeFeatures.cpp` (Python contract-aligned transform).
  - Live/backtest runtime signal path connected to probabilistic runtime bundle.
  - Build path fixed in active docs:
    `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe`.
- 2026-02-20 | `CH-01` | Core-rescue structural cleanup + bottleneck shift confirmed.
  - `StrategyManager` core-rescue path changed from duplicate `shouldEnter` recheck to `reason-eligible + safety-floor` rescue.
  - `FoundationAdaptiveStrategy` entry gate logic unified into one snapshot path (`generateSignal`/`shouldEnter` parity).
  - Regression: `no_signal_generated_share 0.977 -> 0.5687`, while bottleneck moved to manager prefilter (`strength/ev`).
- 2026-02-20 | `CH-01` | Probabilistic-primary runtime refactor applied.
  - Scan stage now has market-level probabilistic prefilter (`probabilistic_market_prefilter`).
  - Signal stage applies probabilistic snapshot as primary overlay (score/strength/filter).
  - Entry sizing now scales by probabilistic margin (`position_size` dynamic scale).
  - New config knobs added for primary mode and scan prefilter tuning.
- 2026-02-20 | `CH-01` | Probabilistic-primary structural follow-up v1.
  - Experiment env paths moved to config-driven toggles (`signal_supply_fallback`, `manager_soft_queue`).
  - Added low-liquidity ranging probe path and wired fallback signal into runtime relief path.
  - Added probabilistic near-miss edge relief in both backtest/live entry-quality heads.
  - Result: candidate supply improved (`no_signal` share down, primary conversion up), but profitability unchanged and bottleneck shifted to `entry_quality_edge_base`.
- 2026-02-20 | `CH-01` | Probabilistic-primary hardening v2.
  - Added manager fast-pass for high-confidence probabilistic signals (`manager_probabilistic_primary_fastpass`).
  - Scaled manager penalty in `FoundationRiskPipeline` by probabilistic confidence instead of fixed stacking.
  - Switched live/backtest probabilistic snapshot inference to strict primary mode (fail-open -> fail-closed).
  - Added explicit reject taxonomy for bundle/market/feature/inference failures.

## Archive Pointer
- archive payload cleaned (legacy full-log docs removed)
