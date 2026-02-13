param(
    [string]$DataDir = ".\data\backtest",
    [string]$CuratedDataDir = ".\data\backtest_curated",
    [switch]$IncludeWalkForward
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-DatasetList {
    param([string]$DirPath)
    if (-not (Test-Path $DirPath)) {
        return @()
    }
    return @(
        Get-ChildItem -Path $DirPath -File -Filter *.csv -ErrorAction SilentlyContinue |
            Sort-Object Name |
            ForEach-Object { $_.FullName }
    )
}

$datasets = @()
$datasets += Get-DatasetList -DirPath $DataDir
$datasets += Get-DatasetList -DirPath $CuratedDataDir

if (@($datasets).Count -eq 0) {
    throw "No datasets found for exploratory profitability run."
}

if ($IncludeWalkForward.IsPresent) {
    & .\scripts\run_profitability_matrix.ps1 `
        -DatasetNames $datasets `
        -ExcludeLowTradeRunsForGate `
        -MinTradesPerRunForGate 1 `
        -MinProfitFactor 0.25 `
        -MinAvgTrades 1 `
        -MinProfitableRatio 0.70 `
        -OutputCsv ".\build\Release\logs\profitability_matrix_exploratory.csv" `
        -OutputProfileCsv ".\build\Release\logs\profitability_profile_summary_exploratory.csv" `
        -OutputJson ".\build\Release\logs\profitability_gate_report_exploratory.json" `
        -IncludeWalkForward
} else {
    & .\scripts\run_profitability_matrix.ps1 `
        -DatasetNames $datasets `
        -ExcludeLowTradeRunsForGate `
        -MinTradesPerRunForGate 1 `
        -MinProfitFactor 0.25 `
        -MinAvgTrades 1 `
        -MinProfitableRatio 0.70 `
        -OutputCsv ".\build\Release\logs\profitability_matrix_exploratory.csv" `
        -OutputProfileCsv ".\build\Release\logs\profitability_profile_summary_exploratory.csv" `
        -OutputJson ".\build\Release\logs\profitability_gate_report_exploratory.json"
}

if ($LASTEXITCODE -ne 0) {
    throw "run_profitability_matrix.ps1 failed with exit code $LASTEXITCODE"
}

Write-Host "[ProfitabilityExploratory] Completed"
Write-Host "dataset_count=$(@($datasets).Count)"
Write-Host "report=build/Release/logs/profitability_gate_report_exploratory.json"
