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

## Personal Use Notice
This project is for personal use and experimentation.  
Read `docs/PERSONAL_USE_NOTICE.md` before real-money trading or paid distribution.
