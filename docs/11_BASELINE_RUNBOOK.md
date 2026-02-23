# Baseline Runbook (v1)
Last updated: 2026-02-23

## Preconditions
- Python environment is active and dependencies are installed.
- Runtime binary and Python scripts are built/available.
- `allow_live_orders=false` unless all gates and staged enable requirements are met.

## 1) Fetch training bundle
Optional first (dynamic universe for scoped 1m):
```powershell
python scripts/build_dynamic_universe.py `
  --active-size 25 `
  --buffer-size 50 `
  --membership-ttl-days 7 `
  --output-json ".\config\universe\runtime_universe.json" `
  --history-json ".\config\universe\runtime_universe_history.json"
```

Fetch:
```powershell
python scripts/fetch_probabilistic_training_bundle.py `
  --markets-major "KRW-BTC,KRW-ETH,KRW-XRP,KRW-SOL,KRW-DOGE" `
  --markets-alt "KRW-ETC,KRW-BCH,KRW-EOS,KRW-XLM,KRW-QTUM" `
  --timeframes "1,5,15,60,240" `
  --history-start-utc "2017-10-24T00:00:00Z" `
  --output-dir ".\data\backtest_probabilistic" `
  --manifest-json ".\data\backtest_probabilistic\probabilistic_bundle_manifest.json" `
  --universe-file ".\config\universe\runtime_universe.json"
```

## 2) Build feature dataset
```powershell
python scripts/build_probabilistic_feature_dataset.py `
  --input-dir ".\data\backtest_probabilistic" `
  --output-dir ".\data\model_input\probabilistic_features_v1_latest" `
  --manifest-json ".\data\model_input\probabilistic_features_v1_latest\feature_dataset_manifest.json" `
  --universe-file ".\config\universe\runtime_universe.json"
```

## 3) Strict feature validation
```powershell
python scripts/validate_probabilistic_feature_dataset.py `
  --dataset-manifest-json ".\data\model_input\probabilistic_features_v1_latest\feature_dataset_manifest.json" `
  --strict
```

## 4) Split manifest + baseline
```powershell
python scripts/generate_probabilistic_split_manifest.py `
  --input-manifest-json ".\data\model_input\probabilistic_features_v1_latest\feature_dataset_manifest.json" `
  --output-split-manifest-json ".\data\model_input\probabilistic_features_v1_latest\probabilistic_split_manifest_v1.json" `
  --dataset-kind feature

python scripts/generate_probabilistic_baseline.py `
  --split-manifest-json ".\data\model_input\probabilistic_features_v1_latest\probabilistic_split_manifest_v1.json"
```

## 5) Global train
```powershell
python scripts/train_probabilistic_pattern_model_global.py `
  --split-manifest-json ".\data\model_input\probabilistic_features_v1_latest\probabilistic_split_manifest_v1.json"
```

## 6) Export runtime bundle
```powershell
python scripts/export_probabilistic_runtime_bundle.py `
  --export-mode global_only `
  --output-json ".\config\model\probabilistic_runtime_bundle_v1.json"
```

## 7) Parity gate
```powershell
python scripts/validate_runtime_bundle_parity.py `
  --runtime-bundle-json ".\config\model\probabilistic_runtime_bundle_v1.json"
```

## 8) Verification gate
```powershell
python scripts/run_verification.py
```

## 9) Optional all-in-one cycle
```powershell
python scripts/run_probabilistic_hybrid_cycle.py `
  --universe-file ".\config\universe\runtime_universe.json"
```

## Operational notes
- Treat strict validation/parity/verification as fail-closed gates.
- Do not enable live orders if any gate fails or produces unknown state.
- Keep all run outputs (manifests/summaries/hashes) for reproducibility.
