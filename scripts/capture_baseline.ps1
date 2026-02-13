param(
    [string]$ExePath = ".\build\Release\AutoLifeTrading.exe",
    [string]$CuratedDataDir = ".\data\backtest_curated",
    [string]$WalkForwardInput = ".\data\backtest\simulation_2000.csv",
    [string]$LogDir = ".\build\Release\logs"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$dateTag = (Get-Date).ToString("yyyyMMdd_HHmmss")

& .\scripts\validate_small_seed.ps1 `
    -ExePath $ExePath `
    -DataDir $CuratedDataDir `
    -OutputCsv (Join-Path $LogDir "small_seed_matrix_baseline_$dateTag.csv") `
    -OutputJson (Join-Path $LogDir "small_seed_summary_baseline_$dateTag.json")

& .\scripts\validate_readiness.ps1 `
    -ExePath $ExePath `
    -DataDir $CuratedDataDir `
    -OutputCsv (Join-Path $LogDir "backtest_matrix_summary_baseline_$dateTag.csv") `
    -OutputStrategyCsv (Join-Path $LogDir "backtest_strategy_summary_baseline_$dateTag.csv") `
    -OutputProfileCsv (Join-Path $LogDir "backtest_profile_summary_baseline_$dateTag.csv") `
    -OutputJson (Join-Path $LogDir "readiness_report_baseline_$dateTag.json")

& .\scripts\walk_forward_validate.ps1 `
    -ExePath $ExePath `
    -InputCsv $WalkForwardInput `
    -OutputCsv (Join-Path $LogDir "walk_forward_windows_baseline_$dateTag.csv") `
    -OutputJson (Join-Path $LogDir "walk_forward_report_baseline_$dateTag.json")

Write-Host "Baseline capture completed. tag=$dateTag"

