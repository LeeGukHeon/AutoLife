param(
    [string]$MatrixScript = ".\scripts\run_profitability_matrix.ps1",
    [string]$BuildConfigPath = ".\build\Release\config\config.json",
    [string]$DataDir = ".\data\backtest",
    [string]$CuratedDataDir = ".\data\backtest_curated",
    [string]$OutputDir = ".\build\Release\logs",
    [string]$SummaryCsv = ".\build\Release\logs\candidate_trade_density_tuning_summary.csv",
    [string]$SummaryJson = ".\build\Release\logs\candidate_trade_density_tuning_summary.json"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-OrThrow {
    param(
        [string]$PathValue,
        [string]$Label
    )
    $resolved = Resolve-Path -Path $PathValue -ErrorAction SilentlyContinue
    if ($null -eq $resolved) {
        throw "$Label not found: $PathValue"
    }
    return $resolved.Path
}

function Ensure-ParentDirectory {
    param([string]$PathValue)
    $parent = Split-Path -Parent $PathValue
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path $parent)) {
        New-Item -Path $parent -ItemType Directory -Force | Out-Null
    }
}

function Set-OrAddProperty {
    param(
        $ObjectValue,
        [string]$Name,
        $Value
    )
    if ($null -eq $ObjectValue) {
        return
    }
    $prop = $ObjectValue.PSObject.Properties[$Name]
    if ($null -eq $prop) {
        $ObjectValue | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    } else {
        $ObjectValue.$Name = $Value
    }
}

function Ensure-StrategyNode {
    param(
        $ConfigObject,
        [string]$StrategyName
    )
    if ($null -eq $ConfigObject.strategies) {
        $ConfigObject | Add-Member -NotePropertyName strategies -NotePropertyValue ([pscustomobject]@{})
    }
    $prop = $ConfigObject.strategies.PSObject.Properties[$StrategyName]
    if ($null -eq $prop) {
        $ConfigObject.strategies | Add-Member -NotePropertyName $StrategyName -NotePropertyValue ([pscustomobject]@{})
    }
}

function Apply-CandidateComboToConfig {
    param(
        $ConfigObject,
        $Combo
    )

    if ($null -eq $ConfigObject.trading) {
        $ConfigObject | Add-Member -NotePropertyName trading -NotePropertyValue ([pscustomobject]@{})
    }

    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "max_new_orders_per_scan" -Value ([int]$Combo.max_new_orders_per_scan)
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "min_expected_edge_pct" -Value ([double]$Combo.min_expected_edge_pct)
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "min_reward_risk" -Value ([double]$Combo.min_reward_risk)
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "min_rr_weak_signal" -Value ([double]$Combo.min_rr_weak_signal)
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "min_rr_strong_signal" -Value ([double]$Combo.min_rr_strong_signal)
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "min_strategy_trades_for_ev" -Value ([int]$Combo.min_strategy_trades_for_ev)
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "min_strategy_expectancy_krw" -Value ([double]$Combo.min_strategy_expectancy_krw)
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "min_strategy_profit_factor" -Value ([double]$Combo.min_strategy_profit_factor)
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "avoid_high_volatility" -Value ([bool]$Combo.avoid_high_volatility)
    Set-OrAddProperty -ObjectValue $ConfigObject.trading -Name "avoid_trending_down" -Value ([bool]$Combo.avoid_trending_down)

    foreach ($strategyName in @("scalping", "momentum", "breakout", "mean_reversion")) {
        Ensure-StrategyNode -ConfigObject $ConfigObject -StrategyName $strategyName
    }

    Set-OrAddProperty -ObjectValue $ConfigObject.strategies.scalping -Name "min_signal_strength" -Value ([double]$Combo.scalping_min_signal_strength)
    Set-OrAddProperty -ObjectValue $ConfigObject.strategies.momentum -Name "min_signal_strength" -Value ([double]$Combo.momentum_min_signal_strength)
    Set-OrAddProperty -ObjectValue $ConfigObject.strategies.breakout -Name "min_signal_strength" -Value ([double]$Combo.breakout_min_signal_strength)
    Set-OrAddProperty -ObjectValue $ConfigObject.strategies.mean_reversion -Name "min_signal_strength" -Value ([double]$Combo.mean_reversion_min_signal_strength)
}

function Get-DatasetList {
    param([string[]]$Dirs)
    $all = @()
    foreach ($dirPath in $Dirs) {
        if (-not (Test-Path $dirPath)) {
            continue
        }
        $all += @(
            Get-ChildItem -Path $dirPath -File -Filter *.csv -ErrorAction SilentlyContinue |
                Sort-Object Name |
                ForEach-Object { $_.FullName }
        )
    }
    return @($all)
}

$resolvedMatrixScript = Resolve-OrThrow -PathValue $MatrixScript -Label "Matrix script"
$resolvedBuildConfig = Resolve-OrThrow -PathValue $BuildConfigPath -Label "Build config"
$resolvedOutputDir = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutputDir))
$resolvedSummaryCsv = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $SummaryCsv))
$resolvedSummaryJson = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $SummaryJson))
Ensure-ParentDirectory -PathValue $resolvedSummaryCsv
Ensure-ParentDirectory -PathValue $resolvedSummaryJson
if (-not (Test-Path $resolvedOutputDir)) {
    New-Item -Path $resolvedOutputDir -ItemType Directory -Force | Out-Null
}

$datasets = Get-DatasetList -Dirs @($DataDir, $CuratedDataDir)
if (@($datasets).Count -eq 0) {
    throw "No datasets found under DataDir/CuratedDataDir."
}

$comboSpecs = @(
    [pscustomobject]@{
        combo_id = "baseline_current"
        description = "Current baseline in build config."
        max_new_orders_per_scan = 2
        min_expected_edge_pct = 0.0010
        min_reward_risk = 1.20
        min_rr_weak_signal = 1.80
        min_rr_strong_signal = 1.20
        min_strategy_trades_for_ev = 30
        min_strategy_expectancy_krw = -2.0
        min_strategy_profit_factor = 0.95
        avoid_high_volatility = $true
        avoid_trending_down = $true
        scalping_min_signal_strength = 0.70
        momentum_min_signal_strength = 0.72
        breakout_min_signal_strength = 0.40
        mean_reversion_min_signal_strength = 0.40
    },
    [pscustomobject]@{
        combo_id = "trade_density_relaxed_a"
        description = "Moderately relaxed quality gates."
        max_new_orders_per_scan = 3
        min_expected_edge_pct = 0.0006
        min_reward_risk = 1.00
        min_rr_weak_signal = 1.40
        min_rr_strong_signal = 0.90
        min_strategy_trades_for_ev = 15
        min_strategy_expectancy_krw = -5.0
        min_strategy_profit_factor = 0.85
        avoid_high_volatility = $false
        avoid_trending_down = $false
        scalping_min_signal_strength = 0.62
        momentum_min_signal_strength = 0.60
        breakout_min_signal_strength = 0.35
        mean_reversion_min_signal_strength = 0.35
    },
    [pscustomobject]@{
        combo_id = "trade_density_relaxed_b"
        description = "Aggressively relaxed quality gates for sample expansion."
        max_new_orders_per_scan = 4
        min_expected_edge_pct = 0.0003
        min_reward_risk = 0.90
        min_rr_weak_signal = 1.20
        min_rr_strong_signal = 0.80
        min_strategy_trades_for_ev = 5
        min_strategy_expectancy_krw = -10.0
        min_strategy_profit_factor = 0.70
        avoid_high_volatility = $false
        avoid_trending_down = $false
        scalping_min_signal_strength = 0.52
        momentum_min_signal_strength = 0.50
        breakout_min_signal_strength = 0.30
        mean_reversion_min_signal_strength = 0.30
    },
    [pscustomobject]@{
        combo_id = "trade_density_balanced_c"
        description = "Balanced candidate between baseline and relaxed."
        max_new_orders_per_scan = 2
        min_expected_edge_pct = 0.0008
        min_reward_risk = 1.10
        min_rr_weak_signal = 1.60
        min_rr_strong_signal = 1.00
        min_strategy_trades_for_ev = 20
        min_strategy_expectancy_krw = -3.0
        min_strategy_profit_factor = 0.90
        avoid_high_volatility = $true
        avoid_trending_down = $false
        scalping_min_signal_strength = 0.66
        momentum_min_signal_strength = 0.64
        breakout_min_signal_strength = 0.35
        mean_reversion_min_signal_strength = 0.35
    },
    [pscustomobject]@{
        combo_id = "trade_density_ultra_open_d"
        description = "Ultra-open candidate to measure trade-density ceiling."
        max_new_orders_per_scan = 6
        min_expected_edge_pct = 0.0001
        min_reward_risk = 0.75
        min_rr_weak_signal = 1.00
        min_rr_strong_signal = 0.70
        min_strategy_trades_for_ev = 1
        min_strategy_expectancy_krw = -20.0
        min_strategy_profit_factor = 0.50
        avoid_high_volatility = $false
        avoid_trending_down = $false
        scalping_min_signal_strength = 0.45
        momentum_min_signal_strength = 0.42
        breakout_min_signal_strength = 0.25
        mean_reversion_min_signal_strength = 0.25
    },
    [pscustomobject]@{
        combo_id = "trade_density_ultra_open_e"
        description = "Maximum-open candidate for hard lower-bound diagnostics."
        max_new_orders_per_scan = 8
        min_expected_edge_pct = 0.0
        min_reward_risk = 0.60
        min_rr_weak_signal = 0.85
        min_rr_strong_signal = 0.60
        min_strategy_trades_for_ev = 1
        min_strategy_expectancy_krw = -40.0
        min_strategy_profit_factor = 0.30
        avoid_high_volatility = $false
        avoid_trending_down = $false
        scalping_min_signal_strength = 0.35
        momentum_min_signal_strength = 0.32
        breakout_min_signal_strength = 0.20
        mean_reversion_min_signal_strength = 0.20
    }
)

$originalBuildConfigRaw = Get-Content -Raw -Path $resolvedBuildConfig -Encoding UTF8
$rows = New-Object "System.Collections.Generic.List[object]"

try {
    foreach ($combo in $comboSpecs) {
        Write-Host ("[TuneCandidate] Running combo: {0}" -f $combo.combo_id)

        $cfg = $originalBuildConfigRaw | ConvertFrom-Json
        Apply-CandidateComboToConfig -ConfigObject $cfg -Combo $combo
        ($cfg | ConvertTo-Json -Depth 32) | Set-Content -Path $resolvedBuildConfig -Encoding UTF8

        $matrixCsvRelPath = Join-Path $OutputDir ("profitability_matrix_" + $combo.combo_id + ".csv")
        $profileCsvRelPath = Join-Path $OutputDir ("profitability_profile_summary_" + $combo.combo_id + ".csv")
        $reportJsonRelPath = Join-Path $OutputDir ("profitability_gate_report_" + $combo.combo_id + ".json")

        & $resolvedMatrixScript `
            -DatasetNames $datasets `
            -ExcludeLowTradeRunsForGate `
            -MinTradesPerRunForGate 1 `
            -OutputCsv $matrixCsvRelPath `
            -OutputProfileCsv $profileCsvRelPath `
            -OutputJson $reportJsonRelPath | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "run_profitability_matrix.ps1 failed for combo=$($combo.combo_id)"
        }

        $matrixCsvPath = Resolve-OrThrow -PathValue $matrixCsvRelPath -Label ("Matrix CSV (" + $combo.combo_id + ")")
        $profileCsvPath = Resolve-OrThrow -PathValue $profileCsvRelPath -Label ("Profile CSV (" + $combo.combo_id + ")")
        $reportJsonPath = Resolve-OrThrow -PathValue $reportJsonRelPath -Label ("Report JSON (" + $combo.combo_id + ")")
        $report = Get-Content -Raw -Path $reportJsonPath -Encoding UTF8 | ConvertFrom-Json
        $summary = $report.profile_summaries | Select-Object -First 1

        $rows.Add([pscustomobject]@{
            combo_id = [string]$combo.combo_id
            description = [string]$combo.description
            overall_gate_pass = [bool]$report.overall_gate_pass
            profile_gate_pass = [bool]$report.profile_gate_pass
            runs_used_for_gate = [int]$summary.runs_used_for_gate
            excluded_low_trade_runs = [int]$summary.excluded_low_trade_runs
            avg_profit_factor = [double]$summary.avg_profit_factor
            avg_expectancy_krw = [double]$summary.avg_expectancy_krw
            avg_total_trades = [double]$summary.avg_total_trades
            profitable_ratio = [double]$summary.profitable_ratio
            gate_profit_factor_pass = [bool]$summary.gate_profit_factor_pass
            gate_trades_pass = [bool]$summary.gate_trades_pass
            report_json = $reportJsonPath
            profile_csv = $profileCsvPath
            matrix_csv = $matrixCsvPath
        }) | Out-Null
    }
}
finally {
    $originalBuildConfigRaw | Set-Content -Path $resolvedBuildConfig -Encoding UTF8
}

if ($rows.Count -eq 0) {
    throw "No tuning rows generated."
}

$sortedRows = @(
    $rows |
    Sort-Object `
        @{ Expression = { [double]$_.avg_total_trades }; Descending = $true }, `
        @{ Expression = { [double]$_.avg_profit_factor }; Descending = $true }, `
        @{ Expression = { [double]$_.avg_expectancy_krw }; Descending = $true }
)

$sortedRows | Export-Csv -Path $resolvedSummaryCsv -NoTypeInformation -Encoding UTF8

$reportOut = [ordered]@{
    generated_at = (Get-Date).ToString("o")
    dataset_count = @($datasets).Count
    datasets = $datasets
    combos = $comboSpecs
    summary = $sortedRows
}
($reportOut | ConvertTo-Json -Depth 10) | Set-Content -Path $resolvedSummaryJson -Encoding UTF8

Write-Host "[TuneCandidate] Completed"
Write-Host "summary_csv=$resolvedSummaryCsv"
Write-Host "summary_json=$resolvedSummaryJson"
Write-Host "best_combo=$($sortedRows[0].combo_id)"
