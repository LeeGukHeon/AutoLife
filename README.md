# AutoLife (Personal Use)

AutoLife is a personal crypto trading program with:
- live/backtest execution paths
- operational safety gates
- strict-live approval workflow
- profitability validation reports

## Quick Start
1. Build
```powershell
& "D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
& "D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe" --build build --config Release
```

2. Prepare API keys (env-only)
```powershell
Copy-Item .env.example .env
# Edit .env and set your real keys. Do not commit .env.
```

3. Set API keys in current shell (environment variables)
```powershell
$env:UPBIT_ACCESS_KEY="YOUR_ACCESS_KEY"
$env:UPBIT_SECRET_KEY="YOUR_SECRET_KEY"
```
or load from `.env`:
```powershell
Get-Content .env | Where-Object { $_ -match '^[A-Za-z_][A-Za-z0-9_]*=' } | ForEach-Object { $k, $v = $_ -split '=', 2; Set-Item -Path ("Env:" + $k) -Value $v }
```

4. Run tests
```powershell
.\build\Release\AutoLifeExecutionStateTest.exe
.\build\Release\AutoLifeTest.exe
.\build\Release\AutoLifeStateTest.exe
.\build\Release\AutoLifeEventJournalTest.exe
```

## Presets
Preset files:
- `config/presets/safe.json`
- `config/presets/active.json`

Apply a preset:
```powershell
python scripts\apply_trading_preset.py --preset safe
```

## Operational Validation
PR-style gate:
```powershell
python scripts\run_ci_operational_gate.py -IncludeBacktest
```

Strict live gate:
```powershell
python scripts\run_ci_operational_gate.py -IncludeBacktest -RunLiveProbe -StrictExecutionParity
```

Strict live trend/alert/tuning/action:
```powershell
python scripts\generate_strict_live_gate_trend_alert.py -GateProfile strict_live -ApplyTunedThresholds -ActionExecutionPolicy safe-auto-execute -EnableActionFeedbackLoop
```

## Profitability Report (Exploratory, Non-blocking)
```powershell
python scripts\run_profitability_exploratory.py
```

Outputs:
- `build/Release/logs/profitability_gate_report_exploratory.json`
- `build/Release/logs/profitability_profile_summary_exploratory.csv`
- `build/Release/logs/profitability_matrix_exploratory.csv`

## Verification Baseline (Reset, Recommended)
Use this as the primary verification entrypoint during reset:

```powershell
python scripts\verify_baseline.py --data-mode fixed --validation-profile adaptive
```

Compatibility check (legacy threshold gate only):
```powershell
python scripts\verify_baseline.py --data-mode fixed --validation-profile legacy_gate
```

Data mode policy:
- `fixed` (default): deterministic baseline for gate decisions, no fetch.
- `refresh_if_missing`: fetch from Upbit only when the dataset file is missing.
- `refresh_force`: always fetch from Upbit on every run.
- Gate decisions must be based on `fixed`; `refresh_*` is robustness-only.

Examples:
```powershell
# Gate baseline (recommended)
python scripts\verify_baseline.py --data-mode fixed --validation-profile adaptive

# Real-data robustness check (fetch only if missing)
python scripts\verify_baseline.py --realdata-only --datasets upbit_KRW_BTC_1m_12000.csv --data-mode refresh_if_missing --validation-profile adaptive
```

Refresh naming contract:
- `refresh_*` requires dataset filename format:
  - `upbit_<QUOTE>_<BASE>_<UNIT>m_<CANDLES>.csv`

Outputs:
- `build/Release/logs/verification_report.json`
- `build/Release/logs/verification_matrix.csv`

## Live MTF Dataset Capture
During `LIVE` mode scans, the engine can accumulate multi-timeframe datasets for later backtest/tuning:
- output dir default: `data/backtest_real_live`
- filename pattern: `upbit_<MARKET>_<TF>_live.csv` (e.g., `upbit_KRW_BTC_1m_live.csv`)
- TF defaults: `1m`, `5m`, `15m`, `1h(=60m)`, `4h(=240m)`, `1d`

Main config keys (`config/config.json -> trading`):
- `enable_live_mtf_dataset_capture`
- `live_mtf_dataset_capture_interval_seconds`
- `live_mtf_dataset_capture_output_dir`
- `live_mtf_dataset_capture_timeframes`

## Strategy Runtime Mode (Foundation-Only)
Runtime execution path is currently hard-switched to foundation-only strategy:

- registered at runtime:
  - `Foundation Adaptive Strategy`
- disconnected from runtime registration:
  - legacy strategy pack (`scalping`, `momentum`, `breakout`, `mean_reversion`, `grid_trading`)
- compile status:
  - legacy strategy source units are excluded from CMake targets and removed from active tree

Current config remains:

```json
"trading": {
  "enabled_strategies": ["foundation_adaptive"]
}
```

`enabled_strategies` is retained for compatibility, but runtime registration is fixed to foundation-only in this rebuild phase.

## Verification Flow (Current)
Use the streamlined probabilistic verification path:

```powershell
python scripts\run_verification.py --exe-path .\build\Release\AutoLifeTrading.exe --data-dir .\data\backtest_real --dataset-names upbit_KRW_BTC_1m_12000.csv upbit_KRW_ETH_1m_12000.csv --require-higher-tf-companions
```

Baseline snapshot generation:

```powershell
python scripts\generate_probabilistic_baseline.py
```

## Personal Use Notice
This project is for personal use and experimentation.  
Read `docs/PERSONAL_USE_NOTICE.md` before real-money trading or paid distribution.

