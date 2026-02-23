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

# EXT-54 optional (default OFF)
python scripts/build_probabilistic_feature_dataset.py `
  --input-dir ".\data\backtest_probabilistic" `
  --output-dir ".\data\model_input\probabilistic_features_v1_latest" `
  --manifest-json ".\data\model_input\probabilistic_features_v1_latest\feature_dataset_manifest.json" `
  --universe-file ".\config\universe\runtime_universe.json" `
  --enable-conditional-cost-model `
  --cost-fee-floor-bps 6.0 `
  --cost-volatility-weight 3.0 `
  --cost-range-weight 1.5 `
  --cost-liquidity-weight 2.5 `
  --cost-volatility-norm-bps 50.0 `
  --cost-range-norm-bps 80.0 `
  --cost-liquidity-ref-ratio 1.0 `
  --cost-liquidity-penalty-cap 8.0 `
  --cost-cap-bps 200.0

# EXT-52 optional (default OFF)
python scripts/build_probabilistic_feature_dataset.py `
  --input-dir ".\data\backtest_probabilistic" `
  --output-dir ".\data\model_input\probabilistic_features_v1_latest" `
  --manifest-json ".\data\model_input\probabilistic_features_v1_latest\feature_dataset_manifest.json" `
  --universe-file ".\config\universe\runtime_universe.json" `
  --sample-mode volatility `
  --sample-threshold 0.0015 `
  --sample-lookback-minutes 60
```

## 3) Strict feature validation
```powershell
python scripts/validate_probabilistic_feature_dataset.py `
  --dataset-manifest-json ".\data\model_input\probabilistic_features_v1_latest\feature_dataset_manifest.json" `
  --contract-json ".\config\model\probabilistic_feature_contract_v1.json" `
  --pipeline-version v1 `
  --strict

# Optional MODE B / v2 strict gate
python scripts/validate_probabilistic_feature_dataset.py `
  --dataset-manifest-json ".\data\model_input\probabilistic_features_v2_draft_latest\feature_dataset_manifest.json" `
  --contract-json ".\config\model\probabilistic_feature_contract_v2.json" `
  --pipeline-version v2 `
  --strict
```

## 4) Split manifest + baseline
```powershell
python scripts/generate_probabilistic_split_manifest.py `
  --input-manifest-json ".\data\model_input\probabilistic_features_v1_latest\feature_dataset_manifest.json" `
  --output-split-manifest-json ".\data\model_input\probabilistic_features_v1_latest\probabilistic_split_manifest_v1.json" `
  --dataset-kind feature

# EXT-51 optional (default OFF)
python scripts/generate_probabilistic_split_manifest.py `
  --input-manifest-json ".\data\model_input\probabilistic_features_v1_latest\feature_dataset_manifest.json" `
  --output-split-manifest-json ".\data\model_input\probabilistic_features_v1_latest\probabilistic_split_manifest_v1.json" `
  --dataset-kind feature `
  --enable-purged-walk-forward `
  --h1-bars 1 `
  --h5-bars 5 `
  --purge-bars -1 `
  --embargo-bars -1 `
  --split-plan-json ".\data\model_input\probabilistic_features_v1_latest\split_plan.json"

python scripts/generate_probabilistic_baseline.py `
  --split-manifest-json ".\data\model_input\probabilistic_features_v1_latest\probabilistic_split_manifest_v1.json"
```

## 5) Global train
```powershell
python scripts/train_probabilistic_pattern_model_global.py `
  --split-manifest-json ".\data\model_input\probabilistic_features_v1_latest\probabilistic_split_manifest_v1.json"

# EXT-53 optional (default OFF)
python scripts/train_probabilistic_pattern_model_global.py `
  --split-manifest-json ".\data\model_input\probabilistic_features_v1_latest\probabilistic_split_manifest_v1.json" `
  --ensemble-k 5 `
  --ensemble-seed-step 1000
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

# Optional MODE B / v2 strict gate
python scripts/run_verification.py `
  --pipeline-version v2
```

## 9) Optional all-in-one cycle
```powershell
python scripts/run_probabilistic_hybrid_cycle.py `
  --universe-file ".\config\universe\runtime_universe.json"

# Hybrid + EXT-52 optional
python scripts/run_probabilistic_hybrid_cycle.py `
  --universe-file ".\config\universe\runtime_universe.json" `
  --sample-mode dollar `
  --sample-threshold 250000000 `
  --sample-lookback-minutes 60

# Hybrid + verification gate (optional)
python scripts/run_probabilistic_hybrid_cycle.py `
  --universe-file ".\config\universe\runtime_universe.json" `
  --run-verification `
  --verification-datasets "upbit_KRW_BTC_1m_2024.csv,upbit_KRW_ETH_1m_2024.csv"

# Optional MODE B draft path (explicit v2)
python scripts/run_probabilistic_hybrid_cycle.py `
  --pipeline-version v2 `
  --universe-file ".\config\universe\runtime_universe.json" `
  --run-verification `
  --verification-datasets "upbit_KRW_BTC_1m_2024.csv,upbit_KRW_ETH_1m_2024.csv"

# Optional: promotion readiness evaluation (prelive)
python scripts/run_probabilistic_hybrid_cycle.py `
  --pipeline-version v2 `
  --universe-file ".\config\universe\runtime_universe.json" `
  --run-verification `
  --verification-datasets "upbit_KRW_BTC_1m_2024.csv,upbit_KRW_ETH_1m_2024.csv" `
  --evaluate-promotion-readiness `
  --promotion-target-stage prelive

# Optional: promotion readiness evaluation (live enable; shadow report required)
# Step 1) Generate shadow report from live/backtest decision logs
# Note: live runtime resets `logs/policy_decisions.jsonl` at engine start.
python scripts/generate_probabilistic_shadow_report.py `
  --live-decision-log-jsonl ".\build\Release\logs\policy_decisions.jsonl" `
  --backtest-decision-log-jsonl ".\build\Release\logs\policy_decisions_backtest.jsonl" `
  --runtime-bundle-json ".\config\model\probabilistic_runtime_bundle_v2.json" `
  --pipeline-version v2 `
  --output-json ".\build\Release\logs\probabilistic_shadow_report_latest.json" `
  --strict

# Step 2) Validate shadow report (recommended)
python scripts/validate_probabilistic_shadow_report.py `
  --shadow-report-json ".\build\Release\logs\probabilistic_shadow_report_latest.json" `
  --pipeline-version v2 `
  --output-json ".\build\Release\logs\probabilistic_shadow_report_validation_latest.json" `
  --strict

# Optional: run full cycle with integrated shadow report generation + validation + promotion readiness
python scripts/run_probabilistic_hybrid_cycle.py `
  --pipeline-version v2 `
  --universe-file ".\config\universe\runtime_universe.json" `
  --run-verification `
  --verification-datasets "upbit_KRW_BTC_1m_2024.csv,upbit_KRW_ETH_1m_2024.csv" `
  --evaluate-promotion-readiness `
  --promotion-target-stage live_enable `
  --generate-shadow-report `
  --validate-shadow-report

# Optional: standalone Gate4 flow runner (existing artifacts + decision logs)
# Note: when canonical default gate files are absent, the runner auto-resolves
# latest run-tagged artifacts for feature/parity/verification and decision logs.
python scripts/run_probabilistic_shadow_gate_flow.py `
  --pipeline-version v2 `
  --target-stage live_enable `
  --runtime-bundle-json ".\config\model\probabilistic_runtime_bundle_v2.json" `
  --live-decision-log-jsonl ".\build\Release\logs\policy_decisions.jsonl" `
  --backtest-decision-log-jsonl ".\build\Release\logs\policy_decisions_backtest.jsonl" `
  --feature-validation-json ".\build\Release\logs\probabilistic_feature_validation_summary.json" `
  --parity-json ".\build\Release\logs\probabilistic_runtime_bundle_parity.json" `
  --verification-json ".\build\Release\logs\verification_report.json" `
  --runtime-config-json ".\build\Release\config\config.json"
```

## 10) EXT-55 optional runtime regime policy (default OFF)
Set in trading config JSON (used by live/backtest runtime):
```json
{
  "probabilistic_uncertainty_ensemble_enabled": true,
  "probabilistic_uncertainty_size_mode": "linear",
  "probabilistic_uncertainty_u_max": 0.06,
  "probabilistic_uncertainty_exp_k": 8.0,
  "probabilistic_uncertainty_min_scale": 0.10,
  "probabilistic_uncertainty_skip_when_high": false,
  "probabilistic_uncertainty_skip_u": 0.12,
  "probabilistic_regime_spec_enabled": true,
  "probabilistic_regime_volatility_window": 48,
  "probabilistic_regime_drawdown_window": 36,
  "probabilistic_regime_volatile_zscore": 1.2,
  "probabilistic_regime_hostile_zscore": 2.0,
  "probabilistic_regime_volatile_drawdown_speed_bps": 3.0,
  "probabilistic_regime_hostile_drawdown_speed_bps": 8.0,
  "probabilistic_regime_hostile_block_new_entries": true,
  "probabilistic_regime_volatile_threshold_add": 0.01,
  "probabilistic_regime_hostile_threshold_add": 0.03,
  "probabilistic_regime_volatile_size_multiplier": 0.5,
  "probabilistic_regime_hostile_size_multiplier": 0.2
}
```

## Operational notes
- Treat strict validation/parity/verification as fail-closed gates.
- Do not enable live orders if any gate fails or produces unknown state.
- Keep all run outputs (manifests/summaries/hashes) for reproducibility.
